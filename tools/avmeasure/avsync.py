#!/usr/bin/env python3
"""Measure the audio-vs-LED timing offset of a capture.

Both the LED light envelope and the audio gate envelope are square waves at the
same commanded rate, so the offset between their rising edges IS the A/V sync
error.  Two things make this trustworthy:

  * Edges are located by the PEAK OF THE DERIVATIVE inside each cycle, not by a
    threshold crossing.  The photocell sits behind a 100 nF cap into a Hi-Z
    input (~1.6 Hz high-pass), so the square droops and a fixed threshold is
    crossed several times per cycle -- that counted 42 edges where 27 existed.
    A drooping square still has one sharp slope spike per edge.
  * All smoothing is a CENTERED moving average, which is zero-phase.  A causal
    filter (or scipy's lfilter) would add its own group delay straight into the
    number being measured.

Run the same .ledc at several rates: a real latency holds a constant TIME
offset, while an envelope-shape artifact holds a constant PHASE.  Reporting
both models side by side is what distinguishes them.

usage: avsync.py <wav> --light <ch> --audio <ch> --rates <f0:t0:t1>[,...] [--marker-hz 3000]
       channels are 1-based; t0/t1 are seconds of DEVICE time
"""
import sys, wave
import numpy as np


def cma(x, n):
    """centered (zero-phase) moving average"""
    n = max(3, int(n) | 1)
    return np.convolve(x, np.ones(n) / n, mode='same')


def load(path):
    w = wave.open(path)
    nch, fr = w.getnchannels(), w.getframerate()
    raw = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2')
    return raw.reshape(-1, nch).astype(np.float64) / 32768.0, fr


def find_t0(d, fr, ch, hz):
    """device t=0 = onset of the marker tone burst"""
    seg = d[:int(6 * fr), ch]
    t = np.arange(len(seg)) / fr
    env = cma(np.abs(seg * np.cos(2 * np.pi * hz * t)), fr * 0.002)
    on = np.where(env > 0.3 * env.max())[0]
    if len(on) == 0:
        raise SystemExit("no marker tone found -- was --marker-hz right?")
    return on[0] / fr


def edge_times(sig, fr, f0, t0, t1, smooth_ms=0.5):
    a, b = int(t0 * fr), int(t1 * fr)
    s = cma(sig[a:b].astype(float), fr * smooth_ms / 1000.0)
    dv = np.gradient(s)
    nn, per = len(s), fr / f0
    t = np.arange(nn) / fr
    # anchor the cycle grid on the fundamental phase of the derivative
    ph = np.angle(np.sum(dv * np.exp(-2j * np.pi * f0 * t)))
    first = ((-ph / (2 * np.pi)) % 1.0) * per
    out, k = [], 0
    while True:
        c = first + k * per
        lo, hi = int(c - per * 0.4), int(c + per * 0.4)
        if hi >= nn:
            break
        if lo < 0:
            k += 1
            continue
        seg = dv[lo:hi]
        i = int(np.argmax(seg))
        dx = 0.0
        if 0 < i < len(seg) - 1:
            y0, y1, y2 = seg[i - 1], seg[i], seg[i + 1]
            den = y0 - 2 * y1 + y2
            if den != 0:
                dx = max(-0.5, min(0.5, 0.5 * (y0 - y2) / den))  # clamp: a flat
                #   peak divides by ~0 and would return unsorted edge times
        out.append((a + lo + i + dx) / fr)
        k += 1
    return np.array(out)


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    wav = argv[0]
    opt = {}
    i = 1
    while i < len(argv):
        opt[argv[i].lstrip('-')] = argv[i + 1]
        i += 2
    lch = int(opt.get('light', 1)) - 1
    ach = int(opt.get('audio', 3)) - 1
    carrier = float(opt.get('carrier', 400))
    mk = float(opt.get('marker-hz', 3000))
    rates = []
    for part in opt['rates'].split(','):
        f0, s0, s1 = part.split(':')
        rates.append((float(f0), float(s0), float(s1)))

    d, fr = load(wav)
    T0 = find_t0(d, fr, ach, mk)
    aenv = cma(np.abs(d[:, ach]), fr / carrier)
    print(f"  device t=0 at {T0:.4f} s   light=ch{lch+1} audio=ch{ach+1}")
    print("  rate  expect  n_light  n_audio    audio - light      robust sd")
    rows = []
    for f0, s0, s1 in rates:
        a0, a1 = T0 + s0 + 1.0, T0 + s1 - 0.5
        le = edge_times(d[:, lch], fr, f0, a0, a1)
        ae = edge_times(aenv, fr, f0, a0, a1)
        off = []
        for ta in ae:
            if len(le) == 0:
                break
            j = int(np.argmin(np.abs(le - ta)))
            dt = ta - le[j]
            if abs(dt) < 0.45 / f0:
                off.append(dt)
        if len(off) < 3:
            print(f"  {f0:4.1f}  too few paired edges")
            continue
        off = np.array(off) * 1000.0
        med = float(np.median(off))
        sd = float(np.median(np.abs(off - med)) * 1.4826)
        rows.append((f0, med, sd))
        print(f"  {f0:4.1f}  {int((a1-a0)*f0):6d}  {len(le):7d}  {len(ae):7d}"
              f"    {med:+8.2f} ms       {sd:5.2f} ms (n={len(off)})")
    if len(rows) >= 2:
        ms = np.array([r[1] for r in rows])
        fs = np.array([r[0] for r in rows])
        ph = ms / 1000 * fs * 360
        print(f"  constant-latency model: spread {ms.max()-ms.min():6.2f} ms")
        print(f"  constant-phase   model: spread {ph.max()-ph.min():6.1f} deg")
    if rows:
        print(f"  OFFSET {np.mean([r[1] for r in rows]):+.2f} ms "
              f"(positive = audio late)")


if __name__ == '__main__':
    main(sys.argv[1:])
