# Entrainment Engine — Firmware Enhancements Plan

> **STATUS (2026-07-03): 5 of 6 IMPLEMENTED, build clean, NOT yet flashed/hw-verified.**
> Done: B2 (sine flicker carrier), A2 (trapezoid isochronic envelope), V-E1 (finer
> flicker tick), V-E2 (per-channel phase offset → cool/warm invisible flicker), A3
> (binaural beat jitter). **B3 (photosensitivity clamp) SKIPPED for now per user.**
> Each capability is engine-only + controlled via a **temporary `/api/*` endpoint**
> (no `.ledc`/settings churn); the per-entry `.ledc` + web authoring for all new
> knobs is a deferred **consolidated pass** (`entrainment_authoring_plan.md`). Test
> endpoints: `GET /api/flicker-carrier?wave=0|1|2`, `/api/flicker-phase?mask=&deg=`,
> `/api/iso-env?env=&duty=&attack=&depth=`, `/api/beat-jitter?amp=&period=`.
> Report: `reports/non_planned_reports/entrainment_firmware_report.md`.
>
> Original: Six firmware additions distilled from the 3-agent technique research (see
> `reports/non_planned_reports/entrainment_techniques_playbook.md` for the full
> math + sources). Everything that does NOT need firmware is in the sibling plan
> `entrainment_authoring_plan.md`.

## Goal

Add the small set of capabilities that genuinely require engine changes — the rest
of the researched techniques are already possible via existing knobs (monaural
beats = two centre-panned channels; AM-noise, rotating pan, breath LFO, split-field
with different frequencies = existing `.ledc` features). The six here are:

1. **B2 — Sine / gamma-corrected flicker carrier** (LED comfort; kills the square-wave harmonics fed into vision).
2. **A2 — Trapezoid isochronic envelope** (proper isochronic pulse shape; depth already exists).
3. **V-E2 — Per-channel flicker phase offset + invert** (unlocks invisible cool/warm flicker, phase-locked split-field, traveling waves).
4. **V-E1 — Finer / configurable flicker tick** (clean antiphase at 40 Hz).
5. **B3 — Photosensitivity safety clamp** (15–25 Hz brightness cap, red-flicker throttle, `safety_mode`).
6. **A3 — Binaural beat-offset jitter** (anti-habituation for the binaural path; bonus).

Design rule throughout: **additive and default-off** — every existing `.ledc`
file must behave identically unless it opts into a new field.

Active project dir: `freeesp32_ave` (`source ./activate.sh && idf.py build`).
Platform: dual-core ESP32 @ 240 MHz, PSRAM. Verified engine facts below are from
the technique-research code reading; confirm exact line numbers at edit time.

---

## Key integration points (verified against current source)

| Concern | Location |
|---|---|
| Audio isochronic AM (`sample *= 1 + mod_depth*fast_sin_q32`) | `audio_generator.c` `apply_modulation()` (~1662) |
| `mod_depth` is a real per-channel field (0.1 = parser default only) | `audio_generator.h` `audio_gen_params_t` (~80–86) |
| Binaural routing / beat-offset hold (`current_freq_r = current_freq + freq_diff`) | `audio_generator.c` (~1153–1160, ~759–762) |
| Per-param LFO engine (reusable for A3) | `audio_generator_set_mod()` / `audio_eval_mod()` |
| Quadratic sweep easing | `audio_generator.c` `interpolate_sweep()` (~1651) |
| LED flicker ISR (hard square carrier) | `led_matrix_example.c` `led_flicker_timer_callback()` (~535–640) |
| Per-channel cycle anchor (no phase field today) | `led_flicker_state_t.cycle_start_time_us` (~69) |
| Intra-cycle `phase_q16` + sine/tri/saw/square shape approx (already computed for MODS) | `led_matrix_example.c` (~460–496) — **reuse for B2** |
| Flicker timer tick = 1 kHz | `s_ensure_timer_and_task(1000)` (~217) |
| Flicker state struct (add fields here) | `led_flicker_state_t` (~92) |
| `.ledc` LED line parse (`time freq duty bright R G B mask`) | `config_parser.c` `parse_led_line()` |
| Runtime settings (add `safety_mode`) | `settings.c` (namespace "devcfg") |

---

## Step 1 — B2: sine / gamma-corrected flicker carrier

Today the carrier is binary: `should_be_on = elapsed_us < latched_on_time_us`. A
10 Hz square injects a 30 Hz harmonic into vision (linked to discomfort); a sine
carrier gives clean single-frequency drive and far less fatigue.

1. Add `uint8_t carrier_waveform` to `led_flicker_state_t` (`0=SQUARE` default,
   `1=SINE`, `2=TRIANGLE`).
2. Add a **gamma-corrected sine LUT** in IRAM: `static const uint8_t s_flicker_sine_lut[256]`
   holding `round(255 * ((1-cos(2π·i/256))/2) ^ (1/2.2))` (perceptual-lightness
   corrected so energy lands on the fundamental, not harmonics).
3. In the ISR, when `carrier_waveform != SQUARE`, replace the on/off decision with a
   per-tick output level: compute intra-cycle `phase_q16` (reuse existing math ~469),
   index the LUT (or triangle), scale by `brightness`, and drive the channel output
   at that level instead of full-on/full-off. SQUARE path unchanged.
4. `.ledc`: add an optional carrier token/suffix on the LED line (e.g. a `~`/`^`
   prefix on the `freq` field, or a trailing `wave=` token — pick the least-ambiguous
   given existing prefix semantics) → parsed in `config_parser.c` into `carrier_waveform`.
   Default when absent = SQUARE (byte-identical to today).

**Success:** build clean; existing files unchanged; a `SINE` flicker channel visibly
smoother; scope/logic-analyzer shows the intended brightness curve. CPU delta measured.

## Step 2 — A2: trapezoid isochronic envelope

1. Add `uint8_t iso_env` (`0=SINE` default, `1=TRAPEZOID`), `uint8_t duty` (percent,
   default 50), `uint16_t attack_ms` (default 3) to `audio_gen_params_t`.
2. In `apply_modulation()`, when `iso_env==TRAPEZOID`, compute `ph = mod_phase_q32 / 2^32`
   and evaluate a trapezoid gate E(ph): raised-cosine attack over `attack_ms`, hold,
   raised-cosine decay, then off for the remainder per `duty`. `sample *= (1-depth) + depth*E`.
   SINE path unchanged. Reuse `AUDIO_AMP_RAMP_SAMPLES` (5 ms) as the click-safe attack floor.
3. `.ledc`: expose `iso_env`/`duty`/`attack` — extend the audio line or add a
   modifier; default absent = SINE.
4. This also makes **AM-noise** carriers (pink/brown + mod) into crisp isochronic
   bursts — no extra work.

**Success:** build clean; a TRAPEZOID isochronic channel produces the gated pulse
(verify envelope on capture), no clicks; `mod_depth` UI-adjustable end-to-end;
existing sine-AM files unchanged.

## Step 3 — V-E2: per-channel flicker phase offset (+ invert)

The single field that unlocks invisible cool/warm flicker, phase-locked split-field,
and traveling waves. Today same-freq channels started together flash IN phase (useless
for luminance cancellation).

1. Add `int16_t phase_offset_deg` (0..359, default 0) and `bool invert` (default false)
   to `led_flicker_state_t`.
2. **Store the offset as DEGREES and re-derive the delay per cycle from the LIVE period —
   never store a fixed µs delay.** Phase and time-delay are only interchangeable at a
   constant frequency: a fixed µs offset that equals 180° at 40 Hz (12.5 ms) silently
   becomes 90° when a sweep drops the flicker to 20 Hz (period 50 ms), so the antiphase
   cancellation collapses mid-ramp. Because sessions routinely ramp the flicker frequency,
   the phase must be recomputed each cycle: `offset_us = phase_offset_deg * cycle_duration_us / 360`
   using the *current* `cycle_duration_us`. Apply that offset to the cycle origin relative
   to a **shared transport clock**, not to each channel's own free-running `now_us` re-anchor
   (the current re-anchor-to-`now_us` lets two channels drift and jitters on non-integer
   periods — see Risks). This is *the* reason a `.ledc` start-time stagger (e.g. `t=0` cool /
   `t=12` warm) is only a fixed-40 Hz bench trick, not a real feature: integer-ms `time`
   can't express 12.5 ms (→ 172.8°/187.2°, never 180°) and, decisively, a fixed time offset
   stops being 180° the instant the frequency ramps.
3. `invert`: emit the complement of the on/off (or 1-level for sine) — the minimal way
   to make a warm channel the inverse of a cool channel on the SAME timing without a
   second phase-shifted entry.
4. `.ledc`: add an optional `phase=<deg>` token (and/or `inv`) on the LED line.

**Success:** build clean; two 40 Hz channels at `phase=0` and `phase=180` are
measurably antiphase (capture); cool+warm antiphase on the user's glasses hardware
reads as near-constant luminance (invisible flicker) once brightness-balanced;
`phase` absent = 0° = today's behavior.

## Step 4 — V-E1: finer / configurable flicker tick

At 1 kHz tick, 40 Hz has 1 ms/25 ms = 8% edge error — too coarse for clean antiphase.

1. Make the tick configurable; raise the default (or the antiphase-mode value) toward
   **10 kHz** (0.4% error). Keep it a named constant.
2. **Measure ISR cost first.** ~250 cy/channel × 8 channels = ~2000 cy/tick →
   1 kHz ≈ 0.8% of a core, 10 kHz ≈ ~8%. If 8% is too much alongside audio/WiFi,
   compromise at **~5 kHz** (0.8% error, ~4% CPU), or raise the tick only while a
   phase/antiphase feature is active.
3. Confirm the ISR stays IRAM-safe and does not starve the audio output task
   (prio/core) or LWIP.

**Success:** build clean; at the chosen tick the Step-3 antiphase pair shows the
expected residual luminance ripple (<~1% at 10 kHz); measured CPU headroom logged;
no audio underruns / no flicker-task starvation over a sustained run.

## Step 5 — B3: photosensitivity safety clamp

Make dangerous flicker un-authorable; keep 40 Hz (lower-risk) allowed.

1. Add `safety_mode` (bool, default ON) to `settings.c` (`devcfg`).
2. **Static validation** in `config_parser.c`: when `safety_mode` and an LED line has
   `freq ∈ [15,25] Hz`, clamp brightness/duty to a safe ceiling (or warn+cap); if a
   channel is saturated red (RGB backend, `R/(R+G+B) ≥ 0.8`) and `freq > 3 Hz`,
   desaturate / cap depth. Emit a parse warning either way.
3. **Live clamp** as a backstop in the flicker path (so a runtime `set_freq` into the
   band is also capped).
4. Thresholds & rationale from the playbook §B3 (Fisher/Harding, ITU-R BT.1702-3).

**Success:** build clean; a `18 Hz / bright / red` LED line is capped with a logged
warning; a 40 Hz line is untouched; toggling `safety_mode` off restores raw behavior;
default-on.

## Step 6 — A3: binaural beat-offset jitter (bonus, anti-habituation)

The binaural beat offset is currently rigid (`current_freq_r = current_freq + freq_diff`).
A steady beat habituates in minutes.

1. Add an optional slow LFO on the beat offset: reuse the `mods[]` machinery targeting a
   new "beat offset" pseudo-param, or add `beat_jitter_hz` + `beat_jitter_period_ms`
   evaluated like `audio_eval_mod` and added to `freq_diff` each buffer.
2. Typical: ±0.1–0.3 Hz, non-commensurate periods (37 s / 53 s). Default off.
3. (Isochronic anti-habituation already works via `set_mod(ch, AUDIO_PARAM_MOD_FREQ, …)`
   — no code; documented in the authoring plan.)

**Success:** build clean; a binaural channel with jitter enabled shows the beat slowly
wandering ±0.3 Hz; default-off leaves existing binaural byte-identical.

## Step 7 — Build clean + hardware verification (orchestrator/user)

1. `idf.py build` clean; measure app size delta and CPU (esp. Step 4).
2. **Orchestrator flashes** (subagents do not). Verify on the user's cool/warm glasses
   + audio: sine-flicker comfort (B2), trapezoid isochronic (A2), **cool/warm antiphase
   invisible flicker (V-E2+V-E1)** balanced to near-constant luminance, safety clamp
   (B3), binaural jitter (A3). Regression: a handful of existing `sessions/library/*.ledc`
   sound/look identical.

**Success metrics:** all six features demonstrable; existing sessions unchanged; CPU
within budget (target <10% total for LED path); no underruns; build clean.

---

## Risks & notes

- **Phase-rigidity (Step 3) is the #1 subtlety.** The per-cycle re-anchor must preserve
  the intended offset between two channels over minutes; re-anchoring each channel to a
  bare `now_us` at its own boundary lets them drift. Anchor to `prev_start + cycle_duration`
  or to a shared transport origin + offset. Verify with a long capture, not just one cycle.
- **V-E1 CPU is a real tradeoff** — measure before committing to 10 kHz; 5 kHz may be the
  sweet spot. Consider raising the tick only when any channel has a non-zero phase offset.
- **Invisible flicker needs luminance balance**, which is per-hardware (warm vs cool LED
  efficacy differ) — expose a per-channel brightness trim or a calibration step; the
  firmware just needs to hold the antiphase timing.
- **B2/B3 interact:** a sine carrier has a lower effective flash contrast than square,
  which is *safer* — factor that into the B3 clamp (sine may be allowed brighter than square).
- **All six are default-off/absent-safe** — the regression bar is "every existing `.ledc`
  behaves identically." Keep it that way.
- On completion write `reports/non_planned_reports/entrainment_firmware_report.md`.
