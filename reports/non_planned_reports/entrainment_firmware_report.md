# Entrainment Firmware Enhancements — Implementation Report

**Plan:** `plans/entrainment_firmware_plan.md`
**Date:** 2026-07-03
**Status:** 5 of 6 steps implemented, `idf.py build` clean. **NOT flashed / hardware-
verified** (orchestrator/user's job). B3 (photosensitivity clamp) skipped per user.

## What was built

All additive and **default-off / absent-safe** — every existing `.ledc` session
behaves identically. Each capability is engine-only, controlled for now via a
temporary `/api/*` endpoint; the permanent per-entry `.ledc` + web authoring for
all new knobs is a deferred consolidated pass.

### B2 — sine / triangle flicker carrier (`led_matrix_example.c/.h`)
- New per-channel `carrier_waveform` (`SQUARE`/`SINE`/`TRIANGLE`) + `output_level`;
  ISR computes a per-tick brightness curve for non-square carriers via a new
  IRAM-safe `led_carrier_level_iram()` (reuses the proven parabolic `(1-cos)/2`).
- **Luminance-linear on purpose (no gamma)** so an antiphase pair sums flat.
- SQUARE path is a separate branch = byte-identical. Control: `led_matrix_set_carrier()`
  / `GET /api/flicker-carrier?wave=0|1|2`.

### A2 — trapezoid isochronic envelope (`audio_generator.c/.h`)
- At the modulation stage, `env=TRAPEZOID` applies a duty-cycled gate
  (`sample *= (1-depth) + depth*env`; linear attack/decay ≥ click-safe) — a true
  isochronic pulse vs the legacy sine tremolo. Read at the call site (audio-task
  context), so no param-struct/start-path changes. Globals + `audio_generator_set_iso()`
  / `GET /api/iso-env?env=&duty=&attack=&depth=`. Depth override provided for testing
  (parser still defaults mod_depth to 0.1).

### V-E1 — finer / scaling flicker tick (`led_matrix_example.c`)
- Root cause found: the tick was *designed* to scale 100× frequency but the boot
  pre-warm pinned it to 1 kHz and it was never re-raised → 40 Hz ran at 8% edge
  error. Now the tick scales **up** with the requested frequency
  (`MULT=250`, floor 1 kHz, **cap 10 kHz**) by reconfiguring the running gptimer
  alarm; only ever increases within a session. 40 Hz → 10 kHz (0.4% edge).
- ⚠️ CPU: ~8%/8ch at 10 kHz — measure on hardware; cap is the guard, 5 kHz is a fallback.

### V-E2 — per-channel flicker phase offset (`led_matrix_example.c/.h`)
- New `phase_offset_deg` (0..359), applied **at use-time** in the ISR:
  `delay = deg * live_period / 360` (32-bit safe: deg≤359 × period≤1e7 fits u32).
  Because it's derived from the live period each tick, it stays a fixed *phase*
  through frequency ramps (a fixed start-time stagger would not). Two channels
  sharing a transport origin (same `.ledc` time / peer-piggyback) re-anchor in
  lockstep, so **180° on one of a cool/warm pair = rock-solid antiphase → luminance-
  flat "invisible" flicker.** Control: `led_matrix_set_phase_masked()` /
  `GET /api/flicker-phase?mask=2&deg=180`.

### A3 — binaural beat-offset jitter (`audio_generator.c/.h`)
- Adds a slow ±`amp` Hz sine wander to the binaural beat (`freq_diff`) per buffer
  for anti-habituation (reuses `fast_sin_q32`; default off = binaural byte-identical).
  `audio_generator_set_beat_jitter()` / `GET /api/beat-jitter?amp=0.2&period=45000`.

### B3 — photosensitivity clamp — SKIPPED (per user, 2026-07-03).

## Verification done
- `idf.py build` clean after each step (app ≈ 0x133830). macOS-host clang
  diagnostics (xtensa flags, IRAM section attr) are false positives.
- IRAM safety preserved in the LED ISR (no 64-bit ops / no flash helpers in the new
  carrier-level + phase-offset math). Audio-side new math runs in the audio task
  (float/`fast_sin_q32` fine).

## Remaining (hardware — orchestrator/user)
Flash + verify on the cool/warm glasses + audio:
- B2 sine flicker looks smooth (`?wave=1`); square unchanged (`?wave=0`).
- V-E2: start a cool/warm pair (same freq, one mask 180°), balance brightness →
  near-constant luminance (invisible flicker). Confirm it holds through a freq ramp.
- V-E1: confirm no audio underruns / flicker-task starvation at 10 kHz (measure CPU).
- A2 trapezoid isochronic sounds like crisp on/off pulses (`?env=1&depth=90`).
- A3 beat slowly wanders (`?amp=0.3&period=45000`); default off = unchanged.
- Regression: a few existing `sessions/library/*.ledc` sound/look identical.

## Follow-ups
- **Consolidated authoring pass**: expose carrier / phase / iso-env per-entry in
  `.ledc` + the web editor (design once for all knobs) — `entrainment_authoring_plan.md`.
- **B3** photosensitivity clamp when desired.
- V-E1 CPU tuning after measurement (10 kHz vs 5 kHz).
