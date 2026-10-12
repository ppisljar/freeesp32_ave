# Audio-vs-LED sync, measured optically

Measured with an Arturia MiniFuse 4 (all four inputs on one ADC clock, so
inter-channel skew is zero): two solar cells on the LEDs (ch1/ch2) and the
device headphone jack via a TRS splitter (ch3/ch4).  Tool:
`tools/avmeasure/avsync.py`.

## Method

The .ledc drives an LED square gate and an audio square gate (`mod`) at the same
rate and phase, so the offset between the two envelopes *is* the sync error.
Two details make the number trustworthy:

* Edges are located by the **peak of the derivative inside each cycle**, not by
  a threshold crossing.  The photocell sits behind a 100 nF cap into a Hi-Z
  input (~1.6 Hz high-pass), so the square droops and a fixed threshold is
  crossed repeatedly -- that counted 42 edges where 27 existed.  A drooping
  square still has exactly one sharp slope spike per edge.
* All smoothing is a **centered** moving average (zero phase).  A causal filter
  would fold its own group delay straight into the measurement.

Running several rates in one session separates a real latency (constant TIME
offset) from an envelope-shape artifact (constant PHASE).  Measured at 2/4/8 Hz
the constant-latency model fit to **0.03 ms** while constant-phase needed
39.9 deg of spread -- so it is a latency, unambiguously.

Per-edge scatter is 0.23-0.31 ms, and the two audio channels agree to 0.01 ms.

## Finding 1 -- a random per-session offset, now fixed

The offset was stable to 0.03 ms *within* a session but re-rolled at every
session start.  14 runs spanned **+1.15 .. +21.11 ms**.

Cause: the audio task generates in `AUDIO_GEN_BUFFER_SIZE` = 1024-frame chunks
(23.22 ms).  A channel goes active the instant the timeline dispatches it
(`ch->active = true` in `audio_generator_start_channel_locked`), but its first
sample is the first sample of the *next* chunk -- up to 23.22 ms later.  The LED
anchor (`config_parser.c`, `transport_origin_us + time_ms*1000 + DMA_LAG`) is
placed exactly, so the whole quantisation landed as audio lateness:

    offset = U[0, chunk] - c,    c ~ 2.5 ms

Confirmed by halving the chunk to 512 frames: the observed range halved from
**21.92 ms to 11.41 ms** against a predicted 23.22 -> 11.61.  That is what
identified the generation chunk rather than the DMA ring (2048 frames) as the
quantiser.

Fix ("V-S1" in `audio_generator.c`): stamp the arming instant
(`ch->start_request_us`) and, on the channel's first buffer, advance the Q32
phase accumulators by however late that buffer is.  Phase only -- amp ramp and
sweeps untouched, so it cannot change levels, only alignment.

Result: range **21.92 ms -> 1.30 ms** (n=8, sd 0.38), residual constant -1.79 ms.
No regression: `write_errors`/`short_writes`/`gen_fail` all 0, `fill_avg_us`
1710 of a 23220 us budget.

## Finding 2 -- entries on an ALREADY-ACTIVE channel are ~48 ms out (FIXED)

A .ledc entry targeting a channel that is already playing does not go through
`start_channel` at all; it goes through the lock-free pending-params latch,
which *deliberately* preserves `phase_l_q32`/`phase_r_q32`/`mod_phase_q32` for
continuity.  So it receives **no** DMA-lag pre-advance and no start catch-up,
while the LED anchor still gets its +`AUDIO_DMA_PIPELINE_LAG_US`.  The LED
therefore sits a full pipeline lag later than the audio.

Measured, same firmware, one session with a fresh start at t=2 s and a rate
change at t=9 s:

    block 1 (8 Hz, fresh start)            -1.74 -1.02 -2.31 -1.89 -0.95 -1.76 ms
    block 2 (6 Hz, change on active chan)  -44.85 -45.93 -50.19 -51.28 -50.58 ms

-48 ms is essentially -DMA_LAG (46.44 ms), plus ~6 ms of spread.

**Caveat on the mechanism.** The *magnitude* above is measured and reproducible;
the code path is inferred and NOT yet verified.  The "no pre-advance on the
pending latch" story explains -46.44 ms, but a competing story fits too: the
timeline may call `start_channel` again for the new entry, in which case the
catch-up and carrier pre-advance DO run -- but note that `mod_phase_q32` is
reset to 0 and, unlike `phase_l_q32`/`phase_r_q32`, never receives the DMA-lag
pre-advance at all (`audio_generator.c`, Step 3.3 only touches the carrier
phases).  That would also put the gate a pipeline lag out.  Before implementing
either candidate fix, instrument which path a follow-up entry actually takes and
what phase it ends up with -- do not fix on the strength of the arithmetic
alone.

This is the dominant real-world case: a therapeutic session is mostly follow-up
entries on the same audio channel, so audio runs ~48 ms early for most of a
session and jumps at each entry.  It is the most likely explanation for
"sometimes audio and flicker are not in sync".

Candidate fixes (needs a decision -- re-anchoring the mod phase mid-flight can
click):
1. On the pending latch, when `mod_frequency` changes, re-anchor `mod_phase_q32`
   to the timeline with DMA-lag + chunk-delay compensation.  Simple, but snaps
   the envelope -- audible with `env=0` (square) unless deferred to the next
   gate-off point.
2. Derive the modulation phase from absolute timeline time rather than
   accumulating it (`phase = frac((t_emit - anchor) * mod_freq)`).  Mirrors what
   the LED side already does with its cycle origin, removes accumulation drift,
   and makes rate changes anchored by construction.  More invasive.

## Rig notes

* The LED->audio 10 Hz crosstalk previously measured at +44 dB was a **ground
  loop** through the shared USB ground with the Mac, not a device fault.  With
  the ESP32 on an isolated USB adapter it drops to +16.9 dB, which is -39.6 dB
  below the audio carrier at -84 dBFS.  Not worth chasing.
* The solar cell must go into a **Hi-Z / instrument** input.  On a line input
  (~20 kOhm) the 100 nF cap differentiates (fc ~80 Hz): the capture shows edge
  spikes instead of a brightness envelope, the harmonic ladder goes flat
  (1 / 0.91 / 0.86 / 0.82 / 0.79 instead of 1 / 0.33 / 0.20 / 0.14 / 0.11), and
  amplitude/brightness grading becomes meaningless.  On Hi-Z the ladder matches
  a 50% square to within 0.01.
* Mains hum is a red herring at 10 Hz: the 5th harmonic of a 10 Hz flicker lands
  on 50 Hz.  Measure the 50 Hz line with the LEDs dark -- it sits at -87.6 dBFS,
  giving a true sensor SNR of +45.2 dB (ch1) / +41.8 dB (ch2).
* ch1 vs ch2 (the two eyes) differ by a constant 0.33 ms, reproducible across
  runs -- the serial LED update order, not a fault.


## Finding 2a -- the audio half: gate phase was free-running (FIXED, V-S2)

A .ledc entry aimed at a channel that is already playing goes through
`audio_generator_update_params_locked`, whose pending-params latch deliberately
preserves `phase_l/phase_r/mod_phase` for continuity -- so it received neither
the DMA-lag pre-advance nor the V-S1 start catch-up.

Fix: the gate phase is now defined by the entry's position on the timeline,

    mod_phase = frac((t_emit - mod_anchor_us) * mod_freq)

where `mod_anchor_us` is set by config_parser to the SAME expression
`logical_anchor_us` uses on the LED side, so audio and light reference one
quantity instead of two corrections that must agree.  Recomputed once per entry,
not per sample, so the hot loop keeps its plain uint32_t accumulator.  `double`
rather than `float`: a 60 min session reaches ~36000 cycles, where float's
24-bit mantissa quantises the phase to ~0.2 ms at 10 Hz.

Measured: the rate-change block's run-to-run spread collapsed from 6.43 ms to
0.94 ms, and the audio grid became consistent across the change.

## Finding 2b -- the LED half: a mid-session rate change re-timed the IN-FLIGHT cycle (FIXED, V-S3)

With the audio anchored, a constant ~44.6 ms remained on the rate-change block.
Timing each stream against its own entry localised it to the LIGHT channel: the
audio grid held steady across the change (-18.98 -> -20.17 ms) while the light
grid jumped ~+42 ms.

`frequency_milliHz` keys the A2 cycle-duration cache (`cd_cache_freq`).  Both
task-context writers -- `start_flicker_masked` and
`led_matrix_update_flicker_masked` -- wrote it immediately, so the ISR closed the
cycle that was currently IN FLIGHT at the NEW period and then advanced the origin
by that new period from a start that sat on the old grid.  The grid therefore
picked up a one-time shift of exactly `new_period - old_period`.

This is a case where the comment was right and the code was wrong: the note in
`led_matrix_update_flicker_masked` already said "the new frequency takes effect
at the next cycle boundary when the ISR recomputes cycle_duration_us from
sw_freq", and the one immediate write was quietly defeating it.

Prediction tested against two rate pairs BEFORE changing anything -- this is what
confirmed the mechanism rather than merely fitting the first number:

| change | predicted shift | measured |
|---|---|---|
| 8 Hz -> 6 Hz  | `166.667 - 125 = +41.67` ms | +41.53 ms |
| 8 Hz -> 10 Hz | `100 - 125 = -25.00` ms     | -24.85 ms |

Fix (V-S3): do not write `frequency_milliHz` on an already-running channel.
`sw_freq` already carries the new rate and the ISR's boundary recompute applies
it to the NEXT cycle.  The write is still needed on a FRESH channel, where there
is no in-flight cycle to protect and the pre-anchor path needs the rate to
compute `latched_on_time_us`.

Result, 3 runs per rate pair:

    8 -> 6 Hz   block1 -2.45 -2.31 -3.41    block2 -2.70 -2.46 -2.64
    8 -> 10 Hz  block1 -2.97 -2.50 -2.89    block2 -2.97 -2.48 -3.00

i.e. the two blocks now agree, where before they differed by 42.8 and 24.8 ms.

Side effect worth knowing: `/api/state` reads `frequency_milliHz`, so after a
rate change on a running channel it reports the OLD rate for up to one cycle
(<=167 ms at 6 Hz) before the boundary applies the new one.  Harmless, but do not
treat a single `/api/state` poll taken immediately after a dispatch as proof the
rate did not change.

## Where A/V sync stands after V-S1 + V-S2 + V-S3

A constant **-2.7 ms** (audio early), stable across rate changes and across
session restarts, versus a random 0..23 ms plus a 25-42 ms jump per rate change
before.

This residual is deliberately NOT trimmed to zero.  It cannot currently be
separated into device-side offset versus measurement-chain bias: the audio path
adds the codec's interpolation-filter group delay and analog output stage, the
light path adds the LED turn-on and the photocell's RC, and neither is
independently known.  Calibrating the constant away would bake my rig's bias into
the firmware.  To zero it honestly, first measure the codec's own output latency
(e.g. an electrical loopback from the DAC output back into the interface) and
subtract only the device-side part.

## Finding 2c -- a rate change lands at the next OLD-rate boundary, not at the entry (FIXED, V-S4)

V-S3 removed the in-flight re-timing, but the rate change still takes effect at
the first LED cycle boundary at or after dispatch.  That boundary sits on the
OLD rate's lattice, while the audio gate is anchored to the entry itself -- so
the whole post-change block is misaligned by (boundary - entry), which ranges
over a full old period as the entry time varies.  At 8 Hz that is up to
+/-62.5 ms, i.e. LARGER than the 42 ms V-S3 fixed.

The avs4/avs5 sessions missed this because t=9000 happens to be an exact
multiple of 125 ms after t=2000, so the boundary coincided with the entry.

Measured by moving the entry within one old period (8 Hz -> 6 Hz at t=9040 and
t=9080).  Boundaries lie at `2000 + DMA_LAG + k*125`; the first at or after
dispatch gives:

| entry | first boundary >= dispatch | predicted block-2 offset | measured |
|---|---|---|---|
| 9040 ms | 9046.44 ms | +40 ms  | +37.1 ms |
| 9080 ms | 9171.44 ms | -45 ms  | -47.3 ms |

Tool note: `tools/avmeasure/avsync.py` only knows the offset modulo the period,
which is useless once the error exceeds half a period, so
`tools/avmeasure/ratechange.py` was added to find the changepoint directly from
the inter-edge intervals.  Its first edge detector was wrong in an instructive
way: behind the 100 nF / Hi-Z high-pass the OFF phase recovers toward baseline
with a positive slope, giving a second broad positive bump per cycle, and a
left-to-right "threshold then skip min_gap" scan with min_gap at half a period
accepted both -- an 8 Hz 50% cycle measured as intervals 63.3 / 62.9 ms (the ON
and OFF durations) instead of 125 ms.  Ranking candidates by slope and
suppressing neighbours fixes it: intervals then read a clean 125.0 ms.


### V-S4 fix and a routing trap worth remembering

Fix: the timeline anchor is stashed in `pending_anchor_us` and consumed at the
next cycle boundary, where the cycle origin is snapped to the lattice point
NEAREST the natural origin (`anchor + k*period`, k rounded).  Nearest, not next,
bounds the one-time distortion to half a period either way.  A lattice point in
the past is preferable -- the ISR just sees a cycle already partway through,
which is the correct phase -- while a point in the future needs the LED held dark
for the wait, else `elapsed_us = 0` makes `should_be_on` true and the LED latches
ON for the whole wait (the "first blink at full power" artifact the pre-anchor
note describes).

Armed on EVERY timeline entry, not only rate changes: when the grid is already
aligned the correction lands on the same instant and is a no-op, so it also
self-heals accumulated slip.  Skipped while the rate is swept, modulated or
jittered -- those change the period every cycle so there is no fixed lattice, and
an earlier fix in this file was reintroduced wholesale by exactly that oversight
on the jitter column.

**The trap:** the first attempt armed the anchor only in `start_flicker_masked`
and measured NO change at all.  config_parser routes an entry with no swept
fields through `orphan_mask`, which for an already-flickering channel calls
`led_matrix_update_flicker_params_masked` -- a function that did not even take an
anchor parameter.  The fix was dead on exactly the case it was written for.  When
a firmware change measures as a perfect no-op, check that the code path you
edited is the one the timeline actually takes before concluding the mechanism was
wrong.  (Three LED entry points exist: `start_sweep_masked` for entries with
swept fields, `start_flicker_masked` for inactive orphan bits, and
`update_flicker_params_masked` for active orphan bits.)

Result -- entry time swept across one old period, which is what makes the bug
visible at all:

| entry | before V-S4 | after V-S4 |
|---|---|---|
| 9000 ms | -2.44 ms | -2.55 ms |
| 9040 ms | +36.83 ms | -3.05 ms |
| 9080 ms | -47.46 ms | -3.00 ms |
| 9100 ms | -27.68 ms | -2.74 ms |

### The sweep path (82 of 85 sessions use it)

`start_sweep_masked` serves every entry with a swept field -- 82 of the 85
library sessions sweep at least one non-freq LED field, so this is the path real
content takes.  It did NOT arm the anchor, and was extended to do so.

The measured error there was much smaller than on the no-sweep path, which is
worth recording so the next person does not expect a dramatic number: it never
wrote `frequency_milliHz` directly, so after V-S3 the only residue was the
next-boundary quantisation.  Rate change 8 -> 6 Hz at t=9040 via the sweep path
measured -4.81 / -4.30 ms against a block-1 reference of -3.00 / -2.49, i.e. only
~1.8 ms out, and arming the anchor brought it to -2.91 / -3.06.  A real but minor
improvement; the value is mostly uniformity and self-healing rather than size.

Armed unconditionally for an already-running channel rather than gating on
`freq_curve == LED_INTERP_NONE` at the call site: the ISR already decides whether
a fixed lattice exists (no freq sweep, no freq mod, no jitter), so that judgement
stays in one place instead of being duplicated.


## Final verification (all four fixes on the device)

Entry time swept across one old period, two runs each, both blocks measured:

| entry | run | 8 Hz block (fresh start) | 6 Hz block (rate change) |
|---|---|---|---|
| 9000 ms | 1 | -2.52 ms | -2.68 ms |
| 9000 ms | 2 | -3.09 ms | -2.80 ms |
| 9040 ms | 1 | -3.04 ms | -2.95 ms |
| 9040 ms | 2 | -3.17 ms | -3.05 ms |
| 9080 ms | 1 | -3.53 ms | -3.03 ms |
| 9080 ms | 2 | -2.99 ms | -3.01 ms |

All twelve within -2.52..-3.53 ms (1.0 ms spread), independent of entry
alignment and of whether the rate changed. Audio health across the run:
0 write_errors, 0 short_writes, 0 gen_fail, fill_avg 910 us of a 23220 us budget.

Where this started: a random 0..23 ms offset re-rolled every session start, plus
a 25-42 ms jump at every rate change, plus a further up-to-+/-62 ms depending on
where the entry fell relative to the old period.

## The −2.7 ms residual is probably the flat strip-write constant (HYPOTHESIS)

Not yet confirmed by re-measurement — the arithmetic below is a prediction, and
the model change it implies has not been made.

The WS2812 timing in `main/led_strip.c:22-25` is `T0H+T0L = T1H+T1L = 1.25 µs`,
so a pixel is 24 bits = **30.0 µs** and the `CONFIG_LED_COUNT=74` strip takes
**2.220 ms** to clock out end to end. `devicemodel.py:169` models the light-side
pipeline as a **flat 2.5 ms** for the neopixel backend, independent of where the
sensor sits.

But the delay is not flat: `s_addressable_set_channel` (`led_strip.c:464-470`)
writes a zone's colour into every pixel whose `channel_map[i]` matches, and pixel
*i*'s bits reach the strip at *i* × 30 µs into the transmission. A sensor on
pixel *k* therefore sees photons at `rmt_start + k × 30 µs`, anywhere from 0 to
2.22 ms — so the model **over-subtracts** for any sensor near the head of the
chain, by up to the full 2.22 ms.

Independent support: the measured **inter-eye offset of 0.33 ms** is
0.33 ms / 30.0 µs = **exactly 11.0 pixels**, which is what two sensors at
different chain positions must look like. That number was previously recorded
without an explanation.

Arithmetic check. If the two sensors sit near the head of the chain, 11 pixels
apart (so a mean index of ~5.5 = 0.165 ms):

    predicted residual = −(2.5 − 0.165) = −2.34 ms
    measured  residual =  −2.7 ms

leaving **~0.35 ms** unaccounted — the sensor's own RC rise plus any recorder
group delay — rather than the full 2.7 ms. If true, the device-side A/V error is
sub-millisecond and the residual is a model/rig artifact, not a fault.

### What closing this needs

| term | magnitude | how to pin it | needs hands? |
|---|---|---|---|
| strip serialisation | 0 .. 2.22 ms | replace the flat 2.5 ms with `rmt_start + k × 30 µs`; take *k* as a measurement input | no (model change) |
| each sensor's pixel index *k* | — | the rig builder knows it; or light zones in a known order and read the map | **yes** (or one capture) |
| DAC + analog group delay | unknown, ~0.1–1 ms? | toggle a spare GPIO at the generator block boundary, wire it into a free recorder input: the recorder then holds the digital event time and the analog audio time on **one** timebase | **yes** (one wire + divider) |
| recorder inter-channel skew | expected < 10 µs | Y-split one signal into an audio input and a light input, one capture | **yes** (one cable) |
| sensor RC rise | ~0.1 ms? | hard-step the LED and measure 10–90 % rise | no |

The strip term is the only one large enough to explain the residual on its own,
and it is the one we can fix without touching the rig.
