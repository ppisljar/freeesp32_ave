#!/usr/bin/env python3
"""Per-cycle anomaly detection: find the ONE flick in a thousand that is wrong.

Aggregate measures -- mean rate, harmonic content, duty -- are blind to
single-cycle events. A dropped flick once per 12 s LFO period moves the mean
rate by 0.01% and the harmonics not at all. One was shipped and verified as
"fixed, h3 36x better" while it was dropping a pulse at the top of every
breath. This module exists because that class of defect is exactly what a
therapeutic device must not have and what averages cannot see.

TWO INDEPENDENT AXES, measured per cycle on NON-OVERLAPPING, phase-aligned
segments:

  amplitude  -- compared against a local median, because a brightness LFO is a
                legitimate slow amplitude change and must not be flagged
  timing     -- phase residual after removing the constant-rate trend,
                expressed in ms

Non-overlapping matters. A sliding one-cycle average smears each cycle into its
neighbours and halves the apparent deviation: with that estimator a pulse at
50% amplitude was MISSED entirely. Measure each cycle on its own samples.

MEASURED SENSITIVITY (10 Hz, real capture, 500 cycles, 0 false positives):

    fault                         detected
    pulse absent                  yes, amp -1.000
    pulse at 50% amplitude        yes, amp -0.488
    pulse at 70%                  yes, amp -0.283
    pulse at 80%                  yes
    pulse at 85%                  yes
    pulse at 90%                  NO     <- floor is ~15% amplitude
    pulse at 120%                 yes
    pulse at 150%                 yes, amp +0.451
    one pulse 20 ms late          yes, time -19.7 ms
    one pulse  5 ms late          yes, time  -4.5 ms
    one pulse  3 ms late          yes, time  -2.5 ms
    one pulse  2 ms late          yes
    one pulse  1.5 ms late        NO     <- floor is ~2 ms timing
    extra pulse inserted          yes
    3 ms permanent phase slip     yes

So: amplitude deviations below ~15% and timing errors below ~2 ms are NOT
detected. Those floors come from sensor noise on this rig (clean baseline
spread was +/-4% amplitude, 0.39 ms timing sd), not from anything fundamental
-- better shielding would lower them.

LIMITATIONS. Assumes a constant flicker rate over the analysed window, so it
cannot be pointed at a rate sweep without adaptation. Needs the rate as input.
Run signalcheck.py first: on a capture with no signal this will happily report
hundreds of confident anomalies.

usage: percycle.py FILE.wav CH FLICKER_HZ [start_s] [dur_s]
"""
import sys, wave, numpy as np

AMP_TOL   = 0.12     # ~4 sigma of the measured clean baseline (+/-4%)
TIME_TOL  = 1.2      # ms, ~3 sigma of the measured clean baseline (0.39 ms)
LOCAL_WIN = 11       # cycles either side for the local-median reference

def find_t0(path, ch):
    """device t=0 = the start of the sync marker burst. Without this the window
    silently includes the marker and the silent tail, and EVERY cycle in those
    regions is reported as an anomaly: 302 of them on a capture that is clean."""
    w = wave.open(path); fr = w.getframerate(); n = w.getnchannels()
    d = np.frombuffer(w.readframes(int(12*fr)), dtype='<i2').reshape(-1, n).astype(np.float64)[:, ch-1]/32768
    k = int(fr/400); e = np.convolve(np.abs(d - d.mean()), np.ones(k)/k, mode='same')
    return float(np.where(e > e.max()*0.5)[0][0])/fr

def load(path, ch, t0, dur, notch_hz=50.0):
    """t0/dur are DEVICE time, measured from the sync marker."""
    t0 = t0 + find_t0(path, ch)
    w = wave.open(path); fr = w.getframerate(); n = w.getnchannels()
    w.setpos(int(t0 * fr))
    N = w.getnframes() - int(t0 * fr)
    if dur: N = min(N, int(dur * fr))
    x = np.frombuffer(w.readframes(N), dtype='<i2').reshape(-1, n).astype(np.float64)[:, ch-1] / 32768
    x = x - x.mean()
    if notch_hz:                      # boxcar nulls sit on notch_hz and its harmonics
        L = int(round(fr / notch_hz))
        for _ in range(2): x = np.convolve(x, np.ones(L)/L, mode='same')
    return x, fr

def percycle(x, fr, f0):
    W = int(round(fr / f0)); n = len(x) // W
    seg = x[:n*W].reshape(n, W)
    win = np.hanning(W)
    e = np.exp(-2j*np.pi*f0*np.arange(W)/fr) * win
    z = seg @ e / win.sum() * 2
    A = np.abs(z); P = np.unwrap(np.angle(z))
    # fit the constant-rate trend on the INTERIOR only; the first and last few
    # cycles sit next to block boundaries and would tilt the line
    m = max(5, n//20); idx = np.arange(n)
    k = np.polyfit(idx[m:n-m], P[m:n-m], 1)
    dt = (P - np.polyval(k, idx)) / (2*np.pi*f0) * 1000.0
    return (np.arange(n)*W + W/2)/fr, A, dt

def anomalies(T, A, dt, amp_tol=AMP_TOL, t_tol=TIME_TOL, win=LOCAL_WIN):
    loc = np.array([np.median(A[max(0,i-win):i+win+1]) for i in range(len(A))])
    rel = A/np.maximum(loc, 1e-12); mdt = np.median(dt)
    out = []
    for i in range(win, len(A)-win):
        if abs(rel[i]-1) > amp_tol:   out.append((T[i], 'amplitude', rel[i]-1, 'x'))
        elif abs(dt[i]-mdt) > t_tol:  out.append((T[i], 'timing',    dt[i]-mdt, 'ms'))
    return out

if __name__ == '__main__':
    a = sys.argv[1:]
    x, fr = load(a[0], int(a[1]), float(a[3]) if len(a)>3 else 0.0,
                 float(a[4]) if len(a)>4 else None)
    T, A, dt = percycle(x, fr, float(a[2]))
    hits = anomalies(T, A, dt)
    print(f"  {len(A)} cycles analysed")
    # amplitude spread must be quoted against the LOCAL median: a brightness
    # LFO legitimately swings the global figure to +/-70% while every cycle is
    # within a few percent of its neighbours.
    loc = np.array([np.median(A[max(0,i-LOCAL_WIN):i+LOCAL_WIN+1]) for i in range(len(A))])
    rel = (A/np.maximum(loc,1e-12))[LOCAL_WIN:-LOCAL_WIN]
    print(f"  baseline: amplitude +/-{100*np.percentile(np.abs(rel-1),95):.1f}% of local  "
          f"timing sd {np.std(dt[LOCAL_WIN:-LOCAL_WIN]):.2f} ms")
    print(f"  anomalies: {len(hits)}")
    for t, kind, v, u in hits:
        print(f"    t={t:8.3f}s  {kind:9s} {v:+8.3f} {u}")
    sys.exit(1 if hits else 0)
