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
    v=np.asarray(v,float); v=v[np.isfinite(v)]
    if len(v)<8: return np.nan
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
    return T[:n], per, duty[:n], amp[:n]

def local_resid(v, win=21, order=2):
    """residual vs a SLIDING polynomial fit that EXCLUDES the point itself.
    Order 2 tracks a rate sweep; excluding the point stops a real outlier from
    pulling its own reference and hiding itself."""
    v=np.asarray(v,float); out=np.full(len(v),np.nan); h=win//2
    for i in range(len(v)):
        a,b=max(0,i-h),min(len(v),i+h+1)
        idx=np.arange(a,b); m=idx!=i
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
    print(f"  {len(per)} cycles (expect ~{int(float(a[4])*f0)})")
    st=max(FLOOR_TIME_MS, NSIGMA*robust_sigma(rt))
    sd_=max(FLOOR_DUTY_PP, NSIGMA*robust_sigma(rd))
    sa=max(FLOOR_AMP,     NSIGMA*robust_sigma(ra))
    print(f"  noise (robust): timing {robust_sigma(rt):.3f} ms   "
          f"duty {robust_sigma(rd):.2f} pp   amplitude {100*robust_sigma(ra):.1f}%")
    print(f"  thresholds    : timing {st:.3f} ms   duty {sd_:.2f} pp   amplitude {100*sa:.1f}%")
    hits=[]
    for i in range(len(per)):
        if abs(rt[i])>st:   hits.append((T[i],'timing',   rt[i],'ms'))
        elif abs(rd[i])>sd_: hits.append((T[i],'duty',     rd[i],'pp'))
        elif abs(ra[i])>sa:  hits.append((T[i],'amplitude',ra[i],'x'))
    print(f"  anomalies: {len(hits)}")
    for t,k,v,u in hits[:40]:
        print(f"    t={t:8.3f}s  {k:9s} {v:+8.3f} {u}")
    sys.exit(1 if hits else 0)
