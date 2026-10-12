#!/usr/bin/env python3
"""Per-cycle anomaly detection: find the ONE flick in a thousand that is wrong.

Aggregate measures cannot see single-cycle faults. The dropped-flick bug fixed
in 2f83be6 moved the mean rate by 0.01% and the harmonics not at all -- it was
introduced, measured as "h3 36x better", and shipped, while dropping a pulse at
the top of every breath.

THREE axes per cycle: timing, duty, amplitude.

METHOD. Segment the signal by DEMODULATED PHASE at the known flicker rate, then
locate the rise and fall inside each cycle window. Phase segmentation is
tolerant of amplitude swings and noise; edges inside a known window give the
precision. There is no amplitude threshold anywhere, which matters because a
brightness LFO legitimately swings the signal several-fold -- a thresholded
edge detector simply loses every cycle in the dim half and reports the missing
data as wild timing noise.

Residuals are taken against a SLIDING quadratic that excludes the point itself.
Quadratic so a rate sweep is tracked rather than flagged; excluding the point so
a real outlier cannot pull its own reference and hide.

MEASURED NOISE FLOORS (real captures, this rig):

    capture              cycles     timing     duty      amplitude
    square, bright      116/120    0.113 ms   0.06 pp      0.2%
    square + LFO        326/330    0.109 ms   0.12 pp      0.8%
    10->6 Hz sweep      301/380    0.217 ms   0.14 pp      0.2%
    sine carrier, dim   496/500    1.263 ms   0.90 pp      0.6%

Practical sensitivity is roughly 5 sigma of those: about 0.5 ms timing and
0.5 pp duty on a square carrier, a few percent on amplitude. A smooth (sine)
carrier has no sharp edge, so its timing floor is ~10x worse; for those the
fundamental's phase is the better estimator.

Injection testing confirms DETECTION of late pulses during a sweep and of duty
changes down to ~2 pp, with zero false positives. It does NOT validate the
reported magnitudes for those two: the crude injection used (zero a window,
re-add it shifted) perturbs more than the intended quantity, and every lateness
from 1-10 ms reported the same -14.7 ms. Trust the detections and the noise
floors; treat timing/duty magnitudes as indicative until a cleaner injector
exists.

TWO TRAPS, both of which produced hundreds of confident false anomalies:
  - the window must be aligned to the sync MARKER; a raw file offset includes
    the marker burst and the silent tail, and every cycle there reads as an
    anomaly (302 on a clean capture).
  - the parabolic sub-sample edge refinement MUST be clamped to +/-0.5 samples.
    Unclamped, a flat peak divides by ~0, which returns UNSORTED edge times and
    periods of +/-6 seconds.

Run signalcheck.py FIRST. On a capture with no signal this reports hundreds of
anomalies with complete confidence.

usage: percycle.py FILE.wav CH FLICKER_HZ START_S DUR_S
       (START_S is DEVICE time, measured from the sync marker)
"""
import sys
import numpy as np, wave

# Thresholds are NOT fixed. A square carrier's timing floor is ~0.11 ms and a
# smooth (sine) carrier's is ~1.3 ms -- a 10x spread -- so any constant
# tolerance is either blind on one and screaming on the other (a fixed 0.6 ms
# produced 374 "anomalies" on a clean sine capture). Each capture calibrates
# its own thresholds from its own noise.
#
# Robust sigma (MAD), not standard deviation: the anomalies we are hunting
# would otherwise inflate the very threshold meant to catch them. On the broken
# firmware the raw sd was 3.16 ms against a true floor of ~1.3 ms, which would
# have hidden the smaller events behind the larger ones.
NSIGMA      = 5.0
FLOOR_TIME_MS = 0.30
FLOOR_DUTY_PP = 0.30
FLOOR_AMP     = 0.02

def robust_sigma(v):
    """MAD-based sigma, NaN-aware.

    Must use nanmedian: cycles that slipped a phase segment are NaN, and a plain
    median would return NaN, make every threshold NaN, and silently report zero
    anomalies for the rest of time -- a detector that never fires looks exactly
    like a clean device."""
    v = np.asarray(v, float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return 0.0
    return 1.4826*np.median(np.abs(v-np.median(v)))

def load(path, ch, t0_dev, dur, notch=50.0):
    w=wave.open(path); fr=w.getframerate(); n=w.getnchannels()
    d=np.frombuffer(w.readframes(int(12*fr)),dtype='<i2').reshape(-1,n).astype(np.float64)[:,ch-1]/32768
    k=int(fr/400); e=np.convolve(np.abs(d-d.mean()),np.ones(k)/k,mode='same')
    T0=float(np.where(e>e.max()*0.5)[0][0])/fr
    w.setpos(int((T0+t0_dev)*fr))
    x=np.frombuffer(w.readframes(int(dur*fr)),dtype='<i2').reshape(-1,n).astype(np.float64)[:,ch-1]/32768
    x=x-x.mean()
    if notch:
        L=int(round(fr/notch))
        for _ in range(2): x=np.convolve(x,np.ones(L)/L,mode='same')
    return x,fr

def _refine(g,i):
    """parabolic sub-sample peak, correction CLAMPED to its valid +/-0.5 range.
    Unclamped, a flat peak divides by ~0 and throws the estimate thousands of
    samples away -- which yields UNSORTED edges and +/-6 s periods."""
    if i<1 or i>=len(g)-1: return float(i)
    a,b,c=g[i-1],g[i],g[i+1]; d=a-2.0*b+c
    if d==0: return float(i)
    off=0.5*(a-c)/d
    return float(i)+(0.5 if off>0.5 else (-0.5 if off<-0.5 else off))

def percycle(x, fr, f0):
    """Segment by DEMODULATED phase (robust to amplitude swings and noise),
    then locate the edge inside each known cycle. No amplitude threshold
    anywhere, so a brightness LFO cannot make cycles disappear."""
    t=np.arange(len(x))/fr
    W=int(round(fr/f0))
    z=np.convolve(x*np.exp(-2j*np.pi*f0*t), np.ones(W)/W, mode='same')
    ph=np.unwrap(np.angle(z))
    # cycle boundaries = where the total phase (carrier + residual) crosses 2*pi*k
    tot=2*np.pi*f0*t+ph

    # PI-SLIP HARDENING. Using the raw unwrapped phase here is not safe over a
    # long window. Wherever |z| dips (noise, a brightness trough), np.unwrap can
    # take a pi step, which shifts every later segment boundary by HALF a cycle
    # -- and once the segment is half a cycle out, argmax(g) latches the FALL
    # instead of the RISE. At 50% duty the duty check cannot see the swap, so it
    # surfaces as a phantom single-cycle timing glitch.
    #
    # Measured on an 11-minute 10 Hz capture: percycle reported a -36.8 ms
    # "glitch" at t=433 s and 138 anomalies over 6000 cycles (2.3%), while the
    # independent NMS detector in ratechange.py measured a 1.16 ms maximum
    # deviation there. The tell was that edges AFTER the event stayed 54 ms
    # (~half a cycle) out of step with those before: 24.46 cycles between two
    # edges instead of a whole number.
    #
    # So drive the lattice from a SMOOTH fit to the total phase instead of the
    # raw signal. Degree 2 still tracks a linear rate sweep (the reason the raw
    # phase was used at all), while a localised pi step becomes an outlier the
    # fit ignores. Two passes, the second excluding gross residuals, so the step
    # cannot drag the fit either.
    tt = t - t[len(t)//2]
    keep = np.ones(len(tt), bool)
    for _ in range(2):
        c = np.polyfit(tt[keep], tot[keep], 2)
        r = tot - np.polyval(c, tt)
        sd = 1.4826*np.median(np.abs(r-np.median(r)))
        if not np.isfinite(sd) or sd <= 0: break
        keep = np.abs(r-np.median(r)) < 4.0*sd
        if keep.sum() < 0.5*len(tt): break
    tot = np.polyval(np.polyfit(tt[keep], tot[keep], 2), tt)
    k0=int(np.ceil(tot[W]/(2*np.pi))); k1=int(np.floor(tot[-W]/(2*np.pi)))
    bnd=np.interp(2*np.pi*np.arange(k0,k1+1), tot, np.arange(len(x))).astype(int)
    g=np.diff(np.convolve(x,np.ones(max(1,int(fr/400)))/max(1,int(fr/400)),mode='same'))
    T=[];per=[];duty=[];amp=[]
    for i in range(len(bnd)-1):
        a,b=bnd[i],bnd[i+1]
        if b-a < 8 or b>=len(g): continue
        seg=g[a:b]
        r=_refine(g,a+int(np.argmax(seg)))      # exactly one rise per cycle
        f=_refine(g,a+int(np.argmin(seg)))      # and one fall
        T.append(r/fr); amp.append(np.ptp(x[a:b]))
        duty.append(((f-r)/ (b-a)) % 1.0)
    T=np.array(T); amp=np.array(amp); duty=np.array(duty)
    per=np.diff(T)
    n=len(per)
    T, per, duty, amp = T[:n], per.copy(), duty[:n], amp[:n]

    # SEGMENT-SLIP GUARD. Exactly one rise is taken per phase segment, so when
    # the real rise sits within noise of a segment BOUNDARY it is attributed to
    # the neighbouring segment instead: one period comes out ~2x and the next
    # ~0x. The residual-vs-sliding-quadratic then reports a huge single-cycle
    # timing error flanked by a smooth ramp as the fit absorbs it.
    #
    # That signature is a measurement artifact, not a device fault, and it is
    # not rare on long windows: an 11-minute 10 Hz capture produced 138 such
    # "anomalies" over 6000 cycles (2.3%), including an apparent -36.8 ms
    # glitch. Dumping the raw waveform there showed clean 100 ms cycles, and the
    # independent NMS edge detector in ratechange.py measured a 1.16 ms maximum
    # deviation against 0.52 ms at a control point.
    #
    # A real single-cycle fault cannot make a period leave [0.5, 1.5] x nominal
    # without the flicker visibly stopping, which the rate check grades
    # separately. So treat anything outside that band as UNMEASURABLE (NaN)
    # rather than as a finding: NaN compares False against every threshold, so
    # those cycles drop out of the anomaly count instead of dominating it.
    # Reported, not hidden -- a high slip count means the window or rate is
    # wrong and the run should not be trusted.
    nominal = 1.0 / f0
    bad = ~np.isfinite(per) | (per < 0.5 * nominal) | (per > 1.5 * nominal)
    # a slip corrupts the period on BOTH sides of the misattributed edge
    bad |= np.roll(bad, 1) | np.roll(bad, -1)
    per = per.astype(float); per[bad] = np.nan
    duty = duty.astype(float); duty[bad] = np.nan
    amp  = amp.astype(float);  amp[bad]  = np.nan
    percycle.last_slips = int(bad.sum())
    return T, per, duty, amp

def local_resid(v, win=21, order=2):
    """residual vs a SLIDING polynomial fit that EXCLUDES the point itself.
    Order 2 tracks a rate sweep; excluding the point stops a real outlier from
    pulling its own reference and hiding itself."""
    v=np.asarray(v,float); out=np.full(len(v),np.nan); h=win//2
    finite=np.isfinite(v)
    for i in range(len(v)):
        if not finite[i]: continue          # unmeasurable point: no residual
        a,b=max(0,i-h),min(len(v),i+h+1)
        idx=np.arange(a,b)
        # exclude the point itself AND any non-finite neighbour. Without the
        # second mask a single slipped cycle poisons the fit for the whole
        # +/-10-cycle window, blinding the detector exactly where a real fault
        # is most likely to sit next to a measurement hiccup.
        m=(idx!=i) & finite[idx]
        if m.sum()<order+2: continue
        out[i]=v[i]-np.polyval(np.polyfit(idx[m],v[idx][m],order),i)
    return out


if __name__ == '__main__':
    a=sys.argv[1:]
    x,fr = load(a[0], int(a[1]), float(a[3]), float(a[4]))
    f0   = float(a[2])
    T,per,duty,amp = percycle(x,fr,f0)
    rt = local_resid(per*1000.0)
    rd = local_resid(duty*100.0)
    ra = local_resid(amp)/np.nanmedian(amp)
    slips = getattr(percycle, 'last_slips', 0)
    print(f"  {len(per)} cycles (expect ~{int(float(a[4])*f0)})"
          + (f"   [{slips} not graded: edge landed on a phase-segment boundary]"
             if slips else ""))
    if slips > 0.05 * max(1, len(per)):
        print("  WARNING: >5% of cycles slipped. The window or the stated rate is")
        print("           probably wrong; do not trust this run.")
    st=max(FLOOR_TIME_MS, NSIGMA*robust_sigma(rt))
    sd_=max(FLOOR_DUTY_PP, NSIGMA*robust_sigma(rd))
    sa=max(FLOOR_AMP,     NSIGMA*robust_sigma(ra))
    print(f"  noise (robust): timing {robust_sigma(rt):.3f} ms   "
          f"duty {robust_sigma(rd):.2f} pp   amplitude {100*robust_sigma(ra):.1f}%")
    print(f"  thresholds    : timing {st:.3f} ms   duty {sd_:.2f} pp   amplitude {100*sa:.1f}%")
    # ISR-TICK CLASSIFICATION. The flicker ISR runs at
    # clamp(f0 * LED_FLICKER_TICK_MULT, 1000, 10000) Hz (led_matrix_example.c),
    # so a cycle boundary can only land on a tick and the edge occasionally slips
    # by exactly ONE tick. At 10 Hz that is 0.400 ms, which sits just above the
    # 0.335 ms threshold, so every slip was being reported as an anomaly: an
    # 11-minute capture produced ~12-24 per 55 s window, always in adjacent
    # +/- pairs (one cycle long, the next short) and always near the same phase.
    # Measured mean displacement 0.402 ms against a one-tick 0.400 ms.
    #
    # These are real device behaviour, not faults, so they are counted and shown
    # SEPARATELY rather than suppressed -- hiding them would also hide a genuine
    # one-tick fault, and a reader needs to see that the rig resolves them.
    tick_hz  = min(10000.0, max(1000.0, f0 * 250.0))
    tick_ms  = 1000.0 / tick_hz
    def is_tick(v):
        return abs(abs(v) - tick_ms) < 0.3 * tick_ms
    # CROSS-CHECK EVERY TIMING FINDING AGAINST AN INDEPENDENT DETECTOR.
    #
    # Phase segmentation has at least two failure modes that manufacture
    # single-cycle timing excursions: the pi slip hardened above (~half a cycle)
    # and a smaller one that produced an apparent -6.24 ms at t=578 s where the
    # amplitude-ranked NMS edge detector in ratechange.py measured 0.48 ms. Both
    # share a signature -- one large value flanked by a smooth residual ramp as
    # the sliding fit absorbs it -- so neither is distinguishable from a real
    # fault by magnitude alone.
    #
    # The two detectors fail differently: this one segments by demodulated phase
    # and takes argmax of the gradient per segment; that one ranks gradient peaks
    # by slope with non-maximum suppression and never uses phase. A displacement
    # only one of them sees is a detector artifact. Requiring agreement is what
    # makes a reported timing anomaly worth acting on -- and the unconfirmed ones
    # are still COUNTED, because a rising unconfirmed count means this tool needs
    # work, not that the device is clean.
    try:
        from ratechange import rising_edges as _nms
    except Exception:
        _nms = None

    def confirmed(t_s, claim_ms):
        if _nms is None: return True
        a, b = max(0.0, t_s-0.6), min(len(x)/fr, t_s+0.6)
        try:
            e = _nms(x, fr, a, b, min_gap_s=0.75/f0)
        except Exception:
            return True
        iv = np.diff(e)*1000.0
        if len(iv) < 4: return True
        dev = float(np.max(np.abs(iv - np.median(iv))))
        # the independent detector must see at least a third of the claim
        return dev > abs(claim_ms)/3.0

    hits=[]; ticks=0; unconfirmed=0
    for i in range(len(per)):
        if abs(rt[i])>st:
            if is_tick(rt[i]):
                ticks += 1
            elif not confirmed(T[i], rt[i]):
                unconfirmed += 1
            else:
                hits.append((T[i],'timing',   rt[i],'ms'))
        elif abs(rd[i])>sd_: hits.append((T[i],'duty',     rd[i],'pp'))
        elif abs(ra[i])>sa:  hits.append((T[i],'amplitude',ra[i],'x'))
    if unconfirmed:
        print(f"  timing findings DISCARDED as detector artifacts (the independent "
              f"NMS edge detector disagrees): {unconfirmed}")
    if ticks:
        print(f"  ISR-tick steps (one {tick_ms:.3f} ms tick, expected quantisation, "
              f"NOT faults): {ticks}")
    print(f"  anomalies: {len(hits)}")
    for t,k,v,u in hits[:40]:
        print(f"    t={t:8.3f}s  {k:9s} {v:+8.3f} {u}")
    sys.exit(1 if hits else 0)
