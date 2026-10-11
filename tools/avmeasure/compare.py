"""Diff an Expectation against an Observation and emit actionable findings.

FRONT-END AGNOSTIC BY CONSTRUCTION
----------------------------------
This module imports nothing from observe_wav. It touches only the fields of
`Observation` (timeline.py), so the on-device trace reader can be dropped in
later without a line changing here. The one optional extra it will *use if
offered* is `obs.meta['_sync_env']` -- a narrowband energy series at the sync
tone -- and it degrades cleanly when that is absent.

THE FOUR THINGS IT SEPARATES, AND WHY THAT MATTERS
--------------------------------------------------
1. t0 -- the recording's device-time origin. Reported FIRST, always, with its
   method and confidence. A wrong t0 makes every other number meaningless, so
   it is never hidden or assumed.

2. CONSTANT audio-vs-light offset -- estimated once and subtracted before any
   per-event timing check. Otherwise a single 38 ms codec delay would be
   reported as "every one of your 40 audio events fired 38 ms late", which is
   forty findings describing one bug.

3. CLOCK RATIO -- fitted and removed before any drift claim. The device cannot
   drift internally (audio and LED both come off the same 40 MHz crystal,
   sdkconfig:1164), so a drift affecting audio and light EQUALLY is the
   recorder's crystal: an artefact, reported as info. Only a DIFFERENTIAL
   drift between the two domains can be a device bug.

4. RESIDUAL per-event and per-value errors -- what is left once 1-3 are
   accounted for. These are the findings a human should act on.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

import devicemodel as dm
from timeline import (
    Expectation,
    Finding,
    Observation,
    Report,
    SyncSolution,
    aggregate_runs,
    find_crossing,
    robust_median,
)


@dataclass
class CompareConfig:
    t0_s: float | None = None
    """Override the solved t0. Use when you know it (e.g. a hardware trigger)."""

    sync_tone_hz: float | None = None
    event_late_ms: float = dm.THRESHOLD_EVENT_LATE_MS
    event_early_ms: float = dm.THRESHOLD_EVENT_EARLY_MS
    flicker_rel: float = dm.THRESHOLD_FLICKER_REL
    duty_pct: float = dm.THRESHOLD_DUTY_PCT
    tone_rel: float = dm.THRESHOLD_TONE_REL
    beat_hz: float = dm.THRESHOLD_BEAT_HZ
    amp_db: float = dm.THRESHOLD_AMP_DB
    drift_ppm: float = dm.THRESHOLD_DRIFT_PPM
    diff_drift_ppm: float = dm.THRESHOLD_DIFF_DRIFT_PPM
    min_run_s: float = 2.0
    """A value discrepancy must persist this long to be reported. Shorter
    excursions are transition artefacts of the estimators, not device bugs."""
    dark_contrast: float = 0.06
    """Normalised contrast below which a light channel counts as dark."""
    event_search_s: float = 0.4
    """Half-width of the window searched for an event's realisation."""

    min_bright_pct: float = 15.0
    """Demanded LED brightness below which the optical estimators are declared
    UNABLE to measure, rather than allowed to report a value.

    This is not a tuning fudge, it is the honest limit of the method. The light
    observables are recovered from an optical envelope whose amplitude scales
    with brightness, so as brightness falls the edge detector starts missing
    alternate edges and the measured rate collapses toward a submultiple. On a
    real 60-minute session whose brightness ramps 45% -> 0% over its last 30 s,
    grading continued into the fade and reported a flawless render as
    "ch1 flickered at 19.155 Hz where 39.999 Hz was expected" (and 9.17 Hz on
    ch4, i.e. a quarter) plus "duty was 18% where 50% was expected" -- nine
    hard errors, every one an artefact of measuring light that was not there.

    15% matches the threshold the sync solver already uses to decide a first
    edge is too dim to time (see `first_edge_dim`). Time excluded this way is
    REPORTED as a LIGHT_NOT_GRADED info finding, never silently dropped --
    silent loss of coverage is the worst failure mode an instrument can have.
    """

    verbose: bool = False


# Findings whose entire meaning is "relative to t0". If the sync solution is
# weak these cannot be trusted at error severity, because the sync error is
# then larger than the discrepancy they report. Value findings (a wrong flicker
# RATE, a wrong carrier, a dead channel) are deliberately NOT in this set: they
# are measured over long steady spans and survive a shifted t0.
_T0_DERIVED_CODES = frozenset({
    "AV_AUDIO_LEADS_LIGHT",
    "AV_CONST_OFFSET",
    "AV_DIFFERENTIAL_DRIFT",
    "EVENT_MISSING",
    "EVENT_NOT_LOCATED",
    "EVENT_LATE",
    "EVENT_EARLY",
    "EVENT_DRIFT",
    # The modulation check compares a demanded SHAPE against the recording at
    # the same device time, so a t0 that is only known to seconds cannot
    # support it -- exactly the argument _profile_bad makes. MEASURED: a 12 Hz
    # AC-coupled input on 90_measure_sync solves t0 to +/-1675 ms and the
    # measured modulation depth then disagrees with the demanded one, which is
    # a statement about the capture, not the device.
    "BRIGHTNESS_MODULATION",
})


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _first_fresh(events: list[dict], kind_prefix: str) -> dict | None:
    for e in events:
        if e["kind"].startswith(kind_prefix) and e.get("fresh"):
            return e
    return None


# Demanded amplitude below which an audio onset cannot be timed acoustically.
# 0.02 of full scale is ~-34 dBFS before the fixed 1/16 mix headroom, i.e.
# already at the edge of what survives a room recording. Shared with the
# per-event observability gate in _check_events so the two cannot drift apart.
_AUDIO_AUDIBLE_AMP = 0.02


def _first_timeable_audio(exp: Expectation, obs: Observation) -> dict | None:
    """First fresh audio activation whose onset is actually loud enough to time.

    Returns None when no audio activation in the session makes a sound at its
    own timestamp -- which is the normal case for a session that opens with a
    volume ramp-in from zero, and the honest answer there is "the audio origin
    is not measurable", not a number from the middle of a fade.
    """
    for e in exp.events:
        if not (e["kind"].startswith("audio_") and e.get("fresh")):
            continue
        A = exp.audio.get(e["channel"])
        if A is None:
            return e
        t_dev = e["t_eff_ms"] / 1000.0
        j = min(max(int(np.searchsorted(A.t, t_dev)), 0), A.t.size - 1)
        # A short way AFTER the timestamp too: a 5 ms generator fade-in is not
        # a fade-in from the comparator's point of view, but a 20 s one is.
        j2 = min(max(int(np.searchsorted(A.t, t_dev + 0.25)), 0), A.t.size - 1)
        amp = max(float(np.nan_to_num(A.amp[j])),
                  float(np.nan_to_num(A.amp[j2])) if bool(A.active[j2]) else 0.0)
        if amp >= _AUDIO_AUDIBLE_AMP:
            return e
    return None


def _xcorr_lag(expected: np.ndarray, observed: np.ndarray, grid_hz: float,
               max_lag_s: float) -> tuple[float, float, float]:
    """Lag (seconds) that best aligns `observed` to `expected`.

    Returns (lag_s, normalised_peak, margin, width_s). `margin` is the peak
    minus the best competing peak at least 0.5 s away.

    `width_s` IS THE NUMBER THAT MATTERS, and it exists because `peak` is
    actively misleading on exactly the sessions this tool is built for. A
    60-minute session that fades brightness in over 20 s and holds one flicker
    rate for the rest has NO sharp feature: its correlation against the
    expectation peaks at 0.9997 (two near-identical plateaus always do) while
    being flat to within 0.04% over +/-5 s. Gating confidence on `peak` there
    reported a t0 that was 2 SECONDS wrong as "HIGH confidence", which then
    cascaded into a phantom "audio leads light" error and ten more findings.

    So we measure how far the lag can move before the correlation meaningfully
    degrades: the contiguous width around the peak where cc stays within 5% of
    the curve's own dynamic range (peak - median). That is scale-free, and it
    turns "the correlation is flat" into an honest +/- in seconds instead of a
    false certainty. Measured: 0.55 s wide on a session with a real step,
    36 s wide on the 60-minute fade-in session above.

    THE ZERO PREFIX IS NOT COSMETIC. The expectation array starts at device
    t=0, i.e. with the session ALREADY RUNNING, while the recording starts with
    lead-in silence. Without a zero region in front of the expectation there is
    no modelled "before the session", so a session with a constant activity
    level (a steady tone at steady volume -- which most of these sessions are)
    has no structure at all to correlate and the lag comes out as noise. Padding
    the expectation with `max_lag_s` of silence puts the ONSET STEP back into
    the signal, and the onset step is the whole reason the correlation works.
    """
    n = min(expected.size, observed.size)
    if n < 8:
        return float("nan"), float("nan"), float("nan"), float("nan")
    npad = max(1, int(max_lag_s * grid_hz))

    def prep(a):
        a = np.asarray(a, dtype=np.float64)[:n].copy()
        a[~np.isfinite(a)] = 0.0
        a = np.concatenate([np.zeros(npad), a])
        a -= a.mean()
        s = a.std()
        return a / s if s > 0 else a

    x, y = prep(expected), prep(observed)
    if x.std() == 0 or y.std() == 0:
        return float("nan"), float("nan"), float("nan"), float("nan")
    n = x.size
    nfft = 1 << int(math.ceil(math.log2(2 * n)))
    cc = np.fft.irfft(np.fft.rfft(y, nfft) * np.conj(np.fft.rfft(x, nfft)), nfft)
    cc = np.concatenate([cc[-(n - 1):], cc[:n]]) / n     # lags -(n-1)..(n-1)
    lags = np.arange(-(n - 1), n)
    max_lag = int(max_lag_s * grid_hz)
    sel = np.abs(lags) <= max_lag
    cc_s, lag_s = cc[sel], lags[sel]
    if cc_s.size == 0:
        return float("nan"), float("nan"), float("nan"), float("nan")
    k = int(cc_s.argmax())
    peak = float(cc_s[k])
    # Localisation width: walk out from the peak while the correlation stays
    # within 1% of its own dynamic range. Normalising by (peak - median) rather
    # than by the peak keeps this meaningful whether the correlation tops out
    # at 0.93 or at 0.9997.
    rng = peak - float(np.median(cc_s))
    width_s = float("nan")
    if rng > 0:
        # 5% of the dynamic range, chosen by MEASUREMENT, not taste: on a
        # fade-in session whose true lag was 2.000 s and whose argmax was
        # 0.000 s, the 1% and 2% contours gave half-widths of 0.53 s and 1.03 s
        # -- both of which EXCLUDE the truth, i.e. they would have been a second
        # lie on top of the first. The 5% contour gives 2.28 s and contains it,
        # while still reporting only 0.28 s on a session that has a real step.
        thr = peak - 0.05 * rng
        i = k
        while i > 0 and cc_s[i - 1] >= thr:
            i -= 1
        j = k
        while j < cc_s.size - 1 and cc_s[j + 1] >= thr:
            j += 1
        width_s = float(lag_s[j] - lag_s[i]) / grid_hz
    guard = int(0.5 * grid_hz)
    other = cc_s.copy()
    lo, hi = max(0, k - guard), min(cc_s.size, k + guard + 1)
    other[lo:hi] = -np.inf
    rival = float(other.max()) if np.isfinite(other).any() else 0.0
    # Parabolic refinement of the correlation peak -> sub-grid lag.
    if 0 < k < cc_s.size - 1:
        a, b, c = cc_s[k - 1], cc_s[k], cc_s[k + 1]
        den = a - 2 * b + c
        d = 0.0 if den == 0 else 0.5 * (a - c) / den
        d = max(-0.5, min(0.5, float(d)))
    else:
        d = 0.0
    return (float(lag_s[k]) + d) / grid_hz, peak, peak - rival, width_s


def _activation_rises(edges_rise: np.ndarray, edges_fall: np.ndarray,
                      quiet_s: float) -> np.ndarray:
    """Rising edges that follow a period of darkness -- i.e. ACTIVATIONS.

    Using "the first rise after the predicted time" for sync would happily
    lock onto the wrong cycle when the coarse estimate is off by more than one
    flicker period, and a t0 that is wrong by one period silently biases every
    subsequent timing figure. An activation edge, by contrast, is unique: it is
    preceded by nothing.
    """
    if edges_rise.size == 0:
        return edges_rise
    allv = np.concatenate([edges_rise, edges_fall])
    allv.sort()
    out = []
    for r in edges_rise:
        k = int(np.searchsorted(allv, r, side="left"))
        prev = allv[k - 1] if k > 0 else -np.inf
        if r - prev > quiet_s:
            out.append(float(r))
    return np.asarray(out, dtype=np.float64)


def _audio_level_series(A, around_s: float | None = None,
                        span_s: float = 4.0) -> tuple[np.ndarray, np.ndarray]:
    """Best available broadband level series for timing an audio transition.

    Prefers the optional fine series (1 ms boxes of mean-square), windowed to
    the region of interest so we never materialise 3.6M points of time axis.
    Falls back to the coarse grid, which costs ~10 ms of early bias -- stated
    rather than hidden, because a front-end without a fine series (the future
    on-device trace, whose blocks are 23 ms) will take this path.
    """
    if A.fine_level is not None and A.fine_fs > 0:
        fs = A.fine_fs
        n = A.fine_level.size
        if around_s is None:
            lo, hi = 0, n
        else:
            lo = max(0, int((around_s - span_s) * fs))
            hi = min(n, int((around_s + span_s) * fs) + 1)
            if hi - lo < 8:
                lo, hi = 0, n
        t = (np.arange(lo, hi, dtype=np.float64)) / fs
        return t, A.fine_level[lo:hi].astype(np.float64)
    return A.t_rec, np.asarray(A.rms, dtype=np.float64) ** 2


def _grid_phase_fix(t_dem: np.ndarray, t_ev: float) -> float:
    """Half-cell correction for a demanded series sampled on a coarse grid.

    `ExpectedLight`/`ExpectedAudioChannel` carry the demanded trajectory sampled
    at 20 Hz, so a transition at t_ev shows up in the samples as a change
    between the last grid point BEFORE it and the first one at or after it.
    Linear interpolation then puts the transition's midpoint half a cell early,
    and a template match against a recording that steps sharply pays that back
    as apparent lateness: MEASURED +26.4 ms on 90_measure_sync's marker-tone
    stop, against a verdict tolerance of -10..+30 ms -- i.e. the grid alone
    nearly manufactured an EVENT_LATE. Shifting the template's time axis so the
    midpoint lands on t_ev removes it exactly for a step, and is at most a
    25 ms shift of the surrounding context otherwise.
    """
    if t_dem.size < 2:
        return 0.0
    dt = float(t_dem[1] - t_dem[0])
    if not (dt > 0):
        return 0.0
    t_prev = dt * (math.ceil(t_ev / dt - 1e-9) - 1)
    return float(t_ev - (t_prev + 0.5 * dt))


def _grid_sharpen(t: np.ndarray, ys: list[np.ndarray], t_ev: float
                  ) -> tuple[np.ndarray, list[np.ndarray]]:
    """Put the breakpoint at `t_ev` BACK into a coarsely sampled demand.

    `ExpectedAudioChannel`/`ExpectedMix` sample the demanded trajectory on a
    grid whose step is chosen to bound memory for a 60-minute file: 0.4 s on a
    33-minute session, not the 50 ms of a short one. The samples themselves are
    exact -- what the grid loses is the BREAKPOINT between two of them, and
    linear interpolation then draws a straight line across the corner. For a
    ramp that ENDS at t_ev the interpolated trajectory does not reach its final
    level until the first grid point at or after t_ev, so the template lags the
    recording by up to one whole cell and the comparator pays it back as the
    event firing EARLY: MEASURED -252 ms on a clean 21_gateway_focus12_TRAP
    (t=61000 ends a 60 s volume ramp; the grid is 0.4 s with its samples at
    ...60.85, 61.25) and -445 ms on a clean 25_gateway_journey_TRAP -- four
    error-severity EVENT_EARLY findings on two bit-perfect renders. The old
    half-cell correction (_grid_phase_fix) is right for a STEP and wrong by
    another half cell for a corner, and it also assumed the grid's samples sit
    at multiples of its step, which they do not.

    Since the trajectory is piecewise linear between entries and the samples
    are exact, the breakpoint is recoverable: extrapolate the two samples on
    each side to t_ev and insert those two values as a pair of points there.
    Interpolation of the augmented series is then exact on both sides of t_ev,
    for a step, a corner, a ramp start or a ramp end alike.
    """
    t = np.asarray(t, dtype=np.float64)
    if t.size < 6:
        return t, ys
    dt = float(t[1] - t[0])
    if not (dt > 0) or not (t[0] < t_ev < t[-1]):
        return t, ys
    i_next = int(np.searchsorted(t, t_ev - 1e-12))
    i_prev = i_next - 1
    if i_prev < 1 or i_next > t.size - 2:
        return t, ys
    if abs(t[i_next] - t_ev) < 1e-9 or abs(t[i_prev] - t_ev) < 1e-9:
        return t, ys          # the breakpoint IS a sample; nothing was lost
    eps = min(1e-6, 1e-3 * dt)
    t_aug = np.concatenate([t[:i_next], [t_ev, t_ev + eps], t[i_next:]])
    out = []
    for y in ys:
        y = np.asarray(y, dtype=np.float64)
        if y.size != t.size:
            return t, ys
        # Secant of the two samples on each side, extrapolated to t_ev.
        s_pre = (y[i_prev] - y[i_prev - 1]) / dt
        s_post = (y[i_next + 1] - y[i_next]) / dt
        v_pre = y[i_prev] + s_pre * (t_ev - t[i_prev])
        v_post = y[i_next] - s_post * (t[i_next] - t_ev)
        out.append(np.concatenate([y[:i_next], [v_pre, v_post], y[i_next:]]))
    return t_aug, out


def _shift_fit(t: np.ndarray, y: np.ndarray, template, lag_lo: float,
               lag_hi: float, lag_step: float
               ) -> tuple[float, float, bool, bool, float, float, float,
                          float]:
    """Best TIME SHIFT of a demanded trajectory onto an observed series.

    Returns (lag_s, quality, localised, at_edge). `quality` is the fraction of
    the observed variance in the window that the shifted template explains
    beyond a constant; `localised` says the minimum is a real minimum rather
    than a flat valley (which is what a featureless ramp gives); `at_edge` says
    the best lag is the widest the caller allowed, i.e. the feature IS there but
    sits further out than the search could reach -- which must be reported as
    present-but-unlocated, never as "nothing to see".

    WHY THIS REPLACES THE STEP DETECTORS FOR LEVEL AND BRIGHTNESS.
    A `.ledc` entry does not always STEP a field. It can also start a ramp, end
    one, start or stop a `~a:b:period` modulation, or step a field while
    something else in the same ear is still ramping. The old realisation search
    only ever looked for a step, and the two failure modes were symmetric and
    both fatal:

      * NOTHING FOUND where something was there. `31000 4 50 ~12:22:10000 ...`
        hands a steady 20% brightness over to a sine LFO between 12% and 22%:
        the MEAN barely moves, so no step exists, and a bit-perfect
        full-length render of 20_gateway_focus10 reported "[ERROR]
        EVENT_NOT_LOCATED ch1..ch4: the entry at t=31000 ms produced NO
        matching change in the recording ... either the entry never took
        effect" with t0 solved to 2.0000 s exactly. 31 of the 48 non-RGB
        shipped sessions use that idiom.
      * SOMETHING FOUND WHERE IT WAS NOT. On 21_gateway_focus12_SINE the mix
        level is still ramping (ch1..ch4) when ch10's own level steps, so the
        two-level change point preferred the window edge and reported
        "EVENT_EARLY ch10: fired 400 ms EARLY" -- exactly the search
        half-width, three times, on a clean recording.

    Matching the whole demanded trajectory, shifted, answers all of those with
    one estimator: a step template reduces to the old change point, a ramp
    corner is located by its corner, and a modulation onset is located by the
    modulation itself. The amplitude is fitted (a + b*template) so an unknown
    sensor gain or mix scale costs nothing, and b is required POSITIVE so a
    feature of the wrong sign is not matched.
    """
    y = np.asarray(y, dtype=np.float64)
    t = np.asarray(t, dtype=np.float64)
    ok = np.isfinite(y) & np.isfinite(t)
    if ok.sum() < 8:
        return (float("nan"), 0.0, False, False, float("inf"), 0.0, 0.0,
                float("inf"))
    t, y = t[ok], y[ok]
    # SUBSAMPLE. The fine audio level series is 1 kHz, so a 32 s context window
    # is 32000 points and a 400-lag scan over it is 13 million interpolations
    # per event per field -- minutes on a 60-minute session. A least-squares lag
    # over 4000 points is already far more precise than the sample interval, so
    # the cost buys nothing.
    if t.size > _SHIFT_MAX_SAMPLES:
        stride = int(math.ceil(t.size / _SHIFT_MAX_SAMPLES))
        t, y = t[::stride], y[::stride]
    n_lag = int(max(3, math.floor((lag_hi - lag_lo) / max(lag_step, 1e-6)) + 1))
    n_lag = min(n_lag, _SHIFT_MAX_LAGS)
    lags = np.linspace(lag_lo, lag_hi, n_lag)

    # ONLY THE SAMPLES THE LAG CAN ACTUALLY MOVE CARRY INFORMATION ABOUT IT.
    # Fitting the whole context window equally is what made the valley a
    # plateau: on 90_measure_sync's marker-tone stop the template explained 63%
    # of the variance at the right lag and 43% at the window edge, because 2.4
    # of the 3.2 s in the window are identical at every lag and contribute
    # nothing but noise. Weighting by how far a sample moves across the search
    # range concentrates the fit on the transition for a step and on the slope
    # for a ramp corner, with no case analysis.
    x0 = template(t, 0.0)
    if x0 is None:
        return (float("nan"), 0.0, False, False, float("inf"), 0.0, 0.0,
                float("inf"))
    x0 = np.asarray(x0, dtype=np.float64)
    probe = np.zeros_like(x0)
    for lag in np.linspace(lag_lo, lag_hi, 5):
        xp = template(t, float(lag))
        if xp is None:
            continue
        probe = np.maximum(probe, np.abs(np.asarray(xp, dtype=np.float64) - x0))
    pmax = float(probe.max()) if probe.size else 0.0
    if pmax <= 0:
        # The lag changes nothing: no timing information at all.
        return (float("nan"), 0.0, False, False, float("inf"), 0.0, 0.0,
                float("inf"))
    # IDENTIFIABILITY IS A PROPERTY OF THE DEMANDED TRAJECTORY ALONE, and it
    # has to be judged before the recording is consulted. Otherwise the two
    # answers "this entry changes nothing a recording can see" and "the change
    # this entry demands is NOT in the recording" collapse into one, and the
    # second is a fault: MEASURED on tests/selftest.ledc with
    # --fault drop:30000, the dropped audio volume step came back as
    # EVENT_NOT_OBSERVABLE (info, exit 0) instead of EVENT_NOT_LOCATED.
    #
    # The model being fitted is a + b*template(t - lag), so the part of a time
    # shift that the fit can ABSORB is whatever lies in span{1, template}. A
    # straight ramp shifted in time is exactly a ramp plus a constant, so it is
    # fully absorbed and the lag is unidentifiable however well the ramp itself
    # fits. Projecting the shifted template out of that span and measuring what
    # is left is the identifiability, in units of the template's own size.
    x0c = x0 - x0.mean()
    nx0 = float(np.dot(x0c, x0c))
    info = 0.0
    if nx0 > 0:
        for lag in np.linspace(lag_lo, lag_hi, 5):
            xp = template(t, float(lag))
            if xp is None:
                continue
            xl = np.asarray(xp, dtype=np.float64)
            xlc = xl - xl.mean()
            resid = xlc - (float(np.dot(xlc, x0c)) / nx0) * x0c
            info = max(info, math.sqrt(float(np.dot(resid, resid)) / nx0))
    w = (probe >= 0.05 * pmax).astype(np.float64)
    if w.sum() < 8:
        w = np.ones_like(x0)
    wsum = float(w.sum())
    ybar = float(np.dot(w, y)) / wsum
    y0 = (y - ybar) * np.sqrt(w)
    sse0 = float(np.dot(y0, y0))
    if not (sse0 > 0):
        return (float("nan"), 0.0, False, False, float("inf"), 0.0, 0.0,
                float("inf"))
    sses = np.full(n_lag, sse0, dtype=np.float64)
    bs = np.zeros(n_lag, dtype=np.float64)
    # Per-BLOCK partial sums, for the leave-one-block-out jackknife below. Six
    # contiguous blocks of the fit window; everything the two-parameter fit
    # needs is a sum, so deleting a block is a subtraction and the whole
    # jackknife costs one extra pass rather than six extra fits.
    n_blk = 6
    edges = np.linspace(0, y.size, n_blk + 1).astype(int)
    def _blocks(v):
        return np.add.reduceat(v, edges[:-1]) if y.size else np.zeros(n_blk)
    b_w = _blocks(w)
    b_wy = _blocks(w * y)
    b_wyy = _blocks(w * y * y)
    b_wx = np.zeros((n_lag, n_blk))
    b_wxx = np.zeros((n_lag, n_blk))
    b_wxy = np.zeros((n_lag, n_blk))
    have = np.zeros(n_lag, dtype=bool)
    for i, lag in enumerate(lags):
        x = template(t, float(lag))
        if x is None:
            continue
        x = np.asarray(x, dtype=np.float64)
        if x.size != y.size or not np.isfinite(x).all():
            continue
        xm = (x - float(np.dot(w, x)) / wsum) * np.sqrt(w)
        den = float(np.dot(xm, xm))
        if not (den > 0):
            continue
        b = float(np.dot(xm, y0)) / den
        if b <= 0:
            continue
        bs[i] = b
        sses[i] = max(0.0, sse0 - b * b * den)
        have[i] = True
        wx = w * x
        b_wx[i] = _blocks(wx)
        b_wxx[i] = _blocks(wx * x)
        b_wxy[i] = _blocks(wx * y)
    i_min = int(np.argmin(sses))
    q = 1.0 - sses[i_min] / sse0
    if q <= 0:
        return (float("nan"), 0.0, False, False, float("inf"), float(info),
                0.0, float("inf"))
    # Sub-grid refinement on the SSE parabola.
    lag = float(lags[i_min])
    if 0 < i_min < n_lag - 1:
        a, b_, c = sses[i_min - 1], sses[i_min], sses[i_min + 1]
        den = a - 2 * b_ + c
        if den > 0:
            d = 0.5 * (a - c) / den
            lag += max(-1.0, min(1.0, d)) * (lags[1] - lags[0])
    # SIGMA, the fit's own half-width, measured on the CONTIGUOUS valley around
    # the minimum at the ONE-SIGMA level of least squares: SSE rises by one
    # residual variance, i.e. by sse_min / (n_eff - 2).
    #
    # Two things had to be right here and both were got wrong first. A global
    # level set sweeps in the secondary minima an isochronic AM puts one AM
    # period away on either side, so the valley must be walked contiguously
    # from the minimum. And the level must be a fraction of the RESIDUAL
    # variance, not of the explained variance: 5% of the explained variance is
    # 280 ms for a clean 6 dB audio level step whose lateness the same fit
    # measured to 7 ms, and adding that to the budget hid a 180 ms-late entry.
    #
    # `n_eff` is the span divided by 50 ms, the analysis grid's own resolution,
    # rather than the sample count: a 1 kHz envelope series is nowhere near 1000
    # independent samples per second, and taking it at face value would claim a
    # precision the signal does not have.
    n_eff = max(4.0, (float(t[-1] - t[0]) / 0.05) if t.size > 1 else 4.0)
    cut = sses[i_min] * (1.0 + 1.0 / max(n_eff - 2.0, 1.0))
    lo_i = i_min
    while lo_i > 0 and sses[lo_i - 1] <= cut:
        lo_i -= 1
    hi_i = i_min
    while hi_i < n_lag - 1 and sses[hi_i + 1] <= cut:
        hi_i += 1
    step_s = float(lags[1] - lags[0]) if n_lag > 1 else 0.0
    sigma = 0.5 * float(lags[hi_i] - lags[lo_i]) + step_s

    # LOCALISED means the fit knows the lag to a useful fraction of the range it
    # searched -- which is what `sigma` measures, and it is the right question.
    #
    # It used to be "the best lag fits appreciably better than the WORST lag in
    # range", which is a different and wrong question: when the template
    # explains nearly all the variance (q = 0.996 for a 30 s volume ramp-in),
    # even the worst lag still explains nearly all of it, so the ratio collapses
    # and a lag the fit had pinned to +/-189 ms was thrown away. MEASURED on
    # 27_genus_40hz_dim with `--fault late:1000:900`: the fit returned
    # lag = +960 ms, sigma = 189 ms, and the result was discarded as
    # "unlocalised" -- the exact silent miss round 2 reported.
    #
    # The featureless-ramp case the old test was there for is caught earlier and
    # properly, by `info`: a straight line shifted in time is absorbed by the
    # fitted offset, so its identifiability is ~0 regardless of fit quality.
    # SIGMA_JACK: the lag's REPRODUCIBILITY, not its SSE curvature.
    #
    # The valley half-width above is the right answer only for independent
    # residuals. The residual of a real mix is neither independent nor
    # model-free: it carries the isochronic AM, the noise bed's own slow level
    # wander, and whatever the keyframe model gets slightly wrong, all of which
    # are smooth on the scale of the shift being measured. With 32000 samples
    # the valley then claims +/-160 ms for a lag that moves by 400 ms when a
    # sixth of the window is removed. MEASURED on three bit-perfect renders:
    # 21_gateway_focus12_TRAP's t=61000 reads -237 ms (valley 160, jackknife
    # 137), 25_gateway_journey_TRAP's t=241000 reads -430 ms (valley 280,
    # jackknife 334) and 18_jhana_absorption's t=2041000 reads +1639 ms
    # (valley 280, jackknife 2406) -- four error-severity EVENT_EARLY/LATE
    # findings on recordings with no device fault. The INJECTED faults
    # reproduce instead: a 300 ms delay on 01_sleep_onset reads +277 ms with a
    # jackknife of 36 ms, and a 4 s delay on 20_gateway_focus10 reads +4525 ms
    # with a jackknife of 542 ms. So the jackknife separates a real shift from
    # a fitted one, which no function of the SSE curve can.
    # 0.0 when it cannot be computed: the jackknife is a FLOOR on the stated
    # half-width, never a reason to discard a measurement by itself.
    sig_jack = 0.0
    if have.any() and n_blk >= 3:
        tot_w = float(b_w.sum())
        tot_wy = float(b_wy.sum())
        tot_wyy = float(b_wyy.sum())
        tot_wx = b_wx.sum(axis=1)
        tot_wxx = b_wxx.sum(axis=1)
        tot_wxy = b_wxy.sum(axis=1)
        # PER-BLOCK FISHER INFORMATION for the lag. The derivative of the
        # template with respect to the lag is -dx/dt, so a block's share of
        # sum(w * (dx/dt)^2) is its share of everything the window knows about
        # the timing. A STEP keeps all of it inside one block; a ramp corner
        # spreads it over the whole ramp. Deleting the block that holds the
        # feature does not test reproducibility, it removes the measurement:
        # counting those replicates made the jackknife 3867 ms for a step the
        # valley pinned to 80 ms and whose 180 ms injected lateness read
        # 212 ms, i.e. it discarded the tool's most reliable measurement
        # (tests/test_avmeasure.py test_14, test_70).
        x_best = template(t, float(lags[i_min]))
        if x_best is None:
            x_best = np.zeros_like(y)
        g = np.gradient(np.asarray(x_best, dtype=np.float64),
                        np.asarray(t, dtype=np.float64))
        b_inf = _blocks(w * g * g)
        inf_tot = float(b_inf.sum())
        loo = []
        for j in range(n_blk):
            if inf_tot > 0 and float(b_inf[j]) > 0.5 * inf_tot:
                continue
            nw = tot_w - float(b_w[j])
            if nw <= 4:
                continue
            my = (tot_wy - float(b_wy[j])) / nw
            syy = (tot_wyy - float(b_wyy[j])) - nw * my * my
            if not (syy > 0):
                continue
            mx = (tot_wx - b_wx[:, j]) / nw
            sxx = (tot_wxx - b_wxx[:, j]) - nw * mx * mx
            sxy = (tot_wxy - b_wxy[:, j]) - nw * mx * my
            with np.errstate(divide="ignore", invalid="ignore"):
                bj = np.where(sxx > 0, sxy / sxx, 0.0)
            ss = np.where((sxx > 0) & (bj > 0) & have, syy - bj * sxy, syy)
            i_j = int(np.argmin(ss))
            # A REPLICATE ONLY COUNTS IF IT STILL CARRIES THE FEATURE. When the
            # whole transition lives inside the deleted block -- a 6 dB volume
            # STEP, whose information is a few hundred ms wide inside a 16 s
            # window -- the remaining fit has nothing to locate and its argmin
            # lands at a search edge. Counting those replicates made the
            # jackknife 3867 ms for a step the valley pinned to 80 ms and whose
            # 180 ms injected lateness read 212 ms, i.e. it would have thrown
            # away the tool's most reliable measurement
            # (tests/test_avmeasure.py test_14, test_70).
            if i_j in (0, n_lag - 1):
                continue
            if 1.0 - float(ss[i_j]) / syy < 0.5 * q:
                continue
            loo.append(float(lags[i_j]))
        if len(loo) >= 3:
            a_loo = np.asarray(loo, dtype=np.float64)
            k = a_loo.size
            sig_jack = math.sqrt(
                (k - 1) / k * float(((a_loo - a_loo.mean()) ** 2).sum()))
    at_edge = i_min in (0, n_lag - 1)
    localised = sigma <= _TRAJ_SIGMA_FRAC * max(lag_hi - lag_lo, 1e-9)
    return (lag, float(q), bool(localised), bool(at_edge),
            float(sigma), float(info), float(bs[i_min]), float(sig_jack))


def _change_point(t: np.ndarray, y: np.ndarray, i0: int, i1: int,
                  rising: bool | None = None) -> tuple[float, float, float, float]:
    """Least-squares change point: the split that best explains `y` as two levels.

    WHY NOT A THRESHOLD CROSSING. The obvious estimator -- "the time y crosses
    halfway between the old and new level" -- breaks on this device's audio.
    The timeline hard-codes a 10% isochronic/tremolo AM (config_parser.c:2357),
    so the level series already ripples by ~20% in energy at the pulse rate,
    and a 1 ms mean-square box of a 240 Hz carrier ripples far more than that.
    A crossing estimator fires on the first ripple trough that dips past the
    threshold, which measured a 30 s volume step as 355 ms EARLY and then, via
    the residual-slope check, as a session-wide "event lateness is growing"
    error. Two confident phantom bugs from one estimator choice.

    A change point is insensitive to zero-mean ripple because the ripple
    contributes to the residual equally on both sides of any candidate split.
    It is also O(n) via cumulative sums, so a 1 kHz series over a 0.8 s window
    costs nothing.

    Returns (t_step, level_before, level_after, sse_ratio). `sse_ratio` is the
    two-level residual over the one-level residual: small means "there really
    is a step here", near 1 means "this window is just noise".
    """
    i0 = max(0, i0)
    i1 = min(len(y), i1)
    n = i1 - i0
    if n < 8:
        return float("nan"), float("nan"), float("nan"), float("nan")
    seg = np.asarray(y[i0:i1], dtype=np.float64)
    ts = t[i0:i1]
    finite = np.isfinite(seg)
    if not finite.all():
        # Refuse a window that is mostly unmeasured. Filling a long NaN tail
        # with the median and then "finding" the boundary between real data and
        # filler reports the END OF THE MEASUREMENT as a device event -- which
        # is how a flicker stop got reported as a 277 ms-early rate step.
        if finite.mean() < 0.8:
            return float("nan"), float("nan"), float("nan"), float("nan")
        fill = robust_median(seg)
        if not math.isfinite(fill):
            return float("nan"), float("nan"), float("nan"), float("nan")
        seg = np.where(finite, seg, fill)
    c1 = np.concatenate([[0.0], np.cumsum(seg)])
    c2 = np.concatenate([[0.0], np.cumsum(seg ** 2)])
    tot1, tot2 = c1[n], c2[n]
    sse_flat = tot2 - tot1 * tot1 / n
    m = np.arange(2, n - 1)
    nl = m.astype(np.float64)
    nr = (n - m).astype(np.float64)
    s1l, s2l = c1[m], c2[m]
    s1r, s2r = tot1 - s1l, tot2 - s2l
    sse = (s2l - s1l * s1l / nl) + (s2r - s1r * s1r / nr)
    k = int(np.argmin(sse))
    mm = int(m[k])
    a = float(s1l[k] / nl[k])
    b = float(s1r[k] / nr[k])
    if rising is not None and ((b > a) != rising):
        return float("nan"), float("nan"), float("nan"), float("nan")
    ratio = float(sse[k] / sse_flat) if sse_flat > 0 else 1.0
    # The step lies between the last "before" sample and the first "after"
    # sample, so the midpoint is the unbiased estimate.
    t_step = 0.5 * (float(ts[mm - 1]) + float(ts[mm]))
    return t_step, a, b, ratio


def _steady_mask(y: np.ndarray, dt: float, rel_per_s: float = 1e-4) -> np.ndarray:
    """True where `y` is not ramping.

    Every rate estimator averages over a window, so while a value RAMPS the
    estimate lags it, and the lag looks exactly like a clock error. On a real
    session (01_sleep_onset ramps the flicker 10 -> 2 Hz over 18 minutes) that
    lag put +874 ppm into the "recorder clock" figure, which then shifted every
    event prediction by a second. Ramping regions must be excluded from any
    clock fit; the ramp itself is checked by the value comparison, which widens
    its own tolerance for the same lag.
    """
    y = np.asarray(y, dtype=np.float64)
    if y.size < 3 or dt <= 0:
        return np.zeros(y.size, dtype=bool)
    d = np.gradient(y, dt)
    with np.errstate(invalid="ignore", divide="ignore"):
        rel = np.abs(d) / np.maximum(np.abs(y), 1e-9)
    ok = np.isfinite(rel) & (rel < rel_per_s)
    # Erode by a few samples so the window straddling the start of a ramp is
    # excluded too.
    k = 3
    if ok.size > 2 * k:
        er = ok.copy()
        for s in range(1, k + 1):
            er[s:] &= ok[:-s]
            er[:-s] &= ok[s:]
        ok = er
    return ok


# Relative rate error above which even the 10-cycle rolling-mean estimator is
# unambiguous. A half- or double-rate zone is 50% or 100% out; the estimator's
# own bias (the LED's PWM carrier beating against the flicker rate, plus the
# rolling window's lag) was measured between 0.9% and 1.3% on clean renders of
# shipped sessions. 15% sits an order of magnitude above the artefact and well
# below the smallest gross fault, so nothing real falls in the gap.
_GROSS_RATE_REL = 0.15

# How long an optical span has to have been stable before its edge TIMES are
# used for a period fit. Just over the 4 s threshold window the shipped
# front-end uses (observe_wav.LightTracker.thr_win_s), so the estimator has
# seen a full window of the settled signal.
_EDGE_SETTLE_S = 5.0

# Demanded brightness below which an optical EDGE cannot be found at all. Much
# lower than the LEVEL floor (CompareConfig.min_bright_pct) on purpose -- see
# _edge_time_mask.
_EDGE_MIN_BRIGHT_PCT = 5.0

# Peak-to-peak brightness difference, in percentage points, below which a
# modulation mismatch is the optical noise floor rather than a missing LFO.
# The shipped LFOs swing 6-16 points.
_MOD_DEPTH_FLOOR_PCT = 4.0

# A light clock ratio is only fitted over a span at least this long, with at
# least this many edges in it. See _edge_time_mask(for_clock=True).
_CLOCK_MIN_RUN_S = 30.0
_CLOCK_MIN_EDGES = 200


def _edge_time_mask(L, cfg: CompareConfig, dt: float,
                    for_clock: bool = False) -> np.ndarray:
    """Spans where an optical edge TIME is a faithful sample of the cycle grid.

    Three conditions, and the third is the one that was missing:

      * the channel is active and actually flickering;
      * the DEMANDED rate holds still, so a single period describes the span;
      * ...and, for a SMOOTH carrier only, the demanded BRIGHTNESS holds still
        too.

    WHY THE BRIGHTNESS MATTERS FOR A SMOOTH CARRIER AND NOT FOR A SQUARE ONE.
    A threshold detector times the instant the optical level crosses a level
    derived from the signal's own percentiles. On a square carrier that instant
    is the step itself, so it does not care how bright the step is. On the
    firmware's sine parabola (led_matrix_example.c:608-645) the crossing sits
    0.164 of a cycle after the cycle origin WHEN THE BRIGHTNESS IS CONSTANT --
    but while the brightness moves, the threshold (set over a window) and the
    instantaneous waveform scale differently, so the crossing walks within the
    cycle and that walk looks exactly like a period error.

    MEASURED on bit-perfect full-length renders, all with a 10-30 s brightness
    LFO over a sine carrier:
      91_measure_17_uniform  light clock fitted +221 ppm (truth +2 ppm by an
                             independent 900 s spectral measurement), published
                             as "AV_DIFFERENTIAL_DRIFT -221 ppm ... the LIGHT
                             timebase is the one that moved"
      21_gateway_focus12_SINE  "[ERROR] flickered at 4.036 Hz where 4.001 Hz
                             was expected (+0.89%)" on all four zones
      34_gateway_obe         the same, +0.9%
    All three are the instrument measuring its own threshold, so the honest
    answer is that those spans are not gradable from edge times -- which is
    reported as LIGHT_NOT_GRADED, not dropped.
    """
    bright = np.nan_to_num(L.bright_pct, nan=0.0)
    # The floor here is NOT cfg.min_bright_pct. That one guards the LEVEL
    # checks (duty, brightness), which need the level itself to be accurate.
    # An edge TIME only needs the edge to be found, and the grid fit has a far
    # better quality test for that than any brightness proxy -- `lock`, the
    # circular concentration of the phase residuals, which measures directly
    # whether the edges form one clean grid. Keeping the 15% floor here cost
    # the whole of 11_lucid_wbtb (8-15% body) and 09_lucid_hypnagogic (12%):
    # sessions dimmed by the photosensitivity caps got NO flicker-rate grading
    # at all, which is why `--fault light-freq:1:0.994` and `:1.006` on
    # 11_lucid_wbtb were both silent. 384 of the library's 665
    # positive-brightness LED entries sit at or below 55%.
    ok = (L.active & (L.emitted_hz > 0.05) & _steady_mask(L.emitted_hz, dt)
          & (bright >= _EDGE_MIN_BRIGHT_PCT))
    smooth = np.nan_to_num(L.env, nan=0.0) >= 0.5
    if for_clock:
        # A CLOCK RATIO IS A PPM MEASUREMENT and needs a quiet span to make it
        # on. Two things disqualify a span, and both were measured producing
        # tens to hundreds of ppm of phantom device-side drift on bit-perfect
        # renders:
        #
        #  * A SMOOTH CARRIER. The threshold detector's crossing sits a fixed
        #    fraction of a cycle into the firmware's sine parabola only while
        #    the brightness is CONSTANT; any level movement walks it within the
        #    cycle, and that walk is indistinguishable from a period error.
        #    MEASURED: 91_measure_17_uniform +221 ppm, 15_ganzflicker +81 ppm,
        #    against +2 ppm from an independent 900 s spectral measurement of
        #    the same recording.
        #  * A SHORT RUN. Even on a SQUARE carrier the trigger levels come from
        #    a window of the signal, so a brightness LFO moves the crossing
        #    inside the ~5 ms optical rise; over a 6 s fragment that is tens of
        #    ppm (04_meditation_theta, +20 ppm from 6 s fragments of its
        #    `~10:30` LFO), while over a span covering many LFO periods it
        #    averages to nothing (01_sleep_onset, +0.9 ppm over 120 s).
        #
        # A session with no qualifying span simply gets no light clock, and the
        # report says so (CLOCK_UNSEPARABLE) instead of printing a number that
        # is really an estimator artefact.
        ok = ok & ~smooth
    else:
        ok = ok & (~smooth | _steady_mask(bright, dt))
    # ...AND THE DETECTOR'S OWN THRESHOLD ESTIMATOR NEEDS TIME TO SETTLE after
    # anything in the optical level changes. A front-end derives its trigger
    # levels from a window of the signal (the shipped one uses 4 s, which is
    # what it takes to see two cycles of a 0.5 Hz flicker), so the window that
    # STRADDLES the end of a brightness ramp carries levels describing partly
    # the ramp -- and on a smooth carrier a wrong level is a wrong within-cycle
    # crossing position. MEASURED on 91_measure_17_uniform: fitting the 15 s
    # span that begins the instant the brightness ramp ends gave +180 ppm,
    # while an independent measurement of the same span taken 2 s later gave
    # -30 ppm. Dropping the first _EDGE_SETTLE_S of every span costs a few
    # seconds of coverage and removes a 200 ppm systematic.
    # ...and this erosion is a CLOCK requirement only. A rate check at 0.5% is
    # a thousand times coarser than a clock fit at ppm, so it does not need the
    # threshold estimator to have settled -- and dropping the first 5 s of every
    # span moved a FLICKER_RATE finding's reported start from 60.2 s to 65.2 s
    # after a 10 -> 20 Hz step was dropped, which is a worse answer to the
    # question "when did this go wrong".
    n_settle = int(round(_EDGE_SETTLE_S / dt)) if (dt > 0 and for_clock) else 0
    if n_settle > 0 and ok.size > n_settle:
        eroded = ok.copy()
        for sft in range(1, n_settle + 1):
            eroded[sft:] &= ok[:-sft]
        eroded[:n_settle] = False
        ok = eroded
    return ok


def _activation_anchor(exp: Expectation, ch: int, t_dev: float) -> float:
    """Device time of the cycle origin of the activation covering `t_dev`.

    The firmware resets a channel's cycle origin ONLY on a fresh activation
    (led_matrix_example.c:1995-2000 "Only reset the cycle origin on FIRST
    activation"), and that origin is the logical anchor
    T0 + t_ms + 46.439 ms (config_parser.c:2699-2702), which `led_start`
    events already carry as `t_eff_ms`. Returns NaN when the run is not
    covered by a single uninterrupted activation.
    """
    anchor = float("nan")
    for ev in exp.events:
        if ev["channel"] != ch or not ev["kind"].startswith("led_"):
            continue
        t_eff = ev["t_eff_ms"] / 1000.0
        if t_eff > t_dev + 1e-9:
            break
        if ev["kind"] == "led_start":
            anchor = t_eff
        elif ev["kind"] == "led_stop":
            anchor = float("nan")
    return anchor


def _fit_edge_grid(sel: np.ndarray, period_hint: float
                   ) -> tuple[float, float, float, float]:
    """Least-squares fit of `sel ~ A + k*T` over integer cycle indices.

    Returns (A, T, lock, se_T). `lock` is the circular concentration of the
    phase residuals: 1.0 means every edge sits exactly on the fitted grid.
    `se_T` is the standard error of the fitted period, in seconds -- the
    estimator's own honest uncertainty, which the differential-drift check
    needs so it stops reporting its own noise as a device fault.

    FITTING T IS THE WHOLE POINT, and it is what makes this ppm-safe. The
    previous estimator took a circular mean of the fractional cycle index
    using the NOMINAL rate, so a recorder crystal error p made the phase
    residual drift linearly across the session and the mean landed halfway,
    biasing t0 by about 0.5 * p * duration. MEASURED on a 4-zone session with
    no device fault and a sync marker present: +50 ppm -> t0 2.0109 s against a
    truth of 2.0000, reported at "HIGH confidence (+/-5 ms)", which then
    produced an AV_AUDIO_LEADS_LIGHT *ERROR* and four spurious EVENT_EARLY
    warnings on a perfect device. Over a 60-minute session the tool's own
    "typical" 100 ppm is ~180 ms of bias -- more than four times the entire
    0..40 ms A/V band this instrument exists to measure.
    """
    NAN = (float("nan"),) * 4
    if sel.size < 20 or not (period_hint > 0):
        return NAN
    d = np.diff(sel)
    d = d[d > 0]
    if d.size < 8:
        return NAN
    # THE HINT IS A MEAN, NOT A MEDIAN, and that is not a style choice. The
    # LED's intensity PWM is not an integer multiple of the flicker rate, so the
    # flicker edge lands at a different PWM phase on successive cycles and the
    # measured intervals follow a short repeating pattern. MEASURED at 40 Hz
    # against a 430 Hz carrier (25 ms = 10.75 PWM periods, so the pattern
    # repeats every 4 cycles): intervals cycle through 24207, 25328, 25651,
    # 24847 us. Their MEAN is 25000.03 us -- 40.0000 Hz to 1.3 ppm -- while
    # their MEDIAN is 25313 us, i.e. 1.25% low. A median hint accumulated 94
    # cycles of index error over a 190 s run and the fit collapsed.
    keep = d[(d > 0.55 * period_hint) & (d < 1.75 * period_hint)]
    if keep.size < 8:
        keep = d
    med = float(keep.mean())
    if not (med > 0):
        return NAN
    # THE FIT IS ANCHORED ON A CONTIGUOUS RUN, THEN EXTENDED. `med` is an
    # interval mean and is biased by whichever intervals survive the [0.55,
    # 1.75] window; indexing the whole selection off it slips by a cycle
    # somewhere in a long run, and because a slip shows up as a step of 2 (still
    # increasing) the old `np.diff(k) <= 0` guard could not see it. That matters
    # most exactly where it is hardest to notice: a threshold detector loses
    # edges during the dim half of a brightness LFO, so the edge list arrives in
    # bursts separated by gaps of unknown cycle count, and indexing ACROSS those
    # gaps off a slightly wrong period accumulates the error.
    #
    # MEASURED on a zero-drift full-length render of 91_measure_17_uniform (10
    # and 7 Hz sine carrier under a 12 s `~8:22` brightness LFO): all four zones
    # fitted +220 ppm, published as "AV_DIFFERENTIAL_DRIFT -221 ppm ... the
    # LIGHT timebase is the one that moved -- look at the flicker ISR tick". An
    # independent 900 s spectral measurement of the same recording puts the
    # flicker fundamental at 6.9978865 Hz against the model's 6.9979006, i.e.
    # +2 ppm: the recording was right and the estimator was wrong.
    #
    # So: pick the longest stretch of consecutive single-cycle intervals, fit
    # the period there (no gaps, no ambiguity), then re-index the full
    # selection with that period and keep only edges that land on the grid.
    step_ok = (d > 0.8 * med) & (d < 1.2 * med)
    best_i, best_n, run_i, run_n = 0, 0, 0, 0
    for i, okv in enumerate(step_ok):
        if okv:
            if run_n == 0:
                run_i = i
            run_n += 1
            if run_n > best_n:
                best_i, best_n = run_i, run_n
        else:
            run_n = 0
    if best_n < 8:
        return NAN
    core = sel[best_i:best_i + best_n + 1]
    kc = np.arange(core.size, dtype=np.float64)

    def _fit(times: np.ndarray, idx: np.ndarray) -> tuple[float, float]:
        cf, *_ = np.linalg.lstsq(
            np.vstack([np.ones_like(idx), idx]).T, times, rcond=None)
        return float(cf[0]), float(cf[1])

    A, T = _fit(core, kc)
    if not (T > 0):
        return NAN
    use_t, use_k = core, kc
    # Extend to every edge that lands on the grid this run defines, so the
    # lever arm grows without ever guessing a cycle count across a gap.
    for _ in range(4):
        k_all = np.round((sel - A) / T)
        on = np.abs(sel - (A + k_all * T)) < 0.25 * T
        if on.sum() < 20 or np.any(np.diff(k_all[on]) <= 0):
            break
        A2, T2 = _fit(sel[on], k_all[on])
        if not (T2 > 0) or abs(T2 / T - 1.0) > 0.02:
            break
        converged = (use_t.size == int(on.sum())
                     and abs(T2 - T) < 1e-12 * max(T, 1.0))
        A, T = A2, T2
        use_t, use_k = sel[on], k_all[on]
        if converged:
            break
    sel, k = use_t, use_k
    if sel.size < 20:
        return NAN
    resid_s = sel - (A + k * T)
    resid = resid_s / T
    z = np.exp(2j * np.pi * resid)
    lock = float(abs(z.mean()))
    n = k.size
    kv = float(np.sum((k - k.mean()) ** 2))
    if n > 2 and kv > 0:
        s2 = float(np.sum(resid_s ** 2)) / (n - 2)
        se_T = math.sqrt(s2 / kv)
    else:
        se_T = float("inf")
    return A, T, lock, se_T


def _light_clock_ppm(exp: Expectation, obs: Observation, ch: int, t0: float,
                     cfg: CompareConfig) -> tuple[float, int, float]:
    """Clock ratio for one light channel, fitted from the EDGE TIMES.

    WHY NOT THE RATE SERIES, which is what this used to use. `ObservedLight.
    freq_hz` is a rolling mean of edge intervals resampled onto the 20 Hz grid,
    and the PWM-beat pattern described in _fit_edge_grid survives both steps:
    the rolling window is 10 cycles, which is not a whole number of beat
    periods, and the grid resample then aliases the leftover ripple. MEASURED on
    a zero-drift render of a 40 Hz session: the rate series gave a "clock" error
    of +105 ppm (median 39.9928 Hz against a true 40.0000), which was then
    reported as "AV_DIFFERENTIAL_DRIFT -104 ppm ... a real device-side
    divergence" at ERROR severity on a bit-perfect recording. The same edges fit
    to 40.0000 Hz within 2 ppm.

    A least-squares line through the edge indices uses every edge once, with the
    beat pattern averaging out instead of being decimated.

    Returns (ppm, n_edges, se_ppm). ppm follows _ratio_ppm's convention:
    positive means the recorder is running FAST, so recovered frequencies read
    LOW. `se_ppm` is the inverse-variance-combined standard error of the fits,
    and it is what keeps the differential-drift check from reporting its own
    noise: at 4 Hz over a 20 s run (80 edges) the honest standard error is tens
    of ppm, which is MORE than the 30 ppm differential threshold, so a fixed
    threshold alone would have fired on test2c's clean render.
    """
    L = exp.light.get(ch)
    O = obs.light.get(ch)
    if L is None or O is None or not O.available or O.edges_rise.size < 20:
        return float("nan"), 0, float("inf")
    dt = float(L.t[1] - L.t[0]) if L.t.size > 1 else 0.05
    # The SAME gates the value checks use: a steady demanded rate and enough
    # brightness for the edge detector to be trusted.
    steady = _edge_time_mask(L, cfg, dt, for_clock=True)
    led_delay = dm.led_edge_delay_ms(exp.led_backend) / 1000.0
    num = 0.0
    wsum = 0.0
    den = 0
    min_run = max(20, int(round(_CLOCK_MIN_RUN_S / dt))) if dt > 0 else 20
    for s, e in aggregate_runs(steady, min_len=min_run):
        f = float(robust_median(L.emitted_hz[s:e]))
        if not math.isfinite(f) or f <= 0:
            continue
        period = 1.0 / f
        lo = t0 + float(L.t[s]) + led_delay + period
        hi = t0 + float(L.t[e - 1]) + led_delay - period
        r = O.edges_rise
        sel = r[(r >= lo) & (r <= hi)]
        if sel.size < _CLOCK_MIN_EDGES:
            continue
        A, T, lock, se_T = _fit_edge_grid(sel, period)
        # A CLOCK fit demands a much tighter lock than a t0 refinement does: a
        # run that is partly at the wrong rate still fits a line, and absorbing
        # that into "the recorder's crystal" would hide the rate fault and
        # invent a differential drift. 0.9 means ~90% of edges sit within a
        # small fraction of a cycle of the fitted grid.
        if not math.isfinite(T) or lock < 0.9:
            continue
        ppm = (f * T - 1.0) * 1e6
        if abs(ppm) > CLOCK_ACCEPT_PPM * 20:
            continue                  # not a clock, and not even a rate error
        se_ppm = f * se_T * 1e6
        if not (se_ppm > 0) or not math.isfinite(se_ppm):
            continue
        w = 1.0 / (se_ppm * se_ppm)
        num += ppm * w
        wsum += w
        den += int(sel.size)
    if den == 0 or wsum <= 0:
        return float("nan"), 0, float("inf")
    return num / wsum, den, math.sqrt(1.0 / wsum)


def _refine_t0_by_phase(obs: Observation, exp: Expectation, t0_coarse: float,
                        led_delay: float, coarse_unc_s: float
                        ) -> tuple[float, float, str]:
    """Pin t0 using the FLICKER PHASE rather than the first edge.

    The LED cycle origin is anchored at T0 + t_ms + 46.439 ms and the realized
    period is known, so every rising edge should land at anchor + k*period.
    Fitting that grid over hundreds of edges gives t0 to a fraction of a
    millisecond.

    This matters because the first-edge method fails on real sessions: every
    shipped session RAMPS BRIGHTNESS IN from 0 over 10-30 s, so the first
    optically detectable edge is hundreds of milliseconds after the channel
    actually started, and t0 comes out late by that much. The phase fit does
    not care how dim the first cycles were.

    THREE THINGS IT MUST GET RIGHT, and it used to get all three wrong:

    1. THE PERIOD IS FITTED, NOT ASSUMED (see _fit_edge_grid) -- otherwise an
       ordinary recorder crystal becomes a firmware accusation.

    2. THE CYCLE GRID HAS AN ORIGIN, and it is the activation anchor, not
       device t=0. Referencing t=0 only gives the right answer when
       (anchor * rate) happens to be a whole number of cycles, which is true of
       tests/selftest.ledc by construction (50 Hz for exactly 600 ms, then
       10 Hz from exactly 1000 ms) and false in general: at 11 Hz the reference
       is 45 ms out, at 13 Hz 31 ms. So the fit is only attempted on a run
       whose rate has been CONSTANT since its own activation anchor, which is
       the only case where the grid is known without integrating the whole
       cycle-by-cycle chain through a ramp. On a session that ramps its rate
       from the first entry the fit DECLINES and says so, rather than returning
       a confident number off an unknown grid.

    3. IT CANNOT RESOLVE WHICH CYCLE: the answer is only known modulo one
       period. The coarse estimate picks the branch, so the returned
       uncertainty is half a period whenever the coarse estimate is not tighter
       than that, and the fit is rejected outright when it disagrees with the
       coarse estimate by more than the coarse estimate's own uncertainty. A
       sync marker removes the ambiguity entirely -- hence the README recipe.
    """
    best = None
    declined: list[str] = []
    for ch, L in sorted(exp.light.items()):
        O = obs.light.get(ch)
        if O is None or not O.available or O.edges_rise.size < 20:
            continue
        dt = float(L.t[1] - L.t[0]) if L.t.size > 1 else 0.05
        steady = L.active & (L.emitted_hz > 0.05) & _steady_mask(L.emitted_hz, dt)
        if steady.sum() < 20:
            continue
        runs = aggregate_runs(steady, min_len=20)
        # Longest run first: the period is a single constant and the lever arm
        # for the period fit is the longest available.
        for s, e in sorted(runs, key=lambda r: r[0] - r[1]):
            f = float(robust_median(L.emitted_hz[s:e]))
            if not math.isfinite(f) or f <= 0:
                continue
            period = 1.0 / f
            anchor = _activation_anchor(exp, ch, float(L.t[s]))
            if not math.isfinite(anchor):
                declined.append(f"ch{ch} run at t={L.t[s]:.0f}s has no single "
                                f"activation covering it")
                continue
            # THE PHASE COLUMN MOVES THE EDGE GRID, and it is a parsed column
            # with real users: the firmware adds phase*cycle/360 to elapsed_us
            # before the duty test (led_matrix_example.c:913-916) and the
            # renderer honours it (synth.py:544), so a bank authored in
            # antiphase (`phase 180`, which sessions/library/01_sleep_onset_RGB
            # and 35_wbtb_lucid_gamma both ship) has its rises exactly half a
            # period away from the activation anchor. Left out of the grid
            # origin, as it was, the fit returned t0 half a period wrong --
            # 32.3 ms on 01_sleep_onset_RGB at 10 Hz, which then produced
            # AV_CONST_OFFSET and four EVENT_LATE errors on a clean render.
            ip = min(max(int(np.searchsorted(L.t, float(L.t[s]))), 0),
                     L.phase_deg.size - 1)
            ph_deg = float(np.nan_to_num(L.phase_deg[ip]))
            i_anchor = max(0, int(np.searchsorted(L.t, anchor)) - 1)
            seg = L.emitted_hz[i_anchor:e]
            seg = seg[np.isfinite(seg)]
            if seg.size == 0 or (seg.max() - seg.min()) > 1e-6 * f:
                declined.append(
                    f"ch{ch}: the rate is not constant between its activation "
                    f"at t={anchor:.1f}s and t={L.t[e - 1]:.0f}s, so the cycle "
                    f"grid is unknown")
                continue
            lo_rec = t0_coarse + float(L.t[s]) + led_delay
            hi_rec = t0_coarse + float(L.t[e - 1]) + led_delay
            r = O.edges_rise
            sel = r[(r >= lo_rec + period) & (r <= hi_rec - period)]
            if sel.size < 20:
                continue
            A, T, lock, _se = _fit_edge_grid(sel, period)
            if not math.isfinite(A) or lock < 0.5:
                declined.append(f"ch{ch}: edges are not phase-locked to a "
                                f"single grid (lock {lock:.2f})")
                continue
            # A rate that is wrong by more than any crystal could be must not
            # be absorbed into t0: that would hide the rate fault AND move
            # every event prediction.
            if abs(T * f - 1.0) > 1e-2:
                declined.append(
                    f"ch{ch}: the measured period is {100 * (T * f - 1):+.2f}% "
                    f"off the modelled rate, which is a rate error rather than "
                    f"a clock error, so it was not used for t0")
                continue
            phase_shift = -(ph_deg / 360.0) * T
            frac = (A - (t0_coarse + anchor + led_delay + phase_shift)) / T
            m = frac - math.floor(frac)
            if m > 0.5:
                m -= 1.0
            cand = t0_coarse + m * T
            # 0.25 * T, not the 0.55 * T this used to allow. 0.55 of a period
            # is 137 ms at 4 Hz and 550 ms at 1 Hz, and -- worse -- it is wider
            # than the dominant systematic it has to catch, which is exactly
            # 0.5 * T (an antiphase bank, see the phase column above). A quarter
            # cycle is the physical bound on the only legitimate disagreement
            # left: the within-cycle position at which a threshold detector
            # crosses a SMOOTH carrier (0.164 * T for the firmware's sine
            # parabola, a little more for a long-attack trapezoid). Past that
            # the two estimators are not measuring the same thing and neither
            # should be trusted over the other.
            if abs(cand - t0_coarse) > max(coarse_unc_s, 0.25 * T):
                declined.append(
                    f"ch{ch}: the phase fit puts t0 {1000 * (cand - t0_coarse):+.0f} "
                    f"ms from the coarse estimate, which is outside the coarse "
                    f"estimate's own +/-{coarse_unc_s * 1000:.0f} ms, so the two "
                    f"disagree and neither was refined")
                continue
            score = (lock, sel.size)
            if best is None or score > best[0]:
                best = (score, cand, T, ch, sel.size, lock, f)
            break
    if best is None:
        note = ""
        if declined:
            note = "flicker-phase fit DECLINED: " + "; ".join(declined[:2])
        return float("nan"), float("nan"), note
    _, cand, T, ch, n, lock, f = best
    ppm = (1.0 / (T * f) - 1.0) * 1e6
    return cand, T * 0.5, (
        f"flicker-phase fit on ch{ch} over {n} edges (lock {lock:.2f}, fitted "
        f"period {T * 1000:.4f} ms = {ppm:+.0f} ppm vs the modelled rate, so "
        f"the recorder clock is fitted out rather than biasing t0) -> t0 "
        f"{cand:.4f} s, ambiguous by multiples of {T * 1000:.1f} ms")


# How fast a level has to rise for its onset to be a MEASUREMENT of when the
# device started rather than a guess at where a fade crossed half-way. The
# device's own onset is instantaneous to within the 5 ms generator fade-in
# (audio_generator.c:384-390) plus the capture path, so 100 ms is generous by a
# factor of ~20 and still 200x shorter than the 20 s volume ramp-ins that every
# shipped session opens with.
_ONSET_RISE_MAX_S = 0.100

# How closely the flicker-phase fit has to agree with the light ACTIVATION EDGE
# before the fit is allowed to replace it. A true activation edge -- a sharp
# optical rise out of a dark period -- is pinned by the sensor's rise time plus
# the LED backend's own edge jitter, which devicemodel puts at a few ms, so 5 ms
# is the edge's own precision and anything beyond it is a real disagreement
# between two independent estimators rather than a refinement of one by the
# other.
_EDGE_AGREE_MS = 5.0


def _onset_is_sharp(t: np.ndarray, y: np.ndarray, t_onset: float,
                    w_b: float, w_a: float) -> bool:
    """Did `y` RISE at `t_onset`, or merely drift upward through it?

    The 10%-to-90% rise time answers that directly, and -- unlike the
    change-point residual ratio -- it is not confused by a sync marker, which is
    a 600 ms BURST: a two-level model of a window containing both the burst's
    rise and its fall fits badly even though the rise itself is razor sharp.
    """
    if t.size < 8 or not math.isfinite(t_onset):
        return False
    dt = float(t[1] - t[0]) if t.size > 1 else 0.0
    if dt <= 0:
        return False
    pre_lo = int(np.searchsorted(t, t_onset - min(w_b, 0.5)))
    pre_hi = int(np.searchsorted(t, t_onset - 2 * dt))
    post_lo = int(np.searchsorted(t, t_onset))
    post_hi = int(np.searchsorted(t, t_onset + min(max(w_a, 0.2), 0.5)))
    if pre_hi - pre_lo < 3 or post_hi - post_lo < 3:
        return False
    base = robust_median(y[pre_lo:pre_hi])
    seg = y[post_lo:post_hi]
    top = float(np.nanpercentile(seg, 90)) if np.isfinite(seg).any() else np.nan
    if not (math.isfinite(base) and math.isfinite(top)) or top <= base:
        return False
    lo_lv = base + 0.1 * (top - base)
    hi_lv = base + 0.9 * (top - base)
    i_search = int(np.searchsorted(t, t_onset - min(w_b, 0.5)))
    t10 = find_crossing(t, y, lo_lv, i_search, post_hi + 1, rising=True)
    t90 = find_crossing(t, y, hi_lv, i_search, post_hi + 1, rising=True)
    if not (math.isfinite(t10) and math.isfinite(t90)):
        return False
    return (t90 - t10) <= _ONSET_RISE_MAX_S


def _onset_from_series(t: np.ndarray, y: np.ndarray, t_pred: float,
                       window_s: float | tuple[float, float]
                       ) -> tuple[float, bool]:
    """Time at which `y` steps up from its pre-onset baseline.

    Returns (t_onset, is_a_real_step). The second value matters: the crossing
    fallback ALWAYS returns something, including on a slow fade where "the
    onset" is not a property of the signal at all but of where the window was
    centred. Callers that need a MEASUREMENT (the A/V offset) must look at the
    flag; callers that only need a best guess (event realisation, which is then
    compared against a tolerance) can ignore it.
    """
    w_b, w_a = ((window_s, window_s) if isinstance(window_s, (int, float))
                else window_s)
    cp, a, b, ratio = _change_point(
        t, y, int(np.searchsorted(t, t_pred - w_b)),
        int(np.searchsorted(t, t_pred + w_a)), rising=True)
    # ratio < 0.5 means the two-level model halves the residual, i.e. there
    # genuinely is an onset in this window rather than just noise.
    if not (math.isfinite(cp) and ratio < 0.5):
        cp = _onset_by_crossing(t, y, t_pred, (w_b, w_a))
    return cp, _onset_is_sharp(t, y, cp, w_b, w_a)


def _onset_by_crossing(t: np.ndarray, y: np.ndarray, t_pred: float,
                       window_s: float | tuple[float, float]) -> float:
    """Fallback: half-plateau crossing. Used when the change-point fit is not
    convincing, e.g. a slow fade-in rather than a step."""
    w_b, w_a = ((window_s, window_s) if isinstance(window_s, (int, float))
                else window_s)
    ok = np.isfinite(y)
    if ok.sum() < 4 or t.size < 4:
        return float("nan")
    i0 = int(np.searchsorted(t, t_pred - w_b))
    i1 = int(np.searchsorted(t, t_pred + w_a))
    if i1 - i0 < 3:
        return float("nan")
    # Margins in SECONDS, not samples: this runs on both the 20 Hz grid and the
    # 1 kHz fine series, and an index-based margin would be 50x shorter on one
    # of them.
    margin = max(1, int(round(0.2 / (t[1] - t[0]))))
    before = y[max(0, i0 - margin):i0]
    base = robust_median(before) if before.size else 0.0
    if not math.isfinite(base):
        base = 0.0
    after = y[i0:min(len(y), i1 + margin)]
    top = np.nanpercentile(after, 90) if np.isfinite(after).any() else np.nan
    if not math.isfinite(top) or top <= base:
        return float("nan")
    return find_crossing(t, y, base + 0.5 * (top - base), i0, i1 + 1, rising=True)


def _blackout(t_dev: np.ndarray, events: list[dict], half_s: float,
              kinds: tuple[str, ...] | None = None) -> np.ndarray:
    """True where a windowed estimator straddles an expected transition.

    EVERY value estimator here is windowed: the tone comes from a 0.25 s FFT,
    the pulse rate from a 4 s envelope FFT, the flicker rate from ~10 cycles of
    edges. Across a step those windows contain BOTH values and the estimate is
    a meaningless blend -- a 4 s envelope window spanning a volume step reports
    the step itself as a ~0.25 Hz "pulse". Masking the straddle region is the
    honest thing to do: the measurement is not available there, so it is not
    reported, rather than reported wrongly.

    This is only possible because the comparator KNOWS where the transitions
    are. The event checks, which use narrow-band estimators aimed at exactly
    those instants, cover the blacked-out regions.
    """
    mask = np.zeros(t_dev.size, dtype=bool)
    if half_s <= 0:
        return mask
    for ev in events:
        if kinds is not None and not ev["kind"].startswith(kinds):
            continue
        c = ev["t_eff_ms"] / 1000.0
        lo = int(np.searchsorted(t_dev, c - half_s))
        hi = int(np.searchsorted(t_dev, c + half_s))
        mask[lo:hi] = True
    return mask


def _dedupe(findings: list[Finding]) -> list[Finding]:
    """Collapse repeats of the same (code, channel) into one counted finding.

    Static `.ledc` lints in particular fire per entry: the Gateway sessions hit
    LEDC_AUDIO_CH8_NO_RAMP on a dozen lines each. Twelve copies of the same
    sentence is noise; one line saying "12 entries, lines 46-64" is a finding.
    """
    groups: dict[tuple, list[Finding]] = {}
    order: list[tuple] = []
    for f in findings:
        key = (f.code, f.domain, f.channel)
        if key not in groups:
            groups[key] = []
            order.append(key)
        groups[key].append(f)
    out = []
    for key in order:
        g = groups[key]
        head = g[0]
        if len(g) == 1:
            out.append(head)
            continue
        lines = sorted({f.line_no for f in g if f.line_no is not None})
        times = sorted({f.t_ms for f in g if f.t_ms is not None})
        extra = f"  [{len(g)} occurrences"
        if lines:
            extra += f", lines {lines[0]}-{lines[-1]}" if len(lines) > 1 \
                else f", line {lines[0]}"
        if times:
            extra += f", t={int(times[0])}..{int(times[-1])} ms"
        extra += "]"
        out.append(Finding(
            code=head.code, severity=head.severity, message=head.message + extra,
            t_ms=head.t_ms, t_end_ms=times[-1] if times else None,
            domain=head.domain, channel=head.channel, expected=head.expected,
            observed=head.observed, delta=head.delta, unit=head.unit,
            confidence=head.confidence, detail=head.detail, line_no=head.line_no))
    return out


# ---------------------------------------------------------------------------
# Sync
# ---------------------------------------------------------------------------


def solve_sync(exp: Expectation, obs: Observation,
               cfg: CompareConfig) -> SyncSolution:
    """Find the recorder time of device t=0, per domain, and say how sure we are.

    Strategy, in order of precision:
      1. an explicit --t0
      2. a SYNC MARKER: a short, loud, unambiguous burst authored at the top of
         the `.ledc` (see README "sync marker recipe"). The light half is the
         first optical edge; the audio half is the onset of a narrowband tone.
      3. cross-correlation of the whole session's activity envelope, refined
         against the first strong event.

    Light is the reference domain, not audio: an LED edge is anchored to
    T0 + t_ms + 46.439 ms and is therefore IMMUNE to dispatch lag
    (config_parser.c:2699), while an audio onset carries the full 8-20 ms
    dispatch jitter. Using audio as the reference would smear t0 by that much
    and then attribute the smear to the light.
    """
    grid_hz = obs.grid_hz
    max_lag = max(5.0, 0.25 * obs.duration_s)

    # ---- expected activity envelopes, in DEVICE time on the obs grid -----
    n_obs = obs.t_rec.size

    def exp_on_obs_grid(t_dev: np.ndarray, y: np.ndarray) -> np.ndarray:
        # The expectation grid and the observation grid are both uniform but
        # may differ in length; resample onto the observation grid with device
        # time == recorder time (lag 0) so the cross-correlation measures the
        # lag rather than inheriting one.
        return np.interp(obs.t_rec, t_dev, y, left=0.0, right=0.0)

    # Only channels whose sensor actually SAW something can contribute a timing
    # reference. Correlating a noise-only channel against a modelled brightness
    # envelope returns a lag -- the correlation is normalised by its own std --
    # and when EVERY mapped channel is dark that lag is pure noise. MEASURED
    # with all mapped zones dead: t0 came out -0.0297 s against a truth of
    # 2.000 and the report then invented "audio lags light by a CONSTANT
    # 2038 ms". A front-end that says `available=False` is telling us it has no
    # measurement; the honest response is to fall back to audio and say so.
    live_light = {ch: O for ch, O in obs.light.items() if O.available}
    dark_light = sorted(ch for ch, O in obs.light.items() if not O.available)
    exp_light = np.zeros(n_obs)
    for ch, L in exp.light.items():
        if ch not in live_light:
            continue
        exp_light += exp_on_obs_grid(
            L.t, np.where(L.active, L.bright_pct / 100.0, 0.0))
    obs_light = np.zeros(n_obs)
    for ch, O in live_light.items():
        v = np.nan_to_num(O.contrast, nan=0.0)
        obs_light += v

    exp_audio = exp_on_obs_grid(exp.mix.t, np.nan_to_num(exp.mix.rms_rel, nan=0.0))
    obs_audio = (np.nan_to_num(obs.audio.rms, nan=0.0) if obs.audio is not None
                 else np.zeros(n_obs))

    lag_l, pk_l, mg_l, wid_l = (_xcorr_lag(exp_light, obs_light, grid_hz, max_lag)
                                if exp.light and live_light else
                                (float("nan"),) * 4)
    lag_a, pk_a, mg_a, wid_a = (_xcorr_lag(exp_audio, obs_audio, grid_hz, max_lag)
                                if obs.audio is not None and exp.audio else
                                (float("nan"),) * 4)

    detail_bits = []
    if dark_light:
        detail_bits.append(
            f"light ch{', ch'.join(str(c) for c in dark_light)} carried NO "
            f"optical signal at all (raw p2p below the sensor floor), so "
            f"{'it was' if len(dark_light) == 1 else 'they were'} excluded from "
            f"the timing reference" +
            ("; NO USABLE LIGHT CHANNEL REMAINS" if not live_light else ""))
    if math.isfinite(lag_l):
        detail_bits.append(f"light xcorr lag {lag_l * 1000:+.0f} ms "
                           f"(peak {pk_l:.2f}, margin {mg_l:.2f}, "
                           f"localised to +/-{wid_l * 500:.0f} ms)")
    if math.isfinite(lag_a):
        detail_bits.append(f"audio xcorr lag {lag_a * 1000:+.0f} ms "
                           f"(peak {pk_a:.2f}, margin {mg_a:.2f}, "
                           f"localised to +/-{wid_a * 500:.0f} ms)")

    # Uncertainty of the COARSE light estimate, which is what has to resolve the
    # flicker-phase fit's modulo-one-cycle ambiguity further down. Starts as the
    # cross-correlation's own localisation half-width and is tightened only when
    # a genuinely sharp feature (an activation edge) is found.
    coarse_unc_s = (wid_l * 0.5) if math.isfinite(wid_l) else float("inf")
    coarse_src = "light xcorr"

    # ---- refine against the first strong event ---------------------------
    led_delay = dm.led_edge_delay_ms(exp.led_backend) / 1000.0
    aud_delay = dm.AUDIO_ONSET_DELAY_MS / 1000.0

    t0_light = float("nan")
    t0_edge = float("nan")      # from a SHARP activation edge: model-free
    first_edge_dim = False
    ev = _first_fresh(exp.events, "led_")
    if ev is not None and live_light:
        t_dev_eff = ev["t_eff_ms"] / 1000.0 + led_delay
        base = lag_l if math.isfinite(lag_l) else 0.0
        pred = t_dev_eff + base
        ch = ev["channel"]
        # Prefer the channel the .ledc actually starts first; fall back to any
        # mapped channel so a mis-mapped sensor still yields a usable t0.
        order = ([ch] if ch in live_light else []) + \
                [c for c in sorted(live_light) if c != ch]
        win = 2.0
        chosen = None
        for c in order:
            O = live_light[c]
            per = _period_at(exp, c, ev["t_eff_ms"] / 1000.0)
            quiet = 5.0 * per if math.isfinite(per) and per > 0 else 0.5
            acts = _activation_rises(O.edges_rise, O.edges_fall, max(quiet, 0.2))
            sel = acts[(acts >= pred - win) & (acts <= pred + win)]
            if sel.size:
                chosen = (float(sel[0]), c, "activation edge")
                break
            # No activation edge in the window (the channel may already have
            # been flickering before the recording started): fall back to the
            # first edge, and say so, because that estimate CAN be a whole
            # cycle out.
            r = O.edges_rise
            k = int(np.searchsorted(r, pred - win))
            if k < r.size and r[k] <= pred + win:
                chosen = (float(r[k]), c, "first edge in window (no dark "
                                          "period before it -- may be off by "
                                          "one flicker cycle)")
        if chosen is not None:
            t0_light = chosen[0] - t_dev_eff
            detail_bits.append(
                f"light {chosen[2]} on ch{chosen[1]} at {chosen[0]:.4f} s vs "
                f"expected {t_dev_eff:.4f} s after t0")
            # A true activation edge (a sharp optical rise out of a dark period)
            # is INDEPENDENT evidence and pins t0 to the sensor rise time plus
            # the backend's edge jitter -- a few tens of ms, not seconds. The
            # "first edge in window" fallback is NOT independent: it can be a
            # whole flicker cycle out, so it earns no tightening.
            if chosen[2] == "activation edge":
                coarse_unc_s = min(coarse_unc_s, 0.020)
                coarse_src = "light activation edge"
                t0_edge = t0_light
            # Is the first activation's brightness high enough for its first
            # optical edge to be detectable at its true time? Every shipped
            # session ramps brightness in from 0, and a sub-threshold first
            # few cycles push the detected edge hundreds of ms late.
            L0 = exp.light.get(ch)
            if L0 is not None:
                i = int(np.searchsorted(L0.t, ev["t_eff_ms"] / 1000.0))
                i = min(max(i, 0), L0.t.size - 1)
                if float(L0.bright_pct[i]) < 15.0:
                    first_edge_dim = True
                    detail_bits.append(
                        f"NOTE: the first activation starts at only "
                        f"{L0.bright_pct[i]:.0f}% brightness (a fade-in), so "
                        f"the first detectable edge is LATE by an unknown "
                        f"amount; the flicker-phase fit below is used instead")
                    t0_light = float("nan")
                    t0_edge = float("nan")
    if not math.isfinite(t0_light) and math.isfinite(lag_l):
        t0_light = lag_l

    # Flicker-phase refinement: sub-ms, and immune to a fade-in. Applied on top
    # of whatever coarse estimate we have, and it supersedes it whenever it
    # locks, because its precision is better by two orders of magnitude.
    phase_unc = float("nan")
    phase_ok = False
    # Disagreement between the two INDEPENDENT light estimators, in ms. It is
    # the honest uncertainty on t0 whenever both exist, and it is what the
    # confidence label is built from below.
    cross_ms = float("nan")
    if math.isfinite(t0_light):
        t0_ph, half_period, ph_detail = _refine_t0_by_phase(
            obs, exp, t0_light, led_delay, coarse_unc_s)
        if math.isfinite(t0_ph):
            phase_unc = half_period
            phase_ok = True
            if math.isfinite(t0_edge):
                cross_ms = (t0_ph - t0_edge) * 1000.0
                if abs(cross_ms) <= _EDGE_AGREE_MS:
                    # They agree inside the activation edge's own precision, so
                    # take the fit: it is better by two orders of magnitude.
                    t0_light = t0_ph
                else:
                    # THEY DISAGREE, AND THE EDGE WINS. The activation edge is a
                    # direct, model-free measurement of a sharp optical rise out
                    # of darkness; the phase fit's absolute origin additionally
                    # assumes that the front-end's reported edge time sits at
                    # the SAME within-cycle position in the session body as it
                    # does on the activation, which is false for any smooth
                    # carrier (env>0): a threshold detector crosses the
                    # firmware's sine parabola 0.164 of a cycle after the cycle
                    # origin. The fit still supplies the period (and therefore
                    # the clock), and the disagreement becomes the stated
                    # uncertainty rather than being discarded.
                    #
                    # MEASURED on bit-perfect marker renders, phase fit vs
                    # activation edge, against a truth of 2.0000 s:
                    #   35_wbtb_lucid_gamma     fit 2.0199  edge 2.0000
                    #   15_ganzflicker_imagery  fit 2.0172  edge 2.0000
                    #   32_hypnagogic_visions   fit 2.0175  edge 2.0000
                    #   11_lucid_wbtb           fit 2.0092  edge 2.0000
                    # Every one of those produced EVENT_EARLY errors on the
                    # marker's own entry -- the tool grading its own t0 feature
                    # against a t0 that had moved away from it.
                    t0_light = t0_edge
                    detail_bits.append(
                        f"the flicker-phase fit and the light activation edge "
                        f"DISAGREE by {cross_ms:+.1f} ms, which is more than "
                        f"the edge's own +/-{_EDGE_AGREE_MS:.0f} ms. The "
                        f"ACTIVATION EDGE is used (it is a sharp rise out of "
                        f"darkness and assumes nothing about the carrier "
                        f"shape); the fit's within-cycle anchor is biased on a "
                        f"smooth carrier. t0 is stated to "
                        f"+/-{abs(cross_ms):.0f} ms accordingly")
            else:
                t0_light = t0_ph
        if ph_detail:
            detail_bits.append(ph_detail)
    # Is the LIGHT ORIGIN good enough to be one half of a 0..40 ms A/V claim?
    # Two separate requirements: the cycle branch must be RESOLVED (a sharp
    # coarse feature, i.e. a real activation edge or a marker), and the
    # within-cycle position must be PRECISE (the phase fit, or a coarse
    # estimate already tighter than the band). Either one alone is not enough.
    light_sharp = bool(math.isfinite(t0_light) and coarse_unc_s <= 0.05
                       and (phase_ok or coarse_unc_s <= 0.025))

    t0_audio = float("nan")
    audio_sharp = False
    sync_env = obs.meta.get("_sync_env")
    # The FIRST AUDIO EVENT THAT IS ACOUSTICALLY TIMEABLE, not simply the first
    # one. An entry demanding (near) zero amplitude at its own timestamp makes
    # no sound, so its onset cannot be timed -- and every shipped session opens
    # with `A ... >0`, a volume ramp-in from silence. Timing that anyway is how
    # tests/fadein.ledc produced an A/V offset that did not move: the change
    # point landed at an arbitrary place inside the slow ramp, a constant
    # distance from wherever the window was centred, so the measurement tracked
    # the SEARCH WINDOW instead of the device. Injecting 300 ms moved it by
    # 0.0004 ms and -1000 ms read as a healthy +11 ms.
    ev_a = _first_timeable_audio(exp, obs)
    if ev_a is not None and obs.audio is not None:
        t_dev_eff = ev_a["t_eff_ms"] / 1000.0 + aud_delay
        # ANCHOR THE AUDIO SEARCH ON THE LIGHT SOLUTION, not on the audio
        # cross-correlation, whenever the light solution exists and the audio
        # correlation is the more poorly localised of the two.
        #
        # WHY: audio and light can only differ by the offset band we are trying
        # to measure -- tens of milliseconds -- so t0_light is always a good
        # anchor for a +/-1 s search. The audio xcorr, by contrast, is degenerate
        # on any session whose volume merely fades in and holds (which is most of
        # them). On a 60-minute 40 Hz session it returned ~0 s instead of 2 s,
        # the search window landed an entire 2 s away from the marker tone, the
        # onset detector gave up and returned the window edge, and the report
        # announced "audio LEADS light by 2048.9 ms" as a hard ERROR.
        unc_a = (wid_a * 0.5) if math.isfinite(wid_a) else float("inf")
        if math.isfinite(t0_light) and unc_a > coarse_unc_s:
            base = t0_light
        elif math.isfinite(lag_a):
            base = lag_a
        else:
            base = t0_light if math.isfinite(t0_light) else 0.0
        pred = t_dev_eff + base
        if sync_env is not None:
            # The marker envelope carries its OWN sample rate (1 kHz), which is
            # 50x the analysis grid. Pairing it with obs.t_rec -- as this did
            # once -- stretched it by that factor and dated the onset ~3.5 ms
            # late, which then reported every later audio event as ~7 ms early
            # on a perfectly clean recording.
            sfs = float(obs.meta.get("_sync_fs") or 0.0)
            sy = np.asarray(sync_env, dtype=np.float64)
            if sfs > 0:
                lo = max(0, int((pred - 4.0) * sfs))
                hi = min(sy.size, int((pred + 4.0) * sfs) + 1)
                if hi - lo < 8:
                    lo, hi = 0, sy.size
                st = np.arange(lo, hi, dtype=np.float64) / sfs
                sy = sy[lo:hi]
            else:
                st = obs.t_rec
        else:
            st, sy = _audio_level_series(obs.audio, pred, 4.0)
        onset, step_quality = _onset_from_series(st, sy, pred, 1.0)
        if math.isfinite(onset):
            t0_audio = onset - t_dev_eff
            audio_sharp = step_quality
            detail_bits.append(
                f"audio onset at {onset:.4f} s vs expected {t_dev_eff:.4f} s "
                f"after t0" + (" (sync tone)" if sync_env is not None else "")
                + ("" if step_quality else
                   " -- NOT a step: the level in this window rises gradually, "
                   "so the onset time is the detector's guess inside the window "
                   "and the audio origin is NOT used for the A/V offset"))
    elif obs.audio is not None:
        detail_bits.append(
            "no audio entry in this session makes a timeable sound at its own "
            "timestamp (they all start from a volume ramp-in), so the audio "
            "origin could not be measured; author a sync marker (README)")
    if not math.isfinite(t0_audio) and math.isfinite(lag_a):
        t0_audio = lag_a
        # A cross-correlation lag is a usable LAST RESORT for t0, but it is not
        # a measurement of the audio ORIGIN unless it is sharply localised, and
        # on a session that just fades in and holds it is not.
        audio_sharp = math.isfinite(wid_a) and wid_a * 0.5 <= 0.05

    # Per-domain ORIGINS, published only when they were actually measured. The
    # A/V offset is their difference, so publishing a guess for either one
    # fabricates the headline figure the external rig exists to produce.
    pub_light = (t0_light * 1000.0 if (math.isfinite(t0_light) and light_sharp)
                 else float("nan"))
    pub_audio = (t0_audio * 1000.0 if (math.isfinite(t0_audio) and audio_sharp)
                 else float("nan"))

    # ---- choose ---------------------------------------------------------
    if cfg.t0_s is not None:
        # A HAND-TYPED t0 IS THE ONE MOST LIKELY TO BE WRONG, so it does not get
        # the strongest possible label for free. The solver has already run, so
        # compare the two: when they disagree by more than the solved estimate's
        # own uncertainty, that disagreement IS the uncertainty on the given
        # value, and saying "HIGH (+/-0 ms)" over the top of it would be the
        # worst lie this report can tell.
        g_conf, g_unc = "high", 0.0
        g_detail = "t0 supplied on the command line; not verified"
        if math.isfinite(t0_light):
            dev_ms = (cfg.t0_s - t0_light) * 1000.0
            solved_unc = max(5.0, coarse_unc_s * 1000.0,
                             (phase_unc * 1000.0 if math.isfinite(phase_unc)
                              and first_edge_dim else 0.0))
            if abs(dev_ms) > solved_unc:
                g_conf = "low" if abs(dev_ms) > 10 * solved_unc else "medium"
                g_unc = abs(dev_ms)
                g_detail = (
                    f"t0 was supplied on the command line as {cfg.t0_s:.4f} s, "
                    f"but the light reference solves {t0_light:.4f} s -- a "
                    f"{dev_ms:+.0f} ms disagreement, well outside the solved "
                    f"estimate's own +/-{solved_unc:.0f} ms. ONE OF THE TWO IS "
                    f"WRONG; every timing figure below assumes yours")
            else:
                g_detail = (f"t0 supplied on the command line; it AGREES with "
                            f"the solved light reference to {dev_ms:+.1f} ms, "
                            f"which is inside the solver's own "
                            f"+/-{solved_unc:.0f} ms")
                g_unc = solved_unc
        return SyncSolution(
            t0_s=cfg.t0_s, method="given", confidence=g_conf,
            uncertainty_ms=g_unc,
            detail=g_detail + ". " + "; ".join(detail_bits),
            light_lag_ms=pub_light, audio_lag_ms=pub_audio,
            xcorr_peak=pk_l, xcorr_margin=mg_l)

    if math.isfinite(t0_light):
        t0 = t0_light
        method = "marker" if sync_env is not None else "xcorr"
        # Confidence rests on the COARSE and REFINED estimates agreeing, plus a
        # strong correlation. Deliberately NOT on the correlation margin: an
        # activity envelope that is a single on/off step has a triangular
        # correlation whose peak is broad by construction, so a small margin
        # there is normal and gating on it would mark every healthy session
        # low-confidence.
        agree = (math.isfinite(lag_l) and abs(lag_l - t0_light) < 0.5)
        if agree and pk_l > 0.5:
            conf, unc = "high", 5.0
        elif agree and pk_l > 0.25:
            conf, unc = "medium", 50.0
        else:
            conf, unc = "low", 250.0

        # ...but `agree` is VACUOUS when t0_light was itself derived from lag_l
        # (no activation edge was found), and `peak` is near 1.0 on any session
        # that just holds a steady state. Both were true of a 60-minute 40 Hz
        # session whose brightness fades in over 20 s: t0 came out 2 s wrong and
        # was labelled HIGH, which is the one failure mode this report must
        # never have. The honest bound is how well the coarse estimate actually
        # localises the lag, so cap confidence by that.
        if coarse_unc_s > 0.5:
            conf = "low"
            unc = max(unc, coarse_unc_s * 1000.0)
            detail_bits.append(
                f"the {coarse_src} localises t0 only to "
                f"+/-{coarse_unc_s * 1000:.0f} ms, because this session has no "
                f"sharp optical feature to correlate against (a slow fade-in "
                f"and then a steady rate). AUTHOR A SYNC MARKER (see README) -- "
                f"without one, t0 here is a guess")
        elif coarse_unc_s > 0.05:
            conf = "low" if conf == "low" else "medium"
            unc = max(unc, coarse_unc_s * 1000.0)

        # THE UNCERTAINTY IS THE SPREAD BETWEEN THE INDEPENDENT ESTIMATORS,
        # whenever there are two of them. Deriving it only from the phase fit's
        # half-period ambiguity and the correlation class is how "HIGH
        # (+/-5 ms)" came to be printed over t0 errors of 10, 17, 18, 20 and
        # 32 ms on clean renders -- and because the weak-t0 DEMOTION is keyed on
        # this label, a wrong label disables the one mechanism that is supposed
        # to stop small findings being published as hard errors. The same
        # cross-check is already applied to a user-supplied --t0 below; the
        # solver's own output had been exempt from it.
        if math.isfinite(cross_ms) and abs(cross_ms) > unc:
            unc = abs(cross_ms)
            if abs(cross_ms) > 10.0 and conf == "high":
                conf = "medium"

        if first_edge_dim:
            # The phase fit is precise but only modulo one flicker cycle, and
            # with no sharp first edge nothing resolves WHICH cycle. Saying
            # "high confidence, +/-0.3 ms" here would be a lie: the answer can
            # be a whole period out, and a period is 100 ms at 10 Hz.
            if math.isfinite(phase_unc):
                unc = max(unc, phase_unc * 1000.0)
            conf = "medium" if conf == "high" else conf
            detail_bits.append(
                "add a sync marker (README) to remove the whole-cycle "
                "ambiguity and get t0 to well under a millisecond")
    elif math.isfinite(t0_audio):
        t0, method = t0_audio, "xcorr"
        conf, unc = "low", 100.0
        detail_bits.append("NO LIGHT REFERENCE: t0 came from audio, which "
                           "carries the full dispatch jitter, so every light "
                           "timing below inherits that uncertainty")
    else:
        return SyncSolution(
            t0_s=0.0, method="failed", confidence="none", uncertainty_ms=float("inf"),
            detail="could not align the recording to the .ledc at all. "
                   "Check the channel map, that the recording actually covers "
                   "the session, and that a sync marker is present. "
                   + "; ".join(detail_bits))

    return SyncSolution(
        t0_s=t0, method=method, confidence=conf, uncertainty_ms=unc,
        detail="; ".join(detail_bits),
        light_lag_ms=pub_light, audio_lag_ms=pub_audio,
        xcorr_peak=pk_l, xcorr_margin=mg_l)


# ---------------------------------------------------------------------------
# Clock ratio
# ---------------------------------------------------------------------------


def _ratio_ppm(expected: np.ndarray, observed: np.ndarray,
               valid: np.ndarray) -> tuple[float, int]:
    """Median (f_expected / f_observed - 1) in ppm.

    A recorder clock running FAST by p ppm writes more samples per real second,
    so every frequency we recover using the nominal sample rate comes out LOW
    by p ppm. Hence expected/observed - 1 estimates the recorder error
    directly. The median (not the mean) because a handful of estimator
    outliers at transitions must not move it.
    """
    m = valid & np.isfinite(expected) & np.isfinite(observed) & (observed > 0) \
        & (expected > 0)
    if m.sum() < 20:
        return float("nan"), int(m.sum())
    r = expected[m] / observed[m] - 1.0
    # Trim the tails: a frequency RAMP is measured with a lag by any
    # windowed estimator, which shows up as a symmetric but fat-tailed error.
    r = np.sort(r)
    k = max(0, int(0.1 * r.size))
    core = r[k:r.size - k] if r.size - 2 * k >= 10 else r
    # TRIMMED MEAN, not median. The per-sample rate ratio is skewed: the
    # optical rate estimate wanders slowly (the LED's PWM carrier beats against
    # the flicker rate) and the 1/period transform plus grid resampling turn
    # that wander into an asymmetric distribution. On a 500 ppm test the mean
    # read 503 ppm while the median read 585 -- and that 80 ppm of median bias
    # showed up as a phantom audio-vs-light differential drift, which is
    # exactly the class of finding this tool must never invent. Trimming the
    # tails keeps the robustness the median was there for.
    return float(core.mean() * 1e6), int(m.sum())


# A real recorder crystal is within a few hundred ppm; even a bad one stays
# under ~500. So an apparent "clock" error larger than this cannot be a clock,
# and absorbing it would be catastrophic: a channel flickering 6% fast once
# dragged the pooled clock estimate to -14000 ppm, which then (a) hid the real
# rate error, (b) invented a device-side differential drift, and (c) shifted
# every event prediction by 400 ms so the event timings became nonsense too.
# One wrong channel, four confident phantom findings.
#
# The window sits far above any crystal and far below the smallest rate error
# worth calling a bug (0.5% = 5000 ppm), so nothing real falls in the gap.
CLOCK_ACCEPT_PPM = 1500.0


def _consensus_ppm(estimates: list[tuple[str, float, int]]
                   ) -> tuple[float, int, list[str]]:
    """Median of the per-channel clock estimates that could BE a clock.

    Returns (ppm, total_samples, names_of_rejected_channels). Rejected channels
    are not an error here -- they are the ones whose rate is genuinely wrong,
    and the value checks will say so with the clock left at its honest value.
    """
    good = [(nm, p, n) for nm, p, n in estimates
            if math.isfinite(p) and abs(p) <= CLOCK_ACCEPT_PPM]
    bad = [nm for nm, p, n in estimates
           if math.isfinite(p) and abs(p) > CLOCK_ACCEPT_PPM]
    if not good:
        return float("nan"), 0, bad
    vals = np.array([p for _, p, _ in good])
    return float(np.median(vals)), int(sum(n for _, _, n in good)), bad


# ---------------------------------------------------------------------------
# The comparison
# ---------------------------------------------------------------------------


def compare(exp: Expectation, obs: Observation,
            cfg: CompareConfig | None = None) -> Report:
    cfg = cfg or CompareConfig()
    findings: list[Finding] = []
    stats: dict = {}

    sync = solve_sync(exp, obs, cfg)
    if sync.method == "failed":
        findings.append(Finding(
            code="SYNC_FAILED", severity="error", domain="sync",
            message="could not establish the recording's t=0, so NO other "
                    "comparison was attempted (every result would be "
                    "meaningless).",
            detail=sync.detail))
        return Report(ledc=exp.source, observation=obs.source,
                      front_end=obs.front_end, sync=sync,
                      findings=_dedupe(findings + exp.lints), stats=stats)
    if sync.confidence in ("low", "none"):
        findings.append(Finding(
            code="SYNC_LOW_CONFIDENCE", severity="warning", domain="sync",
            message=f"t0 = {sync.t0_s:.3f} s was solved by '{sync.method}' with "
                    f"{sync.confidence} confidence (+/-{sync.uncertainty_ms:.0f} ms). "
                    f"Treat every timing finding below as suspect; add a sync "
                    f"marker to the .ledc (see README) and re-record.",
            detail=sync.detail))

    led_delay = dm.led_edge_delay_ms(exp.led_backend) / 1000.0
    aud_delay = dm.AUDIO_ONSET_DELAY_MS / 1000.0

    # ---- per-domain origins --------------------------------------------
    # Light is the reference. The audio origin carries the measured constant
    # offset so that a single codec delay does not become one finding per event.
    t0_l = sync.t0_s
    t0_a = (sync.t0_s + (sync.audio_lag_ms - sync.light_lag_ms) / 1000.0
            if math.isfinite(sync.audio_lag_ms) and math.isfinite(sync.light_lag_ms)
            else sync.t0_s)
    av_residual_ms = (sync.audio_lag_ms - sync.light_lag_ms
                      if math.isfinite(sync.audio_lag_ms)
                      and math.isfinite(sync.light_lag_ms) else float("nan"))
    av_pipeline_ms = dm.av_offset_pipeline_ms(exp.led_backend)
    stats["av_pipeline_ms"] = av_pipeline_ms
    stats["led_backend"] = exp.led_backend
    av_total_ms = (av_residual_ms + av_pipeline_ms
                   if math.isfinite(av_residual_ms) else float("nan"))
    # A t0 known only to a second cannot support a claim inside a 40 ms band.
    # Blank the number rather than print one that carries no information.
    av_unresolvable = (math.isfinite(av_total_ms)
                       and sync.uncertainty_ms > dm.AV_OFFSET_MIN_RESOLVABLE_MS)
    if av_unresolvable:
        av_total_ms = float("nan")
        av_residual_ms = float("nan")

    # ---- clock ratio ----------------------------------------------------
    light_est: list[tuple[str, float, int]] = []
    se_light_terms: list[float] = []
    for ch, L in sorted(exp.light.items()):
        O = obs.light.get(ch)
        if O is None or not O.available:
            continue
        dt = float(L.t[1] - L.t[0]) if L.t.size > 1 else 0.05
        # MEASURABILITY GATE, the same one the value checks use. `_steady_mask`
        # looks only at the DEMANDED rate, so it happily calls a span steady
        # while the brightness is below what a photodiode envelope can resolve
        # into edges -- and there the detector drops alternate edges and the
        # measured rate reads LOW. That is not a clock error, but it was being
        # absorbed as one: on zero-drift renders of shipped sessions the "clock"
        # came out at -162 ppm (04_meditation_theta, whose `~10:30:10000`
        # brightness LFO dips to 10%), +24 ppm (05_focus_smr) and +10 ppm
        # (07_genus_40hz) against a truth of 0. -162 ppm is -350 ms of event
        # prediction over an hour, and it also feeds the AV_DIFFERENTIAL_DRIFT
        # error whose threshold is 30 ppm.
        p, nn, se = _light_clock_ppm(exp, obs, ch, t0_l, cfg)
        if not math.isfinite(p):
            # Fall back to the rate series when no run has enough clean edges
            # for a grid fit (a short session, or one that never holds a rate).
            steady = (L.active & _steady_mask(L.emitted_hz, dt)
                      & (np.nan_to_num(L.bright_pct, nan=0.0)
                         >= cfg.min_bright_pct))
            e = np.interp(O.t_rec - t0_l, L.t, L.emitted_hz,
                          left=np.nan, right=np.nan)
            a = np.interp(O.t_rec - t0_l, L.t, steady.astype(float),
                          left=0.0, right=0.0) > 0.99
            p, nn = _ratio_ppm(e, O.freq_hz, a)
            # The rate-series estimator's own floor, measured on clean renders
            # of shipped sessions: it is median-biased by the PWM beat (see
            # _light_clock_ppm) and was observed between -162 and +105 ppm with
            # a true drift of zero. Only reachable when no run has enough clean
            # edges for the grid fit, and the uncertainty says so.
            se = 120.0
        light_est.append((f"light{ch}", p, nn))
        if math.isfinite(p):
            se_light_terms.append(se)
    ppm_light, n_light, rej_light = _consensus_ppm(light_est)

    audio_est: list[tuple[str, float, int]] = []
    if obs.audio is not None:
        dt_m = float(exp.mix.t[1] - exp.mix.t[0]) if exp.mix.t.size > 1 else 0.05
        for nm, e_src, o_src, prom in (
                ("audioL", exp.mix.dominant_l_hz, obs.audio.tone_l_hz,
                 obs.audio.tone_l_prom),
                ("audioR", exp.mix.dominant_r_hz, obs.audio.tone_r_hz,
                 obs.audio.tone_r_prom)):
            if nm == "audioR" and not obs.audio.has_right:
                continue
            if nm == "audioL" and not obs.audio.has_left:
                continue
            st = _steady_mask(np.nan_to_num(e_src, nan=0.0), dt_m)
            e = np.interp(obs.t_rec - t0_a, exp.mix.t, e_src,
                          left=np.nan, right=np.nan)
            ok = np.interp(obs.t_rec - t0_a, exp.mix.t, st.astype(float),
                           left=0.0, right=0.0) > 0.99
            # A clock fit deserves only the cleanest carriers: 20 dB of
            # spectral prominence. A noise bed sharing the output (every
            # shipped session has one) biases the peak estimate by tens of ppm
            # when the carrier is only marginally above it.
            ok &= np.nan_to_num(prom, nan=0.0) > 20.0
            p, nn = _ratio_ppm(e, o_src, ok)
            audio_est.append((nm, p, nn))
    ppm_audio, n_audio, rej_audio = _consensus_ppm(audio_est)
    # Per-domain UNCERTAINTIES. The differential-drift check is the only hard
    # error in the clock section, and without these it was testing a fixed
    # 30 ppm threshold against an estimator whose own noise is tens of ppm on a
    # short run -- so it fired on a clean render of test2c (light +36 ppm from
    # two 20 s runs at 4 and 8 Hz, i.e. 80 and 160 edges).
    se_light = (min(se_light_terms) if se_light_terms else float("inf"))
    # The audio carrier is measured by phase-slope refinement to ~0.02 Hz on a
    # 0.25 s window and averaged over thousands of windows, so its floor is
    # small; MEASURED between 0.1 and 1.4 ppm on clean shipped renders, with
    # 05_focus_smr's pink-noise bed the worst at 24 ppm. 5 ppm is the honest
    # per-ear floor and the spread between the two ears covers the rest.
    se_audio = 5.0
    aud_vals = [p for _, p, _ in audio_est if math.isfinite(p)]
    if len(aud_vals) > 1:
        se_audio = max(se_audio, abs(aud_vals[0] - aud_vals[1]))
    if not aud_vals:
        se_audio = float("inf")

    # THE COMMON TERM IS WHAT THE TWO DOMAINS SHARE -- not simply the light
    # domain's figure.
    #
    # A recorder crystal error affects audio and light EQUALLY by definition, so
    # the part attributable to the recorder is the component present in BOTH
    # estimates: sign(a) * min(|a|, |b|) when they agree in sign, and zero when
    # they do not. Taking the light domain's figure as "the recorder" meant a
    # genuine DEVICE-side LED timebase error was divided out of the very check
    # that would have found it. MEASURED with all four LED zones 0.12% fast and
    # no recorder error at all: the headline read "AV_DIFFERENTIAL_DRIFT
    # +1205 ppm ... look at the I2S fractional divider" plus
    # "CLOCK_DRIFT_RECORDER -1205 ppm ... not a device bug", two statements that
    # contradict each other, and the actual finding -- the LED flicker rate is
    # 0.12% fast on every zone -- was never stated, because `ppm_light` was fed
    # into the flicker-rate check as a correction.
    #
    # With one domain only, the two cannot be separated at all, and the honest
    # thing is to say so (CLOCK_UNSEPARABLE below) rather than to assume.
    both = math.isfinite(ppm_light) and math.isfinite(ppm_audio)
    if both:
        if (ppm_light >= 0) == (ppm_audio >= 0):
            drift_common = math.copysign(min(abs(ppm_light), abs(ppm_audio)),
                                         ppm_light)
        else:
            drift_common = 0.0
    else:
        drift_common = (ppm_light if math.isfinite(ppm_light)
                        else (ppm_audio if math.isfinite(ppm_audio)
                              else float("nan")))
    drift_diff = (ppm_audio - ppm_light if both else float("nan"))
    stats.update(clock_ppm_light=ppm_light, clock_ppm_audio=ppm_audio,
                 clock_n_light=n_light, clock_n_audio=n_audio,
                 clock_per_channel_ppm={nm: (None if not math.isfinite(p) else
                                             round(p, 1))
                                        for nm, p, _ in light_est + audio_est},
                 clock_rejected=rej_light + rej_audio)
    if rej_light or rej_audio:
        findings.append(Finding(
            code="CLOCK_CHANNEL_EXCLUDED", severity="info", domain="clock",
            message=f"{', '.join(rej_light + rej_audio)} disagree(s) with the "
                    f"demanded rate by more than "
                    f"{CLOCK_ACCEPT_PPM / 10000:.2f}%, which is far too much to "
                    f"be a crystal error, so they were excluded from the clock "
                    f"fit. Their rate error is reported on its own below "
                    f"instead of being absorbed into the clock.",
            detail="A recorder crystal is within a few hundred ppm; anything "
                   "larger is a real rate error."))

    if math.isfinite(drift_common) and abs(drift_common) > cfg.drift_ppm:
        drift_ms = drift_common * 1e-6 * obs.duration_s * 1000.0
        sev = "info" if abs(drift_common) <= dm.RECORDER_PPM_TYPICAL * 3 else "warning"
        findings.append(Finding(
            code="CLOCK_DRIFT_RECORDER", severity=sev, domain="clock",
            observed=drift_common, unit="ppm",
            message=f"recorder clock differs from the device by "
                    f"{drift_common:+.0f} ppm ({drift_ms:+.0f} ms over "
                    f"{obs.duration_s / 60:.0f} min). Audio and light are "
                    f"affected EQUALLY, so this is the capture interface's "
                    f"crystal, not a device bug -- it has been fitted out of "
                    f"every timing figure below.",
            detail="The device cannot drift internally: esp_timer and I2S both "
                   "derive from the same 40 MHz crystal (sdkconfig:1164)."))
    diff_floor = cfg.diff_drift_ppm
    if math.isfinite(se_light) and math.isfinite(se_audio):
        diff_floor = max(diff_floor,
                         3.0 * math.hypot(se_light, se_audio))
    stats.update(clock_se_light_ppm=se_light, clock_se_audio_ppm=se_audio,
                 diff_drift_floor_ppm=diff_floor)
    if math.isfinite(drift_diff) and abs(drift_diff) > diff_floor:
        drift_ms = drift_diff * 1e-6 * obs.duration_s * 1000.0
        # NAME THE DOMAIN THAT MOVED. "audio and light diverge" is true but
        # unactionable, and when the divergence is entirely on the LED side the
        # old wording sent the reader to the I2S divider to look for a fault
        # that was in the flicker engine. The per-domain figures are right here,
        # so use them.
        off_l, off_a = ppm_light - drift_common, ppm_audio - drift_common
        if abs(off_l) > 2.0 * abs(off_a):
            who = (f"the LIGHT timebase is the one that moved ({ppm_light:+.0f} "
                   f"ppm against {ppm_audio:+.0f} ppm on audio), so look at the "
                   f"flicker ISR tick and at the per-zone FLICKER_RATE findings "
                   f"below before looking at the audio path")
        elif abs(off_a) > 2.0 * abs(off_l):
            who = (f"the AUDIO timebase is the one that moved ({ppm_audio:+.0f} "
                   f"ppm against {ppm_light:+.0f} ppm on light) -- look at the "
                   f"I2S fractional divider and at underruns ratcheting the "
                   f"effective DMA latency")
        else:
            who = (f"both domains moved, in opposite directions "
                   f"(light {ppm_light:+.0f} ppm, audio {ppm_audio:+.0f} ppm)")
        findings.append(Finding(
            code="AV_DIFFERENTIAL_DRIFT", severity="error", domain="clock",
            observed=drift_diff, unit="ppm",
            message=f"audio and light timebases DIVERGE by {drift_diff:+.0f} ppm "
                    f"({drift_ms:+.0f} ms of audio-vs-light slip over "
                    f"{obs.duration_s / 60:.0f} min). This cannot be the "
                    f"recorder (it would affect both equally) and it cannot be "
                    f"the crystal (both paths share it), so it is a real "
                    f"device-side divergence: {who}.",
            detail=f"audio sweeps are clocked in SAMPLES "
                   f"(config_parser.c:2472) while LED sweeps stay in "
                   f"MICROSECONDS (led_matrix_example.c:1951). Reported because "
                   f"{abs(drift_diff):.0f} ppm beats this recording's own "
                   f"estimator floor of {diff_floor:.0f} ppm (light "
                   f"+/-{se_light:.0f}, audio +/-{se_audio:.0f})."))
    elif (math.isfinite(drift_diff) and abs(drift_diff) > cfg.diff_drift_ppm
            and abs(drift_diff) <= diff_floor):
        # Above the nominal threshold but inside the estimator's own noise.
        # Said out loud: a reader who sees nothing cannot tell "no divergence"
        # from "could not test for one".
        findings.append(Finding(
            code="AV_DIFFERENTIAL_DRIFT_NOT_TESTED", severity="info",
            domain="clock", observed=drift_diff, unit="ppm",
            message=f"audio and light timebases differ by {drift_diff:+.0f} ppm, "
                    f"which is past the nominal "
                    f"{cfg.diff_drift_ppm:.0f} ppm threshold but INSIDE this "
                    f"recording's own estimator floor of {diff_floor:.0f} ppm "
                    f"(light +/-{se_light:.0f} ppm, audio +/-{se_audio:.0f} "
                    f"ppm), so it is not reported as a device fault.",
            detail="The light clock is fitted from edge times, so its "
                   "uncertainty scales as 1/(rate * run length): a 20 s run at "
                   "4 Hz is only 80 edges and cannot resolve 30 ppm. Record a "
                   "longer steady span, or one at a higher flicker rate, to "
                   "tighten this."))
    if (not both) and math.isfinite(drift_common) \
            and abs(drift_common) > cfg.drift_ppm:
        missing = "audio" if math.isfinite(ppm_light) else "light"
        findings.append(Finding(
            code="CLOCK_UNSEPARABLE", severity="info", domain="clock",
            observed=drift_common, unit="ppm",
            message=f"the {drift_common:+.0f} ppm clock error above was measured "
                    f"in ONE domain only -- no usable {missing} rate reference "
                    f"was available -- so it CANNOT be separated into "
                    f"'capture-interface crystal' and 'device-wide timebase "
                    f"error'. It has been fitted out as a recorder error because "
                    f"that is by far the more likely of the two, but that is an "
                    f"assumption, not a measurement.",
            detail=f"Map a {missing} channel (see --map) to separate them: only "
                   f"a drift that affects audio and light UNEQUALLY can be a "
                   f"device bug."))

    # ---- constant audio-vs-light offset ---------------------------------
    if av_unresolvable:
        findings.append(Finding(
            code="AV_NOT_MEASURABLE", severity="info", domain="sync",
            message=f"the audio-vs-light offset is NOT REPORTED for this "
                    f"recording: t0 is only localised to "
                    f"+/-{sync.uncertainty_ms:.0f} ms, which is wider than the "
                    f"whole {dm.AV_OFFSET_BAND_MS[0]:.0f}..{dm.AV_OFFSET_BAND_MS[1]:.0f} ms "
                    f"band the measurement lives in, so any number here would "
                    f"carry no information about the device.",
            detail="Author a sync marker (README 'sync marker recipe'): a short "
                   "loud burst of light plus a narrowband tone at the top of the "
                   "session pins both origins to a millisecond."))
    elif not math.isfinite(av_total_ms):
        why = []
        if not math.isfinite(sync.light_lag_ms):
            why.append("no light origin could be measured (no sensor saw a "
                       "sharp activation edge)")
        if not math.isfinite(sync.audio_lag_ms):
            why.append("no audio origin could be measured (no entry makes a "
                       "timeable sound at its own timestamp)")
        findings.append(Finding(
            code="AV_NOT_MEASURABLE", severity="info", domain="sync",
            message="the audio-vs-light offset is NOT REPORTED for this "
                    "recording: " + ("; ".join(why) if why else
                                      "one of the two origins is unavailable")
                    + ". An offset is the DIFFERENCE of two origins, so one "
                      "missing origin makes it unmeasurable rather than "
                      "merely imprecise.",
            detail="Author a sync marker (README): its light burst and tone "
                   "give both origins directly, independently of the session's "
                   "own shape."))
    else:
        lo, hi = dm.AV_OFFSET_BAND_MS
        # The ERROR floor is the MEASUREMENT floor, not the physical bound. See
        # devicemodel.AV_OFFSET_FLOOR_MS: on bit-perfect renders the raw
        # residual already spans -1.5..+2.3 ms from estimator bias alone, and a
        # real rig adds phototransistor fall time, AC-coupling phase advance and
        # codec group delay on top. Widened further by t0's own uncertainty,
        # because a t0 known to +/-50 ms cannot support a 20 ms claim.
        floor = max(dm.AV_OFFSET_FLOOR_MS, sync.uncertainty_ms)
        arith = (f"{av_total_ms:+.1f} ms = {av_residual_ms:+.1f} ms measured "
                 f"residual (audio origin minus light origin) "
                 f"{av_pipeline_ms:+.1f} ms modelled pipeline")
        if av_total_ms < -floor:
            findings.append(Finding(
                code="AV_AUDIO_LEADS_LIGHT", severity="error", domain="sync",
                observed=av_total_ms, expected=av_pipeline_ms,
                delta=av_residual_ms, unit="ms",
                message=f"audio LEADS light by {-av_total_ms:.1f} ms ({arith}). "
                        f"On a healthy device this is impossible: both paths are "
                        f"corrected forward by the same 46.439 ms, and audio "
                        f"additionally waits for a generator block, so audio "
                        f"must LAG. One of the two corrections is not being "
                        f"applied (that fault is -46 ms or -93 ms; this is past "
                        f"the {floor:.0f} ms measurement floor).",
                detail="audio_generator.c:402-433 pre-advances the carrier "
                       "phase; config_parser.c:2699-2702 offsets the LED "
                       "anchor. Check both."))
        elif av_total_ms < lo:
            findings.append(Finding(
                code="AV_OFFSET_MARGINAL", severity="info", domain="sync",
                observed=av_total_ms, expected=av_pipeline_ms,
                delta=av_residual_ms, unit="ms",
                message=f"audio measures {-av_total_ms:.1f} ms AHEAD of light "
                        f"({arith}), which is outside the healthy "
                        f"{lo:.0f}..{hi:.0f} ms band but inside this "
                        f"instrument's own {floor:.0f} ms floor, so it is not "
                        f"reported as a fault. A genuinely missing 46.439 ms "
                        f"correction would read -46 ms or -93 ms.",
                detail="devicemodel.AV_OFFSET_FLOOR_MS records the measured "
                       "estimator bias this floor comes from."))
        elif av_total_ms > hi:
            findings.append(Finding(
                code="AV_CONST_OFFSET", severity="error", domain="sync",
                observed=av_total_ms, expected=av_pipeline_ms,
                delta=av_residual_ms, unit="ms",
                message=f"audio lags light by a CONSTANT {av_total_ms:.1f} ms "
                        f"({arith}; expected up to ~{hi:.0f} ms including "
                        f"8-20 ms of dispatch lag). This is one offset, not "
                        f"many late events -- it has been subtracted before the "
                        f"per-event checks below.",
                detail="Candidates: codec group delay, an extra buffer in the "
                       "I2S path, or dispatch lag beyond the usual budget "
                       "(config_parser.c:1933 logs at INFO on the dispatch "
                       "path and costs ~7.6 ms per batch at 115200 baud)."))
        else:
            findings.append(Finding(
                code="AV_OFFSET_OK", severity="info", domain="sync",
                observed=av_total_ms, expected=av_pipeline_ms, unit="ms",
                message=f"audio lags light by {av_total_ms:.1f} ms, inside the "
                        f"expected {lo:.0f}..{hi:.0f} ms band ({arith})."))

    # ---- events ----------------------------------------------------------
    # Capture gains, measured once, so the event realisation can tell an
    # ABSENT change from an unlocatable one (see _traj_realise).
    t_dev_a = (obs.audio.t_rec - t0_a) if obs.audio is not None else np.zeros(0)
    coh_a = np.zeros(t_dev_a.size, dtype=bool)
    if obs.audio is not None and getattr(exp.mix, "coherent_pair", None) is not None \
            and exp.mix.coherent_pair.size == exp.mix.t.size:
        coh_a = np.interp(t_dev_a, exp.mix.t,
                          exp.mix.coherent_pair.astype(float),
                          left=0.0, right=0.0) > 0.5
    gains = {"audio": _audio_level_gain(exp, obs, t_dev_a, coh_a)}
    for _ch, _L in exp.light.items():
        gains[("led", _ch)] = _led_level_gain(_L, obs.light.get(_ch), cfg, t0_l)
    stats["capture_gain_audio"] = gains["audio"]
    findings += _check_events(exp, obs, cfg, t0_l, t0_a, led_delay, aud_delay,
                              drift_common, t0_unc_ms=sync.uncertainty_ms
                              if math.isfinite(sync.uncertainty_ms) else 0.0,
                              gains=gains)

    # ---- values ----------------------------------------------------------
    # BOTH checks are corrected by the COMMON term, never by their own domain's
    # figure. Correcting the light check by `ppm_light` divided a real LED
    # timebase error out of the only check that could see it.
    t0_unc_s = max(0.0, sync.uncertainty_ms / 1000.0)
    findings += _check_light_values(exp, obs, cfg, t0_l, drift_common, t0_unc_s)
    findings += _check_light_modulation(exp, obs, cfg, t0_l)
    findings += _check_audio_values(exp, obs, cfg, t0_a, drift_common, stats,
                                    t0_unc_s)

    findings += exp.lints

    # ---- never present a t0-derived verdict as harder than its t0 --------
    # Every timing finding is measured RELATIVE to t0, so if t0 itself is only
    # known to +/-4.75 s then "audio leads light by 49 ms" is not evidence of
    # anything. Reporting it as an ERROR sent the reader hunting for a missing
    # 46 ms correction that was never missing. The finding is still shown --
    # suppressing it would hide a real bug on a session that happens to sync
    # badly -- but it is demoted to a warning and says why.
    # ...UNLESS the residuals themselves have proved the stated uncertainty
    # wrong. EVENT_COMMON_OFFSET fires when a quarter or more of the located
    # events agree on one offset larger than 3x that uncertainty, which is
    # precisely the evidence that "it may be entirely an artefact of the sync
    # solution" is false. MEASURED: `--fault drop:0` on 90_measure_sync -- the
    # marker burst itself never fires -- left every consequence demoted and the
    # run exiting 0, so a session whose first entry never executed read as a
    # healthy device.
    common_offset = any(f.code == "EVENT_COMMON_OFFSET" for f in findings)
    if sync.confidence in ("low", "none") and not common_offset:
        for f in findings:
            if f.code in _T0_DERIVED_CODES and f.severity == "error":
                f.severity = "warning"
                f.confidence = "low"
                f.detail = (f.detail + "  " if f.detail else "") + (
                    f"DEMOTED: t0 is only known to "
                    f"+/-{sync.uncertainty_ms:.0f} ms, which is larger than "
                    f"this discrepancy, so it may be entirely an artefact of "
                    f"the sync solution. Add a sync marker and re-record "
                    f"before acting on it.")

    rep = Report(ledc=exp.source, observation=obs.source, front_end=obs.front_end,
                 sync=sync, findings=_dedupe(findings),
                 av_offset_ms=av_total_ms, av_offset_sd_ms=float("nan"),
                 drift_common_ppm=drift_common, drift_differential_ppm=drift_diff,
                 stats=stats)
    return rep


# ---------------------------------------------------------------------------
# Event timing
# ---------------------------------------------------------------------------


# How many flicker periods of slack a realisation method earns, by method.
#
# MEASURED on tests/selftest.ledc, whose t=60000 entry is a light-only
# 10 -> 20 Hz step, with nothing else wrong:
#
#   smoothed rate series  clean residual +123 ms; 800 ms injected read 473 ms
#   edge-interval switch   clean residual  +33 ms; 800 ms injected read 833 ms
#
# The smoothed series is both BIASED and COMPRESSIVE, because ObservedLight.
# freq_hz is a 10-cycle rolling mean and timing a step in it times the mean,
# not the step. That compression is why the old tolerance had to be ten periods
# wide -- and a ten-period tolerance is 1025 ms at 10 Hz, which exceeded the
# 400 ms realisation window at every rate the 82 shipped sessions use, so
# EVENT_LATE for an LED value change was unreachable at ANY magnitude.
#
# Timing the step off the edge intervals is linear and sample-exact to one
# cycle, so it earns 2 periods. Duty and brightness have no such estimator
# (they are per-cycle quantities, also 10-cycle smoothed), so they keep the
# wide budget -- and say so in the finding's detail.
_PERIODS_EDGE_SWITCH = 2.0
_PERIODS_SMOOTHED = 10.0
_NOTE_EDGE_SWITCH = "flicker-rate change, timed from the edge intervals"


def _edge_rate_step(O, pred: float, w_b: float, w_a: float,
                    f0: float, f1: float) -> float:
    """Time a flicker-RATE step from the edge intervals themselves.

    The firmware picks up a new rate at the next cycle boundary
    (led_matrix_example.c:802), so the first interval of the new length starts
    at that boundary -- within one OLD period of the write. Classifying each
    interval as "old" or "new" by which period it is closer to, and taking the
    first edge that begins a run of `new`, recovers the step directly.

    Returns (t_step, applicable). `applicable` is False only when the method
    CANNOT be used here -- the two rates are within 15% of each other, or there
    are too few edges -- and the caller then falls back to the smoothed series
    with its wider budget. When it IS applicable and finds nothing, that is the
    ANSWER, not a reason to try a worse estimator: falling through to the
    10-cycle smoothed series found a spurious "step" 822 ms away (the tail of
    the PREVIOUS rate change) and swallowed a dropped entry inside the wide
    budget that estimator needs.
    """
    NO = (float("nan"), False)
    if not (f0 > 0 and f1 > 0):
        return NO
    T0, T1 = 1.0 / f0, 1.0 / f1
    if abs(T1 - T0) < 0.15 * max(T0, T1):
        return NO
    r = O.edges_rise
    pad = 3.0 * max(T0, T1)
    sel = r[(r >= pred - w_b - pad) & (r <= pred + w_a + pad)]
    if sel.size < 8:
        return NO
    d = np.diff(sel)
    good = (d > 0.5 * min(T0, T1)) & (d < 1.6 * max(T0, T1))
    is_new = np.abs(d - T1) < np.abs(d - T0)
    m = 3
    if d.size < 2 * m:
        return NO
    for k in range(m, d.size - m + 1):
        if not (good[k:k + m].all() and is_new[k:k + m].all()):
            continue
        # ...and the run just before it must have been OLD, otherwise this is
        # the tail of a region that was already at the new rate and the step
        # itself is outside the window.
        if is_new[k - m:k].any():
            continue
        return float(sel[k]), True
    return float("nan"), True


def _event_tolerance(exp: Expectation, cfg: CompareConfig, ev: dict,
                     periods: float = _PERIODS_SMOOTHED,
                     t0_unc_ms: float = 0.0
                     ) -> tuple[float, float]:
    """(tol_early_ms, tol_late_ms) for one event. Negative = early.

    `t0_unc_ms` IS NOT OPTIONAL IN SPIRIT. Every residual here is measured
    relative to t0, so a residual cannot be judged tighter than t0 is known.
    Leaving it out is how a report that states "t0 = 2.0092 s, confidence HIGH
    (+/-5 ms)" went on to raise four `EVENT_EARLY ... fired 9 ms EARLY` errors
    against the EVENT THAT DEFINED t0, on a bit-perfect recording: the early
    threshold is -5 ms, so any t0 bias past 5 ms manufactures an error on the
    session's own sync marker. Adding the stated uncertainty is not a widened
    tolerance, it is the term that was missing from the arithmetic.

    Split out of _check_events because THE SEARCH WINDOW MUST BE DERIVED FROM
    THIS, not fixed. When the realisation search was a flat +/-400 ms while the
    verdict tolerance was `25 + slack + 10*period`, the reportable band was
    EMPTY for every flicker rate below ~27 Hz (525 ms at 20 Hz, 1025 ms at
    10 Hz, 5025 ms at 2 Hz) -- i.e. for every rate any of the 82 shipped
    sessions uses. An 800 ms-late 10->20 Hz step printed "the recording matches
    the .ledc within every threshold".
    """
    t_dev = ev["t_eff_ms"] / 1000.0
    slack_ms = dm.CLOCK_SLACK_PPM * 1e-6 * t_dev * 1000.0
    unc = max(0.0, t0_unc_ms)
    tol_late = cfg.event_late_ms + slack_ms + unc
    tol_early = cfg.event_early_ms - slack_ms - unc
    kind, ch = ev["kind"], ev["channel"]
    if kind == "led_update":
        # A value written to a RUNNING channel is only picked up at the next
        # flicker cycle boundary (led_matrix_example.c:802), so one full period
        # of slack is physical. The rest depends on which estimator timed it --
        # see _PERIODS_EDGE_SWITCH / _PERIODS_SMOOTHED.
        per = _period_at(exp, ch, t_dev)
        if math.isfinite(per):
            tol_late += periods * per * 1000.0
            tol_early -= periods * per * 1000.0
    elif kind == "led_stop":
        # A stop is only VISIBLE as the absence of the next edge, so the last
        # observed edge precedes the stop by up to one full period.
        per = _period_at(exp, ch, max(0.0, t_dev - 1.0))
        if math.isfinite(per):
            tol_early -= per * 1000.0
            tol_late += per * 1000.0
    return tol_early, tol_late


# Hard ceiling on the realisation search, in seconds. Past this a "late event"
# is indistinguishable from the NEXT event on the same channel, and a change
# point found 15 s away says more about the session than about the fault.
_SEARCH_CAP_S = 8.0


def _search_window(exp: Expectation, ev: dict, tol_early_ms: float,
                   tol_late_ms: float, cfg: CompareConfig,
                   neighbours: dict[tuple[str, int], np.ndarray]
                   ) -> tuple[float, float, bool]:
    """(before_s, after_s, covers_verdict) for the realisation search.

    Wide enough to QUANTIFY any lateness the verdict would flag, so sensitivity
    stops being a band -- the old fixed window made a 200 ms-late step
    reportable while the same step 700 ms or 1500 ms late produced silence, so
    the worse the fault the likelier it was missed.

    Clamped by the distance to the neighbouring events on the same channel,
    because past the midpoint between two entries a transition can no longer be
    attributed to this one. `covers_verdict` records whether the clamp bit: when
    it did, lateness beyond the window is PRESENT BUT UNQUANTIFIABLE and must be
    reported as such, never swallowed.
    """
    base = max(cfg.event_search_s, dm.LED_FIRST_EDGE_GRACE_MS / 1000.0)
    want_after = max(base, 1.5 * tol_late_ms / 1000.0)
    want_before = max(base, 1.5 * abs(tol_early_ms) / 1000.0)
    key = ("led" if ev["kind"].startswith("led") else "audio", ev["channel"])
    ts = neighbours.get(key)
    # A SEARCH SCALED TO THE FEATURE, not only to the verdict tolerance. The
    # observable of an entry that starts or ends a RAMP is the ramp's corner,
    # and a corner can only be found by a search that spans a useful part of
    # the slope: the shipped sessions ramp over 15-300 s, so a +/-400 ms search
    # sees a straight line and cannot tell a 900 ms-late ramp start from an
    # on-time one. MEASURED: `--fault late:1000:900` on 27_genus_40hz_dim
    # (every zone AND the audio activated 900 ms late) produced LED findings
    # byte-identical to the clean run, and exit 0. Widening the SEARCH does not
    # widen the VERDICT -- the tolerance below is unchanged -- it only lets a
    # fault be quantified instead of being invisible.
    if ts is not None and ts.size > 1:
        t_ev = ev["t_eff_ms"] / 1000.0
        nx = ts[ts > t_ev + 1e-9]
        pv = ts[ts < t_ev - 1e-9]
        if nx.size:
            want_after = max(want_after, 0.35 * float(nx[0] - t_ev))
        if pv.size:
            want_before = max(want_before, 0.35 * float(t_ev - pv[-1]))
    want_after = min(want_after, _SEARCH_CAP_S)
    want_before = min(want_before, _SEARCH_CAP_S)
    after, before = want_after, want_before
    if ts is not None and ts.size > 1:
        # A HARD clamp, not one floored at the default search half-width. With
        # the floor, a pair of entries 400 ms apart still searched +/-400 ms, so
        # the change point found the NEIGHBOUR's transition, called it this
        # event's realisation, and reported nothing -- a dropped entry 400 ms
        # after another one went unreported. The floor below is one LED
        # first-edge grace period, just enough that the window never degenerates.
        t = ev["t_eff_ms"] / 1000.0
        nxt = ts[ts > t + 1e-9]
        prv = ts[ts < t - 1e-9]
        floor_s = dm.LED_FIRST_EDGE_GRACE_MS / 1000.0
        if nxt.size:
            after = min(after, max(floor_s, 0.45 * float(nxt[0] - t)))
        if prv.size:
            before = min(before, max(floor_s, 0.45 * float(t - prv[-1])))
    covers = (after >= tol_late_ms / 1000.0
              and before >= abs(tol_early_ms) / 1000.0)
    return before, after, covers



def _audio_level_gain(exp: Expectation, obs: Observation, t_dev: np.ndarray,
                      coh: np.ndarray) -> float:
    """measured-RMS / demanded-AMPLITUDE, from spans where the model is exact.

    One number for the whole recording, because the capture gain is one number.
    It is needed in three places -- the level-profile check, the coherent-peak
    bound, and the event realisation's "is this change there at all?" test --
    so it is computed once here.
    """
    A = obs.audio
    if A is None:
        return float("nan")
    e = np.interp(t_dev, exp.mix.t, exp.mix.rms_rel, left=np.nan, right=np.nan)
    m = (np.isfinite(e) & (e > 1e-3) & np.isfinite(A.rms) & (A.rms > 0)
         & ~_blackout(t_dev, exp.events, 0.15) & ~coh)
    if m.sum() < 4:
        return float("nan")
    return float(robust_median(A.rms[m] / e[m]))


def _led_level_gain(L, O, cfg: CompareConfig, t0: float) -> float:
    """measured optical level / demanded brightness fraction, per channel."""
    if O is None or not O.available:
        return float("nan")
    t_dev = O.t_rec - t0
    act = np.interp(t_dev, L.t, L.active.astype(float), left=0.0, right=0.0) > 0.99
    e_brt = np.interp(t_dev, L.t, L.bright_pct, left=np.nan, right=np.nan)
    m = (act & np.isfinite(O.level) & np.isfinite(e_brt)
         & (e_brt >= cfg.min_bright_pct))
    if m.sum() < 8:
        return float("nan")
    g = robust_median(O.level[m] / (e_brt[m] / 100.0))
    return float(g) if math.isfinite(g) and g > 0 else float("nan")


def _check_events(exp: Expectation, obs: Observation, cfg: CompareConfig,
                  t0_l: float, t0_a: float, led_delay: float, aud_delay: float,
                  ppm: float, t0_unc_ms: float = 0.0,
                  gains: dict | None = None) -> list[Finding]:
    out: list[Finding] = []
    k = 1.0 + (ppm * 1e-6 if math.isfinite(ppm) else 0.0)
    residuals: list[tuple[float, float, str, float]] = []
    # Event times per (domain, channel), for the search-window clamp.
    neighbours: dict[tuple[str, int], list[float]] = {}
    for ev in exp.events:
        key = ("led" if ev["kind"].startswith("led") else "audio", ev["channel"])
        neighbours.setdefault(key, []).append(ev["t_eff_ms"] / 1000.0)
    nb = {key: np.array(sorted(v)) for key, v in neighbours.items()}
    # An event whose predicted time falls outside the recording cannot be
    # judged. Without this guard a recording that stops early reports every
    # remaining event as tens of seconds "early" (the last edge in the file
    # gets mistaken for the session's end) -- a page of confident nonsense
    # from a perfectly ordinary short capture.
    margin = 0.5
    t_lo, t_hi = margin, obs.duration_s - margin
    n_outside = 0
    n_before = 0
    first_outside_ms = None

    for ev in exp.events:
        kind = ev["kind"]
        ch = ev["channel"]
        dim_reason = ""
        t_dev = ev["t_eff_ms"] / 1000.0
        pred_rough = (t0_l if kind.startswith("led") else t0_a) + t_dev
        if pred_rough < t_lo:
            n_before += 1
            continue
        if pred_rough > t_hi:
            n_outside += 1
            if first_outside_ms is None:
                first_outside_ms = ev["t_ms"]
            continue
        if kind.startswith("led"):
            O = obs.light.get(ch)
            if O is None:
                continue
            # An LED entry whose demanded brightness is (near) ZERO at its own
            # timestamp emits NO PHOTONS, so there is nothing to time and its
            # absence is not evidence of anything. Every shipped session opens
            # with `>0` -- a brightness ramp-in from zero -- so without this the
            # first LED entry of a healthy session is reported as EVENT_MISSING
            # on every channel, which is both wrong and the first thing a reader
            # sees.
            # An LED event is optically timeable only if there are photons on at
            # least one side of it. Take the demanded brightness just BEFORE the
            # event and AT the event: if BOTH are below the measurable floor,
            # nothing visible changes and the event cannot be graded.
            #
            #   ramp-in start (`>0`)     before 0,    at 0     -> not observable
            #   stop from full           before 45,   at 45    -> observable
            #   last entry of a fade-out before ~0.3, at 0     -> not observable
            #
            # Both of the unobservable cases occur in EVERY shipped session,
            # which is why a flawless 60-minute render produced four
            # EVENT_MISSING errors at the open and four EVENT_EARLY warnings at
            # the close before this gate existed.
            L_ev = exp.light.get(ch)
            if L_ev is not None:
                i_at = min(max(int(np.searchsorted(L_ev.t, t_dev)), 0),
                           L_ev.t.size - 1)
                i_pre = min(max(int(np.searchsorted(L_ev.t, t_dev - 0.3)), 0),
                            L_ev.t.size - 1)
                # Brightness only counts while the channel is ACTIVE: after a
                # stop, bright_pct holds its last value, so reading it blind
                # picked up the sync marker's 100% from 0.3 s earlier and
                # concluded a pitch-black ramp-in start was observable.
                def _eff_bright(i: int) -> float:
                    if not bool(L_ev.active[i]):
                        return 0.0
                    return float(np.nan_to_num(L_ev.bright_pct[i]))

                b_at, b_pre = _eff_bright(i_at), _eff_bright(i_pre)
                # THE VERDICT ON OBSERVABILITY IS DEFERRED until the search has
                # actually run (see below). This gate used to skip the entry on
                # the spot whenever the two sampled brightnesses straddling it
                # were both below the floor -- which is true of every
                # `>0` ramp-in start, i.e. the first LED entry of every shipped
                # session. That removed the ramp's ORIGIN from the measurement
                # entirely, and the origin is a real observable: a 900 ms-late
                # activation moves the whole ramp by 900 ms and the trajectory
                # match finds it. The brightness is still recorded, and is used
                # to word the finding if nothing turns out to be measurable.
                dim_reason = (
                    f"the .ledc demands only {max(b_at, b_pre):.1f}% brightness "
                    f"at and just before this entry (a ramp-in start or the "
                    f"tail of a fade-out), so only the shape of the ramp is "
                    f"observable here, not a transition"
                    if max(b_at, b_pre) < cfg.min_bright_pct else "")
            if not O.available:
                out.append(Finding(
                    code="EVENT_NOT_OBSERVABLE", severity="info",
                    domain="led", channel=ch, t_ms=ev["t_ms"],
                    line_no=ev.get("line_no"),
                    message=f"LED ch{ch}: its sensor saw no optical signal at "
                            f"all anywhere in the recording, so no event on "
                            f"this channel could be timed. The channel itself "
                            f"is reported separately (CHANNEL_NEVER_ON).",
                    detail=O.note))
                continue
            # The search window is sized from the WIDEST tolerance any
            # estimator could earn; the verdict tolerance is then recomputed
            # from the estimator that actually answered.
            tol_early, tol_late = _event_tolerance(
                exp, cfg, ev, t0_unc_ms=t0_unc_ms)
            w_before, w_after, covers = _search_window(
                exp, ev, tol_early, tol_late, cfg, nb)
            pred = t0_l + (t_dev + led_delay) * k
            got, note, est_unc = _realise_led(
                exp, obs, ev, pred, (w_before, w_after), cfg,
                dim=bool(dim_reason), gains=gains)
            if note == _NOTE_EDGE_SWITCH:
                tol_early, tol_late = _event_tolerance(
                    exp, cfg, ev, _PERIODS_EDGE_SWITCH, t0_unc_ms=t0_unc_ms)
            # ONE sigma of the realisation estimator's own half-width. Three
            # was tried first and it is too much: the valley of a clean 6 dB
            # audio level step is +/-260 ms wide at the 5% level even though the
            # measurement itself is good to ~10 ms, so 3 sigma hid a 180 ms-late
            # entry that the same fit had measured as 187.5 ms. The cases 3
            # sigma was protecting -- gradual, slope-only changes -- are now
            # excluded from timing altogether by _demand_is_sharp, which is the
            # honest answer for them.
            tol_late += est_unc
            tol_early -= est_unc
            dom, label = "led", f"LED ch{ch}"
        else:
            if obs.audio is None:
                continue
            O = obs.audio
            # The SYMMETRIC audio case, for exactly the same reason as light: an
            # entry demanding (near) zero amplitude on both sides of itself makes
            # no sound, so its onset cannot be timed acoustically. Sessions open
            # with `A ... >0` (volume ramp-in from zero) and close with `A ... 0`
            # at the end of a 30 s fade-out, so both ends of every shipped
            # session hit this. Grading them anyway reported the open as 238 ms
            # LATE and the close as 258 ms EARLY -- the detector simply waiting
            # for the level to clear the noise floor, not the device misfiring.
            A_ev = exp.audio.get(ch)
            if A_ev is not None:
                j_at = min(max(int(np.searchsorted(A_ev.t, t_dev)), 0),
                           A_ev.t.size - 1)
                j_pre = min(max(int(np.searchsorted(A_ev.t, t_dev - 0.3)), 0),
                            A_ev.t.size - 1)

                def _eff_amp(j: int) -> float:
                    if not bool(A_ev.active[j]):
                        return 0.0
                    return float(np.nan_to_num(A_ev.amp[j]))

                a_at, a_pre = _eff_amp(j_at), _eff_amp(j_pre)
                # Deferred for the same reason as the LED gate above: a volume
                # ramp-in from zero still has a locatable ORIGIN, and skipping
                # the entry here is how `--fault late:{T}:900` on a session
                # whose first entries are all `>0` produced zero findings.
                # 0.02 of full scale is ~-34 dBFS before the 1/16 mix headroom.
                dim_reason = (
                    f"the .ledc demands only {max(a_at, a_pre) * 100:.1f}% "
                    f"volume at and just before this entry (a ramp-in start or "
                    f"the tail of a fade-out), so only the shape of the ramp is "
                    f"observable here, not a transition"
                    if max(a_at, a_pre) < 0.02 else "")
            tol_early, tol_late = _event_tolerance(
                exp, cfg, ev, t0_unc_ms=t0_unc_ms)
            w_before, w_after, covers = _search_window(
                exp, ev, tol_early, tol_late, cfg, nb)
            pred = t0_a + (t_dev + aud_delay) * k
            got, note, est_unc = _realise_audio(
                exp, obs, ev, pred, (w_before, w_after), cfg,
                dim=bool(dim_reason), gains=gains)
            tol_late += est_unc
            tol_early -= est_unc
            dom, label = "audio", f"audio ch{ch}"
        if got is None:
            # No observable of its own: the entry changes nothing a recording
            # can see (e.g. an update immediately followed by a stop, where the
            # stop event covers the transition, or a mid-ramp entry that only
            # continues the slope). Not a miss -- but SAY SO, with the reason,
            # because silence here is indistinguishable from a pass.
            out.append(Finding(
                code="EVENT_NOT_OBSERVABLE", severity="info", domain=dom,
                channel=ch, t_ms=ev["t_ms"], line_no=ev.get("line_no"),
                message=f"{label}: the entry at t={ev['t_ms']:.0f} ms could not "
                        f"be timed -- " + (dim_reason or note or
                        "no observable quantity changes at it in a way a "
                        "recording can localise (the demanded trajectory runs "
                        "straight through it)") + ".",
                detail="Not a fault, and not a pass either: this entry's TIMING "
                       "is not graded. Its VALUES still are, by the "
                       "flicker-rate / duty / brightness / level checks."))
            continue
        if not math.isfinite(got):
            # NOTHING WAS FOUND. This used to be dropped silently for every
            # event that was not `fresh`, which is why detection sensitivity was
            # a BAND rather than a threshold: a 200 ms-late audio step was
            # reported at 197 ms, while the same step 700 ms or 1500 ms late
            # produced "Nothing to report: the recording matches the .ledc
            # within every threshold" and exit 0. A fault getting WORSE must
            # never make it more likely to be missed.
            win_txt = (f"-{w_before * 1000:.0f}/+{w_after * 1000:.0f} ms")
            if ev.get("fresh"):
                out.append(Finding(
                    code="EVENT_MISSING", severity="error", domain=dom,
                    channel=ch, t_ms=ev["t_ms"], line_no=ev.get("line_no"),
                    message=f"{label}: the entry at t={ev['t_ms']:.0f} ms should "
                            f"have started it, but nothing observable happened "
                            f"within {win_txt} of the predicted time ({note}).",
                    detail=f"predicted recorder time {pred:.3f} s"))
            elif covers:
                out.append(Finding(
                    code="EVENT_NOT_LOCATED", severity="error", domain=dom,
                    channel=ch, t_ms=ev["t_ms"], line_no=ev.get("line_no"),
                    message=f"{label}: the entry at t={ev['t_ms']:.0f} ms "
                            f"produced NO matching change in the recording "
                            f"within {win_txt} of the predicted time ({note}). "
                            f"That window covers this event's whole lateness "
                            f"budget, so either the entry never took effect or "
                            f"it is out by more than the budget.",
                    detail=f"predicted recorder time {pred:.3f} s; verdict "
                           f"tolerance {tol_early:.0f}..{tol_late:.0f} ms"))
            else:
                # ERROR, not a warning. "Present but unquantifiable" is still
                # PRESENT: the entry demanded a change, the search found none
                # anywhere it could look, and the only thing the recording
                # cannot supply is the magnitude. Grading that as a warning put
                # it below `$?`, so `--fault late:{T}:15000` on a 30-minute
                # session -- a zone switching rate fifteen seconds late --
                # exited 0 and read as a clean device.
                out.append(Finding(
                    code="EVENT_NOT_LOCATED", severity="error", domain=dom,
                    channel=ch, t_ms=ev["t_ms"], line_no=ev.get("line_no"),
                    message=f"{label}: the entry at t={ev['t_ms']:.0f} ms "
                            f"produced NO matching change within {win_txt} of "
                            f"the predicted time ({note}). PRESENT BUT "
                            f"UNQUANTIFIABLE: the search had to stop short of "
                            f"this event's {tol_late:.0f} ms lateness budget "
                            f"because the next entry on this channel arrives "
                            f"too soon to tell the two apart.",
                    detail=f"predicted recorder time {pred:.3f} s. Something is "
                           f"wrong here, but this recording cannot say how far "
                           f"out it is."))
            continue
        delta_ms = (got - pred) * 1000.0
        residuals.append((t_dev, delta_ms, dom, max(abs(tol_late),
                                                    abs(tol_early))))
        if delta_ms > tol_late:
            out.append(Finding(
                code="EVENT_LATE", severity="error", domain=dom, channel=ch,
                t_ms=ev["t_ms"], line_no=ev.get("line_no"),
                expected=0.0, observed=delta_ms, delta=delta_ms, unit="ms",
                message=f"{label}: the entry at t={ev['t_ms']:.0f} ms fired "
                        f"{delta_ms:.0f} ms LATE ({note}).",
                detail=f"threshold {tol_late:.0f} ms; dispatch lag of 8-20 ms is "
                       f"already inside the budget "
                       f"(config_parser.c:1933, :243)"))
        elif delta_ms < tol_early:
            out.append(Finding(
                code="EVENT_EARLY", severity="error", domain=dom, channel=ch,
                t_ms=ev["t_ms"], line_no=ev.get("line_no"),
                expected=0.0, observed=delta_ms, delta=delta_ms, unit="ms",
                message=f"{label}: the entry at t={ev['t_ms']:.0f} ms fired "
                        f"{-delta_ms:.0f} ms EARLY ({note}). Events cannot "
                        f"legitimately precede their deadline by more than the "
                        f"500 us timer look-ahead.",
                detail="timing_engine.c:486"))

    if n_before:
        # Different cause, different sentence. An event predicted BEFORE the
        # recording starts means the solved t0 sits at (or before) the first
        # sample, which on an unmarked session is the sync solver running out of
        # room -- not a short capture. Calling it "the recording covers 253 s of
        # a 240 s session" was arithmetic nonsense.
        out.append(Finding(
            code="EVENTS_BEFORE_RECORDING", severity="info", domain="sync",
            observed=float(n_before),
            message=f"{n_before} expected event(s) are predicted to fall BEFORE "
                    f"the first sample of the recording (t0 solved at "
                    f"{t0_l:.3f} s), so they could not be checked. Either the "
                    f"capture started after the session did, or t0 is early by "
                    f"about that much.",
            detail="Start the capture before you start the session, and author "
                   "a sync marker so t0 does not have to be inferred."))
    if n_outside:
        session_end_s = max((e["t_eff_ms"] for e in exp.events), default=0) / 1000.0
        out.append(Finding(
            code="RECORDING_INCOMPLETE", severity="warning", domain="sync",
            t_ms=first_outside_ms, expected=session_end_s * 1000.0,
            observed=(obs.duration_s - t0_l) * 1000.0, unit="ms",
            message=f"the recording covers {obs.duration_s - t0_l:.0f} s of a "
                    f"{session_end_s:.0f} s session, so {n_outside} expected "
                    f"event(s) from t={first_outside_ms:.0f} ms onward could not "
                    f"be checked. Everything inside the covered span WAS checked.",
            detail="Re-record with the capture running past the end of the "
                   "session if you need those events graded."))

    out += _check_common_offset(residuals, t0_unc_ms)
    out += _check_event_drift(residuals, cfg, ppm)
    return out


def _check_common_offset(residuals: list[tuple[float, float, str, float]],
                         t0_unc_ms: float) -> list[Finding]:
    """Do ALL the located events share one large offset?

    If they do, and it is much larger than the sync solution's own stated
    uncertainty, then one of two things is true and both are errors:

      * t0 is wrong by more than the solver admits -- the feature it locked
        onto is not the one it thinks it is, which happens when an entry near
        the top of the session never executed; or
      * every event really did fire that late, which is a device fault.

    EITHER WAY IT MUST REACH `$?`, and it must NOT be demoted by a low sync
    confidence, because this finding IS the evidence that the confidence is
    understated. MEASURED: `--fault drop:0` on 90_measure_sync -- the sync
    marker burst itself never fires -- made the activation-edge matcher lock
    onto the session's first body entry instead, so t0 came out 1000 ms wrong
    while being stated as "+/-250 ms", and all 32 residuals sat at -967 to
    -1074 ms. Round 2 recorded the result as "0 error(s), 8 warning(s), EXIT=0":
    the session's first entry never executed and the tool reported a healthy
    device, because the fault and the sync degradation were the same event and
    the demotion rule cancelled the finding it was meant to qualify.

    The 3x factor is what separates it from a genuinely unmeasurable recording.
    MEASURED on the same session with `--fault ac-couple:12` (a recorder-domain
    imperfection that must NOT be accused): the residuals also share an offset,
    +2004 to +2050 ms, but the solver already says +/-1675 ms there, so the
    offset is 1.2x its own uncertainty and nothing is claimed. On drop:0 it is
    4x. Nothing in the library falls between.
    """
    if len(residuals) < 8:
        return []
    d = np.array([r[1] for r in residuals], dtype=np.float64)
    tol = np.array([max(abs(r[3]), 1.0) for r in residuals], dtype=np.float64)
    unc = max(float(t0_unc_ms) if math.isfinite(t0_unc_ms) else 0.0, 5.0)
    # The residuals that BROKE their own budget -- i.e. exactly the ones that
    # raised an EVENT_LATE / EVENT_EARLY and then had it demoted.
    out_m = np.abs(d) > tol
    n_out = int(out_m.sum())
    if n_out < max(4, int(0.25 * d.size)):
        return []
    dd = d[out_m]
    med = float(robust_median(dd))
    mad = float(robust_median(np.abs(dd - med)))
    if not (math.isfinite(med) and math.isfinite(mad)):
        return []
    if abs(med) < 3.0 * unc or mad > 0.3 * abs(med):
        return []
    return [Finding(
        code="EVENT_COMMON_OFFSET", severity="error", domain="sync",
        observed=med, expected=0.0, delta=med, unit="ms",
        message=f"{n_out} of {d.size} located events are out by the SAME "
                f"{med:+.0f} ms (spread +/-{mad:.0f} ms), which is more than 3x "
                f"the +/-{unc:.0f} ms this recording's t0 is stated to. That "
                f"many events cannot agree on one offset by accident: either t0 "
                f"is wrong by about {med:+.0f} ms -- most likely an entry near "
                f"the top of the session (the sync marker?) never executed, so "
                f"the solver locked onto the wrong optical feature -- or the "
                f"timeline really did run that late from that point on.",
        detail="Check that the marker burst is present in the recording and "
               "that the .ledc's first entries actually executed. This finding "
               "is deliberately NOT demoted by a low sync confidence: it is "
               "the evidence that the confidence is understated.")]


def _check_event_drift(residuals: list[tuple[float, float, str, float]],
                       cfg: CompareConfig, ppm: float) -> list[Finding]:
    """Does event lateness GROW over the session?

    With absolute-deadline scheduling (config_parser.c:2082) it must not, so a
    real trend is its own finding. But a SLOPE NEEDS FOUR THINGS, and the
    previous version had none of them:

      * a lever arm. It accepted a 30 s span, where 2.4 ms/s of apparent tilt
        extrapolates to "5500 ms per 1000 s". MEASURED on a clean render of
        04_meditation_theta: "[ERROR] EVENT_DRIFT: event lateness GROWS ...
        -171 ms accumulated between t=0 s and t=31 s ... this points at a
        clock-ratio problem", observed -5527.8 ms/1000 s = -5.5 parts per
        THOUSAND, on a recording with no fault in it.
      * weights. compare grants a single led_stop residual +/-one period of
        legitimate slack -- +/-333 ms at 3 Hz -- and then 105 ms of exactly
        that slack was being treated as proof of a clock fault. Each residual
        is now weighted by 1/tolerance^2, so a loose point cannot tilt the line.
      * a goodness-of-fit test. "The residuals are noisy" and "the residuals
        trend" are different answers; without a significance test the check
        reported the first as the second.
      * agreement with the independently measured clock. The same report said
        +/-10 ppm while this finding claimed -5500 ppm, a 550x contradiction
        that was never consulted.

    When a trend is visible but a gate fails, that is said out loud as info
    rather than dropped -- a reader who sees nothing cannot tell "no trend"
    from "could not test".
    """
    out: list[Finding] = []
    if len(residuals) < 3:
        return out
    t = np.array([r[0] for r in residuals])
    d = np.array([r[1] for r in residuals])
    tol = np.array([max(abs(r[3]), 1.0) for r in residuals])
    span = float(t.max() - t.min())
    w = 1.0 / tol
    A = np.vstack([t * w, w]).T
    coef, *_ = np.linalg.lstsq(A, d * w, rcond=None)
    slope = float(coef[0])                    # ms per second
    total = slope * span
    resid = d - (coef[0] * t + coef[1])
    n = t.size
    # Standard error of the weighted slope.
    if n > 2:
        chi2 = float(np.sum((resid * w) ** 2))
        sxx = float(np.sum((w * (t - np.average(t, weights=w ** 2))) ** 2))
        se = math.sqrt(chi2 / (n - 2) / sxx) if sxx > 0 else float("inf")
    else:
        se = float("inf")
    # THE PER-POINT UNCERTAINTY IS THE TOLERANCE, NOT THE SCATTER. `chi2/(n-2)`
    # above rescales the stated tolerances by how well the points happen to sit
    # on a line, so a set of residuals that are all small but systematically
    # TILTED comes out hugely significant -- which is precisely what a mixture
    # of estimators with different BIASES produces. The realisations here come
    # from up to five different estimators (first optical edge, edge-interval
    # rate switch, 10-cycle smoothed brightness, audio level step, carrier step)
    # whose offsets differ by far more than their scatter, and the entries that
    # use each one are not spread evenly over the session.
    #
    # MEASURED on bit-perfect full-length renders: 91_measure_17_uniform
    # "-118 ms accumulated between t=0 s and t=1562 s, a slope of -76 +/-17 ppm"
    # and 36_shamanic_trance_drum "+971 ms ... +674 +/-58 ppm", both on
    # recordings with no fault in them, both quoting a standard error an order
    # of magnitude below the tolerances the same residuals were granted.
    se_tol = math.sqrt(1.0 / sxx) if sxx > 0 else float("inf")
    se = max(se, se_tol)
    slope_ppm = slope * 1e3                   # ms/s -> ppm
    se_ppm = se * 1e3
    big = abs(total) > 3 * cfg.event_late_ms
    if not big:
        return out

    gates = []
    # ...AND IT MUST BE VISIBLE IN BOTH DOMAINS. The scheduler drives audio and
    # light off one absolute T0, so a genuine "lateness accumulates" fault moves
    # both. An estimator artefact lives in one domain. A divergence BETWEEN the
    # domains is a different finding with its own name
    # (AV_DIFFERENTIAL_DRIFT), measured from thousands of edges rather than a
    # handful of event residuals, so it is not this check's job.
    per_dom: dict[str, float] = {}
    for dname in ("led", "audio"):
        sel = np.array([r[2] == dname for r in residuals])
        if sel.sum() < 4:
            continue
        ts, ds, ws = t[sel], d[sel], w[sel]
        if float(np.sum((ws * (ts - np.average(ts, weights=ws ** 2))) ** 2)) <= 0:
            continue
        Ad = np.vstack([ts * ws, ws]).T
        cf, *_ = np.linalg.lstsq(Ad, ds * ws, rcond=None)
        per_dom[dname] = float(cf[0]) * 1e3
    if len(per_dom) < 2:
        gates.append(
            f"only the {'/'.join(sorted(per_dom)) or 'one'} domain has enough "
            f"timeable residuals to fit a slope, so this tilt could not be "
            f"cross-checked against the other domain -- and a real accumulating "
            f"lateness must move audio and light together, because both run off "
            f"one absolute T0")
    else:
        pl, pa = per_dom["led"], per_dom["audio"]
        if (pl >= 0) != (pa >= 0) or abs(pl - pa) > 0.5 * max(abs(pl), abs(pa)):
            gates.append(
                f"the two domains do not agree on it (light {pl:+.0f} ppm, "
                f"audio {pa:+.0f} ppm). One absolute T0 drives both, so a real "
                f"accumulating lateness cannot appear in one and not the other; "
                f"this is the spread between the realisation estimators")
    if span < dm.THRESHOLD_DRIFT_FIT_MIN_SPAN_S:
        gates.append(f"the residuals span only {span:.0f} s, and a per-1000 s "
                     f"drift figure needs at least "
                     f"{dm.THRESHOLD_DRIFT_FIT_MIN_SPAN_S:.0f} s of lever arm "
                     f"to be a measurement rather than an extrapolation")
    if n < dm.THRESHOLD_DRIFT_FIT_MIN_POINTS:
        gates.append(f"only {n} residuals are available, below the "
                     f"{dm.THRESHOLD_DRIFT_FIT_MIN_POINTS} this fit needs")
    if not (abs(slope) > dm.THRESHOLD_DRIFT_FIT_SIGMA * se):
        gates.append(f"the fitted slope ({slope_ppm:+.0f} ppm) is within "
                     f"{dm.THRESHOLD_DRIFT_FIT_SIGMA:.0f} standard errors of "
                     f"zero (+/-{se_ppm:.0f} ppm), i.e. it is the scatter of "
                     f"the residuals and not a trend")
    if abs(slope_ppm) <= dm.CLOCK_SLACK_PPM:
        gates.append(f"the implied {slope_ppm:+.0f} ppm is inside the "
                     f"{dm.CLOCK_SLACK_PPM:.0f} ppm uncertainty already "
                     f"allowed on the fitted clock ratio")
    if math.isfinite(ppm) and abs(slope_ppm) < 0.2 * abs(ppm):
        gates.append(f"the implied {slope_ppm:+.0f} ppm is small next to the "
                     f"{ppm:+.0f} ppm clock ratio that has already been "
                     f"divided out, so it is residual fit error")

    if gates:
        out.append(Finding(
            code="EVENT_DRIFT_NOT_TESTED", severity="info", domain="clock",
            observed=slope_ppm, unit="ppm",
            message=f"the event residuals TILT by {total:+.0f} ms across "
                    f"{span:.0f} s ({slope_ppm:+.0f} ppm), but this is NOT "
                    f"reported as a drift: " + "; ".join(gates) + ".",
            detail="A drift claim needs a baseline of minutes, residuals "
                   "weighted by their own tolerances, a slope that beats its "
                   "own standard error, and agreement with the independently "
                   "fitted clock ratio. State the gate that failed rather than "
                   "printing a number that carries no information."))
        return out

    out.append(Finding(
        code="EVENT_DRIFT", severity="error", domain="clock",
        observed=slope_ppm, unit="ppm",
        message=f"event lateness GROWS over the session: {total:+.0f} ms "
                f"accumulated between t={t.min():.0f} s and t={t.max():.0f} s, "
                f"a slope of {slope_ppm:+.0f} +/-{se_ppm:.0f} ppm over {n} "
                f"residuals. The scheduler uses absolute deadlines off a single "
                f"T0, so lateness cannot accumulate -- and this slope is "
                f"larger than the {ppm:+.0f} ppm clock ratio already divided "
                f"out, so it is not the capture interface either.",
        detail="config_parser.c:2082, 805-812"))
    return out


def _period_at(exp: Expectation, ch: int, t_dev: float) -> float:
    L = exp.light.get(ch)
    if L is None:
        return float("nan")
    i = int(np.searchsorted(L.t, t_dev))
    i = min(max(i, 0), L.t.size - 1)
    f = L.emitted_hz[i]
    return 1.0 / f if f > 0 else float("nan")


# Minimum fraction of the observed variance a shifted demanded trajectory has
# to explain before its best lag is treated as a MEASUREMENT of when the entry
# took effect. Below this there is no feature in the window and the honest
# answer is "this entry changes nothing a recording can see", not a timing.
_TRAJ_Q_MIN = 0.25

# ...and how much it has to explain before the best lag is a MEASUREMENT rather
# than merely evidence that the change happened. A clean recording of a real
# step explains two thirds or more; a 15 s brightness ramp-in from zero, whose
# first seconds are below the sensor floor, explained 29% and came back
# "1151 ms LATE" on all four zones of 11_lucid_wbtb.
_TRAJ_MEASURE_Q_MIN = 0.50

# How much better the best lag has to fit than the WORST lag in range, as a
# fraction of the variance the template explains, before the lag is a
# measurement rather than the search grid. A straight ramp with no corner
# scores 0 here however well the template fits the ramp itself.
# How large the fit's own half-width may be, as a fraction of the lag range it
# searched, before the best lag stops being a measurement.
_TRAJ_SIGMA_FRAC = 0.25

# Multiplier on the fit's own half-width for a SHARP feature (a step). A corner
# gets 2x -- see the note at the audio trajectory return.
_SIGMA_K_SHARP = 1.0

# ...and the bar a match pinned to the EDGE of the search has to clear before it
# is reported as "the feature is real but further out than we could look". A
# shallow kink -- a brightness ramp whose slope merely changes, say 15%/300 s to
# 8%/180 s -- has a high overall fit and almost no information about WHERE the
# kink is, and calling that "no matching change in the recording" produced
# EVENT_NOT_LOCATED on all four zones of a clean 11_lucid_wbtb.
# A match pinned to the EDGE of the search is only reported as
# present-but-unlocated when it is unambiguous: an excellent overall fit, a deep
# valley, and a fit half-width small next to the range it ran out of. A shallow
# kink -- a brightness ramp whose slope merely changes, 15%/300 s to 8%/180 s --
# scores q 0.71 and still has no idea WHERE the kink is, and calling that "no
# matching change in the recording" produced EVENT_NOT_LOCATED on all four
# zones of a clean 11_lucid_wbtb.
# How much of a time shift of the demanded trajectory CANNOT be absorbed by the
# fit's own offset and gain, as a fraction of the template's size, before the
# lag counts as identifiable. 0 for a straight ramp, large for any corner,
# step or modulation onset.
_TRAJ_INFO_MIN = 0.08

# How small the fitted gain has to be, against the gain the session calibration
# implies, before the demanded change counts as ABSENT rather than merely badly
# timed. A third is 10 dB down in amplitude terms.
_TRAJ_ABSENT_FRAC = 0.33
_TRAJ_ABSENT_FRAC_SOFT = 0.10

# How much of the window's total demanded variation has to appear AS A
# DISCONTINUITY at the entry -- in the value or in the modulation depth --
# before the entry's time is measurable. See _demand_is_sharp.
_TRAJ_SHARP_FRAC = 0.35

# One quantization step of the fields the sharpness test is applied to, with a
# 1.5x margin. Brightness and duty are integer percent on the device
# (config_parser.c clamp_pct), and so is volume -- which is 0.01 of full scale
# in the amplitude domain.
_SHARP_MIN_JUMP_PCT = 1.5
_SHARP_MIN_JUMP_AMP = 0.015

# Cost caps for the shift fit. 4000 samples and 201 lags keep a full-length
# session's analysis in the same order as the rest of the pipeline; both are
# far above what the lag's precision needs.
_SHIFT_MAX_SAMPLES = 4000
_SHIFT_MAX_LAGS = 201

_TRAJ_EDGE_GAIN_MIN = 0.50
_TRAJ_EDGE_Q_MIN = 0.80
_TRAJ_EDGE_SIGMA_FRAC = 0.20

# Context added on each side of the lag search when matching a trajectory. The
# corner of a ramp is only locatable if the fit sees some of the flat part and
# some of the slope; two seconds of each is enough at the 20 Hz analysis grid
# and cheap at the 1 kHz audio grid.
_TRAJ_CONTEXT_S = 2.0


def _event_gaps(exp: Expectation, ev: dict) -> tuple[float, float]:
    """(seconds to the previous entry, to the next entry) on this channel."""
    dom = "led" if ev["kind"].startswith("led") else "audio"
    t = ev["t_eff_ms"] / 1000.0
    prv, nxt = float("inf"), float("inf")
    for e in exp.events:
        if e["channel"] != ev["channel"]:
            continue
        if (e["kind"].startswith("led")) != (dom == "led"):
            continue
        te = e["t_eff_ms"] / 1000.0
        if te < t - 1e-9:
            prv = min(prv, t - te)
        elif te > t + 1e-9:
            nxt = min(nxt, te - t)
    return prv, nxt



def _demand_is_sharp(t_dem: np.ndarray, y_dem: np.ndarray, t_ev: float,
                     lo: float, hi: float, min_jump: float = 0.0,
                     pre_s: float = 2.0, post_s: float = 2.0) -> bool:
    """Does the demanded trajectory change CHARACTER discontinuously at t_ev?

    THE DIFFERENCE BETWEEN A STEP AND A KINK IS THE DIFFERENCE BETWEEN A
    MEASUREMENT AND A GUESS. A step (or the start/stop of a modulation) pins
    its own instant: shift the template by 20 ms and the fit gets measurably
    worse. A KINK -- an entry that only changes the SLOPE of a ramp already in
    flight, say 20% -> 16% over 15 s handing over to a hold -- does not, and
    the lag that best fits it is dominated by whatever else moved in the
    window.

    MEASURED on bit-perfect full-length renders, all of them kinks:
      35_wbtb_lucid_gamma  t=106000  "fired 3035 ms LATE"  on four zones
      04_meditation_theta  t=1000    "fired 306 ms LATE"   on four zones
    against a step on tests/selftest.ledc whose injected 180 ms came back as
    187.5 ms. So the character of the demanded change decides whether its time
    is graded at all; when it is not, the entry is reported as
    EVENT_NOT_OBSERVABLE with that reason, and its VALUES are still graded.

    "Character" covers two things, because a `~a:b:period` modulation changes
    the second without changing the first: the demanded VALUE across the event,
    and the demanded peak-to-peak (the modulation depth) across it.

    `min_jump` IS NOT OPTIONAL IN SPIRIT EITHER. Brightness, duty and volume are
    integer percentages on the device, so a SLOW RAMP is a STAIRCASE: an 8% ->
    0% brightness fade over 180 s steps by exactly one point every 22 s, and
    every one of those steps looks, locally, like a step an entry made. Relative
    tests cannot separate them -- the step IS the whole variation in a short
    window, so it scores 1.0 -- but an absolute one can: a change smaller than
    one quantization step of the field cannot be an entry's doing. MEASURED on a
    clean full-length render of 11_lucid_wbtb, whose t=541000 entry starts that
    fade: all four zones reported EVENT_NOT_LOCATED, matched against the
    staircase step that happens to coincide with the entry.
    """
    y = np.asarray(y_dem, dtype=np.float64)
    if y.size < 4 or t_dem.size != y.size:
        return False
    def seg(a, b):
        i0 = int(np.searchsorted(t_dem, a))
        i1 = int(np.searchsorted(t_dem, b))
        return y[i0:max(i1, i0 + 1)]
    # THE TWO SEGMENTS MUST NOT CROSS A NEIGHBOURING ENTRY. A fixed +/-2 s
    # window around the first body entry of a marker-led session reaches back
    # into the marker's own 100%-brightness burst, and a line fitted through
    # "100, 100, ..., 0, 0" extrapolates to a large value: every `>0` ramp-in
    # came out "sharp" for that reason alone. MEASURED on a clean marker render
    # of 07_genus_40hz: four EVENT_LATE errors at t=1000, 114-125 ms, plus the
    # EVENT_COMMON_OFFSET they triggered between them.
    pre = seg(t_ev - max(0.15, pre_s), t_ev - 0.1)
    post = seg(t_ev + 0.1, t_ev + max(0.15, post_s))
    win = seg(lo, hi)
    if pre.size < 2 or post.size < 2 or win.size < 4:
        return False
    span = float(np.nanmax(win) - np.nanmin(win))
    if not (span > 0):
        return False
    def _dep(seg_y):
        """Peak-to-peak with the local SLOPE removed.

        A ramp is not a modulation. Taking the raw peak-to-peak made the START
        of every `>0` brightness ramp-in look like a depth change -- "the
        modulation went from 0 to 2 percentage points" -- when all that happened
        is that a straight line started. MEASURED on clean renders:
        04_meditation_theta "fired 327 ms LATE" and 07_genus_40hz "fired 114 ms
        LATE" on all four zones, both on their first entry, both ramp-ins.
        """
        if seg_y.size < 3:
            return 0.0
        idx = np.arange(seg_y.size, dtype=np.float64)
        try:
            sl, ic = np.polyfit(idx, seg_y, 1)
        except (ValueError, np.linalg.LinAlgError):
            return float(np.nanmax(seg_y) - np.nanmin(seg_y))
        d = seg_y - (sl * idx + ic)
        return float(np.nanmax(d) - np.nanmin(d))

    def _at_edge_value(seg_y, forward: bool) -> float:
        """Linear extrapolation of a segment to the event instant.

        THE VALUE JUMP THAT MATTERS IS THE DISCONTINUITY, not the difference of
        the two medians. Over +/-2 s a ramp-in from zero moves its median by
        several points purely because it is a ramp, and taking that as a step
        made the first entry of every shipped session "sharp": MEASURED
        "fired 327 ms LATE" (04_meditation_theta) and "114 ms LATE"
        (07_genus_40hz) on all four zones of clean recordings, both on a `>0`
        brightness ramp-in. Extrapolating each side's own line to t_ev and
        differencing gives zero for anything CONTINUOUS -- a ramp start, a
        ramp end, a slope change -- and the true step height for a step.
        """
        if seg_y.size < 3:
            return float(robust_median(seg_y)) if seg_y.size else 0.0
        idx = np.arange(seg_y.size, dtype=np.float64)
        try:
            sl, ic = np.polyfit(idx, seg_y, 1)
        except (ValueError, np.linalg.LinAlgError):
            return float(robust_median(seg_y))
        # pre is the segment ENDING just before t_ev, post the one STARTING
        # just after it, so extrapolate to the near end plus the 0.1 s gap.
        gap = 0.1 / max(float(t_dem[1] - t_dem[0]), 1e-9) if t_dem.size > 1 else 2.0
        x = (seg_y.size - 1) + gap if forward else -gap
        return float(sl * x + ic)

    jump_val = abs(_at_edge_value(post, False) - _at_edge_value(pre, True))
    jump_dep = abs(_dep(post) - _dep(pre))
    return (max(jump_val, jump_dep) > _TRAJ_SHARP_FRAC * span
            and max(jump_val, jump_dep) > min_jump)


def _traj_realise(t_obs: np.ndarray, y_obs: np.ndarray,
                  t_dem: np.ndarray, y_dem: np.ndarray,
                  pred: float, t0: float, k: float,
                  w_b: float, w_a: float, gaps: tuple[float, float],
                  grid_dt: float, y_floor: float | None = None,
                  b_expect: float = float("nan"), min_jump: float = 0.0
                  ) -> tuple[float | None, float, bool, float]:
    """Realisation time of an entry, by shifting its demanded trajectory.

    Returns (t_realised, quality, at_edge). `t_realised` is None when the
    demanded trajectory carries no timing information in this window (a plain
    continuing ramp, or a field that does not change), and NaN when a feature IS
    present but the best lag is at the edge of the search.
    """
    prv, nxt = gaps
    ctx_b = min(max(_TRAJ_CONTEXT_S, w_b), 0.9 * prv if math.isfinite(prv) else 30.0, 30.0)
    ctx_a = min(max(_TRAJ_CONTEXT_S, w_a), 0.9 * nxt if math.isfinite(nxt) else 30.0, 30.0)
    lo = pred - w_b - ctx_b
    hi = pred + w_a + ctx_a
    i0 = int(np.searchsorted(t_obs, lo))
    i1 = int(np.searchsorted(t_obs, hi))
    if i1 - i0 < 16:
        return None, 0.0, False, 0.0
    ts = np.asarray(t_obs[i0:i1], dtype=np.float64)
    ys = np.asarray(y_obs, dtype=np.float64)[i0:i1]
    if y_floor is not None:
        # A TEMPLATE CANNOT BE MATCHED WHERE THE DEVICE EMITS NOTHING. The last
        # entry of every shipped session takes brightness to 0, and the entry
        # before it is a 30-240 s ramp down to 0 -- so the optical signal has
        # already vanished into the sensor floor well before the nominal
        # deadline, and matching the template's corner-at-zero dated the entry
        # by when the RECORDING ran out of photons. MEASURED on bit-perfect
        # full-length renders: "fired 5398 ms EARLY", "3085 ms EARLY" and
        # similar, on all four zones of 04_meditation_theta,
        # 15_ganzflicker_imagery, 18_jhana_absorption,
        # 21_gateway_focus12_SINE/TRAP and 25_gateway_journey_TRAP. Blanking the
        # sub-floor part of the window leaves the fit with only the observable
        # stretch, which for a pure fade to zero has no corner in it at all --
        # and "this entry's timing is not observable" is the true answer there.
        keep = np.interp((ts - t0) / k, t_dem,
                         (np.asarray(y_dem, dtype=np.float64) >= y_floor
                          ).astype(np.float64), left=0.0, right=0.0) > 0.5
        # DILATED by the lag range, so a step DOWN to zero keeps the samples
        # that carry its falling edge. Without that, a `bright 0` entry had its
        # whole post-event side blanked, the surviving template was flat, and
        # the entry read as "changes nothing a recording can see".
        dil = int(round((w_b + w_a) / max(grid_dt, 1e-9)))
        if dil > 0 and keep.any():
            k2 = keep.copy()
            for sft in range(1, dil + 1):
                k2[sft:] |= keep[:-sft]
                k2[:-sft] |= keep[sft:]
            keep = k2
        if keep.sum() < 16:
            return None, 0.0, False, 0.0
        ts, ys = ts[keep], ys[keep]

    fix = _grid_phase_fix(t_dem, (pred - t0) * k)
    t_dem_adj = t_dem + fix

    def tmpl(tt: np.ndarray, lag: float) -> np.ndarray:
        return np.interp((tt - lag - t0) / k, t_dem_adj, y_dem,
                         left=y_dem[0], right=y_dem[-1])

    step = max(grid_dt, (w_b + w_a) / 400.0)
    lag, q, localised, at_edge, sigma, info, b_fit, _sig_jack = _shift_fit(
        ts, ys, tmpl, -w_b, w_a, step)
    sharp = _demand_is_sharp(t_dem, np.asarray(y_dem, dtype=np.float64),
                             (pred - t0) * k, (lo - t0) * k, (hi - t0) * k,
                             min_jump=min_jump,
                             pre_s=min(2.0, 0.9 * prv) if math.isfinite(prv)
                             else 2.0,
                             post_s=min(2.0, 0.9 * nxt) if math.isfinite(nxt)
                             else 2.0)
    # IS THE DEMANDED CHANGE THERE AT ALL? The fit's own gain answers that, and
    # it is the only thing that can: "this entry changes nothing a recording
    # can see" and "the change this entry demands is NOT in the recording" look
    # identical to a goodness-of-fit test, and the second is a fault. b is the
    # measured size of the demanded trajectory in recorder units, so comparing
    # it with the gain the session's own calibration implies separates them.
    # MEASURED on tests/selftest.ledc with --fault drop:30000: the dropped
    # audio volume step fitted b ~ 0 where a goodness-of-fit test saw only
    # "the template explains little of what the recording does".
    # ...and the ABSENCE test needs the demanded change to be SHARP. On a
    # gradual change the fitted gain is dominated by whatever else moves in the
    # window, so a small b says nothing: MEASURED on a clean full-length render
    # of 05_focus_smr, whose t=1621000 entry starts a slow fade, "the audio
    # level change this entry demands is NOT in the recording".
    # A GRADUAL change gets a much stricter bar, because on one the fitted gain
    # is partly driven by whatever else moves in the window: a third of the
    # calibrated gain accused a clean 05_focus_smr fade of being absent, while a
    # TENTH of it is 20 dB down and cannot be anything but silence.
    frac = _TRAJ_ABSENT_FRAC if sharp else _TRAJ_ABSENT_FRAC_SOFT
    if (math.isfinite(b_expect) and b_expect > 0 and info >= _TRAJ_INFO_MIN
            and b_fit < frac * b_expect):
        return float("nan"), q, True, 0.0
    if info < _TRAJ_INFO_MIN:
        # The demanded trajectory carries no timing information here.
        return None, q, False, 0.0
    if at_edge:
        # The feature is there but further out than the search could reach --
        # provided it is the kind of feature whose position means anything AND
        # the match is a good one.
        return ((float("nan"), q, True, 0.0)
                if (sharp and q >= _TRAJ_MEASURE_Q_MIN)
                else (None, q, False, 0.0))  # noqa: E501
    if not sharp:
        return None, q, False, 0.0
    if q < _TRAJ_MEASURE_Q_MIN or not math.isfinite(lag) or not localised:
        # The change IS there (b cleared the bar above) but its time cannot be
        # pinned from this recording. Not a fault, and not a pass either: the
        # caller says so and the value checks still grade the values.
        return None, q, False, 0.0
    return pred + lag, q, False, sigma * 1000.0 * _SIGMA_K_SHARP


def _realise_led(exp: Expectation, obs: Observation, ev: dict, pred: float,
                 window: tuple[float, float], cfg: CompareConfig,
                 dim: bool = False, gains: dict | None = None
                 ) -> tuple[float | None, str, float]:
    """When did this LED entry actually become visible?

    `window` is (before_s, after_s), sized by _search_window so that any
    lateness the verdict would flag is inside the search. A fixed search is
    what made gross faults invisible while mild ones were caught.
    """
    ch = ev["channel"]
    O = obs.light[ch]
    L = exp.light.get(ch)
    kind = ev["kind"]
    # The first edge of a cold activation is held off until the anchor
    # (led_matrix_example.c:712), which `t_eff_ms` already includes; widen the
    # search anyway so the grace is in the SEARCH, not in the verdict.
    w_b = max(window[0], dm.LED_FIRST_EDGE_GRACE_MS / 1000.0)
    w_a = max(window[1], dm.LED_FIRST_EDGE_GRACE_MS / 1000.0)

    # WHEN THE DEADLINE ITSELF IS DARK, the optical-edge estimators cannot be
    # used: there are no photons to time at t_eff, and the first/last edge they
    # return belongs to wherever the ramp happened to clear the sensor floor.
    # MEASURED on 20_gateway_focus10, whose close is a 240 s brightness ramp to
    # zero: "the entry at t=1801000 ms fired 3085 ms EARLY (last optical edge
    # before the dark period)" on all four zones of a clean recording. The
    # trajectory match below grades the ramp's SHAPE instead, which is the only
    # thing that is actually observable there.
    if kind in ("led_start", "led_stop") and not dim:
        if kind == "led_start":
            r = O.edges_rise
            if r.size == 0:
                return float("nan"), "no optical edges at all on this channel", 0.0
            k = int(np.searchsorted(r, pred - w_b))
            if k >= r.size or r[k] > pred + w_a:
                return (float("nan"),
                        "no first optical edge near the predicted time", 0.0)
            return float(r[k]), "first optical edge", 0.0
        r = np.concatenate([O.edges_rise, O.edges_fall])
        r.sort()
        if r.size < 2:
            return None, "", 0.0
        # The stop instant is the start of the first long edge gap near pred.
        per = _period_at(exp, ch, max(0.0, ev["t_eff_ms"] / 1000.0 - 1.0))
        quiet = 5.0 * per if math.isfinite(per) else 0.5
        k = int(np.searchsorted(r, pred - w_b))
        for i in range(max(0, k - 2), r.size):
            if r[i] > pred + w_a:
                break
            nxt = r[i + 1] if i + 1 < r.size else np.inf
            if nxt - r[i] > quiet:
                return (float(r[i]),
                        "last optical edge before the dark period", 0.0)
        # NO BOUNDARY FOUND -- but did it stop at all? Those are different
        # answers and only one of them is a fault. A stop is VISIBLE as the
        # absence of the next edge, so in a noisy recording the boundary can be
        # unfindable (spurious edges keep filling the gap) while the channel is
        # plainly dark afterwards. MEASURED on tests/selftest.ledc with
        # --fault noise: two EVENT_NOT_LOCATED errors on a recording whose only
        # defect was broadband noise, on the entry that turns the LEDs off.
        w_chk = max(1.0, 5.0 * (quiet if math.isfinite(quiet) else 0.2))
        tr = O.t_rec
        j0 = int(np.searchsorted(tr, pred + w_a))
        j1 = int(np.searchsorted(tr, pred + w_a + w_chk))
        if j1 - j0 >= 4:
            after = np.nan_to_num(O.contrast[j0:j1], nan=0.0)
            if float(robust_median(after)) < cfg.dark_contrast:
                return (None,
                        "the channel IS dark after this stop, but no edge "
                        "boundary could be timed", 0.0)
        else:
            # The recording does not run far enough past this stop to see the
            # dark period it would be timed by. Reporting "it did not stop"
            # from a recording that ends there is a statement about the capture,
            # and RECORDING_INCOMPLETE already makes it.
            return (None,
                    "the recording ends too soon after this stop to confirm "
                    "it", 0.0)
        return float("nan"), "flicker did not stop near the predicted time", 0.0

    # led_update (and the dark-deadline start/stop cases): time the demanded
    # trajectory.
    if L is None:
        return None, "", 0.0
    i0 = int(np.searchsorted(L.t, ev["t_eff_ms"] / 1000.0))
    i_before = max(0, i0 - 2)
    i_after = min(L.t.size - 1, i0 + 2)
    # If the channel is stopped right after this update (the end-of-timeline
    # handler does exactly that to the final entry, config_parser.c:2104), the
    # update has no observable of its own: the dedicated led_stop event covers
    # the transition. Timing an "update" here would be timing the stop twice,
    # and with the second-rate estimator.
    if kind == "led_update" and not bool(L.active[i_after]):
        return None, "", 0.0
    # A RATE step first, because the edge-interval switch is sample-exact where
    # every level estimator is a 10-cycle rolling mean.
    v0 = float(L.emitted_hz[i_before])
    v1 = float(L.emitted_hz[i_after])
    # ...but only on a channel that was ALREADY RUNNING. `emitted_hz` retains
    # the last rate across a stop (the firmware does too,
    # led_matrix_example.c:1532-1545), so on a cold activation `v0` is the rate
    # of whatever ran before -- the 40 Hz sync marker, typically -- and the
    # edge-interval switch then dates the "40 -> 4 Hz change" at the first edge
    # the new, still-dim channel manages to produce. MEASURED on a clean render
    # of 20_gateway_focus10: "the entry at t=1000 ms fired 2751 ms LATE
    # (flicker-rate change, timed from the edge intervals)" on three zones.
    # ...and not when the deadline itself is dark, for the same reason the
    # rate and duty trajectories are dropped there: the edge intervals simply
    # STOP when the brightness ramp takes the LED below the sensor floor, which
    # on a session that fades to zero happens before the nominal stop. MEASURED
    # on a clean marker render of 01_sleep_onset: four EVENT_NOT_LOCATED errors
    # on its closing entry, noted as "flicker-rate change, timed from the edge
    # intervals".
    if (not dim and bool(L.active[i_before]) and v0 > 0 and v1 > 0
            and abs(v1 - v0) / max(abs(v0), 1e-9) >= 0.05):
        got, applicable = _edge_rate_step(O, pred, w_b, w_a, v0, v1)
        if applicable:
            return got, _NOTE_EDGE_SWITCH, 0.0
    # ...then the whole demanded TRAJECTORY of each level observable, shifted.
    # See _shift_fit for why this replaced the two-level step detector.
    gaps = _event_gaps(exp, ev)
    grid_dt = float(O.t_rec[1] - O.t_rec[0]) if O.t_rec.size > 1 else 0.05
    # Recorder time of DEVICE t=0 as this prediction sees it, including the LED
    # edge delay, so the template's time axis is the expectation's own.
    t0_eff = pred - ev["t_eff_ms"] / 1000.0
    best = None
    # EVERY demanded series is forced to ZERO while the channel is inactive.
    # `emitted_hz` / `duty_pct` / `bright_pct` all RETAIN their last value
    # across a stop, because the firmware does (led_matrix_example.c:1532-1545)
    # and a later '-' column reads them back -- but as a TEMPLATE for what a
    # sensor sees, a retained rate on a dark channel is a lie. Left in, the
    # cold activation at t=1000 of 20_gateway_focus10 was matched as a
    # "40 -> 4 Hz change" (40 Hz being the sync marker's retained rate) and
    # dated at the first edge the still-dim body produced: "fired 2600 ms LATE".
    fields = [("brightness",
               np.where(L.active, np.nan_to_num(L.bright_pct), 0.0), O.level)]
    if kind != "led_start" and not dim:
        # A FRESH activation has no previous rate or duty to change FROM, so
        # those two observables carry no information about its timing -- and
        # neither do they when the deadline itself is dark: the rate and duty
        # series simply STOP when the brightness ramp takes the LED below the
        # sensor floor, which is several seconds before the nominal end of a
        # 240 s fade. MEASURED on a clean render of 20_gateway_focus10: "the
        # entry at t=1801000 ms fired 5398 ms EARLY (flicker rate trajectory
        # match)" on all four zones. Only the brightness profile is observable
        # there, and it is what the trajectory match is left with.
        fields += [
            ("flicker rate",
             np.where(L.active, np.nan_to_num(L.emitted_hz), 0.0), O.freq_hz),
            ("duty", np.where(L.active, np.nan_to_num(L.duty_pct), 0.0),
             O.duty_pct)]
    for name, y_dem, y_obs in fields:
        # The brightness template is in PERCENT and the observed level is on
        # the sensor's own scale, so the calibration converts one to the other.
        # Only brightness has a calibration; rate and duty are absolute.
        b_exp = float("nan")
        if name == "brightness" and gains:
            gv = gains.get(("led", ch), float("nan"))
            if math.isfinite(gv):
                b_exp = gv / 100.0
        got, q, at_edge, sig = _traj_realise(
            O.t_rec, np.asarray(y_obs), L.t, np.asarray(y_dem, dtype=np.float64),
            pred, t0_eff, 1.0, w_b, w_a, gaps, grid_dt,
            y_floor=(_EDGE_MIN_BRIGHT_PCT if name == "brightness" else None),
            b_expect=b_exp,
            min_jump=(_SHARP_MIN_JUMP_PCT
                      if name in ("brightness", "duty") else 0.0))
        if got is None:
            continue
        note = (f"{name} trajectory match (fit explains {100 * q:.0f}% of the "
                f"observed variation; its own half-width is +/-{sig:.0f} ms and "
                f"is added to the budget below)")
        if at_edge:
            best = best or (float("nan"), note, 0.0)
            continue
        return got, note, sig
    if best is not None:
        return best
    return None, "", 0.0


def _realise_audio(exp: Expectation, obs: Observation, ev: dict, pred: float,
                   window: tuple[float, float], cfg: CompareConfig,
                   dim: bool = False, gains: dict | None = None
                   ) -> tuple[float | None, str, float]:
    A = obs.audio
    assert A is not None
    ch = ev["channel"]
    E = exp.audio.get(ch)
    span = max(4.0, 1.5 * max(window))
    # An ONSET is only an onset if there is something to hear at the deadline.
    # Every shipped session opens with `A ... >0`, a volume ramp from silence,
    # and timing that acoustically measures when the ramp cleared the noise
    # floor: MEASURED "the entry at t=1000 ms fired 5033 ms LATE (audio level
    # onset)" on a clean render of 20_gateway_focus10, whose ch1 ramps 0 -> 30
    # over 60 s. The trajectory match below grades the ramp instead.
    if ev["kind"] == "audio_start" and not dim:
        lt, ly = _audio_level_series(A, pred, span)
        onset, _sharp = _onset_from_series(lt, ly, pred, window)
        return onset, "audio level onset", 0.0
    if E is None:
        return None, "", 0.0
    gaps = _event_gaps(exp, ev)
    t0_eff = pred - ev["t_eff_ms"] / 1000.0
    w_b, w_a = window
    # THE TEMPLATE IS THE WHOLE MIX, with only THIS slot shifted.
    #
    # A recording of the DAC carries every generator slot summed, so the level
    # near one slot's entry is dominated by whatever the others are doing --
    # and on the gateway sessions the others are mid-ramp. Matching a
    # single-slot step against the summed level is what produced three
    # "EVENT_EARLY ... fired 400 ms EARLY (audio level step)" errors (exactly
    # the search half-width) on a clean full-length render of
    # 21_gateway_focus12_SINE. Powers add, so the correct template is
    #     P_total(t, lag) = P_others(t) + P_this_slot(t - lag)
    # which is linear in the quantity the 1 ms mean-square series measures and
    # needs no assumption about the others at all.
    M = exp.mix
    # EVERY SLOT WHOSE ENTRY SHARES THIS TIMESTAMP MOVES TOGETHER. Dispatch is
    # per batch (config_parser.c:696), so a delay applies to the whole batch --
    # and the sessions use that: 20_gateway_focus10 fades FIVE generator slots
    # in from one t=181000, each on its own line. Shifting only the slot the
    # event belongs to models the other four as stationary, which no lag can
    # fit, so the identifiability collapsed and `--fault late:181000:4000`
    # produced nothing at all.
    t_ev_s = ev["t_eff_ms"] / 1000.0
    group = [ev["channel"]]
    for e2 in exp.events:
        if (not e2["kind"].startswith("led")
                and abs(e2["t_eff_ms"] / 1000.0 - t_ev_s) < 1e-3
                and e2["channel"] not in group):
            group.append(e2["channel"])
    p_slot_on_mix = np.zeros(M.t.size)
    for gch in group:
        Eg = exp.audio.get(gch)
        if Eg is None:
            continue
        pg = 0.5 * (np.nan_to_num(Eg.gain_l) ** 2 + np.nan_to_num(Eg.gain_r) ** 2)
        pg = np.where(Eg.active, pg, 0.0)
        p_slot_on_mix += np.interp(M.t, Eg.t, pg, left=0.0, right=0.0)
    p_total = np.nan_to_num(M.rms_rel) ** 2
    p_other = np.maximum(p_total - p_slot_on_mix, 0.0)
    lt, ly = _audio_level_series(A, pred, max(span, 8.0 + max(window)))
    # SMOOTH THE 1 ms MEAN-SQUARE SERIES BEFORE FITTING A TRAJECTORY TO IT.
    # A 1 ms box is a 44-sample average at 44.1 kHz, whose first null is at
    # 1002 Hz -- so the rectified ripple at twice the carrier passes almost
    # unattenuated for the low carriers the library favours: -1.5 dB at 200 Hz
    # for a 100 Hz tone. That ripple IS the variance of the series, so the
    # demanded trajectory explained only 3% of it and the fit was discarded:
    # MEASURED on 20_gateway_focus10 (carrier 100 Hz) with `--fault
    # late:181000:4000`, q = 0.030 and nothing reported. A symmetric 20 ms box
    # removes it without moving a step, since it shifts no feature's midpoint.
    grid_dt = float(lt[1] - lt[0]) if lt.size > 1 else 0.05
    nsm = int(round(0.020 / max(grid_dt, 1e-9)))
    if nsm >= 3 and ly.size > 3 * nsm:
        # ...and EDGE-PADDED, never zero-padded. `np.convolve(mode="same")`
        # pads with zeros, which drags the first and last nsm/2 samples of the
        # window toward 0 -- and the fit window reaches the ends of this series
        # by construction. On a ramp that flattens, the (high, flat) right end
        # is dragged down further than the (low) left end, so the template fits
        # better shifted EARLY: MEASURED on a clean 21_gateway_focus12_TRAP,
        # -272 ms at a 20 ms kernel growing to -450 ms at 600 ms, i.e. the
        # artefact scaled with the padding it came from.
        nsm += nsm % 2            # even -> symmetric half-widths below
        k = np.ones(nsm + 1, dtype=np.float64) / (nsm + 1)
        pad = nsm // 2
        ly = np.convolve(
            np.pad(np.asarray(ly, dtype=np.float64), pad, mode="edge"),
            k, mode="valid")

    prv, nxt = gaps
    ctx_b = min(max(_TRAJ_CONTEXT_S, w_b),
                0.9 * prv if math.isfinite(prv) else 30.0, 30.0)
    ctx_a = min(max(_TRAJ_CONTEXT_S, w_a),
                0.9 * nxt if math.isfinite(nxt) else 30.0, 30.0)
    i0 = int(np.searchsorted(lt, pred - w_b - ctx_b))
    i1 = int(np.searchsorted(lt, pred + w_a + ctx_a))
    if i1 - i0 >= 16:
        ts = np.asarray(lt[i0:i1], dtype=np.float64)
        ys = np.asarray(ly[i0:i1], dtype=np.float64)

        # The demanded series' grid rounded the breakpoint at this entry off;
        # put it back rather than correcting the whole axis by half a cell.
        t_mix, (p_other_s, p_slot_s) = _grid_sharpen(
            M.t, [p_other, p_slot_on_mix], ev["t_eff_ms"] / 1000.0)

        def tmpl(tt, lag):
            td = (tt - t0_eff)
            base = np.interp(td, t_mix, p_other_s,
                             left=p_other_s[0], right=p_other_s[-1])
            mine = np.interp(td - lag, t_mix, p_slot_s,
                             left=p_slot_s[0], right=p_slot_s[-1])
            return base + mine

        step = max(grid_dt, (w_b + w_a) / 400.0)
        lag, q, localised, at_edge, sigma, info, b_fit, sig_jack = _shift_fit(
            ts, ys, tmpl, -w_b, w_a, step)
        # The template is in POWER (amplitude^2/2) and `ly` is a mean-square
        # series, so the expected gain is the squared amplitude calibration.
        b_exp = float("nan")
        if gains and math.isfinite(gains.get("audio", float("nan"))):
            b_exp = float(gains["audio"]) ** 2
        # The sharpness test runs in the AMPLITUDE domain, not in power: the
        # quantization that makes a slow fade a staircase is one percent of
        # volume, which is a fixed 0.01 of amplitude and a level-dependent step
        # in power.
        sharp = _demand_is_sharp(
            t_mix, np.sqrt(2.0 * np.maximum(p_slot_s, 0.0)),
            pred - t0_eff, float(ts[0] - t0_eff), float(ts[-1] - t0_eff),
            min_jump=_SHARP_MIN_JUMP_AMP,
            pre_s=min(2.0, 0.9 * prv) if math.isfinite(prv) else 2.0,
            post_s=min(2.0, 0.9 * nxt) if math.isfinite(nxt) else 2.0)
        frac = _TRAJ_ABSENT_FRAC if sharp else _TRAJ_ABSENT_FRAC_SOFT
        if (math.isfinite(b_exp) and b_exp > 0 and info >= _TRAJ_INFO_MIN
                and b_fit < frac * b_exp):
            return (float("nan"),
                    "the audio level change this entry demands is NOT in the "
                    "recording (its measured size is under a third of what the "
                    "session's own level calibration says it should be)", 0.0)
        # THE SHARPNESS REQUIREMENT IS AN LED REQUIREMENT, NOT AN AUDIO ONE.
        # A corner is locatable when the series carrying it is well conditioned,
        # and the two domains are two orders of magnitude apart: the audio level
        # is a 1 kHz mean-square series with the full dynamic range of the mix,
        # while the optical level is a 20 Hz series of 10-cycle means off a
        # sensor whose useful range is the 8-45% brightness the photosensitivity
        # caps allow. MEASURED: the end of a 45 s volume ramp on 01_sleep_onset
        # is located to 7 ms, while the corner where a 20% -> 16% brightness
        # ramp flattens on 35_wbtb_lucid_gamma came out 3.1 s out. So audio
        # grades a kink and light does not -- and `sharp` is still required for
        # the ABSENCE test above in both domains, where a mis-fitted gain would
        # accuse the device of not playing something.
        # THE WIDER OF THE TWO HALF-WIDTHS. The SSE valley is the precision
        # the fit would have if its residual were white; the jackknife is the
        # precision it actually reproduces. Taking the larger is the only one
        # of the two that is not a claim the data cannot support.
        k_sig = 1.0 if sharp else 2.0
        sig_eff = max(sigma, sig_jack)
        if info >= _TRAJ_INFO_MIN and q >= _TRAJ_MEASURE_Q_MIN:
            if not math.isfinite(sig_eff):
                return None, "", 0.0
            localised = sig_eff <= _TRAJ_SIGMA_FRAC * max(w_b + w_a, 1e-9)
            if at_edge:
                # "The change is there, further out than the window reaches" is
                # a claim about LATENESS, and it is only available on the LATE
                # edge. A best fit pinned to the EARLY edge would mean the
                # entry fired seconds BEFORE its deadline, which the scheduler
                # cannot do (500 us of timer look-ahead, config_parser.c:1933)
                # -- so it is proof the fit is lost, not evidence of a fault.
                # MEASURED on two bit-perfect renders: 31_ganzfeld_amber's
                # t=1000 sat on the -400 ms edge (jackknife 1013 ms) and
                # 25_gateway_journey_TRAP's close-out at t=2401000, which has
                # 400 ms of recording after it and 8 s before, sat on the -8 s
                # edge. Reported as EVENT_MISSING and EVENT_NOT_LOCATED.
                # The edge must also be significant against the fit's own
                # half-width, for the same reason any other lag must be.
                # ...and it is not available at all when the entry's own job
                # is to SILENCE the slot. The side of the window that carries
                # lateness is then at the demanded zero, so "the change is
                # further out than the window reaches" reduces to "the level
                # has not died yet", which is a VALUE and belongs to
                # AUDIO_LEVEL_ABSENT / AMPLITUDE. MEASURED on a clean
                # 25_gateway_journey_TRAP: its close-out at t=2401000 ramps
                # five slots to volume 0, the fit ran to the +400 ms edge of a
                # window the budget had already closed to 400 ms, and that was
                # four EVENT_NOT_LOCATED errors on a bit-perfect render.
                a_post = float(np.sqrt(2.0 * max(0.0, float(np.interp(
                    ev["t_eff_ms"] / 1000.0 + 0.3, t_mix, p_slot_s)))))
                if a_post < _AUDIO_GRADE_GAIN:
                    return (None,
                            f"this entry takes the slot down to "
                            f"{100.0 * a_post:.1f}% of full scale, so the side "
                            f"of the window that would carry lateness is at "
                            f"the demanded silence -- the level trajectory "
                            f"cannot time it, and its VALUES are graded "
                            f"instead", 0.0)
                if lag > 0 and lag > k_sig * sig_eff:
                    return (float("nan"),
                            "audio level trajectory match (the best fit sits "
                            "at the edge of the search, so the change is there "
                            "but further out than this window can reach)", 0.0)
                return (None,
                        "the audio level trajectory match ran to the EARLY "
                        "edge of its search window, which no event can do -- "
                        "the fit does not locate this entry", 0.0)
            if not math.isfinite(lag) or not localised:
                return None, "", 0.0
            # TWO SIGMA WHEN THE FEATURE IS A CORNER, one when it is a step.
            # The fit's half-width is an honest statement about the SSE valley,
            # but on a corner the valley's shape is itself model-dependent and
            # the residual bias runs to about 1.6 sigma: MEASURED on a clean
            # full-length render of 20_gateway_focus10, whose ch1 volume ramps
            # 0 -> 30 over 60 s, the origin came back 2374 ms out with the fit
            # claiming +/-1470 ms. A step has no such ambiguity -- the 180 ms
            # injected into tests/selftest.ledc reads 187.5 ms against a 10 ms
            # half-width -- so it keeps the tighter budget and the sub-1%
            # sensitivity that goes with it.
            return (pred + lag,
                    f"audio level trajectory match (fit explains "
                    f"{100 * q:.0f}% of the observed variation; its own "
                    f"half-width is +/-{1000 * sig_eff:.0f} ms -- "
                    f"{1000 * sigma:.0f} ms from the SSE valley, "
                    f"{1000 * sig_jack:.0f} ms from leaving one sixth of the "
                    f"window out -- and {k_sig:.0f}x that is added to the "
                    f"budget below)",
                    k_sig * 1000.0 * sig_eff)
    i0e = int(np.searchsorted(E.t, ev["t_eff_ms"] / 1000.0))
    ib, ia = max(0, i0e - 2), min(E.t.size - 1, i0e + 2)
    # A CARRIER STEP IS ONLY TIMEABLE WHILE THERE IS A TONE. The session-closing
    # entry every shipped file ends with sets volume 0 and often a different
    # nominal carrier in the same line, and timing that step measured when the
    # FFT peak of silence happened to move: MEASURED two EVENT_NOT_LOCATED
    # errors on a clean render of 05_focus_smr, on its close-out.
    v0, v1 = float(E.freq_l_hz[ib]), float(E.freq_l_hz[ia])
    a0, a1 = float(E.amp[ib]), float(E.amp[ia])
    if (v0 > 0 and abs(v1 - v0) / v0 > 0.03
            and min(a0, a1) > _AUDIO_GRADE_GAIN):
        return (_time_step(obs.t_rec, A.tone_l_hz, pred, window,
                           rising=(v1 > v0)), "carrier frequency step", 0.0)
    return None, "", 0.0


def _time_step(t: np.ndarray, y: np.ndarray, pred: float,
               window: tuple[float, float], rising: bool) -> float:
    """Time a step in `y` inside the search window.

    Change-point first (see _change_point for why), half-crossing as a
    fallback. Both derive their levels from the OBSERVED series on both sides,
    so this works for quantities whose absolute scale is unknown (optical
    brightness, sensor gain). The cost is that it cannot tell "the step was the
    wrong SIZE" -- that is the value checks' job.

    The window is ASYMMETRIC (before, after). A late event needs a wide window
    AFTER the prediction, and widening both sides equally would hand the change
    point a long stretch of pre-event signal to find a spurious step in.
    """
    if t.size < 4:
        return float("nan")
    i0 = int(np.searchsorted(t, pred - window[0]))
    i1 = int(np.searchsorted(t, pred + window[1]))
    if i1 - i0 < 3:
        return float("nan")
    cp, a, b, ratio = _change_point(t, y, i0, i1, rising=rising)
    if math.isfinite(cp) and ratio < 0.6:
        return cp
    margin = max(1, int(round(0.3 / (t[1] - t[0]))))
    pre = y[max(0, i0 - margin):i0]
    post = y[i1:min(len(y), i1 + margin)]
    a = robust_median(pre)
    b = robust_median(post)
    if not (math.isfinite(a) and math.isfinite(b)) or a == b:
        return float("nan")
    if (b > a) != rising:
        return float("nan")
    return find_crossing(t, y, (a + b) / 2.0, i0, i1 + 1, rising=(b > a))


# ---------------------------------------------------------------------------
# Value checks
# ---------------------------------------------------------------------------


def _shift_envelope(y: np.ndarray, w: int) -> tuple[np.ndarray, np.ndarray]:
    """Running (min, max) of `y` over a +/-w-sample window, edge-extended."""
    lo = y.copy()
    hi = y.copy()
    for s in range(1, w + 1):
        lo[s:] = np.minimum(lo[s:], y[:-s])
        lo[:-s] = np.minimum(lo[:-s], y[s:])
        hi[s:] = np.maximum(hi[s:], y[:-s])
        hi[:-s] = np.maximum(hi[:-s], y[s:])
    return lo, hi


def _shift_envelope_fwd(y: np.ndarray, w: int) -> tuple[np.ndarray, np.ndarray]:
    """Running (min, max) of `y` over the FORWARD window [i, i+w].

    A centred window puts a finding w seconds past the instant that caused it,
    because the first sample whose whole window lies inside the mismatch is w
    samples in. A forward window puts it AT that instant, which is what a
    reader -- and the acceptance gate's +/-8 s placement tolerance -- needs.
    Windows that straddle the entry see the OLD behaviour in both series and so
    agree, which is why this does not fire early.
    """
    lo = y.copy()
    hi = y.copy()
    for s in range(1, w + 1):
        lo[:-s] = np.minimum(lo[:-s], y[s:])
        hi[:-s] = np.maximum(hi[:-s], y[s:])
    return lo, hi


def _profile_bad(obs_v: np.ndarray, pred: np.ndarray, grid_hz: float,
                 t0_unc_s: float, tol_db: float) -> np.ndarray:
    """Where the measured profile cannot be reconciled with the demanded one.

    A profile check compares the measured SHAPE against the demanded shape at
    the same device time. A t0 that is only known to +/-2 s therefore compares
    the recording against a part of the profile it was never meant to match,
    and no fixed dB tolerance can fix that: on tests/fadein.ledc (volume ramps
    0 -> 50 over 20 s, t0 genuinely ambiguous by seconds without a marker) the
    first 2 s of device time landed in the recording's lead-in SILENCE and the
    check reported "audio level was -53.1 dB from the expected profile" on a
    bit-perfect render.

    So the comparison is against the RANGE of demanded values the sync
    uncertainty allows, not against a single point: a measurement that any
    admissible t0 would explain is not evidence of anything. On a steady
    plateau the range collapses to the point value and this is exactly the old
    test; it only loosens where the profile is actually moving.
    """
    p = np.maximum(np.nan_to_num(np.asarray(pred, dtype=np.float64)), 1e-12)
    w = int(round(max(0.0, t0_unc_s) * grid_hz)) if grid_hz > 0 else 0
    w = min(w, int(5.0 * grid_hz) if grid_hz > 0 else 0)
    if w > 0:
        p_lo, p_hi = _shift_envelope(p, w)
    else:
        p_lo = p_hi = p
    o = np.maximum(np.nan_to_num(np.asarray(obs_v, dtype=np.float64)), 1e-12)
    with np.errstate(divide="ignore", invalid="ignore"):
        above = 20.0 * np.log10(o / p_hi)
        below = 20.0 * np.log10(o / p_lo)
    return ((np.nan_to_num(above, nan=0.0) > tol_db)
            | (np.nan_to_num(below, nan=0.0) < -tol_db))


def _report_runs(findings: list[Finding], code: str, severity: str, domain: str,
                 channel: int | None, t: np.ndarray, bad: np.ndarray,
                 expected: np.ndarray, observed: np.ndarray, unit: str,
                 min_run: int, msg_fn, detail: str = "") -> None:
    for s, e in aggregate_runs(bad, min_len=min_run):
        ex = robust_median(expected[s:e])
        ob = robust_median(observed[s:e])
        findings.append(Finding(
            code=code, severity=severity, domain=domain, channel=channel,
            t_ms=float(t[s] * 1000.0), t_end_ms=float(t[e - 1] * 1000.0),
            expected=ex, observed=ob, delta=ob - ex, unit=unit,
            message=msg_fn(t[s], t[e - 1], ex, ob), detail=detail))


def _check_light_values(exp: Expectation, obs: Observation, cfg: CompareConfig,
                        t0: float, ppm: float,
                        t0_unc_s: float = 0.0) -> list[Finding]:
    out: list[Finding] = []
    min_run = max(1, int(cfg.min_run_s * obs.grid_hz))
    k = 1.0 + (ppm * 1e-6 if math.isfinite(ppm) else 0.0)
    led_delay_s = dm.led_edge_delay_ms(exp.led_backend) / 1000.0

    for ch, L in sorted(exp.light.items()):
        O = obs.light.get(ch)
        if O is None:
            out.append(Finding(
                code="LIGHT_UNMAPPED", severity="info", domain="led", channel=ch,
                message=f"LED channel {ch} is driven by the .ledc but no sensor "
                        f"was mapped to it, so it was not checked."))
            continue
        t_dev = O.t_rec - t0
        dt_L = float(L.t[1] - L.t[0]) if L.t.size > 1 else 0.05
        act = np.interp(t_dev, L.t, L.active.astype(float), left=0.0, right=0.0) > 0.99
        e_freq = np.interp(t_dev, L.t, L.emitted_hz, left=np.nan, right=np.nan) / k
        e_duty = np.interp(t_dev, L.t, L.duty_pct, left=np.nan, right=np.nan)
        e_brt = np.interp(t_dev, L.t, L.bright_pct, left=np.nan, right=np.nan)
        e_env = np.interp(t_dev, L.t, L.env.astype(float), left=0.0, right=0.0)

        # --- never turned on -------------------------------------------
        # Not blacked out: "never on" is about the whole expected-active span,
        # not about a windowed estimate near a transition.
        #
        # `O.available` is the front-end's own absolute verdict ("this sensor's
        # raw peak-to-peak never cleared its noise floor anywhere in the
        # file"), and it is checked FIRST so the finding can fire even when
        # EVERY mapped channel is dark -- the case the relative contrast test
        # structurally could not see, and the single most likely outcome of a
        # first real recording (a photodiode left unplugged, an LED supply off,
        # a sensor facing the wrong way).
        _never_on_detail = (
            f"Check the physical channel map (mask bit {ch - 1}), the wiring, "
            f"and that the sensor faces this zone. Mask bits 4..7 light nothing "
            f"unless the runtime channel map assigns pixels to them "
            f"(Kconfig.projbuild:192-209).")
        if not O.available:
            # Deliberately computed in DEVICE time off the expectation alone,
            # with no reference to t0: when a sensor saw nothing, t0 is itself a
            # guess, and this finding must not depend on it.
            drive = L.active & (np.nan_to_num(L.bright_pct, nan=0.0) > 1.0)
            if drive.any():
                di = np.nonzero(drive)[0]
                out.append(Finding(
                    code="CHANNEL_NEVER_ON", severity="error", domain="led",
                    channel=ch, t_ms=float(L.t[di[0]] * 1000.0),
                    t_end_ms=float(L.t[di[-1]] * 1000.0),
                    expected=float(robust_median(L.bright_pct[drive])),
                    observed=0.0, unit="%",
                    message=f"LED channel {ch} never turned on: the .ledc asks "
                            f"for it from t={L.t[di[0]]:.1f} s to "
                            f"t={L.t[di[-1]]:.1f} s, and this sensor's raw "
                            f"peak-to-peak NEVER cleared its noise floor "
                            f"anywhere in the recording -- not dim, absent.",
                    detail=O.note + "  " + _never_on_detail))
                continue
        want = act & (e_brt > 1.0)
        if want.sum() > obs.grid_hz:          # at least a second of expected light
            dark = want & (np.nan_to_num(O.contrast, nan=0.0) < cfg.dark_contrast)
            if dark.sum() >= 0.9 * want.sum():
                idx = np.nonzero(want)[0]
                out.append(Finding(
                    code="CHANNEL_NEVER_ON", severity="error", domain="led",
                    channel=ch, t_ms=float(t_dev[idx[0]] * 1000.0),
                    t_end_ms=float(t_dev[idx[-1]] * 1000.0),
                    expected=float(robust_median(e_brt[want])), observed=0.0,
                    unit="%",
                    message=f"LED channel {ch} never turned on: the .ledc asks "
                            f"for it from t={t_dev[idx[0]]:.1f} s to "
                            f"t={t_dev[idx[-1]]:.1f} s but the sensor saw no "
                            f"modulation above the noise floor at any point.",
                    detail=_never_on_detail))
                continue

        # --- demanded bright, observed dark ----------------------------
        # NOT the same test as CHANNEL_NEVER_ON, which is about the WHOLE
        # session. This one catches a zone that is dark for a SPAN while the
        # .ledc demands measurable light: a dropped activation, a zone that
        # dies part-way, an entry that never executed. It is scale-free --
        # `contrast` is the normalised per-cycle modulation depth, so no sensor
        # gain, AC-coupling or baseline drift can produce it -- which is why it
        # earns error severity where the BRIGHTNESS profile check only earns a
        # warning.
        #
        # MEASURED: `--fault drop:{first body entry}` on 20_gateway_focus10
        # removes the LED ramp-in and the audio ramp-ins, so the zones stay
        # dark until the next entry 20 s later. Round 2 reported no LED finding
        # at all for that -- "the errors raised (TONE_FREQ, BEAT_*) are
        # unrelated".
        want_lit = act & np.isfinite(e_brt) & (e_brt >= cfg.min_bright_pct)
        if want_lit.any():
            dark_run = want_lit & (np.nan_to_num(O.contrast, nan=0.0)
                                   < cfg.dark_contrast)
            _report_runs(
                out, "LIGHT_DARK_WHEN_DEMANDED", "error", "led", ch, t_dev,
                dark_run, e_brt, np.zeros_like(e_brt), "%",
                max(min_run, int(2.0 * obs.grid_hz)),
                lambda a, b, ex, ob: (
                    f"LED channel {ch} was DARK from t={a:.1f} s to t={b:.1f} s "
                    f"while the .ledc demands {ex:.0f}% brightness there: the "
                    f"sensor saw no per-cycle modulation above its noise floor "
                    f"at all."),
                detail="Measured from the NORMALISED modulation depth, so no "
                       "sensor gain, ambient level or AC-coupling can account "
                       "for it. Either the entry that should have lit this zone "
                       "never executed, or the zone is physically dark.")

        # The rate estimator averages ~10 flicker cycles, so it is invalid
        # across a rate change. Blackout half-width = 10 periods of the slowest
        # rate this channel uses.
        #
        # `slowest` MUST come from the expectation's own samples, not from the
        # interpolated series: np.interp across a step (50 Hz marker -> 0 ->
        # 10 Hz body) manufactures intermediate values, and one spurious
        # 0.1 Hz sample made the half-width 99 s, which blacked out the entire
        # session and hid a real 10-vs-20 Hz error everywhere except a 10 s
        # window. Silent loss of coverage is the worst failure mode an
        # instrument can have, because nothing in the output hints at it.
        on = L.active & (L.emitted_hz > 0.05)
        slowest = float(L.emitted_hz[on].min()) if on.any() else float("nan")
        half = (10.0 / slowest) if math.isfinite(slowest) and slowest > 0 else 1.0
        # Cap it: past a few seconds a blackout costs more coverage than the
        # false positives it prevents.
        bo = _blackout(t_dev, exp.events, min(half, 3.0), ("led",))

        # Below the brightness floor there is not enough optical signal for the
        # edge detector to be trusted (see CompareConfig.min_bright_pct). Report
        # the lost coverage instead of grading through it.
        measurable = np.nan_to_num(e_brt, nan=0.0) >= cfg.min_bright_pct
        ungraded = act & ~measurable & np.isfinite(e_freq) & (e_freq > 0)
        if ungraded.sum() > obs.grid_hz:
            idx = np.nonzero(ungraded)[0]
            out.append(Finding(
                code="LIGHT_NOT_GRADED", severity="info", domain="led",
                channel=ch, t_ms=float(t_dev[idx[0]] * 1000.0),
                t_end_ms=float(t_dev[idx[-1]] * 1000.0),
                expected=float(cfg.min_bright_pct), unit="%",
                message=f"LED channel {ch}: {ungraded.sum() / obs.grid_hz:.0f} s "
                        f"was NOT graded because the .ledc asks for less than "
                        f"{cfg.min_bright_pct:.0f}% brightness there (fade-in / "
                        f"fade-out), which is below what a photodiode envelope "
                        f"can resolve into edges.",
                detail="Rate and duty are unmeasurable at low brightness: the "
                       "detector misses alternate edges and reports a "
                       "submultiple of the true rate. Not a device fault."))

        valid = act & np.isfinite(e_freq) & (e_freq > 0) & ~bo & measurable

        # --- flicker rate ----------------------------------------------
        # TWO ESTIMATORS, EACH DOING ONLY WHAT IT IS GOOD AT.
        #
        # (1) THE EDGE-GRID FIT is the primary one. It fits `rise_k = A + k*T`
        #     over every rise in a span whose DEMANDED rate is constant, so it
        #     uses hundreds of edges, is good to tens of ppm, and -- unlike a
        #     rolling mean of intervals -- is not biased by the LED's PWM beat.
        #     That precision is what the brief's +/-0.01 Hz specification asks
        #     for, and it is what makes a sub-1% error reportable at all.
        #
        # (2) THE SMOOTHED RATE SERIES stays, but only as a GROSS detector
        #     (>15%). It is a 10-cycle rolling mean, so it cannot resolve 0.5%,
        #     and it is the estimator the one-sided dead band was bolted onto:
        #     `bad &= ~((rel < 0) & (|rel| < 2 * flicker_rel))` threw away the
        #     whole SLOW side up to 1.0%, which is how `--fault
        #     light-freq:1:0.991` (a 40 Hz zone emitting 39.643 Hz, 36x the
        #     stated spec) was measured correctly and then reported as nothing
        #     at all, exit 0. The dead band is gone; the series is simply not
        #     asked the question it cannot answer. It still catches a half- or
        #     double-rate zone on spans the grid fit declines (a rate ramp).
        gm = _edge_time_mask(L, cfg, dt_L)
        graded = np.interp(t_dev, L.t, gm.astype(float),
                           left=0.0, right=0.0) > 0.5
        for s_i, e_i in aggregate_runs(gm, min_len=int(max(2, obs.grid_hz))):
            f_dem = float(robust_median(L.emitted_hz[s_i:e_i]))
            if not (f_dem > 0) or not math.isfinite(f_dem):
                continue
            f_exp = f_dem / k
            per = 1.0 / f_dem
            lo_r = t0 + float(L.t[s_i]) + led_delay_s + per
            hi_r = t0 + float(L.t[e_i - 1]) + led_delay_s - per
            r = O.edges_rise
            sel = r[(r >= lo_r) & (r <= hi_r)]
            if sel.size < 20:
                continue
            A_f, T_f, lock_f, se_T = _fit_edge_grid(sel, per)
            if not math.isfinite(T_f) or not (T_f > 0) or lock_f < 0.9:
                continue
            f_obs = 1.0 / T_f
            rel_g = (f_obs - f_exp) / f_exp
            # The fit's OWN uncertainty, three sigma, on top of the threshold.
            tol_g = cfg.flicker_rel
            if math.isfinite(se_T) and se_T > 0:
                tol_g += 3.0 * se_T / T_f
            if abs(rel_g) > tol_g:
                out.append(Finding(
                    code="FLICKER_RATE", severity="error", domain="led",
                    channel=ch, t_ms=float(L.t[s_i] * 1000.0),
                    t_end_ms=float(L.t[e_i - 1] * 1000.0),
                    expected=f_exp, observed=f_obs, delta=f_obs - f_exp,
                    unit="Hz",
                    message=f"LED channel {ch} flickered at {f_obs:.4f} Hz "
                            f"where {f_exp:.4f} Hz was expected "
                            f"({100.0 * rel_g:+.2f}%) from "
                            f"t={L.t[s_i]:.1f} s to t={L.t[e_i - 1]:.1f} s.",
                    detail=f"fitted from {sel.size} optical rises over "
                           f"{hi_r - lo_r:.0f} s (phase lock {lock_f:.3f}, "
                           f"period {T_f * 1000:.4f} ms +/-{se_T * 1e6:.1f} us, "
                           f"so the fit resolves {3e6 * se_T / T_f:.0f} ppm and "
                           f"this reading is {abs(rel_g) / max(tol_g, 1e-12):.1f}x "
                           f"the reporting threshold). `expected` is the rate "
                           f"the firmware can actually emit after its cycle "
                           f"re-anchoring quantization "
                           f"(led_matrix_example.c:891), divided by the fitted "
                           f"recorder clock, not the raw .ledc value."))
        m = valid & np.isfinite(O.freq_hz) & (O.freq_hz > 0)
        if m.any():
            rel = np.zeros(m.shape)
            rel[m] = (O.freq_hz[m] - e_freq[m]) / e_freq[m]
            # While the rate RAMPS, the ~10-cycle averaging estimator lags the
            # true value. That lag is the instrument's, not the device's, so it
            # widens the tolerance here instead of becoming a finding.
            dfdt = np.abs(np.gradient(e_freq, 1.0 / obs.grid_hz))
            lag_s = 5.0 / np.maximum(e_freq, 0.05)
            tol = _GROSS_RATE_REL + np.nan_to_num(
                dfdt * lag_s / np.maximum(e_freq, 0.05), nan=0.0)
            bad = m & (np.abs(rel) > tol)
            # Spans the grid fit already judged are not judged twice, and the
            # grid fit is the better instrument wherever it applies.
            bad &= ~graded
            _report_runs(
                out, "FLICKER_RATE", "error", "led", ch, t_dev, bad,
                e_freq, O.freq_hz, "Hz", min_run,
                lambda a, b, ex, ob: (
                    f"LED channel {ch} flickered at {ob:.3f} Hz where "
                    f"{ex:.3f} Hz was expected "
                    f"({100.0 * (ob - ex) / ex:+.2f}%) from t={a:.1f} s to "
                    f"t={b:.1f} s."),
                detail="measured from a 10-cycle rolling mean of the edge "
                       "intervals, which is why only a GROSS error "
                       f"(>{100 * _GROSS_RATE_REL:.0f}%) is reported from it. "
                       "Spans whose demanded rate holds still are graded far "
                       "more tightly by the edge-grid fit instead. `expected` "
                       "is the rate the firmware can actually emit after its "
                       "cycle re-anchoring quantization "
                       "(led_matrix_example.c:891), not the raw .ledc value.")

        # --- duty (square carrier only) --------------------------------
        # DUTY IS NOT GRADED WHILE THE RATE IS MOVING. `ObservedLight.duty_pct`
        # is on-time divided by PERIOD, both from a 10-cycle rolling mean, so
        # while the demanded rate ramps the two averages are over different
        # rates and the ratio is biased. MEASURED on a clean marker render of
        # 04_meditation_theta, 174 s into its 10 -> 6 Hz glide: "duty was 40%
        # where 50% was expected" on all four zones -- one of this tool's own
        # cry-wolf regression sessions. `graded` is the same rate-steady mask
        # the edge-grid rate fit uses.
        sq = (valid & graded & (e_env < 0.5) & np.isfinite(O.duty_pct)
              & np.isfinite(e_duty))
        if sq.any():
            bad = sq & (np.abs(O.duty_pct - e_duty) > cfg.duty_pct)
            # SEVERITY IS GRADED, and that is a deliberate compromise between
            # two failure modes the earlier rounds hit in turn.
            #
            # Duty comes from the optical EDGE PATTERN -- the ratio of on-time
            # to period -- so unlike brightness it has no sensor-gain,
            # ambient-light or AC-coupling excuse, and round 2 showed that
            # leaving it a warning lets a device emitting HALF the prescribed
            # on-time (25% against 50%) exit 0.
            #
            # But the estimator does have a transient: round 2 also measured
            # "duty was 40% where 50% was expected" over a 3.7 s window at the
            # end of a trimmed 04_meditation_theta -- a 10-point reading on a
            # clean recording. So 8 points (the documented threshold) stays a
            # warning and twice that is an error: a 2:1 duty error is 25 points
            # and lands well clear of it.
            big = bad & (np.abs(O.duty_pct - e_duty) > 2.0 * cfg.duty_pct)
            _report_runs(
                out, "DUTY", "error", "led", ch, t_dev, big, e_duty,
                O.duty_pct, "%", min_run,
                lambda a, b, ex, ob: (
                    f"LED channel {ch} duty was {ob:.0f}% where {ex:.0f}% was "
                    f"expected, from t={a:.1f} s to t={b:.1f} s -- more than "
                    f"twice the reporting threshold, which no estimator "
                    f"transient accounts for."))
            _report_runs(
                out, "DUTY", "warning", "led", ch, t_dev, bad & ~big, e_duty,
                O.duty_pct, "%", min_run,
                lambda a, b, ex, ob: (
                    f"LED channel {ch} duty was {ob:.0f}% where {ex:.0f}% was "
                    f"expected, from t={a:.1f} s to t={b:.1f} s."))
        smooth = valid & (e_env >= 0.5)
        if smooth.any() and not sq.any():
            out.append(Finding(
                code="DUTY_NOT_CHECKED", severity="info", domain="led", channel=ch,
                message=f"LED channel {ch} uses a smooth carrier (env>0), where "
                        f"the firmware ignores duty for sine and triangle "
                        f"(led_matrix_example.c:608-645), so duty was not checked."))

        # --- brightness, on a calibrated relative scale -----------------
        bm = valid & np.isfinite(O.level) & np.isfinite(e_brt) & (e_brt > 2.0)
        if bm.sum() >= min_run * 2:
            # The sensor's absolute gain is unknown (distance, ambient light,
            # preamp), so fit ONE scale factor per channel and then look only at
            # the SHAPE. Reporting absolute brightness would be meaningless.
            scale = robust_median(O.level[bm] / (e_brt[bm] / 100.0))
            if math.isfinite(scale) and scale > 0:
                pred = (e_brt / 100.0) * scale
                with np.errstate(divide="ignore", invalid="ignore"):
                    db = 20.0 * np.log10(np.maximum(O.level, 1e-6)
                                         / np.maximum(pred, 1e-6))
                bad = bm & np.isfinite(db) & _profile_bad(
                    O.level, pred, obs.grid_hz, t0_unc_s, cfg.amp_db)
                _report_runs(
                    out, "BRIGHTNESS", "warning", "led", ch, t_dev, bad,
                    pred, O.level, "rel", min_run,
                    lambda a, b, ex, ob: (
                        f"LED channel {ch} brightness was "
                        f"{20.0 * math.log10(max(ob, 1e-6) / max(ex, 1e-6)):+.1f} dB "
                        f"from the expected profile between t={a:.1f} s and "
                        f"t={b:.1f} s."),
                    detail=f"sensor gain calibrated ONCE over the session "
                           f"(scale {scale:.3f}); only the SHAPE of the "
                           f"brightness profile is checked, never its absolute "
                           f"level. CAVEAT: that single gain assumes a "
                           f"frequency-INDEPENDENT sensor. An AC-coupled input "
                           f"(any audio interface high-passes around 5-20 Hz) "
                           f"attenuates a 10 Hz flicker's recovered level more "
                           f"than a 20 Hz one, so a BRIGHTNESS finding confined "
                           f"to a span after a rate change is more likely the "
                           f"capture input than the device. A SLOW BASELINE "
                           f"WANDER -- ambient light changing, or the sensor's "
                           f"own thermal drift -- does the same thing without "
                           f"any rate change at all: a +/-5% full-scale "
                           f"sinusoid at a 60 s period on an otherwise clean "
                           f"40 Hz render produced +3.7 dB here. So a "
                           f"BRIGHTNESS finding is evidence about the device "
                           f"only once the capture rig has been ruled out, "
                           f"which is why it is a warning. MEASURED on a clean "
                           f"render with a 12 Hz one-pole high-pass: +3.7 dB on "
                           f"the 20 Hz half of the session, 0 dB at 10 Hz. Use "
                           f"the DC-coupled front-end in README section 4.")
    for ch in sorted(obs.light):
        if ch not in exp.light:
            out.append(Finding(
                code="LIGHT_UNEXPECTED", severity="info", domain="led",
                channel=ch,
                message=f"a sensor was mapped to LED channel {ch} but the .ledc "
                        f"never drives it."))
    return out


def _check_light_modulation(exp: Expectation, obs: Observation,
                            cfg: CompareConfig, t0: float) -> list[Finding]:
    """Did the brightness MODULATION the .ledc asks for actually happen?

    The `~a:b:period` brightness LFO is the most common authoring pattern in the
    library (31 of the 48 non-RGB shipped sessions), and both of its edges --
    starting it and stopping it -- are entries whose INSTANT a recording cannot
    pin: the template is periodic, so a lag is only known modulo the period, and
    on a 12 s LFO that is wider than any search window worth running. MEASURED
    on 36_shamanic_trance_drum with `--fault late:{1441000}:15000`, which delays
    the entry that drops its LFO for a steady 26%: the trajectory match found a
    perfectly good alignment 430 ms from the deadline, because shifting a
    periodic template by one period costs almost nothing.

    What a recording CAN say is whether the modulation is there at all, and
    when. That is a VALUE, not a time, so it belongs here: the running
    peak-to-peak of the measured level against the running peak-to-peak of the
    demanded brightness, over the same window. It catches the dropped entry and
    the 15 s-late entry at their own timestamps, which is what a reader needs.
    """
    out: list[Finding] = []
    # A FORWARD window, 15 s long. It has to be long enough to contain a full
    # swing of the slowest LFO the library writes (periods run 6-30 s; a window
    # shorter than the period measures a fraction of the swing, and on
    # 20_gateway_focus10 a 5 s window turned the clean `~12:22:10000`
    # breathing into "4 points peak-to-peak where 8 was demanded"), and
    # forward-looking so that the run it reports STARTS at the entry that broke
    # the modulation rather than one window later.
    win_s = 15.0
    min_run = max(1, int(3.0 * obs.grid_hz))
    w = int(round(win_s * obs.grid_hz))
    for ch, L in sorted(exp.light.items()):
        O = obs.light.get(ch)
        if O is None or not O.available:
            continue
        t_dev = O.t_rec - t0
        act = np.interp(t_dev, L.t, L.active.astype(float),
                        left=0.0, right=0.0) > 0.99
        # ZERO WHILE INACTIVE. `bright_pct` retains its last value across a
        # stop (the firmware does too, led_matrix_example.c:1532-1545), so a
        # window straddling the session's close saw a demanded depth of 0 --
        # "45 points peak-to-peak where 0 was demanded" on all four zones of a
        # clean 07_genus_40hz, 15 s before its close.
        e_brt = np.where(
            np.interp(t_dev, L.t, L.active.astype(float),
                      left=0.0, right=0.0) > 0.5,
            np.interp(t_dev, L.t, L.bright_pct, left=np.nan, right=np.nan),
            0.0)
        # The GAIN is fitted only where the demanded brightness is above the
        # measurable floor, pointwise -- that is where the level series is
        # linear in brightness.
        m_fit = act & np.isfinite(e_brt) & (e_brt >= cfg.min_bright_pct) \
            & np.isfinite(O.level)
        if m_fit.sum() < min_run * 2:
            continue
        scale = robust_median(O.level[m_fit] / (e_brt[m_fit] / 100.0))
        if not (math.isfinite(scale) and scale > 0):
            continue
        # EVERY SAMPLE IN THE WINDOW HAS TO BE A MEASUREMENT. A running
        # peak-to-peak is only as good as its worst sample, so one NaN -- or the
        # end of the recording -- turns into a full-scale "modulation". MEASURED
        # on a clean 40 s render of tests/selftest.ledc, whose brightness is a
        # flat 60%: "61 points peak-to-peak where 0 was demanded", because the
        # window reached past the last sample of the file.
        finite = np.isfinite(O.level) & np.isfinite(e_brt)
        inner = finite.copy()
        for sft in range(1, w + 1):
            inner[:-sft] &= finite[sft:]
        inner[-w:] = False
        if (m_fit & inner).sum() < min_run * 2:
            continue
        lo_d, hi_d = _shift_envelope_fwd(np.nan_to_num(e_brt, nan=0.0), w)
        dep_dem = hi_d - lo_d
        lvl = np.nan_to_num(O.level, nan=0.0)
        lo_o, hi_o = _shift_envelope_fwd(lvl, w)
        dep_obs = (hi_o - lo_o) / scale * 100.0
        # GRADED WHERE THE WINDOW IS MEASURABLE, not where every sample in it
        # is. The statistic describes [t, t+w], so the floor that matters is
        # whether the BRIGHTEST part of that window is above the measurable
        # level -- and the whole channel active across it. Testing the floor
        # pointwise instead cut the mask out under the dim half of every
        # swing, which fragmented one dropped LFO into eleven findings, one
        # per LFO period (synthetic test_87).
        act_all = _shift_envelope_fwd(act.astype(np.float64), w)[0] > 0.5
        m0 = act_all & inner & (hi_d >= cfg.min_bright_pct)
        # A PRESENCE TEST, not a depth measurement -- deliberately, because
        # the depth this front-end reports is COMPRESSED by a known mechanism.
        # `ObservedLight.level` is the mean of the envelope over each detected
        # ON interval (observe_wav.py:367-380), and the Schmitt trigger's
        # thresholds come from 4 s percentile blocks (observe_wav.py:457-461).
        # A brightness LFO swings the contrast within one block, so in the dim
        # half of the swing the blended upper trigger is unreachable, the ON
        # segment runs on, and its mean is pulled up by the brighter samples
        # around it. MEASURED on a clean 04_meditation_theta (`~10:30:10000`,
        # i.e. 20 points demanded): the level series floors at the equivalent
        # of 16.5% and never reaches the demanded 10%, so the measured
        # peak-to-peak is 0.45-0.65 of the demand with the demand itself
        # perfectly rendered. Grading that ratio as a depth ERROR is grading
        # the estimator.
        #
        # What the series does do reliably is distinguish BREATHING from
        # STEADY: when the modulation is genuinely gone the ratio is ~0.05
        # (what is left is sensor noise), an order of magnitude below the
        # 0.45 floor of a healthy one. So: flag the modulation as ABSENT below
        # a quarter of the demanded depth, flag it as PRESENT-UNDEMANDED where
        # the demand is steady, and say nothing about any value in between.
        bad = m0 & (
            ((dep_dem >= _MOD_DEPTH_FLOOR_PCT)
             & (dep_obs < _MOD_PRESENT_FRAC * dep_dem))
            | ((dep_dem < _MOD_DEPTH_FLOOR_PCT)
               & (dep_obs > 3.0 * _MOD_DEPTH_FLOOR_PCT)))
        _report_runs(
            out, "BRIGHTNESS_MODULATION", "error", "led", ch, t_dev, bad,
            dep_dem, dep_obs, "%", min_run,
            lambda a, b, ex, ob: (
                f"LED channel {ch}: the brightness MODULATION measured "
                f"{ob:.0f} points peak-to-peak where {ex:.0f} was demanded, "
                f"from t={a:.1f} s to t={b:.1f} s -- the `~a:b:period` "
                f"breathing this span asks for is "
                + ("absent" if ob < ex else "present where none is asked for")
                + "."),
            detail=f"Measured as the running peak-to-peak of the optical level "
                   f"over the following {win_s:.0f} s, divided by the one sensor "
                   f"gain fitted for this channel, against the same statistic "
                   f"of the demanded brightness. A modulation that is present "
                   f"or absent is a VALUE question, unlike its start instant, "
                   f"which a periodic template cannot pin. The two depths are "
                   f"NOT comparable as numbers: the per-cycle ON-level "
                   f"estimator under-reports the dim half of a brightness LFO "
                   f"by up to 2x, so only a depth below a quarter of the "
                   f"demand (or breathing where a steady value is demanded) is "
                   f"reported, and the printed depth is indicative only.")
    return out


# Fraction of the DEMANDED brightness modulation depth below which the
# measured depth means the `~a:b:period` breathing is not happening at all.
# See _check_light_modulation: a healthy LFO measures 0.45-0.65 of its demand
# through this front-end, an absent one ~0.05.
_MOD_PRESENT_FRAC = 0.25


# Per-ear linear gain below which a generator slot cannot be heard well enough
# for anything about it to be measured. Shared with _check_events' onset gate
# (_AUDIO_MIN_AMP) on purpose: 0.02 of full scale is ~-34 dBFS before the fixed
# 1/16 mix headroom.
_AUDIO_GRADE_GAIN = 0.02

# A second slot this far below the loudest one still modulates the summed
# envelope as deeply as the firmware's own 0.1 isochronic depth (depth ~= 2 *
# A2/A1, so A2/A1 = 0.05 gives 0.1), which is why the AM-rate check has to
# know about it.
_AM_PAIR_REL = 0.05

# Spectral prominence a measured peak needs before it is graded as a CARRIER.
# The front-end's own floor is 9 dB, which a noise bed's largest bin can clear
# by chance; 12 dB is the level at which the peak is a tone and not the loudest
# sample of a continuum.
_TONE_PROM_DB = 12.0


def _audible_slots(exp: Expectation, t_dev: np.ndarray, ear: str
                   ) -> list[tuple[np.ndarray, np.ndarray, np.ndarray, bool]]:
    """[(gain, carrier_hz, pulse_hz, tonal)] per generator slot, for one ear.

    Only slots that are audible SOMEWHERE are returned, so the pairwise loops
    below stay over the two or three slots a session actually uses rather than
    all sixteen.

    `tonal` is False for the NOISE waveforms (wave 5 pink, wave 6 brown,
    audio_generator.h:42-55). Their `freq` column is still parsed and still
    carries a number -- 28_noise_sleep_bed writes `A 0 200 0 >0 0.85 1 0 5`,
    i.e. 200 with wave 5 -- but the firmware uses it only to run the AM
    accumulator; there is no carrier at 200 Hz to measure. Treating that number
    as a carrier produced "[ERROR] TONE_FREQ" on clean renders of
    28_noise_sleep_bed and 29_noise_sleep_pulsed, where the only thing in the
    recording is broadband noise.
    """
    outs = []
    for ch, A in sorted(exp.audio.items()):
        g_src = A.gain_l if ear == "left" else A.gain_r
        f_src = A.freq_l_hz if ear == "left" else A.freq_r_hz
        g = np.interp(t_dev, A.t, np.where(A.active, np.nan_to_num(g_src), 0.0),
                      left=0.0, right=0.0)
        if not (g > _AUDIO_GRADE_GAIN).any():
            continue
        f = np.interp(t_dev, A.t, np.nan_to_num(f_src), left=np.nan, right=np.nan)
        p = np.interp(t_dev, A.t, np.nan_to_num(A.pulse_hz), left=0.0, right=0.0)
        # ...and only WHILE THE SLOT IS ACTIVE: `wave` is sampled over the
        # whole grid and reads 0 (sine) before the slot ever starts, so taking
        # the whole series made every noise slot look like it carried a tone
        # for part of the session.
        wv = np.asarray(A.wave)[np.asarray(A.active, dtype=bool)]
        tonal = not (wv.size > 0
                     and bool(np.isin(np.unique(wv.astype(int)),
                                      (5, 6)).all()))
        outs.append((g, f, p, tonal))
    return outs



def _running_max(y: np.ndarray, w: int) -> np.ndarray:
    """Running maximum of `y` over a +/-w-sample window, edge-extended."""
    hi = np.asarray(y, dtype=np.float64).copy()
    for sft in range(1, max(1, w) + 1):
        hi[sft:] = np.maximum(hi[sft:], y[:-sft])
        hi[:-sft] = np.maximum(hi[:-sft], y[sft:])
    return hi


def _check_coherent_peak(exp: Expectation, obs: Observation,
                         cfg: CompareConfig, t_dev: np.ndarray, slots: dict,
                         coh: np.ndarray, min_run: int,
                         scale: float) -> list[Finding]:
    """Grade the one thing that IS predictable where two slots interfere.

    `ExpectedMix.rms_rel` is an incoherent power sum, so on a span where two
    audible slots sit within a couple of Hz of each other in one ear it is
    simply the wrong number and the level check correctly declines
    (AMPLITUDE_NOT_GRADED). But declining was ALL it did, and that lost the
    whole level measurement on the two-slot panned binaural idiom -- which is
    the idiom the library uses and the lint used to recommend.

    MEASURED (round 2): a session in the 04_meditation_theta shape rendered with
    slot 1 at volume 15 instead of the prescribed 50 -- left-ear RMS 0.3535 ->
    0.2298, i.e. -3.7 dB, against a 3 dB threshold -- produced "0 error(s), 0
    warning(s)", EXIT 0, and a finding list identical to the clean run.

    What survives the coherence is the ENVELOPE PEAK. Two tones of amplitude
    a1, a2 beat between |a1-a2| and (a1+a2), and over a window longer than one
    beat period 1/df the envelope reaches the upper bound. So the running
    maximum of the measured level is comparable with (sum of demanded
    amplitudes)/sqrt(2), with no assumption about the relative phase at all.
    The LOWER bound is not usable (it is ~0 for equal amplitudes), so only a
    peak that falls SHORT is reported -- which is exactly the direction a slot
    playing too quietly moves it.
    """
    out: list[Finding] = []
    A = obs.audio
    if A is None:
        return out
    for ear, rms_e in (("left", getattr(A, "rms_l", None)),
                       ("right", getattr(A, "rms_r", None))):
        sl = slots.get(ear) or []
        if rms_e is None or len(sl) < 2:
            continue
        rms_e = np.asarray(rms_e, dtype=np.float64)
        if not np.isfinite(rms_e).any():
            continue
        amp_sum = np.zeros(t_dev.size)
        fmin = np.full(t_dev.size, np.inf)
        fmax = np.full(t_dev.size, -np.inf)
        n_on = np.zeros(t_dev.size)
        for g, f, _p, tonal in sl:
            if not tonal:
                continue
            on = (g > _AUDIO_GRADE_GAIN) & np.isfinite(f) & (f > 0)
            amp_sum += np.where(on, g, 0.0)
            fmin = np.where(on, np.minimum(fmin, f), fmin)
            fmax = np.where(on, np.maximum(fmax, f), fmax)
            n_on += on.astype(float)
        df = np.where(np.isfinite(fmax) & np.isfinite(fmin), fmax - fmin, 0.0)
        # The window has to span at least one beat period for the peak to be
        # reached. A sub-0.05 Hz beat needs more than 20 s and is not graded.
        gradable = coh & (n_on >= 2) & (amp_sum > 2 * _AUDIO_GRADE_GAIN) \
            & (df > 0.05) & (df <= dm.THRESHOLD_COHERENT_CARRIER_HZ)
        if gradable.sum() < min_run:
            continue
        per = float(np.median(1.0 / np.maximum(df[gradable], 1e-6)))
        w = int(round(min(20.0, max(2.0, 1.2 * per)) * obs.grid_hz))
        hi_obs = _running_max(np.nan_to_num(rms_e, nan=0.0), w)
        # NO 1/sqrt(2) HERE. `scale` already carries it: it was fitted as
        # measured-RMS / demanded-AMPLITUDE on a single-tone span, and the RMS
        # of a sinusoid of amplitude a is a/sqrt(2). The envelope peak of two
        # beating tones has amplitude (a1+a2), so the level measured near that
        # peak is (a1+a2) * scale.
        pred_peak = amp_sum
        m = gradable & (pred_peak > 0) & (hi_obs > 0)
        if m.sum() < min_run:
            continue
        # THE SCALE IS NOT FITTED HERE. Calibrating the sensor/mix gain on the
        # very spans under test is what made this check blind: with the whole
        # session coherent (the panned-pair idiom), a median of
        # measured/predicted absorbs the fault exactly and the residual is zero
        # by construction. The scale comes from spans where the incoherent model
        # IS right -- on a README-compliant recording, at minimum the sync
        # marker's own single-slot tone.
        with np.errstate(divide="ignore", invalid="ignore"):
            db = 20.0 * np.log10(np.maximum(hi_obs, 1e-9)
                                 / np.maximum(pred_peak * scale, 1e-9))
        # ERROR SEVERITY, and at the ordinary 3 dB bar. Unlike the mean-level
        # profile check, the quantity here is a BOUND -- (a1+a2) is the most the
        # envelope can ever reach -- and the gain that converts it to recorder
        # units was fitted on a span where the model is exact. A shortfall this
        # persistent cannot come from the calibration, from AC coupling, or from
        # the interference itself. It is reported over a longer minimum run than
        # the warning-level checks because the running maximum needs a whole
        # beat period per sample to be meaningful.
        for code, sev, thr, mr in (
                ("AUDIO_PEAK_SHORT", "error", cfg.amp_db,
                 max(min_run, int(5.0 * obs.grid_hz))),):
            bad = m & np.isfinite(db) & (db < -thr)
            _report_runs(
                out, code, sev, "audio", None, t_dev, bad,
                pred_peak * scale, hi_obs, "rel", mr,
                lambda a, b, ex, ob, ear=ear: (
                    f"{ear}-ear envelope PEAK was "
                    f"{20.0 * math.log10(max(ob, 1e-9) / max(ex, 1e-9)):+.1f} dB "
                    f"below what the sum of the demanded amplitudes allows, "
                    f"between t={a:.1f} s and t={b:.1f} s -- one of the two "
                    f"interfering generator slots is quieter than prescribed."),
                detail="Two slots within "
                       f"{dm.THRESHOLD_COHERENT_CARRIER_HZ:.0f} Hz of each "
                       "other in one ear beat between |a1-a2| and (a1+a2), so "
                       "the running maximum of the measured level over one beat "
                       "period IS predictable even though the mean level is "
                       "not. Only a peak falling SHORT is reported: the lower "
                       "bound of the beat is ~0 for equal amplitudes and "
                       "carries no information.")
    return out


def _check_audio_values(exp: Expectation, obs: Observation, cfg: CompareConfig,
                        t0: float, ppm: float, stats: dict,
                        t0_unc_s: float = 0.0) -> list[Finding]:
    out: list[Finding] = []
    A = obs.audio
    if A is None:
        out.append(Finding(
            code="AUDIO_UNMAPPED", severity="info", domain="audio",
            message="no audio channel was mapped, so nothing about the audio "
                    "was checked."))
        return out
    min_run = max(1, int(cfg.min_run_s * obs.grid_hz))
    k = 1.0 + (ppm * 1e-6 if math.isfinite(ppm) else 0.0)
    t_dev = obs.t_rec - t0
    M = exp.mix

    def pull(y):
        return np.interp(t_dev, M.t, y, left=np.nan, right=np.nan)

    e_rms = pull(M.rms_rel)
    e_pulse = pull(M.pulse_hz)
    e_beat = pull(M.beat_hz)

    # Blackout half-widths matched to each estimator's own window length.
    bo_tone = _blackout(t_dev, exp.events, 1.0)
    bo_pulse = _blackout(t_dev, exp.events, 2.5)
    bo_amp = _blackout(t_dev, exp.events, 0.6)

    slots = {ear: _audible_slots(exp, t_dev, ear) for ear in ("left", "right")}

    for ear, e_src, o_src in (("left", M.dominant_l_hz, A.tone_l_hz),
                              ("right", M.dominant_r_hz, A.tone_r_hz)):
        if ear == "right" and not A.has_right:
            continue
        if ear == "left" and not A.has_left:
            continue
        # THE CHECK IS AGAINST THE SET OF CARRIERS THIS EAR IS ASKED FOR, not
        # against a single "dominant" slot. Two reasons, both measured:
        #
        #  * COVERAGE. `dominant_*_hz` is NaN wherever no slot is 6 dB above the
        #    rest, which is exactly the two-slot panned binaural idiom that
        #    LEDC_* lint recommends and that 04_meditation_theta ships -- so a
        #    carrier fault on that idiom was graded NOWHERE. `--fault
        #    audio-freq:0.98` on it (both carriers 2% low, a 5 Hz error on a
        #    250 Hz tone) produced zero findings and exit 0.
        #  * CORRECTNESS. A recording cannot tell which of several audible slots
        #    an FFT peak belongs to. The only claim it supports is "the measured
        #    peak matches NONE of the carriers this ear was asked for", and that
        #    is what is reported.
        #
        # The expected value quoted in the finding is the NEAREST demanded
        # carrier, so the percentage is the smallest error consistent with the
        # measurement rather than the largest.
        audible = slots[ear]
        # A RECORDING CANNOT SEPARATE TWO COMPARABLE TONES 5 Hz APART. The
        # spectral peak of a 0.25 s window is 4 Hz wide, so when several audible
        # slots carry comparable, resolvably-different carriers the peak lands
        # BETWEEN them and is not any slot's carrier at all. MEASURED on clean
        # full-length renders: 36_shamanic_trance_drum plays 160 Hz against
        # 164.99 Hz and the peak reads 162.52 -- the midpoint -- reported as
        # "-1.50%"; 25_gateway_journey_TRAP stacks 100/200/250/300 Hz at equal
        # volume and the peak reads 181.24, reported as "-9.38%".
        #
        # So the ear's carrier is gradable in exactly two situations, and both
        # are checked per sample:
        #   DOMINANT -- one tonal slot is at least 6 dB above the rest, so the
        #     peak is its carrier (this is what ExpectedMix.dominant_*_hz meant);
        #   AGREED  -- every audible tonal slot asks for the same carrier to
        #     within the grading tolerance, so the blended peak IS that carrier.
        #     This is the two-slot panned binaural idiom (04_meditation_theta),
        #     which the dominance test alone excluded -- leaving `--fault
        #     audio-freq:0.98` on it, a 5 Hz error on a 250 Hz tone, silent.
        n = t_dev.size
        gl_best = np.zeros(n)
        gl_second = np.zeros(n)
        f_best = np.full(n, np.nan)
        num = np.zeros(n)
        den = np.zeros(n)
        fmin = np.full(n, np.inf)
        fmax = np.full(n, -np.inf)
        loud_any = np.zeros(n, dtype=bool)
        for g, f, _p, tonal in audible:
            if not tonal:
                continue
            on = (g > _AUDIO_GRADE_GAIN) & np.isfinite(f) & (f > 0)
            gi = np.where(on, g, 0.0)
            loud_any |= on
            take = gi > gl_best
            gl_second = np.where(take, gl_best, np.maximum(gl_second, gi))
            f_best = np.where(take, f, f_best)
            gl_best = np.where(take, gi, gl_best)
            num += np.where(on, gi * f, 0.0)
            den += gi
            fmin = np.where(on, np.minimum(fmin, f), fmin)
            fmax = np.where(on, np.maximum(fmax, f), fmax)
        with np.errstate(invalid="ignore", divide="ignore"):
            f_mean = np.where(den > 0, num / np.maximum(den, 1e-12), np.nan)
            spread = np.where(np.isfinite(fmax) & np.isfinite(fmin),
                              fmax - fmin, np.inf)
        dominant = loud_any & (gl_best > 2.0 * gl_second)
        agreed = loud_any & (spread <= cfg.tone_rel * np.maximum(f_mean, 1e-9))
        gradable = dominant | agreed
        best_e = np.where(dominant, f_best, f_mean) / k
        with np.errstate(invalid="ignore", divide="ignore"):
            rel = np.abs(o_src - best_e) / np.maximum(best_e, 1e-9)
        near = gradable & np.isfinite(rel) & (rel <= cfg.tone_rel)
        loud_any = gradable
        if not audible:
            # Fall back to the mix's dominant carrier when the per-slot model is
            # unavailable (a front-end or test that supplies only a mix).
            e = pull(e_src) / k
            on = np.isfinite(e) & (e > 0)
            loud_any = on
            with np.errstate(invalid="ignore"):
                d = np.abs(o_src - e) / np.maximum(e, 1e-9)
            near = on & np.isfinite(d) & (d <= cfg.tone_rel)
            best_e = e
        prom = (obs.audio.tone_l_prom if ear == "left"
                else obs.audio.tone_r_prom)
        m = loud_any & np.isfinite(o_src) & (o_src > 0) & ~bo_tone \
            & np.isfinite(best_e) \
            & (np.nan_to_num(prom, nan=0.0) >= _TONE_PROM_DB)
        if not m.any():
            continue
        bad = m & ~near
        _report_runs(
            out, "TONE_FREQ", "error", "audio", None, t_dev, bad, best_e, o_src,
            "Hz", min_run,
            lambda a, b, ex, ob, ear=ear: (
                f"{ear}-ear carrier was {ob:.2f} Hz where {ex:.2f} Hz was "
                f"expected ({100.0 * (ob - ex) / ex:+.2f}%) from t={a:.1f} s to "
                f"t={b:.1f} s."),
            detail="`expected` is the NEAREST carrier any audible generator "
                   "slot was asked for in this ear, so the error quoted is the "
                   "smallest one consistent with the recording. Spans where "
                   "every slot in this ear is below "
                   f"{_AUDIO_GRADE_GAIN:.2f} of full scale (a fade-out tail) "
                   "are not graded: an FFT peak there is the noise floor, not a "
                   "carrier.")

    # --- the beat the EARS are asked for -------------------------------
    # `M.beat_hz` is filled only when ONE slot dominates BOTH ears, which the
    # panned-pair idiom never satisfies: 04_meditation_theta runs two binaural
    # slots at 250/260 each, so the left ear is asked for 250 and the right for
    # 260 -- a perfectly well-defined 10 Hz beat that the mix model reported as
    # NaN, leaving BEAT_FREQ and BEAT_DRIFT ungraded for the whole session.
    # `--fault beat-drift:0.25` and `:-0.5` on it were both silent.
    #
    # The beat is well defined whenever every audible slot in a given ear asks
    # for carriers within THRESHOLD_COHERENT_CARRIER_HZ of each other (so "the
    # carrier of that ear" is unambiguous), which is exactly the idiom above.
    def _ear_carrier(ear: str) -> tuple[np.ndarray, np.ndarray]:
        """(carrier, unique) per sample: the gain-weighted demanded carrier of
        one ear, and whether every audible slot agrees on it."""
        n = t_dev.size
        num = np.zeros(n)
        den = np.zeros(n)
        fmin = np.full(n, np.inf)
        fmax = np.full(n, -np.inf)
        for g, f, _p, tonal in slots[ear]:
            if not tonal:
                continue
            on = (g > _AUDIO_GRADE_GAIN) & np.isfinite(f) & (f > 0)
            num += np.where(on, g * f, 0.0)
            den += np.where(on, g, 0.0)
            fmin = np.where(on, np.minimum(fmin, f), fmin)
            fmax = np.where(on, np.maximum(fmax, f), fmax)
        with np.errstate(invalid="ignore", divide="ignore"):
            c = np.where(den > 0, num / np.maximum(den, 1e-12), np.nan)
        uniq = (den > 0) & np.isfinite(fmin) & np.isfinite(fmax) \
            & ((fmax - fmin) < dm.THRESHOLD_COHERENT_CARRIER_HZ)
        return c, uniq

    if A.beat_available and slots["left"] and slots["right"]:
        cl, ul = _ear_carrier("left")
        cr, ur = _ear_carrier("right")
        fill = ul & ur & np.isfinite(cl) & np.isfinite(cr)
        e_beat = np.where(np.isfinite(e_beat), e_beat,
                          np.where(fill, np.abs(cr - cl), np.nan))

    # --- binaural beat -------------------------------------------------
    if not A.beat_available:
        wants_beat = np.isfinite(e_beat) & (e_beat > 0.05)
        if wants_beat.any():
            out.append(Finding(
                code="BEAT_UNAVAILABLE", severity="warning", domain="audio",
                expected=float(robust_median(e_beat[wants_beat])), unit="Hz",
                message=f"the .ledc asks for a binaural beat around "
                        f"{robust_median(e_beat[wants_beat]):.2f} Hz, but only "
                        f"one audio channel was mapped. A beat is the DIFFERENCE "
                        f"between the two ear carriers and cannot be recovered "
                        f"from one ear -- it is not reported rather than guessed.",
                detail=A.note or "map both audioL and audioR to check the beat."))
    else:
        # The same audibility gate the carrier check uses: a beat is the
        # difference of two measured carriers, so where neither ear carries an
        # audible tone the "beat" is the difference of two noise peaks. On
        # 01_sleep_onset with -40 dBFS of recorder noise, the last 9 s of the
        # 300 s volume fade produced "[ERROR] binaural beat was 3282.265 Hz
        # where 2.000 Hz was expected" -- a recorder imperfection reported as a
        # device fault.
        loud_beat = np.zeros(t_dev.size, dtype=bool)
        for ear in ("left", "right"):
            e_loud = np.zeros(t_dev.size, dtype=bool)
            for g, _f, _p, _t in slots[ear]:
                e_loud |= g > _AUDIO_GRADE_GAIN
            loud_beat = e_loud if ear == "left" else (loud_beat & e_loud)
        if not slots["left"] or not slots["right"]:
            loud_beat = np.ones(t_dev.size, dtype=bool)
        m = np.isfinite(e_beat) & (e_beat > 0.05) & np.isfinite(A.beat_hz) \
            & ~bo_tone & loud_beat
        if m.any():
            bad = m & (np.abs(A.beat_hz - e_beat) > cfg.beat_hz)
            _report_runs(
                out, "BEAT_FREQ", "error", "audio", None, t_dev, bad,
                e_beat, A.beat_hz, "Hz", min_run,
                lambda a, b, ex, ob: (
                    f"binaural beat was {ob:.3f} Hz where {ex:.3f} Hz was "
                    f"expected ({ob - ex:+.3f} Hz) from t={a:.1f} s to "
                    f"t={b:.1f} s."),
                detail="beat = |right carrier - left carrier|, each measured by "
                       "phase-slope refinement to about 0.02 Hz.")
            # Slow beat drift is its own story. Compared via the MEDIAN
            # residual of the first vs the last third rather than a
            # least-squares slope: a handful of estimator outliers at one end
            # of the session can tilt a least-squares line into a confident
            # "your beat is drifting" claim, and that is exactly the phantom
            # bug this instrument exists not to invent.
            if m.sum() > 60:
                tt, dd = t_dev[m], A.beat_hz[m] - e_beat[m]
                if tt.max() - tt.min() > 60.0:
                    n3 = tt.size // 3
                    d0 = robust_median(dd[:n3])
                    d1 = robust_median(dd[-n3:])
                    thirds = d1 - d0
                    span = tt[-1] - tt[0]
                    # The two medians sit at the CENTRES of the outer thirds,
                    # i.e. 2/3 of the span apart -- so dividing the difference
                    # by the full span under-reported the slope by a third, and
                    # the headline number did not match its own sentence
                    # ("drifted X Hz over Y minutes").
                    slope = (1.5 * thirds / span) if span > 0 else 0.0
                    total = slope * span
                    if abs(thirds) > 2 * cfg.beat_hz:
                        out.append(Finding(
                            code="BEAT_DRIFT", severity="error", domain="audio",
                            observed=float(total), unit="Hz",
                            t_ms=float(tt.min() * 1000.0),
                            t_end_ms=float(tt.max() * 1000.0),
                            message=f"binaural beat DRIFTED {total:+.2f} Hz over "
                                    f"{(tt.max() - tt.min()) / 60.0:.1f} minutes "
                                    f"relative to the demanded profile "
                                    f"({slope * 1000.0:+.3f} Hz per 1000 s).",
                            detail="freqR is the ABSOLUTE right-ear carrier, "
                                   "including while the left carrier is swept "
                                   "(audio_generator.c:995-1013), so a carrier "
                                   "ramp is EXPECTED to glide the beat and the "
                                   "expectation above already contains that "
                                   "glide. A residual drift on top of it means "
                                   "either the global beat-jitter feature is on "
                                   "(config_parser.c:2404) or the two carriers "
                                   "are not tracking each other."))

    # --- pulse / isochronic rate ---------------------------------------
    # THREE THINGS THIS HAS TO KNOW BEFORE IT IS ALLOWED TO ACCUSE ANYTHING,
    # each of which produced error-severity PULSE_RATE findings on clean
    # full-length renders of shipped sessions:
    #
    # 1. WHAT THE WINDOW CAN RESOLVE. The AM estimator is a 4 s FFT, i.e.
    #    0.25 Hz bins. 18_jhana_absorption ramps its isochronic pulse 6 -> 0 Hz
    #    over four minutes; grading continued down to 0.574 Hz -- two cycles in
    #    the window -- and reported "0.500 Hz where 0.574 Hz was expected".
    #    Below ~4 cycles in the window the rate is not measurable to 5%, and
    #    saying so is the honest answer.
    #
    # 2. THAT A MIX OF TONES HAS AN ENVELOPE OF ITS OWN. Two audible carriers
    #    in the SAME ear beat at their difference frequency, and that beat is a
    #    real, deep amplitude modulation of the recording -- nothing to do with
    #    the isochronic gate. MEASURED: 09_lucid_hypnagogic plays 220 Hz
    #    (pan 0) against a 250/256 binaural pair, so the right ear carries a
    #    genuine 36 Hz envelope beat, and the tool reported "pulse rate was
    #    36.000 Hz where 6.000 Hz was expected". 35_wbtb_lucid_gamma puts
    #    340 Hz clicks against a 200 Hz bed: 140 Hz, reported as "140.001 Hz
    #    where 5.000 Hz was expected". Both are arithmetic, not faults.
    #
    # 3. THAT SLOTS CAN DISAGREE. When two audible slots are gated at different
    #    rates the mix envelope contains both, and no single number is "the"
    #    pulse rate.
    #
    # So the test is the same shape as TONE_FREQ: flag only where the measured
    # AM rate matches NONE of the rates this recording could legitimately
    # contain. A device gating at the wrong rate matches none of them and is
    # still reported.
    pulse_win_s = obs.meta.get("pulse_win_s")
    pulse_win_s = (float(pulse_win_s)
                   if isinstance(pulse_win_s, (int, float)) and pulse_win_s
                   else 4.0)
    pulse_floor_hz = max(0.4, 4.0 / pulse_win_s)
    adm = np.zeros(t_dev.size, dtype=bool)        # observed matches something
    legit: list[np.ndarray] = []
    for ear in ("left", "right"):
        if ear == "right" and not A.has_right:
            continue
        if ear == "left" and not A.has_left:
            continue
        sl = slots[ear]
        for i in range(len(sl)):
            gi, fi, pi, ti = sl[i]
            oni = gi > _AUDIO_GRADE_GAIN
            legit.append(np.where(oni & (pi > 0), pi, np.nan))
            for j in range(i + 1, len(sl)):
                gj, fj, _pj, tj = sl[j]
                if not (ti and tj):
                    continue    # no carriers, so no inter-carrier beat
                both_on = oni & (gj > _AUDIO_GRADE_GAIN)
                with np.errstate(invalid="ignore", divide="ignore"):
                    ratio = np.minimum(gi, gj) / np.maximum(np.maximum(gi, gj),
                                                            1e-12)
                deep = both_on & (ratio >= _AM_PAIR_REL)
                df = np.abs(np.nan_to_num(fi, nan=0.0) - np.nan_to_num(fj, nan=0.0))
                for n in (1, 2, 3):
                    legit.append(np.where(deep, n * df, np.nan))
    for cand in legit:
        with np.errstate(invalid="ignore", divide="ignore"):
            d = np.abs(A.pulse_hz - cand) / np.maximum(cand, 1e-9)
        adm |= np.isfinite(d) & (d <= 0.05)
    if not legit:
        with np.errstate(invalid="ignore", divide="ignore"):
            d = np.abs(A.pulse_hz - e_pulse) / np.maximum(e_pulse, 1e-9)
        adm = np.isfinite(d) & (d <= 0.05)
    resolvable = np.isfinite(e_pulse) & (e_pulse >= pulse_floor_hz)
    m = resolvable & np.isfinite(A.pulse_hz) & ~bo_pulse
    unresolvable = (np.isfinite(e_pulse) & (e_pulse > 0.4)
                    & (e_pulse < pulse_floor_hz))
    if unresolvable.sum() > obs.grid_hz:
        ui = np.nonzero(unresolvable)[0]
        out.append(Finding(
            code="PULSE_NOT_GRADED", severity="info", domain="audio",
            t_ms=float(t_dev[ui[0]] * 1000.0),
            t_end_ms=float(t_dev[ui[-1]] * 1000.0),
            expected=float(pulse_floor_hz), unit="Hz",
            message=f"the isochronic/tremolo rate was NOT graded for "
                    f"{unresolvable.sum() / obs.grid_hz:.0f} s: the .ledc asks "
                    f"for less than {pulse_floor_hz:.2f} Hz there, which is "
                    f"fewer than four cycles in the {pulse_win_s:.0f} s AM "
                    f"analysis window, so the rate cannot be recovered to the "
                    f"5% this check uses.",
            detail="Not a fault. A longer analysis window would trade time "
                   "resolution for this; the shipped window is sized for the "
                   "0.5-150 Hz band the sessions actually use."))
    if m.any():
        rel = np.abs(A.pulse_hz - e_pulse) / np.maximum(e_pulse, 1e-9)
        bad = m & (rel > 0.05) & ~adm
        _report_runs(
            out, "PULSE_RATE", "error", "audio", None, t_dev, bad, e_pulse,
            A.pulse_hz, "Hz", min_run,
            lambda a, b, ex, ob: (
                f"audio pulse (isochronic/tremolo) rate was {ob:.3f} Hz where "
                f"{ex:.3f} Hz was expected from t={a:.1f} s to t={b:.1f} s."),
            detail="the pulse accumulator is per-sample and sample-exact "
                   "(audio_generator.c:1385), so a rate error here is a value "
                   "error, not a timing one. Spans where the measured rate "
                   "matches the envelope beat between two audible carriers, or "
                   "another audible slot's own gate rate, are NOT flagged -- "
                   "those modulations are really in the recording.")
    elif np.isfinite(e_pulse).any() and (e_pulse[np.isfinite(e_pulse)] > 0.4).any():
        out.append(Finding(
            code="PULSE_NOT_MEASURED", severity="info", domain="audio",
            message="the .ledc asks for an isochronic/tremolo pulse but no AM "
                    "was recoverable from the envelope. The timeline hard-codes "
                    "the depth to 0.1 (config_parser.c:2357), i.e. only ~0.9 dB, "
                    "so a noisy capture can hide it."))

    # --- overall level, on a calibrated relative scale ------------------
    # Spans where two audible slots share a carrier are NOT GRADED: the
    # expectation is an incoherent power sum and the real sum is coherent there,
    # so a mismatch says nothing about the device. Reported, not silently
    # dropped -- lost coverage with no trace in the output is the failure mode
    # this whole file is written against.
    coh = getattr(M, "coherent_pair", None)
    coh_dev = (np.interp(t_dev, M.t, coh.astype(float), left=0.0, right=0.0) > 0.5
               if coh is not None and coh.size == M.t.size
               else np.zeros(t_dev.size, dtype=bool))
    if coh_dev.any():
        ci = np.nonzero(coh_dev)[0]
        out.append(Finding(
            code="AMPLITUDE_NOT_GRADED", severity="info", domain="audio",
            t_ms=float(t_dev[ci[0]] * 1000.0),
            t_end_ms=float(t_dev[ci[-1]] * 1000.0),
            message=f"the overall audio level was NOT graded for "
                    f"{coh_dev.sum() / obs.grid_hz:.0f} s: two audible "
                    f"generator slots sit within "
                    f"{dm.THRESHOLD_COHERENT_CARRIER_HZ:.0f} Hz of each other "
                    f"in one ear there, so they interfere coherently and the "
                    f"summed level genuinely swings through nulls that an "
                    f"incoherent power sum cannot predict.",
            detail="This is the two-channel panned binaural idiom (two slots "
                   "detuned by a few Hz), which is what LEDC_BEAT_NOT_SWEPT "
                   "recommends, so it is expected rather than exotic. The "
                   "individual carriers are still checked by TONE_FREQ."))
    m = np.isfinite(e_rms) & (e_rms > 1e-3) & np.isfinite(A.rms) & (A.rms > 0) \
        & ~bo_amp & ~coh_dev
    # The gain calibration needs only a SHORT clean span -- the level estimate
    # over 0.3 s of a steady tone is already solid -- and on the panned-pair
    # idiom a short span is all there is: 04_meditation_theta is coherent from
    # its first entry to its last, so the only incoherent reference in the whole
    # recording is the 600 ms sync marker. Demanding 4 s here left the coherent
    # peak check (below) with no scale and therefore no coverage at all.
    # ...and the calibration gets its OWN, narrow blackout. The 0.6 s one the
    # deviation check uses covers a 600 ms marker burst completely from both
    # ends, so on the panned-pair idiom the calibration mask came out EMPTY.
    # A median of a level ratio does not need transitions excluded as widely as
    # a per-sample deviation does.
    m_cal = (np.isfinite(e_rms) & (e_rms > 1e-3) & np.isfinite(A.rms)
             & (A.rms > 0) & ~_blackout(t_dev, exp.events, 0.15) & ~coh_dev)
    scale = float("nan")
    if m_cal.sum() >= 4:
        scale = robust_median(A.rms[m_cal] / e_rms[m_cal])
        stats["audio_level_scale"] = scale
        stats["audio_level_scale_span_s"] = float(m_cal.sum()) / obs.grid_hz
    # (This is the same number _audio_level_gain computes for the event
    # realisation; both are medians of A.rms / e_rms over the non-coherent,
    # lightly-blacked-out span.)
    if coh_dev.any() and math.isfinite(scale) and scale > 0:
        out += _check_coherent_peak(exp, obs, cfg, t_dev, slots, coh_dev,
                                    min_run, scale)
    if m.sum() >= min_run * 2:
        if math.isfinite(scale) and scale > 0:
            pred = e_rms * scale
            with np.errstate(divide="ignore", invalid="ignore"):
                db = 20.0 * np.log10(np.maximum(A.rms, 1e-9)
                                     / np.maximum(pred, 1e-9))
            bad = m & np.isfinite(db) & _profile_bad(
                A.rms, pred, obs.grid_hz, t0_unc_s, cfg.amp_db)
            # A level 12 dB (4x in amplitude) below the demanded profile is
            # not a calibration question. The single session-wide gain cannot
            # be that wrong, and neither can AC coupling or a coherent sum: the
            # sound asked for is simply not there. Error severity, where the
            # ordinary profile deviation stays a warning. MEASURED: dropping a
            # session's first audio entries leaves the mix silent for a minute
            # while the model ramps in, and round 2 reported only an
            # AMPLITUDE warning for it, exit 0.
            gone = m & np.isfinite(db) & (db < -12.0) & _profile_bad(
                A.rms, pred, obs.grid_hz, t0_unc_s, 12.0)
            _report_runs(
                out, "AUDIO_LEVEL_ABSENT", "error", "audio", None, t_dev, gone,
                pred, A.rms, "rel", max(min_run, int(2.0 * obs.grid_hz)),
                lambda a, b, ex, ob: (
                    f"audio level was "
                    f"{20.0 * math.log10(max(ob, 1e-9) / max(ex, 1e-9)):+.1f} dB "
                    f"from the demanded profile between t={a:.1f} s and "
                    f"t={b:.1f} s -- the sound asked for is not in the "
                    f"recording."),
                detail="12 dB is four times in amplitude; the one session-wide "
                       "gain this check calibrates cannot absorb that, and "
                       "neither can AC coupling or two slots interfering. "
                       "Spans where two audible slots share a carrier are "
                       "excluded (AMPLITUDE_NOT_GRADED).")
            _report_runs(
                out, "AMPLITUDE", "warning", "audio", None, t_dev, bad, pred,
                A.rms, "rel", min_run,
                lambda a, b, ex, ob: (
                    f"audio level was "
                    f"{20.0 * math.log10(max(ob, 1e-9) / max(ex, 1e-9)):+.1f} dB "
                    f"from the expected profile between t={a:.1f} s and "
                    f"t={b:.1f} s."),
                detail=f"level calibrated once over the session (scale "
                       f"{scale:.4g}); only the SHAPE is checked. Absolute dBFS "
                       f"is unknowable from a recording because of the fixed "
                       f"1/16 mix headroom (audio_generator.c:705) and the "
                       f"runtime audio_max_volume setting.")
    return out
