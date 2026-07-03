# `.ledc` Format v2 — Implementation Report

**Spec:** `ledc_format.md` (authoritative). **Date:** 2026-07-03.
**Status:** Spec locked; web layer fully implemented + regression-proven; device
parser made tolerant. **Full per-channel wiring + unbuilt capabilities are a
follow-up** (task 25).

## What v2 adds
Shared per-channel **pulse** fields on both line types (the two-layer model —
carrier vs pulse), each a positional field with the `-` = "leave unchanged"
sentinel:
- LED: `… mask [env] [phase] [attack] [jitter]`
- Audio: `… waveType [duty] [env] [phase] [attack] [jitter]`
- `env`: 0 square · 1 sine · 2 triangle · 3 trapezoid · 4 sine-tremolo. Default =
  each line's legacy (LED 0, audio 4) — both explicitly selectable, so "absent" is
  pure sugar for the default and existing sessions are byte-identical.
- `phase`/`attack`/`duty` are compound cells (take `>`/`~` ramp/mod prefixes).
- `-` in any field = keep the channel's current value.

## Done

### Web (validated)
- `model.js`: new fields on led/audio rows; constructors use `=== undefined` so an
  explicit `null` (`-`) survives.
- `parse.js`: LED 8..12 / audio 5..13 tokens; `parseCell('-')→null`; new-field helpers.
- `serialize.js`: `cellStr(null)→'-'`; trailing-with-`-` logic (omit trailing absent,
  `-` for gaps, carrier placeholders when a new field forces them). New fields
  emitted only when set.
- **Tests: 155 → 163 green.** Two regression gates pass: **fixpoint across all 25
  library sessions**, and **no v1 line gains any pulse token**.
- Consumer guards: `synth.js` carries `-` forward per channel (bounce-correct);
  `table.js` renders `null` fields as a read-only "—" chip (no crash).

### Firmware (tolerant)
- `config_parser.c`: LED accepts 8..12 tokens (`>=8` canonical branch); audio already
  accepted `>=5`. v2 files **load**; new pulse tokens are **parsed-tolerated but not
  yet applied** (logged). Existing 5/8-token files behave identically. Build clean.

## Symmetry complements (user chose: build all 4 + `-` device skip-semantics)
The symmetric format implies 4 mirror capabilities the engine hadn't built:
- **[DONE] audio unipolar gates** `env=0/1/2` (square/sine/triangle) + **env renumbered**
  to match the format (0 sq,1 sine,2 tri,3 trapezoid,4 tremolo; audio default 4). Generalized
  `iso_gate()`; build clean.
- **[TODO] LED trapezoid (`env=3`) + attack** — extend `led_carrier_level_iram` (IRAM Q16 math).
- **[TODO] LED flicker-rate jitter** — slow LFO on flicker freq (mirrors audio A3).
- **[TODO] audio phase** — phase offset on the mod accumulator (mirrors LED V-E2).
- **[TODO] `-` device skip-semantics** — per-field `present` flags in the entry structs;
  execute path skips `-` fields (keep current). Plus full per-channel wiring in config_parser
  (retire the `/api/*` globals).

## Known gaps → follow-up (task 25)
1. **Wire v2 fields to the engine** (retire the temporary `/api/*` globals):
   LED `env`→per-channel carrier (already a per-channel field), LED `phase`→
   `set_phase_masked`, audio `duty`/`env`→per-channel iso, audio `jitter`→beat jitter.
2. **Build the capabilities the spec names but the engine lacks:** LED trapezoid
   (`env=3`) + LED `attack` + LED `jitter` (flicker-rate); audio unipolar gates
   `env=0/1/2` (only trapezoid+tremolo exist); audio `phase`.
3. **`-` on the device:** the tolerant parser degrades `-`→0. Cleanest fix is
   **resolving `-` web-side before send** (carry-forward in `serializeForDevice`, like
   `synth.js` already does) so the device never sees `-`; alternatively add skip-
   semantics to the device execute path. Also matters for device-stored sessions.
4. **UI**: Table/Wizard controls for the new fields (text view round-trips them today).

## Regression contract (holds)
Bare number still just sets the value; `-` is purely additive; the 25 library
sessions round-trip byte-stable on the web and load identically on the device.

---

## Follow-up landed (2026-07-03): full engine wiring + device `-` skip-semantics

All four "Next steps" above are now implemented (build clean incl. link; not yet
flashed — user validates on hardware). Summary of the changes:

**Per-channel engine setters** (mirror the existing `led_matrix_set_phase_masked` /
`audio_generator_set_phase` pattern; write per-channel state under the engine mux so
they also affect an already-running channel):
- `led_matrix_set_carrier_masked(mask, wave)`, `led_matrix_set_attack_masked(mask, ms)`,
  `led_matrix_set_jitter_masked(mask, amp_hz, period_ms)` — the global
  `led_matrix_set_*` variants only applied at the next flicker start (latch).
- `audio_generator_set_iso_channel(ch, env, duty, attack, depth)` with per-arg "leave"
  sentinels (`env<0` / `duty<0` / `attack<0` / `depth<0`). The audio channel struct
  gained per-channel `iso_env/iso_duty/iso_attack_ms/iso_depth`, latched from the
  `s_iso_*` globals at `start_channel_locked`; `fill_buffer` now reads them per channel
  (previously it read the globals). The global `audio_generator_set_iso` additionally
  broadcasts to every live channel so `/api/iso-env` is audible immediately.
- `audio_generator_get_current_freq_r_locked` — no-mutex reader for the held-lock
  timeline dispatch path.

**`-` skip-semantics (three-state) on the device.** `parse_led_line` / `parse_audio_line`
now record a `present` bitmask (`LED_SET_*` / `AUD_SET_*`). A lone `-` token (detected
by `tok_is_dash`, so a negative number like `-50` still parses) or an absent trailing
token leaves the bit clear. In `execute_timeline_entry_ctx`, any absent CORE field is
back-filled with the channel's LIVE value so the write is a no-op ("leave unchanged"):
LED via `led_matrix_get_snapshot` (lowest set bit of the mask; a multi-bit mask with
divergent live values collapses to the low bit's value — an unusual authoring choice),
audio via `audio_generator_get_param_locked` + the new `_freq_r_locked`. The NEW pulse
fields are applied per-channel only when their present bit is set. This fixes a latent
corruption: `atof("-")` → `0` would previously have silently zeroed any `-` core field.
(Chosen over web-side carry-forward so device-stored sessions behave correctly too.)

**Test endpoints** for at-the-end hardware verification: `/api/flicker-carrier?wave=`,
`/api/flicker-phase?mask=&deg=`, `/api/flicker-attack?ms=`, `/api/flicker-jitter?amp=&period=`,
`/api/iso-env?env=&duty=&attack=&depth=`, `/api/beat-jitter?amp=&period=`,
`/api/audio-phase?ch=&deg=`.

**Scope note.** Sweep/modulation ON the new pulse fields (e.g. `~10:80:5000`) is parsed
as a step value only for now (glyph ignored) — animating the pulse fields is a further
follow-up. `wave_type` keeps its prior absent→`0` (SINE) default rather than "leave".

**Regression contract still holds:** the 25 library sessions carry all core fields and
no trailing fields → `present` fully set → no substitution and no new setters fire →
behaviour byte-identical to pre-v2.

---

## Web-UI support landed (2026-07-03): all three structured editors

An audit found the three structured generator editors did not surface the v2 pulse
fields, and two of them silently DESTROYED them on edit. Fixed in two phases.

**Phase A — lossless pass-through (the critical data-loss fix).** The lane and wizard
editors are *projection-based*: they decompose rows into keyframes/layers and recompile
fresh rows on every edit, so they dropped `env/phase/attack/jitter` (+ audio `duty`) and
the `-` sentinel on any edit. Table only lost them on `cloneRow` (Duplicate/type-change).
- `table.js`: `cloneRow` forwards the new fields via a `cloneOpt` helper (preserves the
  `undefined`/`null`/cell tri-state).
- `lane_serialize.js`: a time-keyed `lane.pulse` sidecar (captured in `reconstructLanes`,
  merged in `lanesToDoc`, times added to `laneUnionTimes`) rides through the `@ave-lane`
  metadata JSON untouched.
- `wizard_compile.js`: a layer-level `layer.pulse` (captured in `audioRowToLayer`/
  `ledRowToLayer`, stamped on every slot in `expandLayer`, merged in `makeRow`/`makeOffRow`).
- Tests: `web/test/pulse_passthrough.test.js` (lane structural + metadata + wizard).

**Phase B — actual edit controls.** New pure shared module `web/src/js/gen/pulse.js`
provides the tri-state text↔value helpers (`''`→omit / `-`→leave / value→set), one tested
implementation backing all three editors (`web/test/pulse.test.js`).
- **Table**: a per-row `⋯` toggle reveals a hidden sub-row (desktop) / a `pulse` `<details>`
  (mobile card) with an env `<select>` + text inputs for phase/attack/jitter (+duty on audio).
- **Lane**: a keyframe long-press → "Pulse fields at this time…" opens an inspector editing
  the time-keyed `lane.pulse[t]` (pulse is time-keyed and the lane view is canvas-based, so a
  time-anchored context-menu entry fits better than a per-lane DOM header).
- **Wizard**: a "Pulse shape" block in the Advanced panel of each layer edits `layer.pulse`
  (layer-level → applies to every row the layer expands to).

Ramps *on* the pulse fields (e.g. `~10:80:5000`) remain Text-view-only for now — the
structured editors set step values, matching the device (ramp-on-pulse is a deferred
firmware feature). 163 web tests green; bundle builds.
