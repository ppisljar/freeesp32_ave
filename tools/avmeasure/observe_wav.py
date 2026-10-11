"""Observation front-end (a): a multi-channel recording.

WHAT IT PRODUCES
----------------
An `Observation` (see timeline.py) in RECORDER time. Nothing in here knows what
the `.ledc` demanded -- deliberately. If this module took the expectation as an
input it could tune its estimators toward the expected answer, and the
instrument would confirm its own prior instead of measuring.

LIGHT CHANNELS
--------------
A phototransistor sees the LED's *optical* waveform, which has two
superimposed rates: the entrainment flicker (0.1-100 Hz, what we want) and the
LED's own intensity PWM (WS2812 internal oscillator, several hundred Hz; or the
25 kHz LEDC carrier on the DIRECT backend). So: two-stage decimation into a
linear-phase FIR low-pass, then a Schmitt trigger whose *decision* uses
hysteresis but whose *timestamp* is the interpolated 50% crossing.

Linear phase matters more than it looks: a minimum-phase filter would delay
rising and falling edges differently, biasing every duty-cycle measurement.
A symmetric FIR delays everything by exactly (taps-1)/2 samples, which we
subtract exactly.

AUDIO CHANNELS
--------------
Two different time resolutions, on purpose:
  * RMS on the full 20 Hz grid -- cheap, and it is what times an audio ONSET.
  * tone / beat / pulse on a coarser spectral grid -- expensive, and they are
    slow-moving quantities where 0.2 s resolution costs nothing.

Carrier frequency is estimated in two stages because a single FFT is not nearly
accurate enough for a binaural beat: a 6 Hz beat between 240 and 246 Hz needs
each carrier to ~0.05 Hz, while a 0.25 s FFT has 4 Hz bins. So: parabolic peak
interpolation for a coarse f0, then a complex-demodulation phase-slope fit for
the residual, which gets to ~0.02 Hz.

GRACEFUL DEGRADATION
--------------------
  * one audio channel mapped -> tone + amplitude reported, `beat_available`
    False, and the comparator says "beat unavailable" instead of guessing.
  * no prominent spectral peak (noise waveforms 4-6, or silence) -> tone is NaN,
    not the arbitrary argmax of a noise floor.
  * flat light channel -> no edges, zero contrast; the comparator decides
    "never turned on" globally, after seeing the whole file.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field

import numpy as np

import devicemodel as dm
from timeline import (
    DEFAULT_GRID_HZ,
    Observation,
    ObservedAudio,
    ObservedLight,
    make_grid,
)
from wavio import WavReader

# ---------------------------------------------------------------------------
# Channel map
# ---------------------------------------------------------------------------

_ROLE_ALIASES = {
    "audiol": "audioL", "al": "audioL", "l": "audioL", "left": "audioL",
    "audio_l": "audioL", "audiolr": None,
    "audior": "audioR", "ar": "audioR", "r": "audioR", "right": "audioR",
    "audio_r": "audioR",
    "audio": "audioL",   # a single mono audio input
    "x": None, "-": None, "skip": None, "unused": None, "none": None,
}


@dataclass
class ChannelMap:
    """Which WAV channel carries which role.

    Indices stored 0-based internally; the CLI and all messages are 1-based,
    because that is how every DAW and interface labels its inputs and a
    silent off-by-one here would mislabel every finding in the report.
    """

    audio_l: int | None = None
    audio_r: int | None = None
    lights: dict[int, int] = field(default_factory=dict)   # LED ch (1..8) -> wav idx
    invert: set[int] = field(default_factory=set)          # wav indices to negate

    def describe(self) -> str:
        bits = []
        if self.audio_l is not None:
            bits.append(f"audioL=ch{self.audio_l + 1}")
        if self.audio_r is not None:
            bits.append(f"audioR=ch{self.audio_r + 1}")
        for led, idx in sorted(self.lights.items()):
            inv = "!" if idx in self.invert else ""
            bits.append(f"light{led}={inv}ch{idx + 1}")
        return ", ".join(bits) if bits else "(empty)"

    def used(self) -> list[int]:
        u = set(self.lights.values())
        if self.audio_l is not None:
            u.add(self.audio_l)
        if self.audio_r is not None:
            u.add(self.audio_r)
        return sorted(u)


def _norm_role(tok: str) -> tuple[str | None, int | None]:
    """Return ('audioL'|'audioR'|'light', led_channel_or_None)."""
    t = tok.strip().lower()
    m = re.fullmatch(r"(?:light|l|photo|pd)[\s_]*([1-8])", t)
    if m:
        return "light", int(m.group(1))
    if t in _ROLE_ALIASES:
        v = _ROLE_ALIASES[t]
        return (v, None) if v else (None, None)
    raise ValueError(
        f"unrecognised channel-map role '{tok}'. Use audioL, audioR, "
        f"light1..light8, or '-' to skip.")


def parse_channel_map(spec: str, n_channels: int) -> ChannelMap:
    """Parse `--map`.

    Two accepted forms:
      explicit   "audioL=1,audioR=2,light1=3,light4=4"
      positional "AL,AR,L1,L4"   (token i names the role of WAV channel i+1)

    A '!' before the index (explicit) or before the role (positional) inverts
    that channel, for transimpedance front-ends that pull DOWN on light.
    """
    cm = ChannelMap()
    spec = spec.strip()
    if not spec:
        raise ValueError("--map is required; see README for the syntax")
    toks = [t for t in re.split(r"[,;]", spec) if t.strip()]
    explicit = any("=" in t for t in toks)

    taken: dict[int, str] = {}

    def claim(role: str, led: int | None, idx: int, inv: bool):
        if idx < 0 or idx >= n_channels:
            # Say WHICH convention, because 0-based is the single most likely
            # first mistake and "channel 0 but the file has only 4" reads as a
            # complaint about the file rather than about the index.
            raise ValueError(
                f"--map references WAV channel {idx + 1} but the file has only "
                f"{n_channels}. Channels are 1-based, as your interface labels "
                f"them: the first input is 1, not 0.")
        name = role if led is None else f"light{led}"
        if idx in taken:
            # Two roles on one input is silent and destructive: two zones on one
            # sensor makes a genuinely dead zone read as alive (which defeats
            # CHANNEL_NEVER_ON), and two ears on one input makes every binaural
            # beat read as 0 Hz (which manufactures BEAT_FREQ errors).
            raise ValueError(
                f"--map points both {taken[idx]} and {name} at WAV channel "
                f"{idx + 1}. Each input can carry only one role; use '-' to skip "
                f"a channel that carries nothing.")
        taken[idx] = name
        if inv:
            cm.invert.add(idx)
        if role == "audioL":
            cm.audio_l = idx
        elif role == "audioR":
            cm.audio_r = idx
        elif role == "light":
            assert led is not None
            if led in cm.lights:
                raise ValueError(f"--map assigns light{led} twice")
            cm.lights[led] = idx

    if explicit:
        for t in toks:
            # A bare skip token is allowed even in the explicit form: it is how
            # you say "channel 2 exists but carries nothing".
            if t.strip().lower() in ("-", "x", "skip", "unused", "none"):
                continue
            if "=" not in t:
                raise ValueError(f"--map mixes explicit and positional forms at '{t}'")
            lhs, rhs = t.split("=", 1)
            role, led = _norm_role(lhs)
            if role is None:
                continue
            rhs = rhs.strip()
            inv = rhs.startswith("!")
            if inv:
                rhs = rhs[1:]
            if not rhs.isdigit():
                raise ValueError(f"--map value '{rhs}' is not a channel number")
            claim(role, led, int(rhs) - 1, inv)
    else:
        for i, t in enumerate(toks):
            t = t.strip()
            inv = t.startswith("!")
            if inv:
                t = t[1:]
            role, led = _norm_role(t)
            if role is None:
                continue
            claim(role, led, i, inv)

    if cm.audio_l is None and cm.audio_r is None and not cm.lights:
        raise ValueError("--map assigned no usable channels")
    return cm


# ---------------------------------------------------------------------------
# Filter design
# ---------------------------------------------------------------------------


def design_lowpass(fs: float, fc: float, taps: int) -> np.ndarray:
    """Hamming-windowed sinc, normalised to unity DC gain.

    Odd tap count -> exactly symmetric -> exactly linear phase -> a group delay
    of (taps-1)/2 samples that we can subtract without biasing rise vs fall.
    """
    if taps % 2 == 0:
        taps += 1
    n = np.arange(taps) - (taps - 1) / 2.0
    h = np.sinc(2.0 * fc / fs * n) * np.hamming(taps)
    s = h.sum()
    return (h / s) if s != 0 else h


def rolling_median(a: np.ndarray, k: int) -> np.ndarray:
    """Edge-padded rolling median, used to find the LOCAL SCALE of a sequence.

    Only ever used here to decide which intervals are plausible, never as the
    final estimate -- see rolling_mean for why.
    """
    if a.size == 0:
        return a
    k = min(k if k % 2 else k + 1, a.size if a.size % 2 else a.size - 1)
    if k < 3:
        return a.astype(np.float64)
    pad = k // 2
    ap = np.pad(a.astype(np.float64), pad, mode="edge")
    sw = np.lib.stride_tricks.sliding_window_view(ap, k)
    return np.median(sw, axis=1)


def interp_gapped(grid: np.ndarray, xp: np.ndarray, fp: np.ndarray,
                  min_gap_s: float = 0.5, span_mult: float = 25.0) -> np.ndarray:
    """np.interp, but NaN wherever it would have to invent data across a gap.

    WHY THIS EXISTS. Every light observable is sampled ONCE PER FLICKER CYCLE,
    so wherever the LED is too dim to produce detectable edges there are simply
    no samples. Plain np.interp bridges such a hole with a straight line between
    whatever lies on either side, and a straight line through a hole is a
    FABRICATED MEASUREMENT.

    This was not hypothetical. With a sync marker at 100% brightness followed by
    a 15 s ramp-in from 0%, the ramp produced no detectable edges for its first
    few seconds, so np.interp drew a line down from the marker's level and the
    tool reported a level of 1.12 where the session demanded 15% brightness --
    a 7x error that then surfaced as a "brightness was +10.8 dB from the
    expected profile" warning on a bit-perfect synthetic render. The comparator
    had no way to know the number was imaginary.

    An instrument must say "I did not measure this" rather than guess, so a hole
    wider than `max(min_gap_s, span_mult * median_spacing)` becomes NaN and
    every consumer downstream treats it as unavailable.
    """
    grid = np.asarray(grid, dtype=np.float64)
    xp = np.asarray(xp, dtype=np.float64)
    fp = np.asarray(fp, dtype=np.float64)
    if xp.size == 0:
        return np.full(grid.size, np.nan)
    out = np.interp(grid, xp, fp, left=np.nan, right=np.nan)
    if xp.size < 2:
        return out
    d = np.diff(xp)
    med = float(np.median(d)) if d.size else 0.0
    gap = max(min_gap_s, span_mult * med if med > 0 else 0.0)
    # For each grid point, the width of the source interval that encloses it.
    j = np.clip(np.searchsorted(xp, grid, side="right") - 1, 0, xp.size - 2)
    out[d[j] > gap] = np.nan
    return out


def rolling_mean(a: np.ndarray, k: int) -> np.ndarray:
    """Edge-padded rolling mean over an EVEN window.

    WHY MEAN, AND WHY EVEN -- this is a real bug that a median-based estimator
    walked straight into. The LED's intensity PWM (several hundred Hz) is not
    an integer multiple of the flicker rate, so the flicker edge lands at a
    different PWM phase on alternate cycles and the measured periods ALTERNATE
    (at 20 Hz flicker against a 430 Hz carrier: 49.16, 50.84, 49.16, ... ms).
    Their mean is exact to 1 part in 10^6, but a median of an odd window just
    picks whichever branch is in the majority -- so a median-based rate came
    out 1.4% low and reported a phantom "flicker rate wrong" finding.

    An EVEN window cancels a period-2 alternation exactly. Outliers are handled
    before this, by excluding implausible intervals outright.
    """
    a = np.asarray(a, dtype=np.float64)
    if a.size == 0:
        return a
    k = max(2, k - (k % 2))                     # force even
    k = min(k, a.size if a.size % 2 == 0 else a.size - 1)
    if k < 2:
        return a
    ap = np.pad(a, (k // 2, k // 2), mode="edge")
    c = np.concatenate([[0.0], np.cumsum(ap)])
    return (c[k:k + a.size] - c[:a.size]) / k


# ---------------------------------------------------------------------------
# Light channel tracker
# ---------------------------------------------------------------------------


class LightTracker:
    """Streaming envelope + edge extraction for one optical channel."""

    def __init__(self, sr: float, led_channel: int, grid: np.ndarray,
                 lp_hz: float = 200.0, invert: bool = False,
                 abs_floor: float = 0.002):
        self.sr = sr
        self.led_channel = led_channel
        self.grid = grid
        self.invert = invert
        self.abs_floor = abs_floor

        # Stage 1: boxcar decimate so the FIR runs at a rate where a modest tap
        # count already gives a sharp transition. 10 samples per cutoff period
        # keeps the boxcar's own droop at the cutoff below 1.5%.
        self.M = max(1, int(sr // max(1.0, 10.0 * lp_hz)))
        self.fs1 = sr / self.M
        # 4 cutoff periods of taps -> transition width ~= fs1/taps*4 ~= lp_hz.
        taps = int(4 * self.fs1 / lp_hz) | 1
        self.h = design_lowpass(self.fs1, lp_hz, taps)
        self.taps = self.h.size
        self.half = (self.taps - 1) // 2
        self.lp_hz = lp_hz

        self._dec_carry = np.zeros(0, dtype=np.float64)
        self._fir_state = np.zeros(self.taps - 1, dtype=np.float64)
        self._dec_emitted = 0

        # Threshold estimation works on a FIXED TIME window, not on whatever
        # the WAV reader handed us. Deriving the on/off levels from the read
        # block would couple detection quality to a memory-tuning knob: at a
        # 1.5 s block a 0.5 Hz flicker has less than one cycle in view, so the
        # 5th/95th percentiles would describe one phase of the flicker rather
        # than its two levels. 4 s covers >=2 cycles down to 0.5 Hz.
        self.thr_win_s = 4.0
        self._pend_t = np.zeros(0, dtype=np.float64)
        self._pend_y = np.zeros(0, dtype=np.float64)

        self.state = False          # current logical on/off
        self._prev_v: float | None = None
        self._prev_t: float | None = None
        self._lo = None
        self._hi = None
        self._mid = None

        self.rises: list[float] = []
        self.falls: list[float] = []
        # PER-CYCLE ON and OFF levels, sampled once per flicker cycle.
        #
        # The obvious brightness proxy -- the mean envelope in each 50 ms grid
        # cell -- is wrong, and wrong in a way that produces confident
        # nonsense: for a 10 Hz 50%-duty flicker a 50 ms cell covers half a
        # period, so the cell mean swings with the FLICKER PHASE, not with
        # brightness. That oscillation read as a +/-2 dB brightness error and
        # fired BRIGHTNESS findings on a perfectly healthy recording.
        #
        # The right estimator is the mean of the envelope over each ON
        # interval, which is exactly "how bright was the LED while it was lit",
        # sampled at the flicker rate.
        self.on_t: list[float] = []
        self.on_v: list[float] = []
        self.off_v: list[float] = []
        self._seg_sum = 0.0
        self._seg_cnt = 0
        # Per-grid-cell accumulators (sum / count / running min & max) so the
        # 60-minute level and contrast series cost 4 x n_grid floats, not the
        # whole envelope.
        n = grid.size
        self._sum = np.zeros(n)
        self._cnt = np.zeros(n)
        self._min = np.full(n, np.inf)
        self._max = np.full(n, -np.inf)
        self.grid_hz = 1.0 / (grid[1] - grid[0]) if grid.size > 1 else 1.0

    # -- streaming -------------------------------------------------------

    def push(self, x: np.ndarray):
        if self.invert:
            x = -x
        x = np.concatenate([self._dec_carry, x.astype(np.float64)])
        n_full = (x.size // self.M) * self.M
        self._dec_carry = x[n_full:]
        if n_full == 0:
            return
        d = x[:n_full].reshape(-1, self.M).mean(axis=1)

        buf = np.concatenate([self._fir_state, d])
        y = np.convolve(buf, self.h, mode="valid")
        self._fir_state = buf[-(self.taps - 1):] if self.taps > 1 else buf[:0]

        # y[i] is the filtered value AT decimated index (dec_emitted + i - half):
        # 'valid' convolution aligns output i with the window centre, and the
        # window is offset by the (taps-1) samples of carried state.
        g0 = self._dec_emitted - self.half
        idx = g0 + np.arange(y.size)
        t = (idx * self.M + (self.M - 1) / 2.0) / self.sr
        self._dec_emitted += d.size

        keep = idx >= 0
        if not keep.all():
            y, t = y[keep], t[keep]
        if y.size == 0:
            return

        self._accumulate_grid(t, y)
        # Edge detection runs on fixed-length chunks (see thr_win_s).
        self._pend_t = np.concatenate([self._pend_t, t])
        self._pend_y = np.concatenate([self._pend_y, y])
        chunk = int(self.thr_win_s * self.fs1)
        while self._pend_y.size >= chunk:
            self._detect_edges(self._pend_t[:chunk], self._pend_y[:chunk])
            self._pend_t = self._pend_t[chunk:]
            self._pend_y = self._pend_y[chunk:]

    def flush(self):
        """Process the final partial chunk. Must be called before finalize()."""
        if self._pend_y.size > 8:
            self._detect_edges(self._pend_t, self._pend_y)
        self._pend_t = self._pend_t[:0]
        self._pend_y = self._pend_y[:0]

    def _accumulate_grid(self, t: np.ndarray, y: np.ndarray):
        cell = np.floor(t * self.grid_hz + 0.5).astype(np.int64)
        np.clip(cell, 0, self.grid.size - 1, out=cell)
        lo, hi = int(cell[0]), int(cell[-1])
        local = cell - lo
        width = hi - lo + 1
        self._sum[lo:hi + 1] += np.bincount(local, weights=y, minlength=width)[:width]
        self._cnt[lo:hi + 1] += np.bincount(local, minlength=width)[:width]
        # np.minimum.at / maximum.at are the only correct way to scatter-reduce
        # extrema; they are slow-ish but run on the decimated rate, not the
        # sample rate, so the cost is ~1/20th of the file.
        np.minimum.at(self._min, cell, y)
        np.maximum.at(self._max, cell, y)

    def _detect_edges(self, t: np.ndarray, y: np.ndarray):
        # Thresholds from this block's own distribution. Percentiles (not
        # min/max) so one ADC glitch cannot set the scale.
        p05, p50, p95 = np.percentile(y, [5.0, 50.0, 95.0])
        contrast = float(p95 - p05)
        if contrast >= self.abs_floor:
            lo_n = p05 + 0.40 * contrast
            hi_n = p05 + 0.60 * contrast
            mid_n = p05 + 0.50 * contrast
            # Light smoothing across blocks keeps a slow baseline drift (the
            # AC-coupling of an audio input, or ambient light changing) from
            # jolting the trigger at every block boundary.
            #
            # ...but a STEP in contrast is not a drift, and blending across one
            # blinds the detector. The README's own sync-marker recipe mandates
            # a 100%-brightness burst, and 384 of the 665 positive-brightness
            # LED entries in sessions/library sit at or below 55% because of the
            # photosensitivity caps -- so on essentially every real capture the
            # block after the marker carries a body 2-10x dimmer, and an
            # EMA-blended `hi` derived half from the MARKER's contrast is above
            # anything the body ever reaches. MEASURED on a marker render of
            # 07_genus_40hz's 45% body: 1401 ms / 56 real cycles of 40 Hz
            # flicker produced no edges at all, and the comparator turned that
            # into four EVENT_MISSING errors on a bit-perfect recording.
            #
            # So: blend while the scale is comparable, JUMP when it is not.
            # ...and the TEST is whether the blended trigger is still REACHABLE
            # in this block, not how much the contrast moved. A brightness LFO
            # (the `~a:b:period` idiom 31 of the 48 non-RGB shipped sessions
            # use) swings the contrast by up to 2.75x between adjacent 4 s
            # blocks, and a reset inside one of those moves the within-cycle
            # crossing position of a SMOOTH carrier at every block boundary:
            # MEASURED on 91_measure_17_uniform, a contrast-ratio reset put the
            # light clock fit at +221 ppm on a zero-drift render (against
            # +76 ppm with no reset at all) and raised AV_DIFFERENTIAL_DRIFT.
            # Testing reachability instead leaves every LFO block blended and
            # only jumps where the blend would emit no edges at all.
            if self._lo is None:
                self._lo, self._hi, self._mid = lo_n, hi_n, mid_n
            else:
                a = 0.5
                b_lo = a * lo_n + (1 - a) * self._lo
                b_hi = a * hi_n + (1 - a) * self._hi
                b_mid = a * mid_n + (1 - a) * self._mid
                if b_hi >= p95 or b_lo <= p05:
                    # Unreachable: every sample in this block is below the
                    # blended upper trigger (or above the lower one), so the
                    # blend would report NO transitions. That is the
                    # marker-to-body step the README recipe guarantees.
                    self._lo, self._hi, self._mid = lo_n, hi_n, mid_n
                else:
                    self._lo, self._hi, self._mid = b_lo, b_hi, b_mid
        if self._lo is None:
            self._prev_v, self._prev_t = float(y[-1]), float(t[-1])
            return
        if contrast < self.abs_floor:
            # Flat block: no edges, and do not let the stale trigger invent any.
            self.state = False
            self._prev_v, self._prev_t = float(y[-1]), float(t[-1])
            return

        hi, lo, mid = self._hi, self._lo, self._mid
        # Hysteresis decides WHETHER a transition happened; the interpolated
        # mid-level crossing decides WHEN. Using the hysteresis level as the
        # timestamp would bias rising edges late and falling edges early, i.e.
        # it would systematically under-report duty cycle.
        above = y >= hi
        below = y <= lo
        ev = np.zeros(y.size, dtype=np.int8)
        ev[above] = 1
        ev[below] = -1
        nz = np.nonzero(ev)[0]
        if nz.size == 0:
            self._prev_v, self._prev_t = float(y[-1]), float(t[-1])
            return

        prev_v = self._prev_v if self._prev_v is not None else float(y[0])
        prev_t = self._prev_t if self._prev_t is not None else float(t[0])
        cur = self.state
        # One cumulative sum per block lets every ON/OFF segment mean be a
        # single subtraction, including segments that span a block boundary
        # (carried in _seg_sum/_seg_cnt).
        cy = np.concatenate([[0.0], np.cumsum(y)])
        seg_base = 0
        for k in nz:
            s = ev[k] > 0
            if s == cur:
                continue
            # Finalise the segment that is ending at k.
            tot = self._seg_sum + float(cy[k] - cy[seg_base])
            cnt = self._seg_cnt + (k - seg_base)
            if cnt > 0:
                mean = tot / cnt
                if cur:                       # the segment that ended was ON
                    self.on_v.append(mean)
                    self.on_t.append(float(t[k]))
                else:
                    self.off_v.append(mean)
            self._seg_sum, self._seg_cnt = 0.0, 0
            seg_base = int(k)
            cur = s
            # Walk back to the last sample on the other side of mid, then
            # linearly interpolate the mid crossing.
            j = k
            while j > 0 and ((y[j] >= mid) == s):
                j -= 1
            if j == k:
                v0, t0 = prev_v, prev_t
            else:
                v0, t0 = float(y[j]), float(t[j])
            v1, t1 = float(y[k]), float(t[k])
            if v1 != v0:
                f = (mid - v0) / (v1 - v0)
                f = min(1.0, max(0.0, f))
                tc = t0 + f * (t1 - t0)
            else:
                tc = t1
            (self.rises if s else self.falls).append(tc)
        self.state = cur
        self._seg_sum += float(cy[-1] - cy[seg_base])
        self._seg_cnt += int(y.size - seg_base)
        self._prev_v, self._prev_t = float(y[-1]), float(t[-1])

    # -- finalise --------------------------------------------------------

    def finalize(self, global_contrast: float) -> ObservedLight:
        """`global_contrast` is the shared normaliser (see observe_wav)."""
        n = self.grid.size
        with np.errstate(invalid="ignore", divide="ignore"):
            cell_mean = np.where(self._cnt > 0,
                                 self._sum / np.maximum(self._cnt, 1), np.nan)
        cell_span = np.where(np.isfinite(self._max) & np.isfinite(self._min),
                             self._max - self._min, 0.0)
        scale = global_contrast if global_contrast > 0 else 1.0

        # Primary: per-cycle ON level and ON-minus-OFF modulation depth,
        # resampled onto the grid. Both are sampled once per flicker cycle, so
        # they carry no flicker-phase artefact at all.
        on_t = np.asarray(self.on_t, dtype=np.float64)
        on_v = np.asarray(self.on_v, dtype=np.float64)
        off_v = np.asarray(self.off_v, dtype=np.float64)
        level = np.full(n, np.nan)
        contrast = np.full(n, np.nan)
        if on_t.size >= 3:
            lv = rolling_mean(on_v, 10)      # kill the PWM-phase ripple
            level = interp_gapped(self.grid, on_t, lv)
            k = min(on_v.size, off_v.size)
            if k >= 3:
                depth = rolling_mean(on_v[:k] - off_v[:k], 10)
                contrast = interp_gapped(self.grid, on_t[:k], depth)
        # Fallback for a region with no edges -- but ONLY where the channel is
        # genuinely DC, never merely too dim to detect.
        #
        # `level` means "the ON-phase level". `cell_mean` means "the average
        # over the whole grid cell", and for a FLICKERING channel those differ
        # by the duty factor (~2x at 50%). Substituting one for the other
        # wherever edge detection failed put two incompatible quantities into
        # one series: during a dim ramp-in the fallback read ~1/1.9 of the
        # per-cycle scale, and since the BRIGHTNESS check fits a single sensor
        # gain across the session, that step was reported as a real +10.8 dB
        # brightness error on a flawless render.
        #
        # A DC-on channel (reachable by a freq RAMP to 0,
        # led_matrix_example.c:723-779) has a near-constant envelope, so its
        # span is a small fraction of its mean. A dim but flickering channel
        # swings fully on and off, so its span is ~1/duty of its mean. That
        # ratio separates the two cases cleanly; anything else stays NaN.
        base = float(np.nanmin(cell_mean)) if n and np.isfinite(cell_mean).any() else 0.0
        with np.errstate(invalid="ignore", divide="ignore"):
            dc_like = cell_span < 0.25 * np.maximum(cell_mean, 1e-12)
        level = np.where(np.isfinite(level), level,
                         np.where(dc_like, cell_mean - base, np.nan))
        # `contrast` is a span either way, so the cell span IS the right
        # fallback for it -- and "contrast ~ 0" is what the darkness test needs.
        contrast = np.where(np.isfinite(contrast), contrast, cell_span)
        level = np.clip(level / scale, 0.0, 4.0)
        contrast = contrast / scale

        rises = np.asarray(sorted(self.rises), dtype=np.float64)
        falls = np.asarray(sorted(self.falls), dtype=np.float64)
        freq = np.full(n, np.nan)
        duty = np.full(n, np.nan)
        if rises.size >= 4:
            rt = rises[:-1]
            per = np.diff(rises)
            # Step 1: local SCALE from a median, so "plausible" is defined
            # relative to the rate actually in force (sessions ramp rates).
            med = rolling_median(per, 9)
            # Step 2: keep only intervals that look like exactly ONE cycle. A
            # missed edge gives ~2x and a spurious edge gives two short ones;
            # excluding both is better than trying to repair them, because an
            # excluded interval costs precision while a mis-repaired one costs
            # correctness.
            ok = (per > 0.55 * med) & (per < 1.75 * med) & (per > 0)
            # Step 3: EVEN-window mean -- see rolling_mean's docstring.
            if ok.sum() >= 4:
                rt_ok, per_ok = rt[ok], per[ok]
                per_s = rolling_mean(per_ok, 10)
                freq = interp_gapped(self.grid, rt_ok, 1.0 / per_s)
                # Duty: for each kept rise, the next fall after it.
                if falls.size:
                    k = np.searchsorted(falls, rt_ok, side="right")
                    valid = k < falls.size
                    on = np.full(rt_ok.size, np.nan)
                    on[valid] = falls[k[valid]] - rt_ok[valid]
                    d = 100.0 * on / per_ok
                    dok = np.isfinite(d) & (d > 0) & (d < 100.0)
                    if dok.sum() >= 4:
                        d_s = rolling_mean(d[dok], 10)
                        duty = interp_gapped(self.grid, rt_ok[dok], d_s)
        # DOES THIS SENSOR SEE ANY LIGHT AT ALL? An absolute question, answered
        # against this channel's own raw span and the front-end's declared noise
        # floor -- never against the other channels, because when every mapped
        # channel is dark the brightest of them IS the noise floor.
        signal, peak, n_cells = self.saw_signal()
        note = (f"lp={self.lp_hz:g}Hz M={self.M} taps={self.taps}"
                f"{' inverted' if self.invert else ''}")
        if not signal:
            note += (f"; NO OPTICAL SIGNAL: the largest peak-to-peak in any "
                     f"50 ms cell of the whole recording was {peak:.3g}, against "
                     f"a {self.abs_floor:g} sensor floor ({n_cells} cells "
                     f"cleared it, 3 are needed), so this channel carries noise "
                     f"only and cannot contribute a timing reference")
        return ObservedLight(
            channel=self.led_channel, t_rec=self.grid, level=level,
            contrast=contrast, freq_hz=freq, duty_pct=duty,
            edges_rise=rises, edges_fall=falls, available=bool(signal),
            note=note)

    def _cell_span(self) -> np.ndarray:
        return np.where(np.isfinite(self._max) & np.isfinite(self._min),
                        self._max - self._min, 0.0)

    def raw_contrast(self) -> float:
        """Robust scale of this channel's optical modulation (99th pct)."""
        span = self._cell_span()
        if span.size == 0:
            return 0.0
        return float(np.percentile(span, 99.0))

    def saw_signal(self) -> tuple[bool, float, int]:
        """Did this sensor see ANY optical signal, anywhere in the recording?

        A COUNT OF CELLS, not a percentile, because the two questions are
        different. `raw_contrast` is a SCALE and must be robust to a single ADC
        glitch, so it uses the 99th percentile -- but a 600 ms sync marker on an
        otherwise-DC channel is 12 cells out of 2440, far below the 99th
        percentile, so asking the scale whether the sensor works answered "no"
        for a perfectly good recording of 05_focus_smr (whose LEDs are a steady
        lamp, freq 0, plus the marker burst). Counting cells that clear the
        absolute floor is sensitive to a short burst and still needs three
        consecutive-ish cells before it believes one.
        """
        span = self._cell_span()
        n = int(np.count_nonzero(span >= self.abs_floor))
        peak = float(span.max()) if span.size else 0.0
        return n >= 3, peak, n


# ---------------------------------------------------------------------------
# Audio tracker
# ---------------------------------------------------------------------------


def _refine_frequency(x: np.ndarray, sr: float, f0: float,
                      n_sub: int = 8) -> float:
    """Phase-slope refinement of a coarse FFT peak.

    Demodulating by exp(-i2*pi*f0*t) turns a tone at f0+d into a complex
    exponential at d, so the unwrapped phase of successive sub-block means is a
    straight line with slope 2*pi*d. A least-squares fit of that line gets to
    ~0.02 Hz on a 0.25 s window -- two orders of magnitude better than the 4 Hz
    FFT bin, which is what makes a beat-frequency measurement possible at all.

    Returns NaN when the sub-blocks are too short to average away the image at
    -2*f0; a wrong answer here would corrupt the beat measurement, and NaN at
    least tells the truth.

    THE SUB-BLOCK LENGTH IS SNAPPED TO A WHOLE NUMBER OF CARRIER PERIODS, and
    that is the difference between a 50 ppm bias and a 0.4 ppm one. The
    sub-block mean of x*exp(-i2*pi*f0*t) keeps a residual at -2*f0 (and at every
    harmonic k*f0 the waveform carries -- the library uses `wave 7`, an
    EEG-contour shape, on 180 of its audio entries) whose magnitude is
    1/(2*pi*C) for C carrier cycles in the block: 2.5% at the 6.25 cycles a
    0.25 s window / 8 sub-blocks gives on a 200 Hz carrier. Each residual
    appears as a phase ripple whose sign alternates or rotates between
    sub-blocks, and such a sequence is NOT orthogonal to a straight line once
    the sub-blocks are magnitude-weighted, so it tilts the fit. Choosing `sub`
    so that f0*sub/sr is an INTEGER makes the geometric sum of every one of
    those terms vanish at once (half-integer snapping only kills the even ones):
    the residual drops to the sample-rounding error, ~3.6e-4 relative.

    MEASURED on full-length clean renders through the shipped pipeline, carrier
    200 Hz against a 200.000062 Hz truth recovered by a 40 s FFT:
      15_ganzflicker_imagery  old -51.8 ppm   new +0.40 ppm
      30_splitfield_lucid     old -60.0 ppm   new +0.31 ppm   (sd 0.43 mHz -> 6.5 uHz)
    Those tens of ppm were the whole of `clock_ppm_audio` on three shipped
    sessions and produced "[ERROR] AV_DIFFERENTIAL_DRIFT: ... the AUDIO
    timebase is the one that moved -- look at the I2S fractional divider" on
    bit-perfect recordings. The bias is a property of the ESTIMATOR, not of the
    signal, which is why it was almost the same number (+57 ppm) on three
    unrelated sessions, and why no amount of threshold widening would have been
    the right answer.
    """
    n = x.size
    if f0 <= 0 or n < 64:
        return float("nan")
    # THE SUB-BLOCK COUNT IS DERIVED FROM THE CARRIER, not fixed at 8. Each
    # sub-block needs >= 4 carrier cycles for the negative-frequency image to
    # average out, so a fixed 8 blocks silently gave up on every carrier below
    # ~140 Hz at a 0.25 s window -- and the fallback is the raw FFT peak, whose
    # parabolic interpolation error is ~1% of a 4 Hz bin. MEASURED on
    # 20_gateway_focus10 (binaural 100 / 101.5 Hz): the left ear read +0.4 ppm
    # because 100 Hz lands exactly on a bin, while the right ear read
    # -540 ppm -- 0.055 Hz of pure interpolation error -- and that fed
    # "clock_ppm_audio -275 ppm" and an AV_DIFFERENTIAL_DRIFT of -290 ppm on a
    # clean recording. The library puts 112 audio entries on 100-104 Hz.
    cycles = f0 * n / sr
    if cycles < 16.0:
        return float("nan")
    n_sub = max(4, min(int(n_sub), int(cycles // 6)))
    # Snap the sub-block to an integer number of carrier periods (see above),
    # staying as close as possible to n/n_sub.
    per = sr / f0
    m = int(round((n / n_sub) / per)) if per > 0 else 0
    sub = int(round(m * per)) if m >= 1 else (n // n_sub)
    if sub < 8:
        return float("nan")
    n_sub = n // sub
    if n_sub < 4:
        return float("nan")
    if f0 * sub / sr < 4.0:
        # Fewer than ~4 carrier cycles per sub-block: the negative-frequency
        # image does not average out and the phase fit is meaningless.
        return float("nan")
    t = np.arange(n_sub * sub, dtype=np.float64) / sr
    ref = np.exp(-2j * np.pi * f0 * t)
    z = (x[: n_sub * sub].astype(np.float64) * ref).reshape(n_sub, sub).mean(axis=1)
    mag = np.abs(z)
    if mag.min() <= 0 or mag.max() <= 0:
        return float("nan")
    ph = np.unwrap(np.angle(z))
    tc = (np.arange(n_sub) + 0.5) * sub / sr
    # Weight by magnitude so a gated carrier's quiet sub-blocks do not dominate.
    w = mag / mag.sum()
    tm = float((w * tc).sum())
    pm = float((w * ph).sum())
    den = float((w * (tc - tm) ** 2).sum())
    if den <= 0:
        return float("nan")
    slope = float((w * (tc - tm) * (ph - pm)).sum()) / den
    d = slope / (2.0 * np.pi)
    if abs(d) > sr / 4.0:
        return float("nan")
    return f0 + d


class AudioTracker:
    def __init__(self, sr: float, grid: np.ndarray, has_l: bool, has_r: bool,
                 spec_win_s: float = 0.25, spec_hop_s: float = 0.2,
                 min_tone_hz: float = 30.0, tone_prom_db: float = 9.0,
                 pulse_win_s: float = 4.0,
                 sync_tone_hz: float | None = None):
        self.sr = sr
        self.grid = grid
        self.grid_hz = 1.0 / (grid[1] - grid[0]) if grid.size > 1 else 1.0
        self.has_l, self.has_r = has_l, has_r
        self.win = max(64, int(spec_win_s * sr))
        self.hop = max(1, int(spec_hop_s * sr))
        self.min_tone_hz = min_tone_hz
        self.tone_prom_db = tone_prom_db
        self.window_fn = np.hanning(self.win)
        self.sync_tone_hz = sync_tone_hz

        n = grid.size
        self._e_l = np.zeros(n)
        self._e_r = np.zeros(n)
        self._cnt = np.zeros(n)

        self._tail_l = np.zeros(0)
        self._tail_r = np.zeros(0)
        self._tail_start = 0        # global frame index of tail[0]
        self._next_win = 0          # global frame index of the next window

        self.spec_t: list[float] = []
        self.spec_fl: list[float] = []
        self.spec_fr: list[float] = []
        self.spec_pl: list[float] = []
        self.spec_pr: list[float] = []

        # Fine broadband level, for ONSET TIMING only. 1 kHz float32 is 14 MB
        # for a 60-minute recording -- the one array in the whole tool that
        # scales with file length, and worth it: without it an audio onset is
        # quantised to the 50 ms grid, which biases it ~10 ms early and
        # manufactures "audio leads light" findings.
        self.fine_fs = 1000.0
        self.fine_M = max(1, int(round(sr / self.fine_fs)))
        self.fine_fs = sr / self.fine_M
        n_fine = int(math.ceil(grid[-1] * self.fine_fs)) + 8 if grid.size else 8
        self._fine = np.zeros(n_fine, dtype=np.float32)
        self._fine_carry = np.zeros(0)
        self._fine_emitted = 0

        # Pulse (AM) detection runs on the SAME 1 ms mean-square boxes as
        # `_fine`, square-rooted back to an amplitude envelope.
        #
        # WHY NOT ITS OWN DECIMATION, which is what this did: a boxcar average
        # of |x| by sr//500 = 88 gives an envelope rate of 501.136 Hz, and the
        # rectified carrier's 2f component survives the boxcar's sinc and
        # ALIASES straight into the 0.4-150 Hz search band. It is not a subtle
        # effect: a 300 Hz carrier put a peak at |600 - 501.136| = 98.86 Hz,
        # and `analyze` reported "audio pulse rate was 98.845 Hz where 10.000
        # Hz was expected" as a hard ERROR on a bit-perfect render of
        # 05_focus_smr. Across the shipped library 940 of 1381 audio entries
        # sit on a carrier whose 2f aliases into that band (200 Hz x272,
        # 250 x131, 300 x80, 204/208 x112, 220 x43).
        #
        # The 1 ms box is a 44-sample boxcar at 44.1 kHz, so its nulls sit at
        # multiples of 1002.27 Hz and its Nyquist is 501.1 Hz. Every carrier in
        # the library puts 2f at 400-600 Hz, i.e. BELOW that Nyquist, so there
        # is no fold at all; the residual leakage near the 501 Hz edge is
        # -29 dB and cannot outrun a real 10%-depth AM.
        #
        # sqrt() AFTER the decimation, not before: squaring is the demodulation
        # whose product has to be filtered, and sqrt is a static map applied to
        # the already-bandlimited series, so it adds no new aliasing. An
        # amplitude (not energy) envelope also keeps `_pulse_of`'s relative
        # depth calibrated the same way the old |x| envelope was, because
        # mean|x| and RMS are both proportional to the carrier amplitude.
        self.pulse_fs = self.fine_fs
        self.pulse_len = max(32, int(pulse_win_s * self.fine_fs))
        self._pbuf = np.zeros(0)
        self._p_emitted = 0
        self.pulse_t: list[float] = []
        self.pulse_hz: list[float] = []
        self.pulse_depth: list[float] = []
        self._p_next = 0            # next pulse-analysis index

        # Sync-marker narrowband envelope, on the SAME 1 ms boxes as _fine.
        #
        # WHY NOT THE 20 Hz GRID (which is what this used to do): the sync
        # marker exists to pin t0 MORE precisely than the generic onset
        # detector, and it is the method the README tells people to use. Timing
        # its onset on the 50 ms analysis grid made it strictly WORSE than the
        # 1 ms broadband series it bypasses -- it pushed the measured audio
        # origin ~3.5 ms late, and every audio event afterwards was then
        # reported as ~7 ms EARLY. That is the exact failure this instrument
        # must not have: the recommended, highest-precision path was the one
        # manufacturing false findings on a clean recording.
        #
        # Boxing the COHERENT demodulate (mean of z, then magnitude) and only
        # then taking |.| keeps the box a narrowband filter: at a 3 kHz marker
        # tone a 1 ms box spans 3 cycles, so the box-mean has its first null at
        # 1 kHz offset -- more than narrow enough to reject the session's
        # 240 Hz carriers while still resolving the onset to one millisecond.
        self.sync_fs = self.fine_fs
        self._sync_mag = np.zeros(n_fine, dtype=np.float32)
        self._sync_carry = np.zeros(0, dtype=np.complex128)
        self._sync_emitted = 0

    # -- streaming -------------------------------------------------------

    def push(self, start_frame: int, xl: np.ndarray | None, xr: np.ndarray | None):
        ref = xl if xl is not None else xr
        assert ref is not None
        n = ref.size
        t = (start_frame + np.arange(n)) / self.sr
        cell = np.floor(t * self.grid_hz + 0.5).astype(np.int64)
        np.clip(cell, 0, self.grid.size - 1, out=cell)
        lo, hi = int(cell[0]), int(cell[-1])
        local = cell - lo
        width = hi - lo + 1
        self._cnt[lo:hi + 1] += np.bincount(local, minlength=width)[:width]
        if xl is not None:
            self._e_l[lo:hi + 1] += np.bincount(
                local, weights=xl.astype(np.float64) ** 2, minlength=width)[:width]
        if xr is not None:
            self._e_r[lo:hi + 1] += np.bincount(
                local, weights=xr.astype(np.float64) ** 2, minlength=width)[:width]

        if self.sync_tone_hz:
            # Narrowband energy at the sync-marker tone. A complex demodulation
            # against ABSOLUTE time keeps the reference phase continuous across
            # block boundaries, which is what lets the per-box coherent average
            # below act as a stable narrowband filter rather than a phase-
            # scrambled one.
            mono = xl if xl is not None else xr
            rf = np.exp(-2j * np.pi * self.sync_tone_hz * t)
            self._push_sync(mono.astype(np.float64) * rf)

        fine_block = self._push_fine(xl, xr)
        self._push_spectral(start_frame, xl, xr)
        if fine_block is not None and fine_block.size:
            self._push_pulse(np.sqrt(np.maximum(fine_block, 0.0)))

    def _push_fine(self, xl, xr):
        """Mean-square in 1 ms boxes. Mean-SQUARE, not RMS, because the box
        average of energy is LINEAR in the active fraction of the box, so
        linear interpolation across the box containing a step recovers the step
        time exactly. Interpolating RMS would not.

        Returns the newly completed boxes so the pulse detector can share this
        one properly anti-aliased decimation instead of rolling its own.
        """
        parts = [a.astype(np.float64) ** 2 for a in (xl, xr) if a is not None]
        e = parts[0] if len(parts) == 1 else 0.5 * (parts[0] + parts[1])
        x = np.concatenate([self._fine_carry, e])
        nf = (x.size // self.fine_M) * self.fine_M
        self._fine_carry = x[nf:]
        if nf == 0:
            return None
        d = x[:nf].reshape(-1, self.fine_M).mean(axis=1)
        lo = self._fine_emitted
        hi = min(lo + d.size, self._fine.size)
        if hi > lo:
            self._fine[lo:hi] = d[: hi - lo]
        self._fine_emitted += d.size
        return d

    def _push_sync(self, z: np.ndarray):
        """Coherent 1 ms boxes of the demodulated marker tone, magnitude kept.

        Mirrors _push_fine's carry discipline exactly: boxes are aligned to
        absolute frame 0 and blocks arrive contiguously, so a box that straddles
        a block boundary is completed from the carry rather than being split
        into two short boxes (which would put a spurious notch in the envelope
        at every block edge -- and the onset detector would happily lock onto
        one of those instead of the real marker).
        """
        x = np.concatenate([self._sync_carry, z])
        nf = (x.size // self.fine_M) * self.fine_M
        self._sync_carry = x[nf:]
        if nf == 0:
            return
        d = np.abs(x[:nf].reshape(-1, self.fine_M).mean(axis=1))
        lo = self._sync_emitted
        hi = min(lo + d.size, self._sync_mag.size)
        if hi > lo:
            self._sync_mag[lo:hi] = d[: hi - lo]
        self._sync_emitted += d.size

    def _push_spectral(self, start_frame: int, xl, xr):
        if xl is not None:
            self._tail_l = np.concatenate([self._tail_l, xl.astype(np.float64)])
        if xr is not None:
            self._tail_r = np.concatenate([self._tail_r, xr.astype(np.float64)])
        if self._tail_l.size == 0 and self._tail_r.size == 0:
            return
        avail = (self._tail_l.size if self._tail_l.size else self._tail_r.size)
        while self._next_win + self.win <= self._tail_start + avail:
            off = self._next_win - self._tail_start
            segl = self._tail_l[off:off + self.win] if self._tail_l.size else None
            segr = self._tail_r[off:off + self.win] if self._tail_r.size else None
            tc = (self._next_win + self.win / 2.0) / self.sr
            fl, pl = self._tone(segl)
            fr, pr = self._tone(segr)
            self.spec_t.append(tc)
            self.spec_fl.append(fl)
            self.spec_fr.append(fr)
            self.spec_pl.append(pl)
            self.spec_pr.append(pr)
            self._next_win += self.hop
        drop = self._next_win - self._tail_start
        if drop > 0:
            if self._tail_l.size:
                self._tail_l = self._tail_l[drop:]
            if self._tail_r.size:
                self._tail_r = self._tail_r[drop:]
            self._tail_start += drop

    def _tone(self, seg: np.ndarray | None) -> tuple[float, float]:
        if seg is None or seg.size < self.win:
            return float("nan"), float("nan")
        rms = float(np.sqrt(np.mean(seg ** 2)))
        if rms < 1e-6:
            return float("nan"), float("nan")
        sp = np.abs(np.fft.rfft(seg * self.window_fn))
        freqs = np.fft.rfftfreq(self.win, 1.0 / self.sr)
        k0 = int(np.searchsorted(freqs, self.min_tone_hz))
        if k0 >= sp.size - 2:
            return float("nan"), float("nan")
        band = sp[k0:]
        k = int(band.argmax()) + k0
        peak = float(sp[k])
        med = float(np.median(band)) + 1e-30
        prom = 20.0 * math.log10(peak / med)
        if prom < self.tone_prom_db or k <= 0 or k >= sp.size - 1:
            # No clear carrier: noise waveform, silence, or a dense chord.
            # Returning the argmax here would be reporting a noise peak as a
            # measured carrier.
            return float("nan"), prom
        # Parabolic interpolation on the log magnitude -- the standard
        # unbiased-for-Hann peak estimator.
        a, b, c = (20.0 * math.log10(sp[k - 1] + 1e-30),
                   20.0 * math.log10(peak + 1e-30),
                   20.0 * math.log10(sp[k + 1] + 1e-30))
        den = a - 2.0 * b + c
        delta = 0.0 if den == 0 else 0.5 * (a - c) / den
        delta = max(-0.5, min(0.5, delta))
        f0 = float(freqs[k] + delta * (freqs[1] - freqs[0]))
        f1 = _refine_frequency(seg, self.sr, f0)
        # Reject a refinement that disagrees with the FFT by more than a bin:
        # that means the phase fit locked onto something else.
        if math.isfinite(f1) and abs(f1 - f0) < 1.5 * (freqs[1] - freqs[0]):
            return f1, prom
        return f0, prom

    def _push_pulse(self, env: np.ndarray):
        """`env` is the 1 ms-box amplitude envelope (see __init__ for why)."""
        self._pbuf = np.concatenate([self._pbuf, env])
        hop = max(1, self.pulse_len // 8)
        fs = self.pulse_fs
        while self._p_next + self.pulse_len <= self._p_emitted + self._pbuf.size:
            off = self._p_next - self._p_emitted
            seg = self._pbuf[off:off + self.pulse_len]
            tc = (self._p_next + self.pulse_len / 2.0) / fs
            f, dep = self._pulse_of(seg, fs)
            self.pulse_t.append(tc)
            self.pulse_hz.append(f)
            self.pulse_depth.append(dep)
            self._p_next += hop
        drop = self._p_next - self._p_emitted
        if drop > 0:
            self._pbuf = self._pbuf[drop:]
            self._p_emitted += drop

    @staticmethod
    def _pulse_of(seg: np.ndarray, fs: float) -> tuple[float, float]:
        """Recover the AM rate from the amplitude envelope.

        The timeline hard-codes mod_depth to 0.1 (config_parser.c:2357), so the
        AM we are looking for is only ~10% -- shallow but spectrally clean,
        because it is a single rate. The prominence bar is set accordingly.

        TWO ESTIMATOR BUGS ARE FIXED HERE, both of which produced error-severity
        PULSE_RATE findings on bit-perfect renders of shipped sessions:

        1. THE PEAK IS INTERPOLATED AGAINST THE FULL SPECTRUM, not against the
           search sub-band. The sub-band starts at 0.4 Hz, so a rate whose peak
           lands in the FIRST in-band bin had `0 < k < size-1` fail and got no
           parabolic correction at all -- it was snapped to the bin centre.
           MEASURED on 18_jhana_absorption, whose isochronic pulse ramps 6 -> 0
           Hz: "[ERROR] audio pulse rate was 0.500 Hz where 0.574 Hz was
           expected", i.e. exactly the 0.5 Hz bin of a 4 s window.

        2. THE FUNDAMENTAL IS PREFERRED OVER ITS HARMONICS. A narrow-duty gate
           (the library uses 4%, 10%, 30%, 35%) spreads most of its envelope
           energy into harmonics, and which harmonic happens to be largest in a
           given 4 s window is noise. MEASURED, all on clean or
           noise-only renders: 80.000 Hz where 40.000 was expected (2x,
           27_genus_40hz_dim at -60 dBFS), 119.982 vs 40.000 (3x, 07_genus_40hz
           at -40 dBFS), 36.000 vs 6.000 (6x, 09_lucid_hypnagogic). Walking down
           to the lowest submultiple that still carries a real peak reports the
           rate the device was asked for. It does NOT mask a genuine doubling:
           if the device really emits 80 Hz there is no 40 Hz component to find,
           so 80 Hz is still what comes back.
        """
        m = float(seg.mean())
        if m <= 1e-9 or seg.size < 32:
            return float("nan"), float("nan")
        # LINEAR detrend, not just mean removal. Every shipped session ramps its
        # volume in over 10-30 s and out again, so a 4 s analysis window sitting
        # in a ramp contains a strong linear trend. Subtracting only the mean
        # leaves that trend, whose spectral leakage piles into the lowest bins
        # and beats a 10%-depth AM component hands down -- a 40 Hz isochronic
        # session was being measured as a "0.5 Hz pulse" during its fade-in and
        # fade-out, i.e. a pure artefact of the ramp. Removing the best-fit line
        # costs one degree of freedom and makes the estimate honest there.
        idx = np.arange(seg.size, dtype=np.float64)
        slope, icept = np.polyfit(idx, seg, 1)
        y = (seg - (slope * idx + icept)) * np.hanning(seg.size)
        sp = np.abs(np.fft.rfft(y)) * (2.0 / seg.size)
        fr = np.fft.rfftfreq(seg.size, 1.0 / fs)
        hi_hz = min(150.0, fs / 2.2)
        band = (fr >= 0.4) & (fr <= hi_hz)
        if not band.any():
            return float("nan"), float("nan")
        idx_band = np.nonzero(band)[0]
        sub = sp[band]
        med = float(np.median(sub)) + 1e-30
        BAR = 4.0
        k = int(idx_band[int(sub.argmax())])           # index into the FULL sp
        if sp[k] / med < BAR:
            return float("nan"), float("nan")
        df = float(fr[1] - fr[0])

        # --- walk down to the fundamental -----------------------------
        # The peak may be the n-th harmonic of the real rate. Accept the
        # LOWEST submultiple that is itself a local peak clearing the bar;
        # a harmonic-only spectrum has nothing there and k is kept.
        # A candidate must be a real local peak IN BAND, clear the same
        # prominence bar, and be at least half as tall as the peak it is
        # claimed to be the fundamental of. That last test is the one that
        # matters: for the narrow gates the library uses (4%, 10%, 30%, 35%
        # duty) the Fourier coefficients |2 sin(n*pi*d)/(n*pi)| of the first
        # 1/d harmonics are nearly EQUAL -- 0.0799, 0.0786, 0.0778 for d=0.04 --
        # so which one is the argmax is decided by noise, and a genuine
        # fundamental is always within 2x of it. Without the 0.5 test the walk
        # happily descended into the detrending residue at the bottom of the
        # band and reported "0.236 Hz where 6.000 Hz was expected".
        k_top = k
        top = float(sp[k_top])
        for n in range(2, 13):
            kk = int(round(k_top / float(n)))
            if kk < 1 or kk >= sp.size - 1:
                continue
            lo = max(1, kk - 1)
            hiI = min(sp.size - 2, kk + 1)
            kc = lo + int(np.argmax(sp[lo:hiI + 1]))
            if not (0.4 <= fr[kc] <= hi_hz):
                continue
            if (sp[kc] / med >= BAR and sp[kc] >= 0.5 * top
                    and sp[kc] >= sp[kc - 1] and sp[kc] >= sp[kc + 1]):
                k = kc
        if 0 < k < sp.size - 1:
            a, b, c = sp[k - 1], sp[k], sp[k + 1]
            den = a - 2 * b + c
            delta = 0.0 if den == 0 else 0.5 * (a - c) / den
            delta = max(-0.5, min(0.5, float(delta)))
        else:
            delta = 0.0
        # Envelope AM of depth d on |x| produces a fundamental of amplitude
        # ~d*mean*(2/pi scaling already folded into the rectified mean), so the
        # ratio below is a usable relative depth, not an absolute calibration.
        depth = float(2.0 * sp[k] / m) if m > 0 else float("nan")
        return float(fr[k] + delta * df), depth

    # -- finalise --------------------------------------------------------

    def finalize(self) -> ObservedAudio:
        n = self.grid.size
        cnt = np.maximum(self._cnt, 1.0)
        rms_l = np.sqrt(self._e_l / cnt) if self.has_l else np.full(n, np.nan)
        rms_r = np.sqrt(self._e_r / cnt) if self.has_r else np.full(n, np.nan)
        if self.has_l and self.has_r:
            rms = np.sqrt((self._e_l + self._e_r) / (2.0 * cnt))
        elif self.has_l:
            rms = rms_l
        else:
            rms = rms_r
        rms[self._cnt == 0] = np.nan

        def to_grid(ts: list[float], vs: list[float]) -> np.ndarray:
            if not ts:
                return np.full(n, np.nan)
            a = np.asarray(ts)
            b = np.asarray(vs)
            ok = np.isfinite(b)
            if ok.sum() < 2:
                return np.full(n, np.nan)
            # Interpolate only INSIDE the measured span and only across short
            # NaN gaps; a long gap means "not measurable there", and bridging it
            # would invent data.
            out = np.interp(self.grid, a[ok], b[ok], left=np.nan, right=np.nan)
            gaps = np.interp(self.grid, a, np.where(ok, 0.0, 1.0),
                             left=1.0, right=1.0)
            out[gaps > 0.5] = np.nan
            return out

        fl = to_grid(self.spec_t, self.spec_fl)
        fr = to_grid(self.spec_t, self.spec_fr)
        pl = to_grid(self.spec_t, self.spec_pl)
        pr = to_grid(self.spec_t, self.spec_pr)
        beat_avail = self.has_l and self.has_r
        beat = np.abs(fr - fl) if beat_avail else np.full(n, np.nan)
        note = ""
        if not beat_avail:
            note = ("only one audio channel was mapped, so the binaural beat is "
                    "UNAVAILABLE (it is the difference between the two ear "
                    "carriers and cannot be inferred from one ear)")
        return ObservedAudio(
            t_rec=self.grid, rms=rms, rms_l=rms_l, rms_r=rms_r,
            tone_l_hz=fl, tone_r_hz=fr, tone_l_prom=pl, tone_r_prom=pr,
            beat_hz=beat, pulse_hz=to_grid(self.pulse_t, self.pulse_hz),
            pulse_depth=to_grid(self.pulse_t, self.pulse_depth),
            has_left=self.has_l, has_right=self.has_r,
            beat_available=beat_avail, note=note,
            fine_fs=self.fine_fs, fine_level=self._fine)

    def sync_envelope(self) -> np.ndarray:
        """Marker-tone envelope at `self.sync_fs` (1 kHz), starting at t_rec=0."""
        return self._sync_mag


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def observe_wav(path: str, cm: ChannelMap, *, grid_hz: float = DEFAULT_GRID_HZ,
                block_frames: int = 1 << 15, light_lp_hz: float = 200.0,
                light_floor: float = 0.002, spec_win_s: float = 0.25,
                spec_hop_s: float = 0.2, sync_tone_hz: float | None = None,
                max_seconds: float | None = None,
                progress=None) -> Observation:
    """Single streaming pass over the recording."""
    with WavReader(path) as rd:
        sr = float(rd.sample_rate)
        dur = rd.duration_s
        if max_seconds is not None:
            dur = min(dur, max_seconds)
        grid = make_grid(dur, grid_hz)

        lights = {led: LightTracker(sr, led, grid, lp_hz=light_lp_hz,
                                    invert=(idx in cm.invert),
                                    abs_floor=light_floor)
                  for led, idx in sorted(cm.lights.items())}
        at = None
        if cm.audio_l is not None or cm.audio_r is not None:
            at = AudioTracker(sr, grid, cm.audio_l is not None,
                              cm.audio_r is not None, spec_win_s=spec_win_s,
                              spec_hop_s=spec_hop_s, sync_tone_hz=sync_tone_hz)

        limit_frames = int(dur * sr)
        for start, block in rd.blocks(block_frames):
            if start >= limit_frames:
                break
            if start + block.shape[0] > limit_frames:
                block = block[: limit_frames - start]
            for led, idx in sorted(cm.lights.items()):
                lights[led].push(block[:, idx])
            if at is not None:
                xl = block[:, cm.audio_l] if cm.audio_l is not None else None
                xr = block[:, cm.audio_r] if cm.audio_r is not None else None
                if cm.audio_l is not None and cm.audio_l in cm.invert:
                    xl = -xl
                if cm.audio_r is not None and cm.audio_r in cm.invert:
                    xr = -xr
                at.push(start, xl, xr)
            if progress is not None:
                progress(start / max(1, limit_frames))

        for lt in lights.values():
            lt.flush()
        # One contrast scale across all light channels, so a dim channel is dim
        # RELATIVE to a bright one rather than being renormalised up to look
        # healthy. "channel 2 never turned on" depends on that.
        #
        # BUT THE SCALE MUST BE FLOORED, and this is not a detail: `max over the
        # mapped channels` is the noise floor when EVERY mapped channel is dark,
        # so dividing by it renormalised pure noise up to contrast ~1.0 and the
        # darkness test (which compares against an absolute-looking 0.06) could
        # not fire at all. MEASURED: with all four zones dead the normalised
        # contrast read min 0.302 / median 0.673 / max 1.230 against a 0.06
        # threshold, so the single most likely first-recording failure -- a
        # photodiode left unplugged -- produced ZERO findings about the light and
        # a fabricated 2-second A/V offset instead.
        #
        # Flooring at light_floor / THRESHOLD_DARK_CONTRAST maps the sensor's
        # own noise floor exactly onto the darkness threshold, so the test is
        # RELATIVE whenever there is light to be relative to and ABSOLUTE in the
        # one case where that is the only honest answer. A live channel's raw
        # p2p is ~0.32 against a floor of 0.033, so nothing changes for a working
        # rig; a dead one reads 1.9e-05 / 0.033 = 6e-04, four orders of
        # magnitude clear of the threshold.
        contrasts = {led: lt.raw_contrast() for led, lt in lights.items()}
        gc = max(contrasts.values()) if contrasts else 1.0
        gc = max(gc, light_floor / dm.THRESHOLD_DARK_CONTRAST)
        obs_light = {led: lt.finalize(gc) for led, lt in lights.items()}
        obs_audio = at.finalize() if at is not None else None

        markers: dict[str, float] = {}
        meta = {
            "sample_rate": sr,
            "n_channels": rd.n_channels,
            "sampwidth_bits": rd.sampwidth * 8,
            "float_wav": rd.is_float,
            "map": cm.describe(),
            "light_contrast": {f"ch{k}": round(v, 6) for k, v in contrasts.items()},
            "light_contrast_scale": gc,
            "light_abs_floor": light_floor,
            "light_no_signal": sorted(k for k, v in obs_light.items()
                                      if not v.available),
            "light_lp_hz": light_lp_hz,
            "spec_win_s": spec_win_s,
            "spec_hop_s": spec_hop_s,
            # The AM analysis window length. The comparator needs it to know
            # which pulse rates this recording can RESOLVE at all: the FFT bin
            # is 1/pulse_win_s, so a rate with only two cycles in the window
            # cannot be graded to a few percent and must be reported as
            # unmeasurable rather than as a device fault.
            "pulse_win_s": (at.pulse_len / at.pulse_fs) if at is not None else None,
            "edges": {f"ch{k}": int(v.edges_rise.size) for k, v in obs_light.items()},
        }
        if sync_tone_hz and at is not None:
            env = at.sync_envelope()
            meta["sync_tone_hz"] = sync_tone_hz
            # Stored so the sync solver can look for the marker's onset without
            # re-reading the file. `_sync_fs` is MANDATORY alongside it: the
            # envelope is on 1 ms boxes, NOT on the 20 Hz analysis grid, and a
            # consumer that assumed `t_rec` here would mis-date the onset by a
            # factor of 50.
            meta["_sync_env"] = env
            meta["_sync_fs"] = at.sync_fs

        return Observation(source=path, front_end="wav", grid_hz=grid_hz,
                           t_rec=grid, duration_s=dur, light=obs_light,
                           audio=obs_audio, markers=markers, meta=meta)
