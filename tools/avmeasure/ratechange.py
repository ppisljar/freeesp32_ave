#!/usr/bin/env python3
"""Find the instant a flicker channel actually changed rate.

The per-cycle offset from avsync.py is only known modulo the period, which makes
it useless for "did the rate change land on time?" when the error can exceed half
a period.  This measures the changepoint directly: detect every rising edge
across the transition, look at consecutive intervals, and report the first edge
whose following interval belongs to the NEW period rather than the old one.

usage: ratechange.py <wav> --light <ch> --audio <ch> --f-old <Hz> --f-new <Hz>
                     --entry <s> --win <s> [--marker-hz 3000]
       <entry> is the .ledc entry time in DEVICE seconds.
"""
import sys
import numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from avsync import load, find_t0, cma


def rising_edges(sig, fr, t0, t1, min_gap_s):
    """Rising-edge times via amplitude-ranked non-maximum suppression.

    A naive "derivative above a robust threshold, then skip min_gap" scan does
    NOT work here. Behind the 100 nF / Hi-Z high-pass the OFF phase recovers
    toward baseline with a positive slope, so a 50%-duty cycle offers a second,
    broader positive bump. A left-to-right scan locks onto whichever comes
    first, and with min_gap at half a period both survive: an 8 Hz 50% cycle
    measured as intervals 63.3 / 62.9 ms (the ON and OFF durations) instead of
    125 ms. Ranking candidates by slope and suppressing neighbours keeps the
    true edge and discards the recovery bump regardless of order.
    """
    a, b = int(t0 * fr), int(t1 * fr)
    s = cma(sig[a:b].astype(float), max(3, fr * 0.0005))
    dv = np.gradient(s)
    thr = 0.35 * np.percentile(dv, 99.9)      # real edges are far steeper than
    if thr <= 0:                              # the baseline recovery
        return np.array([])
    cand = np.where((dv > thr) & (dv >= np.roll(dv, 1)) & (dv > np.roll(dv, -1)))[0]
    cand = cand[(cand > 0) & (cand < len(dv) - 1)]
    order = cand[np.argsort(-dv[cand])]       # strongest slope first
    gap = int(min_gap_s * fr)
    kept = []
    for i in order:
        if all(abs(int(i) - int(j)) >= gap for j in kept):
            kept.append(int(i))
    out = []
    for i in sorted(kept):
        y0, y1, y2 = dv[i - 1], dv[i], dv[i + 1]
        den = y0 - 2 * y1 + y2
        dx = 0.0 if den == 0 else max(-0.5, min(0.5, 0.5 * (y0 - y2) / den))
        out.append((a + i + dx) / fr)
    return np.array(out)


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    wav = argv[0]
    o = {}
    i = 1
    while i < len(argv):
        o[argv[i].lstrip('-')] = argv[i + 1]
        i += 2
    lch = int(o.get('light', 1)) - 1
    ach = int(o.get('audio', 3)) - 1
    f_old, f_new = float(o['f-old']), float(o['f-new'])
    entry = float(o['entry'])
    win = float(o.get('win', 3.0))
    d, fr = load(wav)
    T0 = find_t0(d, fr, ach, float(o.get('marker-hz', 3000)))
    p_old, p_new = 1.0 / f_old, 1.0 / f_new
    e = rising_edges(d[:, lch], fr, T0 + entry - win, T0 + entry + win,
                     min_gap_s=0.75 * min(p_old, p_new))
    if len(e) < 4:
        raise SystemExit(f"only {len(e)} edges found")
    iv = np.diff(e)
    # classify each interval as old or new by nearest period
    is_new = np.abs(iv - p_new) < np.abs(iv - p_old)
    k = None
    for j in range(len(is_new) - 1):
        if not is_new[j] and is_new[j + 1] and all(is_new[j + 1:j + 3]):
            k = j + 1
            break
    print(f"  edges {len(e)}   intervals (ms): "
          + " ".join(f"{v*1000:.1f}" for v in iv[:14]))
    if k is None:
        print("  no clean changepoint found")
        return
    t_change = e[k] - T0
    print(f"  last {f_old:g} Hz interval ends / first {f_new:g} Hz cycle starts at "
          f"device t = {t_change*1000:.1f} ms")
    print(f"  .ledc entry at {entry*1000:.1f} ms  ->  LED rate change is "
          f"{(t_change - entry)*1000:+.1f} ms late")
    print(f"  one old period = {p_old*1000:.2f} ms")


if __name__ == '__main__':
    main(sys.argv[1:])
