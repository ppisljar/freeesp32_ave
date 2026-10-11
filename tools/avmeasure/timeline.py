"""Shared data shapes: the contract between front-ends and the comparator.

WHY A SEPARATE CONTRACT MODULE
------------------------------
There will be TWO observation front-ends:
  (a) observe_wav.py  -- a multi-channel recording (audio + photodiodes)
  (b) later, an on-device trace reader (plans/onboard_av_trace_plan.md)

compare.py must not be able to tell them apart. So it is forbidden from
touching anything audio-specific: no sample rates, no FFTs, no WAV channel
indices. It consumes exactly the structures below. If a field cannot be
measured by a front-end, that front-end leaves it as NaN / empty / None and
sets the matching `available` flag -- and the comparator says "unavailable"
instead of guessing. A guess from a measurement instrument is a lie.

TIME BASES
----------
Two clocks, and conflating them is the single easiest way to make this tool
produce garbage:

  DEVICE TIME  (`t_ms`, `t_s`)  -- the `.ledc` timeline, origin T0, which is
      the instant main/config_parser.c:655 captured
      `transport_origin_us = esp_timer_get_time()`.

  RECORDER TIME (`t_rec_s`)     -- seconds from the first sample of the WAV
      (or the first record of the trace).

They are related by  t_rec_s = t0_s + t_s * (1 + ppm/1e6).  Everything in an
`Observation` is in RECORDER time. `SyncSolution` carries the mapping. Nothing
else is allowed to assume it knows t0.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any

import numpy as np

# The shared analysis grid. 20 Hz / 50 ms is the compromise:
#   - fine enough to localise a step to a fraction of the 25 ms lateness
#     threshold once sub-grid interpolation is applied,
#   - coarse enough that a 60-minute session is 72k points per series, so the
#     whole expected+observed model is a few MB rather than a few GB.
DEFAULT_GRID_HZ = 20.0


def nan_array(n: int) -> np.ndarray:
    return np.full(n, np.nan, dtype=np.float64)


# ---------------------------------------------------------------------------
# Expected (demanded) side
# ---------------------------------------------------------------------------


@dataclass
class ScheduledEntry:
    """One `.ledc` entry as the firmware's executor would actually see it.

    `t_demanded_ms` is what the file says; `t_dispatch_ms` is when the firmware
    dispatches it. They differ for the FIRST batch only, because of the
    first-batch hoisting bug (main/config_parser.c:696): the initial dispatch
    uses entries[0]'s timestamp and fires IMMEDIATELY, so a timeline whose
    earliest entry is at t=5000 executes that batch at wall-clock 0.
    """

    index: int                  # index in the sorted, capped entry list
    line_no: int                # 1-based line number in the source file
    kind: str                   # 'led' | 'audio' | 'speech'
    t_demanded_ms: int
    t_dispatch_ms: int
    executed: bool              # False if dropped by the 50-per-batch cap
    drop_reason: str | None
    raw: Any                    # LedEntry | AudioEntry | SpeechEntry


@dataclass
class ExpectedLight:
    """Dense model of one LED channel (1..8, i.e. mask bit+1)."""

    channel: int
    t: np.ndarray               # device seconds
    active: np.ndarray          # bool: flicker running (or DC-on)
    demanded_hz: np.ndarray     # what the .ledc asks for; 0.0 = DC / stopped
    emitted_hz: np.ndarray      # demanded passed through the firmware's
                                # cycle re-anchoring quantization
    duty_pct: np.ndarray
    bright_pct: np.ndarray
    r: np.ndarray
    g: np.ndarray
    b: np.ndarray
    env: np.ndarray             # carrier id: 0 square, 1 sine, 2 tri, 3 trapz
    phase_deg: np.ndarray
    tick_hz: np.ndarray         # ISR tick in force (needed for the tolerance)


@dataclass
class ExpectedAudioChannel:
    """Dense model of one audio generator slot (0..15)."""

    channel: int
    t: np.ndarray
    active: np.ndarray
    freq_l_hz: np.ndarray       # the `freq` column = LEFT-ear carrier
    freq_r_hz: np.ndarray       # effective right carrier (== L when mono)
    beat_hz: np.ndarray         # |freq_r - freq_l|, 0 when mono
    amp: np.ndarray             # volume/100, before pan/headroom
    pan: np.ndarray             # -1..+1
    pulse_hz: np.ndarray        # the `mod` column: isochronic/tremolo rate
    pulse_duty_pct: np.ndarray
    env: np.ndarray             # 0..3 unipolar gate, 4 bipolar tremolo
    wave: np.ndarray            # 0 sine .. 7 EEG contour
    gain_l: np.ndarray          # per-ear linear gain incl. pan law / bypass
    gain_r: np.ndarray


@dataclass
class ExpectedMix:
    """What a 2-channel recording of the DAC output should see.

    A recording cannot separate the 16 generator slots -- they all sum into one
    stereo pair. So the comparator checks the DOMINANT tone per ear plus the
    total level, and only checks a beat when exactly one binaural channel
    dominates. `dominant_*` is NaN when no single channel is loud enough for
    the measurement to be meaningful.
    """

    t: np.ndarray
    dominant_l_hz: np.ndarray
    dominant_r_hz: np.ndarray
    beat_hz: np.ndarray
    pulse_hz: np.ndarray
    rms_rel: np.ndarray         # relative linear level, arbitrary scale
    active: np.ndarray
    n_active: np.ndarray        # how many slots are audible (confidence hint)
    dominant_channel: np.ndarray  # which slot dominates L, -1 if none
    # True where two audible slots sit on carriers close enough to interfere
    # COHERENTLY in one ear. `rms_rel` is an incoherent power sum and is simply
    # wrong there -- the real level swings through nulls with period 1/df -- so
    # the level check must declare those spans not graded instead of grading
    # them. Default all-False so a front-end or test that builds an ExpectedMix
    # by hand keeps the old behaviour.
    coherent_pair: np.ndarray = field(
        default_factory=lambda: np.zeros(0, dtype=bool))


@dataclass
class Expectation:
    """The complete ground-truth model of one `.ledc`."""

    source: str
    grid_hz: float
    t: np.ndarray
    schedule: list[ScheduledEntry]
    light: dict[int, ExpectedLight]
    audio: dict[int, ExpectedAudioChannel]
    mix: ExpectedMix
    bg: Any | None
    lints: list["Finding"] = field(default_factory=list)
    duration_s: float = 0.0
    led_backend: str = "neopixel"
    # Discrete observable transitions, in DEVICE time. Each dict carries:
    #   kind      'led_start' | 'led_update' | 'led_stop' |
    #             'audio_start' | 'audio_update'
    #   channel   LED channel 1..8, or audio slot 0..15
    #   t_ms      the timestamp the .ledc demands
    #   t_eff_ms  when the firmware should make it OBSERVABLE, including the
    #             +46.439 ms LED anchor on fresh activation and first-batch
    #             hoisting on audio. This is what the comparator diffs against.
    #   fresh     True on a cold activation (so the 50 ms first-edge grace and
    #             the audio 5 ms fade-in apply)
    events: list[dict] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Observed side -- the front-end-agnostic contract
# ---------------------------------------------------------------------------


@dataclass
class ObservedLight:
    """One light sensor, in RECORDER time.

    `edges_rise` / `edges_fall` are the primary product: exact transition
    timestamps. The firmware's own trace format records edges too
    (plans/onboard_av_trace_plan.md), so the on-device front-end fills these
    directly instead of recovering them from an envelope.

    `freq_hz` / `duty_pct` are DERIVED from the edges on the shared grid, which
    is why they are here rather than being recomputed by the comparator -- a
    front-end that knows its own noise characteristics can do better than a
    generic estimator.
    """

    channel: int
    t_rec: np.ndarray
    level: np.ndarray           # 0..1 normalised envelope, per grid cell
    contrast: np.ndarray        # p95-p05 of the envelope in the cell; the
                                # "is there any signal at all" statistic
    freq_hz: np.ndarray
    duty_pct: np.ndarray
    edges_rise: np.ndarray      # recorder seconds
    edges_fall: np.ndarray
    available: bool = True
    note: str = ""


@dataclass
class ObservedAudio:
    """The audio front-end's product, in RECORDER time."""

    t_rec: np.ndarray
    rms: np.ndarray             # combined, linear
    rms_l: np.ndarray
    rms_r: np.ndarray
    tone_l_hz: np.ndarray       # NaN where no prominent tone
    tone_r_hz: np.ndarray
    tone_l_prom: np.ndarray     # peak/median spectral prominence (dB)
    tone_r_prom: np.ndarray
    beat_hz: np.ndarray         # NaN when not measurable
    pulse_hz: np.ndarray        # AM rate recovered from the envelope
    pulse_depth: np.ndarray     # 0..1
    has_left: bool
    has_right: bool
    beat_available: bool
    note: str = ""
    # OPTIONAL higher-rate broadband level series. This is the audio analogue
    # of ObservedLight.edges_rise: the shared 20 Hz grid quantises an onset to
    # +/-25 ms, which is the same size as the lateness threshold and bigger
    # than the whole audio-vs-light offset we are trying to measure, so timing
    # an onset off the coarse grid produced a systematic ~10 ms early bias and
    # a false "audio leads light" verdict. A front-end that can offer a finer
    # level series should; the comparator falls back to `rms` when it cannot.
    fine_fs: float = 0.0            # samples/second of `fine_level`, 0 = absent
    fine_level: np.ndarray | None = None   # RMS-like, starts at t_rec = 0


@dataclass
class Observation:
    """Everything a front-end produces. compare.py sees only this."""

    source: str
    front_end: str              # 'wav' | 'trace' | 'synth'
    grid_hz: float
    t_rec: np.ndarray
    duration_s: float
    light: dict[int, ObservedLight]
    audio: ObservedAudio | None
    markers: dict[str, float] = field(default_factory=dict)
    meta: dict[str, Any] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Sync
# ---------------------------------------------------------------------------


@dataclass
class SyncSolution:
    """The recording's t=0, and how much to believe it.

    A wrong t0 makes every other number in the report meaningless, so this is
    reported first, always, with its method and confidence -- never hidden.
    """

    t0_s: float                 # recorder time of device t=0
    method: str                 # 'marker' | 'xcorr' | 'given' | 'failed'
    confidence: str             # 'high' | 'medium' | 'low' | 'none'
    uncertainty_ms: float
    detail: str
    # Separate per-domain lags, measured independently. The audio-vs-light
    # constant offset falls straight out of the difference, and must be
    # reported as ONE finding rather than as thousands of small timing errors.
    light_lag_ms: float = float("nan")
    audio_lag_ms: float = float("nan")
    xcorr_peak: float = float("nan")
    xcorr_margin: float = float("nan")

    def dev_to_rec(self, t_dev_s: np.ndarray | float, ppm: float = 0.0):
        return self.t0_s + np.asarray(t_dev_s, dtype=np.float64) * (1.0 + ppm * 1e-6)

    def rec_to_dev(self, t_rec_s: np.ndarray | float, ppm: float = 0.0):
        return (np.asarray(t_rec_s, dtype=np.float64) - self.t0_s) / (1.0 + ppm * 1e-6)


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------

SEVERITY_ORDER = {"error": 0, "warning": 1, "info": 2}


@dataclass
class Finding:
    """One actionable discrepancy.

    `code` is machine-stable; `message` is for humans. `expected`/`observed`/
    `delta` are kept as numbers so --json consumers can threshold on them
    without re-parsing prose.
    """

    code: str
    severity: str               # 'error' | 'warning' | 'info'
    message: str
    t_ms: float | None = None   # device time, start of the discrepancy
    t_end_ms: float | None = None
    domain: str | None = None   # 'led' | 'audio' | 'sync' | 'ledc' | 'clock'
    channel: int | None = None
    expected: float | None = None
    observed: float | None = None
    delta: float | None = None
    unit: str = ""
    confidence: str = "high"
    detail: str = ""
    line_no: int | None = None

    def sort_key(self):
        return (
            SEVERITY_ORDER.get(self.severity, 3),
            self.t_ms if self.t_ms is not None else -1.0,
            self.code,
        )

    def to_dict(self) -> dict:
        d = {
            "code": self.code,
            "severity": self.severity,
            "message": self.message,
            "domain": self.domain,
            "confidence": self.confidence,
        }
        for k in ("t_ms", "t_end_ms", "channel", "expected", "observed", "delta",
                  "line_no"):
            v = getattr(self, k)
            if v is not None and not (isinstance(v, float) and math.isnan(v)):
                d[k] = v
        if self.unit:
            d["unit"] = self.unit
        if self.detail:
            d["detail"] = self.detail
        return d


@dataclass
class Report:
    ledc: str
    observation: str
    front_end: str
    sync: SyncSolution
    findings: list[Finding]
    av_offset_ms: float = float("nan")
    av_offset_sd_ms: float = float("nan")
    drift_common_ppm: float = float("nan")
    drift_differential_ppm: float = float("nan")
    stats: dict[str, Any] = field(default_factory=dict)

    def sorted_findings(self) -> list[Finding]:
        return sorted(self.findings, key=lambda f: f.sort_key())


# ---------------------------------------------------------------------------
# Small numeric helpers shared by expectation, observation and comparison
# ---------------------------------------------------------------------------


def make_grid(duration_s: float, grid_hz: float) -> np.ndarray:
    n = int(math.floor(duration_s * grid_hz)) + 1
    return np.arange(n, dtype=np.float64) / grid_hz


def find_crossing(t: np.ndarray, y: np.ndarray, level: float,
                  i_from: int, i_to: int, rising: bool) -> float:
    """First time in [i_from, i_to) where `y` crosses `level`, interpolated.

    Used to time a step transition to better than the grid resolution: the
    50 ms grid would otherwise impose a +/-25 ms quantization on every event,
    which is exactly the size of our lateness threshold.
    """
    i_from = max(0, i_from)
    i_to = min(len(y), i_to)
    if i_to - i_from < 2:
        return float("nan")
    seg = y[i_from:i_to]
    ok = np.isfinite(seg)
    if ok.sum() < 2:
        return float("nan")
    if rising:
        hit = np.nonzero(ok & (seg >= level))[0]
    else:
        hit = np.nonzero(ok & (seg <= level))[0]
    if hit.size == 0:
        return float("nan")
    k = int(hit[0])
    if k == 0:
        return float(t[i_from])
    j = k - 1
    while j >= 0 and not np.isfinite(seg[j]):
        j -= 1
    if j < 0:
        return float(t[i_from + k])
    y0, y1 = float(seg[j]), float(seg[k])
    t0, t1 = float(t[i_from + j]), float(t[i_from + k])
    if y1 == y0:
        return t1
    frac = (level - y0) / (y1 - y0)
    frac = min(1.0, max(0.0, frac))
    return t0 + frac * (t1 - t0)


def robust_median(a: np.ndarray) -> float:
    a = np.asarray(a, dtype=np.float64)
    a = a[np.isfinite(a)]
    if a.size == 0:
        return float("nan")
    return float(np.median(a))


def aggregate_runs(mask: np.ndarray, min_len: int = 1) -> list[tuple[int, int]]:
    """Contiguous True runs of `mask`, as [start, end) index pairs.

    This is what stops a constant offset or a slow drift from being reported as
    thousands of individual grid-point errors. The task brief is explicit about
    it: "a constant offset is a different bug from a drift and must not be
    reported as many small errors".
    """
    mask = np.asarray(mask, dtype=bool)
    if mask.size == 0:
        return []
    d = np.diff(mask.astype(np.int8))
    starts = list(np.nonzero(d == 1)[0] + 1)
    ends = list(np.nonzero(d == -1)[0] + 1)
    if mask[0]:
        starts.insert(0, 0)
    if mask[-1]:
        ends.append(mask.size)
    return [(s, e) for s, e in zip(starts, ends) if e - s >= min_len]
