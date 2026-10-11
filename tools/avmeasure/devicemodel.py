"""Device-specific physical constants and quantization models.

WHY THIS FILE EXISTS SEPARATELY
-------------------------------
Every number in here is a *property of the firmware and the hardware*, not of
the measurement method. If the comparator hard-coded them inline, a firmware
change (bigger DMA ring, different LED backend, a fix to the cycle re-anchoring
bug) would silently invalidate the instrument's verdicts. Keeping them here,
each with a file:line citation, means a firmware change is a one-file edit and
the provenance of every threshold is auditable.

THE CENTRAL IDEA: "EXPECTED" IS NOT "DEMANDED"
----------------------------------------------
The `.ledc` file *demands* 7.83 Hz. The firmware, by design, *emits* 7.8184 Hz,
because its flicker cycle re-anchors to the ISR tick instant every cycle
instead of to anchor + k*period (led_matrix_example.c:891). A comparator that
flags that as a bug would cry wolf on every single low-rate session ever
recorded, and the user would stop trusting the tool within one run.

So: ledc_expect.py models what the file DEMANDS, this file models the
firmware's distortion of that demand, and compare.py checks the observation
against demand-passed-through-this-model. Both numbers get reported, so a human
can always see "you asked 7.83, the firmware can only do 7.8184, we measured
7.8191" and draw their own conclusion.
"""

from __future__ import annotations

import math

# ---------------------------------------------------------------------------
# Audio clock / output pipeline
# ---------------------------------------------------------------------------

AUDIO_SAMPLE_RATE = 44100
"""main/audio_config.h — AUDIO_SAMPLE_RATE. Also AUDIO_GEN_SAMPLE_RATE."""

DMA_PIPELINE_FRAMES = 2048
"""main/audio_config.h:74 — AUDIO_DMA_PIPELINE_SAMPLES = 8 descriptors x 256 frames."""

DMA_LAG_US = 46439
"""main/audio_config.h:91 — AUDIO_DMA_PIPELINE_LAG_US = 2048 * 1e6 / 44100.

This is the constant the firmware uses for BOTH corrections:
  - audio channel start pre-advances its Q32 phase by this much
    (main/audio_generator.c:402-433)
  - LED cycle anchors are pushed forward by this much
    (main/config_parser.c:2699-2702)
So nominal audio-vs-LED skew at the same timestamp is zero by construction.
"""

DMA_DESCRIPTOR_FRAMES = 256
"""main/audio_config.h:61 — 1024 bytes / (2 ch * 2 bytes)."""

DMA_LAG_TRUE_RANGE_US = (
    (DMA_PIPELINE_FRAMES - DMA_DESCRIPTOR_FRAMES) * 1_000_000 // AUDIO_SAMPLE_RATE,
    DMA_PIPELINE_FRAMES * 1_000_000 // AUDIO_SAMPLE_RATE,
)
"""True transit is [7, 8] descriptors, i.e. [40635, 46439] us (mean 43537).

The firmware's single 46439 us constant is the UPPER bound, so it over-states
the real lag by about half a descriptor (2.90 ms). That shows up as a constant
~3 ms audio-EARLY bias relative to the LED anchor. It is a known, expected,
harmless bias -- included in the offset band below so we do not report it.
"""

GEN_BLOCK_FRAMES = 1024
"""main/audio_generator.h:40 — AUDIO_GEN_BUFFER_SIZE, in FRAMES not bytes."""

GEN_BLOCK_US = GEN_BLOCK_FRAMES * 1_000_000 // AUDIO_SAMPLE_RATE  # 23219
"""23.22 ms. The single biggest audio/LED asymmetry.

A channel start or param update is only latched on the next fill_buffer
boundary (main/audio_generator.c:821), so EVERY audio event onset carries an
extra U(0, 23.22 ms) delay -- mean 11.61 ms -- that the LED path does not have.
"audio lags light by ~12 ms" is the expected centre, not a bug.
"""

TIMELINE_AM_DEPTH = 0.1
"""main/config_parser.c:2357 — mod_depth is hard-coded to 0.1 for every
timeline entry; there is no `.ledc` depth field. So the isochronic gate only
swings amplitude between 0.9 and 1.0 of the carrier (~0.9 dB), and env=4
tremolo is +/-10%. Expect SHALLOW AM, not full-depth gating.
"""

AUDIO_HEADROOM = 1.0 / 16.0
"""main/audio_generator.c:705 — 1/NUM_AUDIO_CHANNELS fixed mix headroom.

Combined with the runtime `settings.audio_max_volume` (audio_test.c:201-216),
which an external recording CANNOT observe, this makes absolute dBFS
unpredictable. Always compare amplitude RATIOS, never absolute levels.
"""

# ---------------------------------------------------------------------------
# Timeline scheduler
# ---------------------------------------------------------------------------

TIMING_TICK_US = 1000
"""main/timing_engine.c:265 — gptimer alarm every 1000 us."""

TIMING_LOOKAHEAD_US = 500
"""main/timing_engine.c:486 — events with deadline <= now+500us fire now.

The look-ahead CENTRES the quantization, so an event fires uniformly within
[-500, +500] us of its deadline rather than 0..+1000 us late. This is the only
genuinely tight term in the whole budget.
"""

MAX_BATCH_SIZE = 50
"""main/config_parser.c:698, 1942 — entries 51+ sharing one timestamp are
NEVER executed: batch_end stops at 50 and the "find next strictly later
timestamp" scan walks past the leftovers. A direct cause of "events that seem
not to fire".
"""

MAX_ENTRIES = 100
"""main/config_parser.h:19 — CONFIG_PARSER_MAX_ENTRIES. Entries past 100 are
silently dropped at PARSE time, i.e. in FILE order, before the sort.
"""

MAX_LINE_LENGTH = 256
"""main/config_parser.h:18 — lines >= 256 bytes are truncated to 255 with a
warning, which drops tail tokens and can change the token count (and therefore
how the line is interpreted).
"""

MAX_TOKENS = 16
"""main/config_parser.c:1354-1361 — at most 16 tokens kept; extras dropped."""

SPEECH_TEXT_MAX = 191
"""main/config_parser.h:40."""

# ---------------------------------------------------------------------------
# LED flicker engine
# ---------------------------------------------------------------------------

NUM_LED_CHANNELS = 8
"""main/led_strip.h:32."""

NUM_AUDIO_CHANNELS = 16
"""main/audio_generator.h:24-25. Note `ch` in the `.ledc` is a RAW 0-based
generator index with no bounds check on the timeline path
(main/config_parser.c:1623), so ch 16 is an out-of-bounds struct write.
"""

LED_FLICKER_TICK_MULT = 250
LED_FLICKER_TICK_MIN = 1000
LED_FLICKER_TICK_MAX = 10000
"""main/led_matrix_example.c:194-196."""

LED_FREQ_MAX_HZ = 100.0
"""main/led_matrix_example.c:1314 — flicker start/update is REJECTED above
100 Hz (and for duty>100 / bright>100 at :1318-1325). The entry does nothing
and only an error is logged, so the LED simply keeps doing whatever it was
doing. ledc_format.md documents no such limit.
"""

LED_FIRST_EDGE_GRACE_MS = 50.0
"""main/led_matrix_example.c:712 — on fresh activation the cycle origin is the
logical anchor, up to 46.4 ms in the FUTURE, and the ISR skips the channel
entirely until then. So the FIRST edge of any channel activation appears ~46 ms
after its `.ledc` timestamp, by design. Never report that as "never turned on".
"""

# NEOPIXEL: 74 LEDs x 24 bits x 1.25 us = 2.22 ms data + 280 us latch = 2.50 ms,
# and s_neopixel_refresh blocks on the PREVIOUS transmission first
# (main/led_strip.c:517), so an edge can be up to ~2 frames late.
LED_BACKEND_LATENCY_MS = {
    "neopixel": (2.5, 5.0),   # (typical, worst) — 74 px at CONFIG_LED_COUNT=74
    "dotstar": (2.5, 5.0),    # SPI, same order of magnitude
    "direct": (0.04, 0.08),   # one 25 kHz LEDC period, main/led_strip.c:1016
}
DEFAULT_LED_BACKEND = "neopixel"
"""sdkconfig:613 CONFIG_LED_DEFAULT_BACKEND_NEOPIXEL=y, settings.c:62."""


def led_freq_milli_hz(freq_hz: float) -> int:
    """Firmware's float->milliHz conversion: (uint32_t)(frequency * 1000.0f)."""
    if freq_hz <= 0.0:
        return 0
    return int(freq_hz * 1000.0)


def flicker_tick_hz_for(freq_hz: float) -> int:
    """ISR tick rate the firmware would *request* for this flicker frequency.

    main/led_matrix_example.c:1248:
        desired = (min_freq_milliHz / 1000u) * LED_FLICKER_TICK_MULT

    TRAP: the integer divide by 1000 happens FIRST, so 7.83 Hz -> 7 -> 1750 Hz,
    and anything below 1.0 Hz -> 0 -> clamped to the 1000 Hz floor. Getting this
    wrong makes every sub-Hz session look broken.
    """
    desired = (led_freq_milli_hz(freq_hz) // 1000) * LED_FLICKER_TICK_MULT
    return max(LED_FLICKER_TICK_MIN, min(LED_FLICKER_TICK_MAX, desired))


def flicker_tick_period_us(tick_hz: int) -> int:
    """Actual ISR period: alarm_count = 1000000ULL / desired (INTEGER divide).

    main/led_matrix_example.c:1253. For desired=1750 this is 571 us, not
    571.43 -- and that 0.43 us matters, because the realized flicker period is
    an integer multiple of it.
    """
    return 1_000_000 // max(1, tick_hz)


def quantized_flicker_hz_vec(demanded_hz, tick_hz):
    """Vectorised `quantized_flicker_hz`, elementwise over both arguments.

    Kept bit-compatible with the scalar version (same integer truncations) and
    tested against it, because the scalar version is the one carrying the
    firmware citations and the vector version is the one actually used on a
    72000-point grid or a 44.1 kHz render.
    """
    import numpy as _np
    f = _np.asarray(demanded_hz, dtype=_np.float64)
    th = _np.asarray(tick_hz, dtype=_np.int64)
    f_mhz = (f * 1000.0).astype(_np.int64)   # (uint32_t)(freq * 1000.0f)
    pos = f_mhz > 0
    dur = _np.zeros(f_mhz.shape, dtype=_np.int64)
    dur[pos] = (1_000_000 * 1_000) // f_mhz[pos]
    t_tick = _np.maximum(1_000_000 // _np.maximum(th, 1), 1)
    # -(-a // b) is integer ceil without leaving int64.
    ticks = _np.where(pos, -(-dur // t_tick), 0)
    period = ticks * t_tick
    out = _np.zeros(f.shape, dtype=_np.float64)
    good = period > 0
    out[good] = 1_000_000.0 / period[good]
    return out


def quantized_flicker_hz(demanded_hz: float, tick_hz: int) -> float:
    """The rate the firmware actually emits for a demanded rate.

    main/led_matrix_example.c:891: the cycle boundary fires on the first ISR
    tick where elapsed >= cycle_duration_us, then re-anchors cycle_start to
    that TICK INSTANT rather than to cycle_start + cycle_duration. So the
    realized period is exactly ceil(D / T_tick) * T_tick and the realized rate
    is ALWAYS <= demanded, never above.

    This is ONE-SIDED. compare.py relies on that: a measured rate that is LOW
    by less than the quantization is expected; a measured rate that is HIGH is
    always suspicious.
    """
    if demanded_hz <= 0.0:
        return 0.0
    f_mhz = led_freq_milli_hz(demanded_hz)
    if f_mhz == 0:
        return 0.0
    # Firmware: cycle_duration_us = (1000000ULL * 1000ULL) / frequency_milliHz
    duration_us = (1_000_000 * 1_000) // f_mhz
    t_tick = flicker_tick_period_us(tick_hz)
    ticks = math.ceil(duration_us / t_tick)
    period_us = ticks * t_tick
    if period_us <= 0:
        return 0.0
    return 1_000_000.0 / period_us


# ---------------------------------------------------------------------------
# The net expected-offset budget
# ---------------------------------------------------------------------------

# Audio onset = T0 + t_ms + dispatch_lag + U(0, 23.22 ms) + [40.6, 46.4] ms
# LED edge    = T0 + t_ms + 46.439 ms + U(0, T_tick) + LED hw latency
#               (immune to dispatch lag, because the anchor is absolute)
#
# Audio - LED at onset therefore sits around +13 ms (NEOPIXEL) to +17 ms
# (DIRECT), with a band of roughly 0..+35 ms once dispatch lag is included.
# AUDIO IS EXPECTED TO LAG LIGHT. Audio *leading* light is always a bug: it
# means one of the two +46.439 ms corrections is not being applied.
AUDIO_ONSET_DELAY_MS = (GEN_BLOCK_US / 2.0 + sum(DMA_LAG_TRUE_RANGE_US) / 2.0) / 1000.0
"""Pipeline-only delay from "the executor started the channel" to "the sound
left the DAC": mean generator-block latch delay (11.61 ms) + mean true DMA
transit (43.54 ms) = 55.15 ms. Dispatch lag is NOT included -- that is the
variable term we are trying to measure.
"""


def led_edge_delay_ms(backend: str = DEFAULT_LED_BACKEND) -> float:
    """Pipeline-only delay from the LED's logical anchor to photons.

    The anchor itself already carries +46.439 ms (that is the whole point of
    the symmetry), so what is left is the strip write: ~2.5 ms on NEOPIXEL,
    ~0.04 ms on DIRECT.
    """
    return LED_BACKEND_LATENCY_MS.get(backend, LED_BACKEND_LATENCY_MS["neopixel"])[0]


def av_offset_pipeline_ms(backend: str = DEFAULT_LED_BACKEND) -> float:
    """Expected audio-minus-light offset from the pipelines alone.

    LED edges are placed at (anchor = demanded + 46.439 ms) + strip write.
    Audio onsets are placed at dispatch + 55.15 ms, where dispatch == demanded
    only if the timeline task was not delayed.

      audio - light = 55.15 - (46.439 + strip)  ~= +6.2 ms on NEOPIXEL
                                                ~= +8.7 ms on DIRECT

    Add the dispatch lag (8-20 ms typical, main/config_parser.c:1933, :243),
    which lands entirely on audio, and the real centre is +14..+26 ms -- which
    is where the budget's "~+13 ms" figure comes from. Either centre gives the
    same verdicts because the flag band below is deliberately wide.
    """
    return AUDIO_ONSET_DELAY_MS - (DMA_LAG_US / 1000.0 + led_edge_delay_ms(backend))


AV_OFFSET_BAND_MS = (0.0, 40.0)
"""The HEALTHY band for the audio-vs-light constant offset.

The lower bound is 0 and that is not a rounding choice: audio LEADING light is
physically impossible if both +46.439 ms corrections are applied. But "outside
the healthy band" is not the same as "proven broken" -- see
AV_OFFSET_FLOOR_MS, which is the measurement floor the ERROR is gated on.
"""

AV_OFFSET_FLOOR_MS = 15.0
"""How negative the measured A/V offset must be before it is a device ERROR.

MEASURED, not chosen: on bit-perfect synthetic renders the raw residual
(audio_lag - light_lag) already sits between -1.5 ms (01_sleep_onset,
04_meditation_theta) and +2.3 ms (selftest), purely from estimator bias, and
av_total adds a modelled +6.2 ms pipeline term. So the band's hard 0.0 floor
left only 4.7-8.5 ms of headroom ON AN IDEAL SIGNAL, before a real rig adds
phototransistor fall time into a 10k load, AC-coupling phase advance on the
light channel, and codec group delay.

The fault this check exists to catch -- one of the two +46.439 ms corrections
missing -- is -46 ms or -93 ms, so a 15 ms floor still detects it with 3x
margin while no longer firing on an instrument artefact. Between -15 ms and 0
the finding is reported as AV_OFFSET_MARGINAL info, with the raw residual and
the modelled term printed separately so a human can do the arithmetic.

The effective floor is widened further by the sync solution's own uncertainty:
a t0 known only to +/-50 ms cannot support a 20 ms claim about anything.
"""

AV_OFFSET_MIN_RESOLVABLE_MS = 20.0
"""t0 uncertainty above which the A/V offset is not reported AS A NUMBER.

av_offset = (audio origin - light origin) + pipeline, and both origins are
measured against the recording. When t0 is only localised to a second, the two
origin estimates are not independent of each other -- the audio search is
anchored on the light solution -- so their difference stops tracking the truth
instead of merely getting noisier. MEASURED on tests/fadein.ledc (a 20 s
brightness fade-in then a steady rate, the shape of most real sessions):
injecting a 300 ms audio delay moved the reported offset by 0.0004 ms, and
-1000 ms read as a healthy +11 ms. A number that does not move with the truth
must not be printed as if it did.

Half the width of the 0..40 ms band: above that the band carries no
information at all.
"""

THRESHOLD_DARK_CONTRAST = 0.06
"""Normalised optical contrast below which a light channel counts as DARK.

Shared between the front-end and the comparator ON PURPOSE. The front-end
normalises contrast by the brightest MAPPED channel so a dim channel reads as
dim rather than being renormalised up to look healthy -- but when NO mapped
channel carries any signal the normaliser IS the noise floor, and a completely
unplugged rig then normalises to ~1.0 and reads healthy. So the front-end
floors its normaliser at `light_floor / THRESHOLD_DARK_CONTRAST`, which makes
this threshold absolute in exactly the case where the relative one breaks.
MEASURED: a dead channel's raw 99th-percentile cell span is 1.9e-05 against
0.32 for a live one, so the two cases are separated by four orders of
magnitude -- there is nothing marginal about this test once it is anchored.
"""

# Dispatch lag budget, main/config_parser.c:1933 and :243.
#   +-0.5 ms   timing-engine ISR grid
#   +7.6 ms    one INFO log line at 115200 baud, emitted BEFORE the entries run
#   0..10 ms+  timeline_execution_task is prio 4 on CORE 0, the same core as
#              WiFi/lwIP/httpd, so a browser polling /api/state can delay it
#   0..6 ms    audio_generator_lock() wait
# => typically 8..20 ms with a long tail under network load.
DISPATCH_LAG_TYPICAL_MS = (8.0, 20.0)

# Verdict thresholds. Deliberately loose: a false positive from this instrument
# costs a human hours of chasing a bug that does not exist, which is strictly
# worse than missing a marginal real one.
THRESHOLD_EVENT_LATE_MS = 25.0
THRESHOLD_EVENT_EARLY_MS = -5.0      # earlier than this is physically impossible
THRESHOLD_FLICKER_REL = 0.005        # 0.5% beyond the computed quantization
THRESHOLD_DUTY_PCT = 8.0             # absolute percentage points
THRESHOLD_TONE_REL = 0.01            # 1% of the demanded carrier
THRESHOLD_BEAT_HZ = 0.25             # Hz; beats are 0.5-10 Hz so this is tight
THRESHOLD_AMP_DB = 3.0               # relative level, after normalisation
THRESHOLD_DRIFT_PPM = 20.0           # below this, it is crystal tolerance
THRESHOLD_DIFF_DRIFT_PPM = 30.0
"""Differential (audio-vs-light) drift threshold.

PROVENANCE, AND ITS LIMITS. On tests/selftest.ledc with 0, +/-500 and +400 ppm
injected, the two domains' estimates agree to within 2 ppm (test_12 / test_13).
That figure does NOT generalise on its own: on zero-drift renders of shipped
sessions the per-domain estimates were measured at -162 ppm (04_meditation_theta),
+24 ppm (05_focus_smr) and +10 ppm (07_genus_40hz) before the measurability gate
below was added. The mechanism was always the same -- a brightness LFO or
fade-in dips the demanded brightness below what a photodiode envelope can
resolve into edges, alternate edges are lost, and the measured rate reads low
while `_steady_mask` (which looks only at the DEMANDED rate) still calls the
span steady.

So the clock fit now applies the same measurability gate the value checks use
(CompareConfig.min_bright_pct) and the same spectral-prominence gate on the
audio side. With that gate the shipped-session floor is a few ppm, which is
what this threshold is set from. If you change the gate, re-measure this.

CLOCK_SLACK_PPM below is the companion number for event timing.
"""

THRESHOLD_DRIFT_FIT_MIN_SPAN_S = 300.0
"""Minimum event-residual baseline before a DRIFT claim is allowed at all.

A drift is a SLOPE, and a slope needs a lever arm. The residual-slope check
used to accept a 30 s span, where 2.4 ms/s of apparent tilt -- which six noisy
fade-in onsets supply easily -- extrapolates to "5500 ms per 1000 s" and was
reported as a hard clock-ratio error on a bit-perfect render of
04_meditation_theta, while the clock fit in the same report said +/-10 ppm.
Five minutes is the shortest baseline on which a per-hour drift figure is a
measurement rather than an extrapolation.
"""

THRESHOLD_DRIFT_FIT_MIN_POINTS = 8
"""Minimum residual count for the drift fit. Six points, two of them carrying
a full flicker period of legitimate slack (+/-333 ms at 3 Hz), is not a fit."""

THRESHOLD_DRIFT_FIT_SIGMA = 3.0
"""The fitted slope must exceed this many standard errors of its own fit.

Without a significance test the check reports the scatter of its own inputs.
With it, "the residuals are noisy" and "the residuals trend" are different
answers, which is the whole point of the finding.
"""

THRESHOLD_COHERENT_CARRIER_HZ = 2.0
"""Carrier separation below which two audible slots' SUM is not gradeable.

ExpectedMix.rms_rel models an INCOHERENT power sum of the generator slots. Two
slots on near-identical carriers interfere coherently instead, and the summed
level swings through real nulls with period 1/df. The two-channel panned
binaural idiom (04_meditation_theta: ch1 pan -30 and ch2 pan +30, both 250 Hz
ramping to 254 / 253.7) puts df between 0 and 0.3 Hz, which produced a -8.1 dB
AMPLITUDE warning over 56 s of a flawless render.

2 Hz is set from the aggregation window, not from taste: a finding needs
min_run_s = 2 s of continuous error, so a beat that completes inside 2 s
cannot hold the level away from the incoherent prediction for long enough.
Below 2 Hz the level is genuinely unpredictable and the span is declared
not graded instead of being graded wrongly.
"""

CLOCK_SLACK_PPM = 50.0
"""Uncertainty allowance on the fitted clock ratio, used to widen event-timing
tolerances in proportion to how far into the session the event is.

The clock ratio is fitted from noisy frequency measurements, so a prediction
for an event 90 minutes in inherits that uncertainty: at 50 ppm it is +/-270 ms
at t=90 min. Without this term, late-session events get flagged for the
instrument's own extrapolation error -- which looks exactly like a real
progressive drift and would send someone hunting for one.
"""

# A recorder crystal is typically +-50..100 ppm, i.e. ~120..360 ms over 60 min.
# The DEVICE cannot drift internally: audio and LED both derive from the same
# 40 MHz crystal (sdkconfig:1164), so a drift that affects audio and light
# EQUALLY is the recorder and must be fitted out before reporting anything.
# Only a DIFFERENTIAL drift (audio vs light) can be a device bug.
RECORDER_PPM_TYPICAL = 100.0
