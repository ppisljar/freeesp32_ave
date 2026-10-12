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
