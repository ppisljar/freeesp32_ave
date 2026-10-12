#!/usr/bin/env python3
"""Per-cycle anomaly detection: find the ONE flick in a thousand that is wrong.

Aggregate measures cannot see single-cycle faults. The dropped-flick bug fixed
in 2f83be6 moved the mean rate by 0.01% and the harmonics not at all -- it was
introduced, measured as "h3 36x better", and shipped, while dropping a pulse at
the top of every breath.

FOUR axes per cycle: timing, duty, amplitude, and "did the pulse happen at
all". The fourth is not a refinement of the third -- see DROPPED FLICKS below.

METHOD. Segment the signal by DEMODULATED PHASE at the known flicker rate, then
locate the rise and fall inside each cycle window. Phase segmentation is
tolerant of amplitude swings and noise; edges inside a known window give the
precision. There is no amplitude threshold anywhere, which matters because a
brightness LFO legitimately swings the signal several-fold -- a thresholded
edge detector simply loses every cycle in the dim half and reports the missing
data as wild timing noise.

Duty and amplitude are measured over the PULSE, not over the phase segment: the
lattice has no reason to start where a pulse starts and measurably does not (its
boundaries sat 19.7 ms after the rise on an 11-minute capture, and that offset
drifts and wraps). Measuring on the segment instead cost a factor of EIGHT in
amplitude sensitivity and mislabelled timing faults as duty faults -- see the
comments at the measurement site.

Residuals are taken against a SLIDING quadratic that excludes the point itself.
Quadratic so a rate sweep is tracked rather than flagged; excluding the point so
a real outlier cannot pull its own reference and hide.

MEASURED NOISE FLOORS (real captures, this rig):

    capture                     cycles     timing     duty      amplitude
    square, bright, 10 Hz      496/500    0.068 ms   0.07 pp     0.06%
    square, short window       146/150    0.093 ms   0.08 pp     0.07%
    sine carrier + LFO         496/500    0.895 ms   0.94 pp     2.3%
    (historical, pre-2026-10-12 measures, kept for comparison)
    square, bright             116/120    0.113 ms   0.06 pp     0.2%
    10->6 Hz sweep             301/380    0.217 ms   0.14 pp     0.2%

MEASURED SENSITIVITY -- by FAULT INJECTION, not by multiplying sigma. inject.py
perturbs ONE cycle of a real clean capture by a known amount and asks whether
this file reports it; the floor below is the smallest magnitude detected at 10
injection positions out of 10 (9/10 where noted). Rerun it after touching
anything here; "anomalies: 0" is also what total blindness prints.

    axis                       square carrier        sine carrier
                               (drift.wav ch1)       (nd.wav ch2)
                               before    after       before    after
    whole pulse late/early     0.4 ms a  0.6 ms      none b    none b
    rise only (duty+timing)    0.6 ms    0.5 ms      -         -
    duty, one cycle            0.5 pp    0.5 pp      8 pp c    none c
    amplitude, one cycle       25%       3%          none d    15%
    dropped flick              1 of 10   10 of 10    0 of 10   9 of 10

  a The pre-fix 0.4 ms is not a like-for-like loss. At 0.3-0.4 ms the old code
    detected a PURE TIMING shift through its DUTY axis (9 duty findings against
    3 timing ones), because the duty it measured paired a rise with the PREVIOUS
    pulse's fall. It was reporting the right alarm with the wrong label, on the
    same metric that produced 4 of the clean set's false positives. Pure timing
    alone was 0.5 ms then and 0.6 ms now, and the remaining gap is the ISR tick
    band (below), which no threshold can remove.
  b Injector-limited, so NOT a property of this file. A smooth carrier has no
    quiet interval to hide a time warp in, so displacing one cycle necessarily
    reshapes its flanks: the independent NMS detector confirms only 70-80% of
    the asked displacement arrives, and below ~1.5 ms it confirms none of it.
    Detection never reaches 9/10 below 16 ms -- 8/10 at 5 and 8 ms after, 7/10
    and 8/10 before. Read it as "a few ms, not reliably"; the threshold there is
    4.5 ms, being 5 sigma of a 0.9 ms noise floor. For smooth carriers prefer
    the fundamental's phase.
  c Also a labelling difference, in the other direction. Before reached 9/10 at
    8 pp but with 12 of those findings on the TIMING axis against 2 on duty;
    after reaches 7/10 at 8 and at 12 pp. Nothing on a smooth carrier separates
    "the fall moved" from "the cycle moved" cleanly, and the 4.8 pp threshold
    leaves little room above it.
  d The pre-fix amplitude axis never reached 9/10 on the sine carrier at any
    magnitude tested, up to a 40% single-cycle dim.

REPORTED MAGNITUDES are now worth reading, which they previously were not --
the old injector perturbed several quantities at once and every injected
lateness from 1 to 10 ms came back as the same -14.7 ms. Against the clean
injector: timing reads 1.08-1.12x the injected displacement over 0.7-5 ms (the
~10% overshoot is the sliding fit tilting on the neighbours), duty reads 0.96x
at 1 pp, amplitude 1.01x at 5%. They COMPRESS for large faults -- 0.70x at
3 pp, 0.42x at a 20% dim -- because the front-end's 40 ms smoothing spreads a
big single-cycle excursion into its neighbours and the sliding fit absorbs a
share. Read a large magnitude as a lower bound.

DROPPED FLICKS are the reason the fourth axis exists. The historical bug (one
pulse missing at the top of every breath, 2f83be6) was invisible to all three
numeric axes and scored 1 detection in 10 injections, with 37 FALSE findings
alongside it: no pulse means no edge, which means a nonsense period, which NaNs
all three series for that cycle and its neighbours, while the garbage "rise"
lands inside the slip guard's band and makes the two adjacent periods look like
+/-32 ms faults. It is now graded from the ON-minus-OFF level of the window the
lattice predicts -- which needs no edge -- and scores 10 of 10 with no
collateral. A RUN of dark cycles is reported as a dead sensor instead, because
that is what it is.

ONE ISR TICK IS A BLIND BAND, 0.30-0.50 ms at 10 Hz, and it cannot be tuned
away. The flicker ISR quantises edges to clamp(f0*250, 1000, 10000) Hz, so a
one-tick slip IS a 0.400 ms displacement and so is a genuine one-tick fault --
the same number, from the same quantum. An injected, NMS-verified 0.400 ms
displacement is reported as a fault 3 times in 10 (5 of 10 for an early one)
and absorbed into the "ISR-tick steps" count almost all the rest -- 6 of 10 late,
5 of 10 early, 1 landing under threshold. That is the best behaviour available:
the count is always printed, so "ticks went from 11 to 12" is visible, and
claiming to tell the two apart would be a lie. Two ticks (0.8 ms) and up are
reported as faults 10 times in 10.

FALSE POSITIVES on the clean captures, same code as the table above:
    12 square windows, 3300 cycles ........ 11 findings  (pre-fix: 9)
      of which one 2-3% amplitude cluster of 6 cycles in a single 50 s window,
      which the pre-fix amplitude metric was too blunt to see at all, plus two
      sub-ms timing events in the last 0.4 s of another window
    sine carrier, 496 cycles, no warning ... 1 finding   (pre-fix: 1)
    sine carrier, 495 cycles, 6% ungraded .. 7 findings  (pre-fix: 5)
    antiphase near-cancelled channel ...... 76 findings  (pre-fix: 57)
      -- 60 of its 495 cycles ungraded, so it raises the WARNING. A capture
      that warns is not evidence about the device, in either direction; the
      counts are listed so that nobody reads a warning run as a clean one.

TWO TRAPS, both of which produced hundreds of confident false anomalies:
  - the window must be aligned to the sync MARKER; a raw file offset includes
    the marker burst and the silent tail, and every cycle there reads as an
    anomaly (302 on a clean capture).
  - the parabolic sub-sample edge refinement MUST be clamped to +/-0.5 samples.
    Unclamped, a flat peak divides by ~0, which returns UNSORTED edge times and
    periods of +/-6 seconds.

Run signalcheck.py FIRST. On a capture with no signal this reports hundreds of
anomalies with complete confidence.

Run inject.py after ANY change here. It is the only thing standing between this
file and a detector that has quietly gone blind.

usage: percycle.py FILE.wav CH FLICKER_HZ START_S DUR_S
       (START_S is DEVICE time, measured from the sync marker)
"""
import sys
import numpy as np, wave

# Thresholds are NOT fixed. A square carrier's timing noise is ~0.07 ms and a
# smooth (sine) carrier's is ~0.9 ms -- a 13x spread -- so any constant
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
# FLOOR_AMP, not the noise, is what sets the 3% amplitude sensitivity: the
# measured amplitude noise on a clean square capture is 0.06%, so 5 sigma would
# be 0.3%. It stays at 2% because the captures contain real brightness ripple
# just under it -- dropping it to 1% took one 50 s window from 8 findings to 34,
# and 0.5% took it to 59, all of them a genuine 1-3% wobble nobody wants
# reported per cycle. Lower it only for a session where 1% matters.
FLOOR_AMP     = 0.02

# Knobs the fault-injection harness flips to reproduce the PREVIOUS behaviour
# and measure what each one cost in sensitivity. See inject.py and the
# sensitivity table in this docstring; do not retune them without re-running it.
TICK_BAND     = 0.25   # fraction of one ISR tick that still counts as "a tick"
XCHECK_MIN_MS = 1.5    # below this claim the NMS cross-check abstains
DARK_DETECT   = True   # grade an isolated zero-amplitude cycle as a dropped flick
PULSE_ANCHORED= True   # measure duty/amplitude over the pulse, not the phase segment

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

def _box(v, L):
    """Exactly np.convolve(v, ones(L)/L, mode='same'), via prefix sums.

    Same numbers, ~100x faster. It matters: the 10 Hz demodulator needs a
    4410-tap boxcar, which np.convolve evaluates as 4e9 multiply-adds over a
    20 s window, and that made the fault-injection sweep in inject.py (hundreds
    of runs) unaffordable. Written out rather than taken from scipy because
    nothing else here needs scipy."""
    L = int(L)
    if L <= 1:
        return np.array(v, copy=True)
    if np.iscomplexobj(v):
        return _box(v.real, L) + 1j*_box(v.imag, L)
    n = len(v)
    S = np.concatenate(([0.0], np.cumsum(np.asarray(v, float))))
    i = np.arange(n)
    return (S[np.minimum(i + (L-1)//2 + 1, n)] - S[np.maximum(i - L//2, 0)]) / L

def read_window(path, ch, t0_dev, dur):
    """RAW samples of the device-time window [t0_dev, t0_dev+dur), un-filtered.

    Split out of load() so a fault injector can perturb the signal BEFORE the
    front-end filtering, which is the only honest place to do it: inject into
    the already-notched signal and the injected fault skips the 40 ms smoothing
    that a real fault has to pass through, which flatters the detector."""
    w=wave.open(path); fr=w.getframerate(); n=w.getnchannels()
    d=np.frombuffer(w.readframes(int(12*fr)),dtype='<i2').reshape(-1,n).astype(np.float64)[:,ch-1]/32768
    k=int(fr/400); e=np.convolve(np.abs(d-d.mean()),np.ones(k)/k,mode='same')
    T0=float(np.where(e>e.max()*0.5)[0][0])/fr
    w.setpos(int((T0+t0_dev)*fr))
    x=np.frombuffer(w.readframes(int(dur*fr)),dtype='<i2').reshape(-1,n).astype(np.float64)[:,ch-1]/32768
    return x,fr

def prep(x, fr, notch=50.0):
    """mean removal + the mains notch. Zero-phase (centered) throughout."""
    x=x-x.mean()
    if notch:
        L=int(round(fr/notch))
        for _ in range(2): x=_box(x,L)
    return x

def load(path, ch, t0_dev, dur, notch=50.0):
    x,fr = read_window(path, ch, t0_dev, dur)
    return prep(x, fr, notch), fr

def _refine(g,i):
    """parabolic sub-sample peak, correction CLAMPED to its valid +/-0.5 range.
    Unclamped, a flat peak divides by ~0 and throws the estimate thousands of
    samples away -- which yields UNSORTED edges and +/-6 s periods."""
    if i<1 or i>=len(g)-1: return float(i)
    a,b,c=g[i-1],g[i],g[i+1]; d=a-2.0*b+c
    if d==0: return float(i)
    off=0.5*(a-c)/d
    return float(i)+(0.5 if off>0.5 else (-0.5 if off<-0.5 else off))

def _local_median(v, win=21):
    """median of the +/-10 neighbouring cycles, NaN-aware."""
    v=np.asarray(v,float); n=len(v); out=np.full(n,np.nan); h=win//2
    for i in range(n):
        w=v[max(0,i-h):min(n,i+h+1)]
        w=w[np.isfinite(w)]
        if len(w)>=5: out[i]=np.median(w)
    return out

def dark_cycles(depth, frac=0.40, win=21):
    """Which cycles carried no pulse, judged against their NEIGHBOURS.

    Against the GLOBAL median this test is unusable on the captures that matter:
    a brightness LFO breathing 8->22% puts its own trough cycles at 0.46 of the
    global median, a hair above any threshold low enough to catch a dropped
    pulse, and one deeper LFO would have the detector calling a whole trough
    'missing pulses'. Against a local median an LFO is flat by construction and
    only a single-cycle collapse stands out.

    Returns (dark, local_median). dark is False wherever the local median is
    itself ~0, so a window that overruns the session does NOT produce thousands
    of findings -- that is signalcheck.py's job, not this one's."""
    ar = np.asarray(depth, float)
    lm = _local_median(ar, win)
    return (np.isfinite(ar) & np.isfinite(lm) & (lm > 0) & (ar < frac*lm)), lm

def percycle(x, fr, f0):
    """Segment by DEMODULATED phase (robust to amplitude swings and noise),
    then locate the edge inside each known cycle. No amplitude threshold
    anywhere, so a brightness LFO cannot make cycles disappear."""
    t=np.arange(len(x))/fr
    W=int(round(fr/f0))
    z=_box(x*np.exp(-2j*np.pi*f0*t), W)
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
    g=np.diff(_box(x,max(1,int(fr/400))))
    A=[];B=[];R=[];F=[]
    for i in range(len(bnd)-1):
        a,b=bnd[i],bnd[i+1]
        if b-a < 8 or b>=len(g): continue
        seg=g[a:b]
        r=_refine(g,a+int(np.argmax(seg)))      # exactly one rise per cycle
        # ANCHOR THE FALL TO THE PULSE, NOT TO THE SEGMENT. The phase lattice
        # has no reason to start where a pulse starts, and measurably does not:
        # on an 11-minute 10 Hz capture its boundaries sit 19.7 ms after the
        # rise, so the fall found inside a segment is the one BEFORE the rise
        # found inside it. The ((f-r)/W)%1 wrap turns that into the right duty
        # value attributed to the WRONG cycle, one late. Searching one period
        # forward from the rise makes each duty describe one identified pulse.
        ri=int(round(r)); w=b-a
        if PULSE_ANCHORED and ri+w <= len(g):
            f=_refine(g, ri+int(np.argmin(g[ri:ri+w])))
        else:                                   # last cycle: no room, fall back
            f=_refine(g,a+int(np.argmin(seg)))
        A.append(a); B.append(b); R.append(r); F.append(f)
    # AMPLITUDE OVER THE PULSE WINDOW THE LATTICE PREDICTS, not over the phase
    # segment and not over the measured rise. All three were tried:
    #   * ptp(phase segment) straddles two pulses (19.7 ms offset, above), so
    #     dimming ONE of them changes almost nothing: an injected -20% single
    #     cycle moved it 1.8%, under the 2% floor, i.e. invisible, while the ptp
    #     of the pulse itself changes by the full 20%.
    #   * ptp(window starting at the MEASURED rise) breaks on the fault that
    #     matters most: a dropped flick has no rise, argmax returns noise, and
    #     when that lands shortly before the next real pulse the window captures
    #     that pulse instead and reads FULL amplitude. Measured on an injected
    #     drop: 0.1442 against a 0.1442 median, i.e. perfectly healthy.
    # The lattice-predicted window (segment start + the LOCAL rise offset, see
    # below) is the only one of the three that is both aligned to the pulse AND
    # independent of the edge it is trying to grade.
    A=np.array(A,float); B=np.array(B,float); R=np.array(R); F=np.array(F)
    T=[];duty=[];amp=[]
    # WHERE the predicted window sits must be a LOCAL and CIRCULAR estimate.
    # The lattice phase drifts against the signal -- a few ms per 10 s is normal,
    # being the difference between a degree-2 fit and the real rate -- so the
    # rise's offset into its segment sweeps round and WRAPS. A plain median of
    # (rise - segment start) over a 20 s window therefore lands between the two
    # sides of the wrap: measured 70.9 ms where every nearby cycle said 35-39 ms,
    # which put the window 29 ms off the pulse and read a fully dropped flick as
    # 91% of normal amplitude. A circular mean over +/-10 cycles tracks the drift
    # and cannot be confused by the wrap, and one garbage rise is one unit vector
    # in 21.
    w_s = np.maximum(B-A, 1.0)
    frac = _box(np.exp(2j*np.pi*(R-A)/w_s), 21)
    frac = (np.angle(frac)/(2*np.pi)) % 1.0
    win=[]
    for i in range(len(R)):
        w=int(w_s[i]); p0=int(A[i]+frac[i]*w)
        p0=max(0,min(p0,len(x)-1)); p1=min(len(x),p0+w)
        T.append(R[i]/fr)
        amp.append(np.ptp(x[p0:p1]) if PULSE_ANCHORED
                   else np.ptp(x[int(A[i]):int(A[i])+w]))
        duty.append(((F[i]-R[i])/ w) % 1.0)
        win.append((p0,w))
    # DID A PULSE HAPPEN AT ALL? A SEPARATE measure from the amplitude, because
    # ptp cannot answer it: the front-end's 40 ms smoothing drags the NEXT
    # pulse's leading edge back into this cycle's window, so a fully dropped
    # flick still reads 56% of normal ptp (measured) -- not dark by any
    # threshold that would survive a brightness LFO. The ON-minus-OFF level
    # taken from the MIDDLE of each phase is immune to that bleed: same drop
    # measured +0.1216 -> -0.0051 against untouched neighbours.
    d_med = float(np.median(duty)) if duty else 0.5
    d_med = min(max(d_med, 0.1), 0.9)
    depth=[]
    for (p0,w) in win:
        o0=p0+int(0.15*d_med*w); o1=p0+int(0.85*d_med*w)
        f0_=p0+int((d_med+0.15*(1-d_med))*w); f1_=p0+int((d_med+0.85*(1-d_med))*w)
        if o1-o0 < 3 or f1_-f0_ < 3 or f1_ > len(x):
            depth.append(np.nan); continue
        depth.append(float(np.median(x[o0:o1])-np.median(x[f0_:f1_])))
    percycle.last_depth = np.array(depth,float)
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
    # A DARK CYCLE HAS NO EDGES, so every timing number derived from it is
    # noise: argmax(g) over a segment with no pulse returns wherever the
    # smoothing happened to peak. Measured on an injected dropped flick, that
    # put the "rise" 71 ms after its predecessor -- inside the slip guard's
    # [0.5, 1.5] band, so the guard passed it and the two neighbouring periods
    # were reported as +/-32 ms faults. One missing pulse raised 20 anomalies.
    # So mark the dark cycle AND the two periods that reference its rise
    # unmeasurable, and let grade() report the darkness itself.
    dark, _lm = dark_cycles(percycle.last_depth)
    percycle.last_dark = dark
    bad = ~np.isfinite(per) | (per < 0.5 * nominal) | (per > 1.5 * nominal)
    # a slip corrupts the period on BOTH sides of the misattributed edge
    bad |= np.roll(bad, 1) | np.roll(bad, -1)
    bad |= dark[:len(bad)] | np.roll(dark[:len(bad)], -1)
    per = per.astype(float); per[bad] = np.nan
    duty = duty.astype(float); duty[bad] = np.nan
    amp  = amp.astype(float);  amp[bad]  = np.nan
    percycle.last_slips = int((bad & ~(dark[:len(bad)] |
                                      np.roll(dark[:len(bad)], -1))).sum())
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


def grade(x, fr, f0, T, per, duty, amp, slips=0, depth=None):
    """Turn the three per-cycle series into findings. Returns a dict so the
    same decision logic serves the CLI and the fault-injection harness in
    inject.py -- a sensitivity figure measured against a reimplementation of
    the thresholds would be measuring the wrong program."""
    rt = local_resid(per*1000.0)
    rd = local_resid(duty*100.0)
    ra = local_resid(amp)/np.nanmedian(amp)
    st=max(FLOOR_TIME_MS, NSIGMA*robust_sigma(rt))
    sd_=max(FLOOR_DUTY_PP, NSIGMA*robust_sigma(rd))
    sa=max(FLOOR_AMP,     NSIGMA*robust_sigma(ra))
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
    #
    # BAND WIDTH IS MEASURED, NOT CHOSEN. The displacement is quantised but the
    # MEASUREMENT of it is not: the 83 supra-threshold timing residuals in 400 s
    # of clean 10 Hz capture (8 windows, drift.wav ch1+ch2) form one cluster from
    # 0.311 to 0.489 ms around the 0.400 ms tick -- +/-22% -- plus three
    # outliers at 0.516, 0.550 and 0.782 ms (the last is two ticks).
    #
    # So +/-25% is the narrowest band that still absorbs the cluster. Narrowing
    # it to +/-15% was tried and costs false positives with nothing gained:
    # 14 tick steps reappeared as "anomalies" at 0.31-0.49 ms across the clean
    # set. Widening it to +/-30% additionally swallows the 0.516 ms event.
    #
    # The consequence is a BLIND BAND of 0.30-0.50 ms at 10 Hz, and it is not
    # removable by tuning: a one-tick device fault and an ISR tick slip are the
    # SAME displacement, so no threshold can separate them. That is why the
    # timing floor below is 0.6 ms rather than the 0.5 ms this file used to
    # claim -- 0.5 ms lands inside the band and is reported as a tick.
    tick_hz  = min(10000.0, max(1000.0, f0 * 250.0))
    tick_ms  = 1000.0 / tick_hz
    def is_tick(v):
        return abs(abs(v) - tick_ms) < TICK_BAND * tick_ms
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
        # The cross-check is only MEANINGFUL above the second detector's own
        # noise. NMS edge timing on this rig scatters a few tenths of a ms, so
        # asking it to corroborate a 0.5 ms displacement asks it to resolve
        # something it cannot, and it vetoed 70-100% of injected sub-ms faults.
        # It exists to kill the half-cycle and several-ms phase artifacts; below
        # that, the cross-check abstains rather than voting no.
        if abs(claim_ms) < max(XCHECK_MIN_MS, 3.0*robust_sigma(rt)): return True
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

    # DARK CYCLES FIRST. The period/duty/amplitude triplet cannot describe a
    # cycle whose pulse never happened -- there is no edge to time and no duty to
    # measure -- so a DROPPED FLICK, which is the historical bug this whole file
    # exists for (one pulse missing at the top of every breath), fell through all
    # three checks: it zeroes the amplitude, which scrambles the edge positions,
    # which NaNs the three series for that cycle and its neighbours. Fault
    # injection confirmed it: 0/10 detections, silently filed as "not graded".
    #
    # Isolated is the operative word. A RUN of dark cycles is a dead sensor or a
    # window that overruns the session, which must not be reported as thousands
    # of single-cycle faults.
    ar = depth if depth is not None else getattr(percycle,'last_depth',None)
    if ar is None or len(ar) < len(amp): ar = amp
    ar = np.asarray(ar, float)
    if DARK_DETECT:
        # on the FULL series, so this agrees element-for-element with the mask
        # percycle() already applied -- a local median taken over a truncated
        # series would differ in the last 10 cycles, and then a cycle could be
        # called dark here that was never masked there
        dk, lm = dark_cycles(ar)
    else:
        dk, lm = np.zeros(len(ar), bool), np.full(len(ar), np.nan)
    dk, lm, ar = dk[:len(amp)], lm[:len(amp)], ar[:len(amp)]
    # Split the under-modulated cycles. |depth| near zero is a MISSING PULSE
    # and gets reported; a large NEGATIVE depth is an INVERTED pulse, which
    # means the ON window is sitting on the OFF phase -- the cycle's phase is
    # grossly wrong, so it is unmeasurable rather than dark, and labelling it
    # produced findings like "-1.5x", a cycle 150% darker than nothing. Both are
    # masked out of the three axes (percycle() does that); only the first is a
    # finding. Common on the near-cancelled channel of an antiphase session.
    inverted = dk & ~(np.abs(ar) < 0.40*lm)
    dk = dk & ~inverted
    run = dk & (np.roll(dk,1) | np.roll(dk,-1))

    hits=[]; ticks=0; unconfirmed=0
    for i in range(len(per)):
        if inverted[i]:
            continue                      # unmeasurable, counted in slips
        if dk[i]:
            if not run[i]:
                hits.append((T[i],'dark', ar[i]/lm[i] - 1.0,'x'))
            continue
        if abs(rt[i])>st:
            if is_tick(rt[i]):
                ticks += 1
            elif not confirmed(T[i], rt[i]):
                unconfirmed += 1
            else:
                hits.append((T[i],'timing',   rt[i],'ms'))
        elif abs(rd[i])>sd_: hits.append((T[i],'duty',     rd[i],'pp'))
        elif abs(ra[i])>sa:  hits.append((T[i],'amplitude',ra[i],'x'))
    n_dark_run = int(run.sum()); n_inv = int(inverted.sum())
    return dict(n=len(per), slips=slips, hits=hits, ticks=ticks,
                unconfirmed=unconfirmed, dark_run=n_dark_run, inverted=n_inv,
                sig=(robust_sigma(rt), robust_sigma(rd), robust_sigma(ra)),
                thr=(st, sd_, sa), rt=rt, rd=rd, ra=ra, tick_ms=tick_ms)


if __name__ == '__main__':
    a=sys.argv[1:]
    x,fr = load(a[0], int(a[1]), float(a[3]), float(a[4]))
    f0   = float(a[2])
    T,per,duty,amp = percycle(x,fr,f0)
    g = grade(x, fr, f0, T, per, duty, amp, getattr(percycle,'last_slips',0),
              getattr(percycle,'last_depth',None))
    slips = g['slips']; inv = g['inverted']; ung = slips + inv
    why = ", ".join(([f"{slips} edge on a phase-segment boundary"] if slips else [])
                    + ([f"{inv} ON level below the OFF level, i.e. an inverted "
                        f"pulse"] if inv else []))
    print(f"  {g['n']} cycles (expect ~{int(float(a[4])*f0)})"
          + (f"   [{ung} not graded: {why}]" if ung else ""))
    if ung > 0.05 * max(1, g['n']):
        print("  WARNING: >5% of cycles could not be graded. The window, the")
        print("           stated rate or the sensor placement is probably wrong;")
        print("           do not trust this run.")
    if g['dark_run']:
        print(f"  WARNING: {g['dark_run']} consecutive dark cycles (no pulse at all).")
        print("           That is a dead sensor or a window outside the session,")
        print("           not a per-cycle fault. Run signalcheck.py.")
    print(f"  noise (robust): timing {g['sig'][0]:.3f} ms   "
          f"duty {g['sig'][1]:.2f} pp   amplitude {100*g['sig'][2]:.1f}%")
    print(f"  thresholds    : timing {g['thr'][0]:.3f} ms   duty {g['thr'][1]:.2f} pp"
          f"   amplitude {100*g['thr'][2]:.1f}%")
    if g['unconfirmed']:
        print(f"  timing findings DISCARDED as detector artifacts (the independent "
              f"NMS edge detector disagrees): {g['unconfirmed']}")
    if g['ticks']:
        print(f"  ISR-tick steps (one {g['tick_ms']:.3f} ms tick, expected quantisation, "
              f"NOT faults): {g['ticks']}")
    print(f"  anomalies: {len(g['hits'])}")
    for t,k,v,u in g['hits'][:40]:
        print(f"    t={t:8.3f}s  {k:9s} {v:+8.3f} {u}")
    sys.exit(1 if g['hits'] else 0)
