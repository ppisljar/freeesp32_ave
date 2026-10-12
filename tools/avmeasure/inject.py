#!/usr/bin/env python3
"""Single-cycle fault injection, to measure what percycle.py can actually SEE.

WHY. Cutting false positives is easy if you also go blind, and the two failures
look identical from the outside: a detector that never fires and a clean device
both print "anomalies: 0". percycle.py's hardening (phase-lattice fit, ISR-tick
classification, NMS cross-check, segment-slip guard) was all tuned against
false positives on clean captures. This file measures the other half: take a
clean capture, break exactly ONE cycle by a known amount, and ask whether the
detector reports it.

PERTURB ONLY THE INTENDED QUANTITY. The earlier crude injector -- zero a window,
re-add it shifted -- changed amplitude, duty and spectrum at the same time as
timing, which is why every injected lateness from 1 to 10 ms came back as the
same -14.7 ms: the reported number was describing the injection artifact, not
the injection. Here:

  * TIMING is a monotone piecewise-linear TIME WARP, anchored at the middle of
    the neighbouring flat regions, so only the edge moves. The plateaus either
    side are stretched/compressed by delta/(quarter period) -- 2% for a 0.5 ms
    shift at 10 Hz -- and a stretched flat plateau is the same flat plateau.
    Nothing is cut and nothing is pasted, so there is no discontinuity anywhere.
  * AMPLITUDE multiplies the PULSE ONLY -- (x - off_level) over the ON region --
    with the window boundaries sitting mid-OFF, where that quantity is zero.
    So the injection starts and ends at zero and adds no step of its own.
  * DUTY moves the FALL alone, which is what a duty change physically is.
  * A DROPPED FLICK is amplitude -100%.

Every injection is VERIFIED against the independent NMS edge detector in
ratechange.py before the result is believed: inject 0.8 ms and NMS must see
0.8 ms move at that edge and nothing move anywhere else. An injector is a
measuring instrument too.

INJECT BEFORE THE FRONT-END FILTER. percycle.load() mean-removes and notches at
50 Hz (two 20 ms boxcars). A fault injected after that skips the smoothing a
real fault has to survive. So injection runs on percycle.read_window()'s raw
samples and the result goes through percycle.prep().

usage:
  inject.py FILE.wav CH FLICKER_HZ START_S DUR_S KIND MAG [OUT.wav]
      kinds: late (whole pulse late, ms) | edge (rise only, ms)
             duty (pp) | amp (fraction) | drop
      writes OUT.wav if given, else just reports what percycle makes of it
  inject.py --sweep FILE.wav CH FLICKER_HZ START_S DUR_S [--old]
      sweep every axis over magnitude at 10 positions and print the
      detection floor per axis. --old restores the pre-fix classifier
      settings, for the before/after table.
"""
from __future__ import annotations

import sys
import wave

import numpy as np

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import percycle as P
from ratechange import rising_edges


# ---------------------------------------------------------------------------
# site selection
# ---------------------------------------------------------------------------

def sites(x, fr, f0, n=10, edge_guard=25):
    """Pick n well-separated cycles that measure cleanly BEFORE injection.

    Injecting into a cycle that percycle already cannot grade measures nothing,
    and injecting near the window ends measures the convolution taper. Both
    guards matter: the first 1 s of a window can carry no signal at all (the
    session had not started), and a fault injected there is simply invisible for
    reasons that have nothing to do with sensitivity."""
    T, per, duty, amp = P.percycle(P.prep(x, fr), fr, f0)
    good = np.isfinite(per) & np.isfinite(duty) & np.isfinite(amp)
    good &= np.roll(good, 1) & np.roll(good, -1)
    med = np.nanmedian(amp)
    good &= amp > 0.8 * med           # not in a brightness trough either
    idx = np.where(good)[0]
    idx = idx[(idx > edge_guard) & (idx < len(per) - edge_guard)]
    if len(idx) < n:
        raise SystemExit(f"only {len(idx)} usable cycles for injection")
    pick = idx[np.linspace(0, len(idx) - 1, n).astype(int)]
    out = []
    for i in pick:
        pr = per[i]
        out.append(dict(i=int(i), t=float(T[i]), period=float(pr),
                        duty=float(duty[i]),
                        rise=float(T[i] * fr),
                        fall=float((T[i] + duty[i] * pr) * fr)))
    return out


# ---------------------------------------------------------------------------
# the three perturbations
# ---------------------------------------------------------------------------

def _warp(x, a, e, b, delta):
    """Move the content at sample e to e+delta, holding a and b fixed.

    Monotone and piecewise-linear in time, so the signal stays continuous and
    single-valued: no sample is duplicated or dropped, which is the whole point
    of warping instead of cutting and pasting."""
    a, b = int(np.floor(a)), int(np.ceil(b))
    if not (0 <= a < e < b <= len(x)):
        raise ValueError("warp window outside the signal")
    e2 = e + delta
    if not (a < e2 < b):
        raise ValueError("displacement larger than the anchor span")
    p = np.arange(a, b, dtype=float)
    src = np.where(p <= e2,
                   a + (p - a) * (e - a) / (e2 - a),
                   e + (p - e2) * (b - e) / (b - e2))
    out = x.copy()
    out[a:b] = np.interp(src, np.arange(len(x), dtype=float), x)
    return out


def shift_rise(x, fr, s, dt_ms):
    """rise moves, fall stays -> period pair +/-dt AND duty shrinks by dt/T."""
    d = dt_ms * 1e-3 * fr
    on, off = s['duty'] * s['period'] * fr, (1 - s['duty']) * s['period'] * fr
    return _warp(x, s['rise'] - 0.5 * off, s['rise'], s['rise'] + 0.5 * on, d)


def shift_fall(x, fr, s, dt_ms):
    """fall moves, rise stays -> duty only."""
    d = dt_ms * 1e-3 * fr
    on, off = s['duty'] * s['period'] * fr, (1 - s['duty']) * s['period'] * fr
    return _warp(x, s['fall'] - 0.5 * on, s['fall'], s['fall'] + 0.5 * off, d)


def shift_pulse(x, fr, s, dt_ms):
    """the WHOLE flick is late: rise and fall both move, duty unchanged.

    RIGID over the pulse. Two single-edge warps composed would do the job on a
    square -- stretching a flat plateau changes nothing -- but on a SMOOTH
    carrier they do not: the "edge" of a sine spans a whole quarter period, so
    stretching one side of it and compressing the other reshapes the flank
    instead of translating it, and the steepest point then moves by less than
    asked. Measured on nd.wav ch2 (sine carrier): 2.0 ms asked, 0.87 ms
    delivered; 8.0 ms asked, 4.55 ms delivered. Every sine-carrier timing
    sensitivity figure taken with that injector was measuring the injector.
    Here the map is identity outside the cycle, a RIGID +dt through the whole
    pulse, and the two ramps that get it there live in the OFF/trough halves."""
    d = dt_ms * 1e-3 * fr
    on, off = s['duty'] * s['period'] * fr, (1 - s['duty']) * s['period'] * fr
    A, B = s['rise'] - 0.5*off, s['fall'] + 0.5*off
    m1, m2 = s['rise'] - 0.1*off, s['fall'] + 0.1*off
    a, b = int(np.floor(A)), int(np.ceil(B))
    if a < 0 or b > len(x):
        raise ValueError("warp window outside the signal")
    if abs(d) >= 0.9*min(m1-A, B-m2):
        raise ValueError("displacement larger than the ramp span")
    p = np.arange(a, b, dtype=float)
    src = np.where(p <= m1, p - d*np.clip((p-A)/(m1-A), 0, 1),
                   np.where(p <= m2, p - d,
                            p - d*np.clip((B-p)/(B-m2), 0, 1)))
    out = x.copy()
    out[a:b] = np.interp(src, np.arange(len(x), dtype=float), x)
    return out


def _hp1(d, fr, hz):
    """one-pole high-pass, applied to the injected DIFFERENCE only."""
    if hz <= 0:
        return d
    a = 1.0 / (1.0 + 2 * np.pi * hz / fr)
    y = np.empty_like(d)
    yp = 0.0
    xp = 0.0
    for i in range(len(d)):
        yp = a * (yp + d[i] - xp)
        xp = d[i]
        y[i] = yp
    return y


def scale_amp(x, fr, s, frac, hp_hz=0.0):
    """Scale ONE pulse by (1+frac), leaving the OFF baseline alone.

    frac = -1 drops the flick entirely. The window runs mid-OFF to mid-OFF so
    the injected difference is zero at both ends; with hp_hz>0 that difference
    is then high-passed, which is what the sensor's 100 nF coupling does to a
    real brightness change (it leaves a small baseline recovery behind a
    dropped pulse)."""
    on, off = s['duty'] * s['period'] * fr, (1 - s['duty']) * s['period'] * fr
    a = int(s['rise'] - 0.5 * off)
    b = int(s['fall'] + 0.5 * off)
    if a < 1 or b >= len(x):
        raise ValueError("amplitude window outside the signal")
    # OFF level either side, from the middle 60% of each OFF region
    def off_med(c0, c1):
        c0, c1 = int(c0), int(c1)
        m = x[c0:c1]
        return float(np.median(m)) if len(m) > 3 else float(x[c0])
    lo = off_med(s['rise'] - 0.8 * off, s['rise'] - 0.2 * off)
    hi = off_med(s['fall'] + 0.2 * off, s['fall'] + 0.8 * off)
    base = np.linspace(lo, hi, b - a)
    d = np.zeros_like(x)
    d[a:b] = frac * (x[a:b] - base)
    if hp_hz > 0:
        d = _hp1(d, fr, hp_hz)
    return x + d


KINDS = {
    'late': lambda x, fr, s, m: shift_pulse(x, fr, s, m),
    'edge': lambda x, fr, s, m: shift_rise(x, fr, s, m),
    'duty': lambda x, fr, s, m: shift_fall(x, fr, s, m * s['period'] * 1000.0 / 100.0),
    'amp':  lambda x, fr, s, m: scale_amp(x, fr, s, m),
    'drop': lambda x, fr, s, m: scale_amp(x, fr, s, -1.0),
}
UNIT = {'late': 'ms', 'edge': 'ms', 'duty': 'pp', 'amp': 'x', 'drop': ''}


# ---------------------------------------------------------------------------
# did the injector do what it claimed?
# ---------------------------------------------------------------------------

def verify(x0, x1, fr, f0, s, want_ms):
    """Measure the achieved edge displacement with the INDEPENDENT detector.

    Returns (moved_ms, worst_other_ms). A trustworthy injector moves the target
    edge by want_ms and every other edge in the neighbourhood by ~0."""
    t = s['rise'] / fr
    a, b = max(0.0, t - 0.45), min(len(x0) / fr, t + 0.45)
    e0 = rising_edges(P.prep(x0, fr), fr, a, b, min_gap_s=0.75 / f0)
    e1 = rising_edges(P.prep(x1, fr), fr, a, b, min_gap_s=0.75 / f0)
    if len(e0) < 3 or len(e0) != len(e1):
        return float('nan'), float('nan')
    d = (e1 - e0) * 1000.0
    k = int(np.argmin(np.abs(e0 - t)))
    other = np.delete(d, k)
    return float(d[k]), float(np.max(np.abs(other))) if len(other) else 0.0


# ---------------------------------------------------------------------------
# run one injection through the detector
# ---------------------------------------------------------------------------

def detect(x, fr, f0, s, kind, mag, tol_cycles=1.6):
    """Inject, grade, and report what happened AT THE INJECTED CYCLE."""
    y = KINDS[kind](x, fr, s, mag)
    xp = P.prep(y, fr)
    T, per, duty, amp = P.percycle(xp, fr, f0)
    g = P.grade(xp, fr, f0, T, per, duty, amp, getattr(P.percycle, 'last_slips', 0))
    t0 = s['t']
    tol = tol_cycles / f0
    near = [h for h in g['hits'] if abs(h[0] - t0) <= tol]
    # BLINDNESS MODE, counted rather than merely missed: the injected cycle was
    # declared unmeasurable. Matched by TIME, never by index -- an injection can
    # change the cycle COUNT, and an index-matched check then compares the wrong
    # cycle and reports a fault as clean.
    k = int(np.argmin(np.abs(T - t0))) if len(T) else -1
    graded = bool(k >= 0 and k < len(per) and np.isfinite(per[k]))
    return dict(hit=bool(near), near=near, g=g, y=y, graded=graded)


def baseline(x, fr, f0):
    xp = P.prep(x, fr)
    T, per, duty, amp = P.percycle(xp, fr, f0)
    return P.grade(xp, fr, f0, T, per, duty, amp,
                   getattr(P.percycle, 'last_slips', 0))


# ---------------------------------------------------------------------------
# sweep
# ---------------------------------------------------------------------------

LADDERS = {
    'late': [0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 1.0, 1.5, 2.0, 3.0, 5.0,
             8.0, 12.0, 20.0],
    'edge': [0.3, 0.5, 0.7, 1.0, 2.0, 5.0],
    'duty': [0.1, 0.2, 0.3, 0.4, 0.5, 0.7, 1.0, 1.5, 2.0, 3.0, 5.0, 8.0, 12.0],
    'amp':  [-0.01, -0.02, -0.03, -0.05, -0.07, -0.10, -0.15, -0.25, -0.40],
    'drop': [0.0],
}


def sweep(path, ch, f0, t0, dur, kinds=None, npos=10, need=9):
    x, fr = P.read_window(path, ch, t0, dur)
    base = baseline(x, fr, f0)
    ss = sites(x, fr, f0, npos)
    print(f"== {path} ch{ch} {f0} Hz  device {t0}..{t0+dur} s")
    print(f"   clean: {base['n']} cycles, {len(base['hits'])} anomalies, "
          f"{base['ticks']} ticks, {base['unconfirmed']} discarded, "
          f"{base['slips']} ungraded;  noise {base['sig'][0]:.3f} ms / "
          f"{base['sig'][1]:.2f} pp / {100*base['sig'][2]:.1f}%")
    print(f"   thresholds {base['thr'][0]:.3f} ms / {base['thr'][1]:.2f} pp / "
          f"{100*base['thr'][2]:.1f}%")
    out = {}
    for kind in (kinds or ['late', 'duty', 'amp', 'drop']):
        print(f"   -- {kind}")
        floor = None
        for mag in LADDERS[kind]:
            nhit = 0
            kinds_seen = {}
            extra = 0
            ungraded = 0
            tickified = 0
            achieved = []
            for s in ss:
                try:
                    r = detect(x, fr, f0, s, kind, mag)
                except ValueError as e:
                    continue
                nhit += r['hit']
                ungraded += (not r['graded'])
                if not r['hit'] and r['g']['ticks'] > base['ticks']:
                    tickified += 1
                for h in r['near']:
                    kinds_seen[h[1]] = kinds_seen.get(h[1], 0) + 1
                extra += max(0, len(r['g']['hits']) - len(r['near'])
                             - len(base['hits']))
                if kind in ('late', 'edge'):
                    mv, oth = verify(x, r['y'], fr, f0, s, mag)
                    achieved.append(mv)
            av = (f" achieved {np.nanmean(achieved):+.2f} ms"
                  if achieved and np.isfinite(np.nanmean(achieved)) else "")
            print(f"      {mag:+7.3f} {UNIT[kind]:2s}  detected {nhit}/{len(ss)}"
                  f"  as {kinds_seen or '-'}  ungraded {ungraded}"
                  f"  tick-absorbed {tickified}  collateral {extra}{av}")
            if nhit >= need and floor is None:
                floor = mag
        out[kind] = floor
        print(f"      FLOOR({kind}) = {floor}")
    return out


def write_wav(src, dst, ch, t0_dev, y):
    """Copy src to dst with channel ch's window [t0_dev, +len(y)) replaced.

    Everything else -- the other channels, the sync marker, the lead-in -- is
    copied byte-for-byte, so the written file goes through the SAME marker
    alignment as the original and `percycle.py dst ...` is a like-for-like
    rerun of `percycle.py src ...`."""
    w = wave.open(src)
    fr, nch, n = w.getframerate(), w.getnchannels(), w.getnframes()
    d = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, nch).copy()
    # same marker search as percycle.read_window, on the same channel
    probe = d[:int(12*fr), ch-1].astype(np.float64)/32768
    k = int(fr/400)
    e = np.convolve(np.abs(probe-probe.mean()), np.ones(k)/k, mode='same')
    T0 = float(np.where(e > e.max()*0.5)[0][0])/fr
    a = int((T0+t0_dev)*fr)
    d[a:a+len(y), ch-1] = np.clip(np.round(y*32768), -32768, 32767)
    o = wave.open(dst, 'wb')
    o.setnchannels(nch); o.setsampwidth(2); o.setframerate(fr)
    o.writeframes(d.astype('<i2').tobytes()); o.close()


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    if a[0] == '--sweep':
        a = a[1:]
        if '--old' in a:
            a.remove('--old')
            (P.TICK_BAND, P.XCHECK_MIN_MS,
             P.DARK_DETECT, P.PULSE_ANCHORED) = 0.30, 0.0, False, False
            print("[pre-fix classifier settings]")
        kinds = None
        if '--kinds' in a:
            i = a.index('--kinds'); kinds = a[i+1].split(','); del a[i:i+2]
        sweep(a[0], int(a[1]), float(a[2]), float(a[3]), float(a[4]), kinds)
        raise SystemExit(0)
    if len(a) < 6 or a[0] in ('-h', '--help'):
        # Match the other tools: usage on stdout, exit 2 for operator error, so a
        # typo cannot be mistaken for "the device is fine".
        print(__doc__)
        raise SystemExit(2)
    path, ch, f0, t0, dur, kind = a[0], int(a[1]), float(a[2]), float(a[3]), float(a[4]), a[5]
    mag = float(a[6]) if len(a) > 6 and kind != 'drop' else 0.0
    x, fr = P.read_window(path, ch, t0, dur)
    ss = sites(x, fr, f0, 1)
    s = ss[0]
    r = detect(x, fr, f0, s, kind, mag)
    print(f"injected {kind} {mag} at device t={t0+s['t']:.3f}s")
    if kind in ('late', 'edge'):
        mv, oth = verify(x, r['y'], fr, f0, s, mag)
        print(f"   independent NMS check: target edge moved {mv:+.3f} ms, "
              f"worst other edge {oth:+.3f} ms")
    for h in r['g']['hits']:
        print(f"   t={h[0]:8.3f}s {h[1]:9s} {h[2]:+8.3f} {h[3]}")
    print("DETECTED" if r['hit'] else "MISSED")
    if len(a) > 7:
        write_wav(path, a[7], ch, t0, r['y'])
        print(f"   wrote {a[7]}")
