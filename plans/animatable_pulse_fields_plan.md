# Animatable pulse fields — `phase`, `attack`, audio `duty`

**Status:** Steps 1–4 DONE and hardware-verified 2026-08-24 (OTA). Step 5 (web
UI editor) NOT started — see the note at the end.

Hardware verification, read live from `/api/state` while the timeline ran:

| What | Expected | Measured |
|---|---|---|
| audio duty ramp 10→80 / 5 s | 38.0 @ t=2000 | **37.6** |
| " | 73.7 @ t=4550 | **73.4** |
| audio duty mod `~10:80:5000` | oscillate 10↔80, 5 s period | **10.8 → 78.3 → 12.9 → 77.7**, `modf.duty=true` |
| audio phase ramp 0→180 / 10 s | 10.8° @ 0.6 s in | **10.9°** |
| audio attack ramp 3→40 ms / 10 s | 16.8 @ 3.74 s | **16.7** |
| " | 32.4 @ 7.94 s | **32.3**, then holds 40.0 |

Real-time path unaffected: 0 write errors, 0 short writes, 0 gen failures across
8892 buffers; all health subsystems ok. IRAM ended at the pre-change baseline
(126187 bytes, 4885 free) after the de-inlining work — the feature costs DRAM
only (+6.9 KB).

## Problem

`ledc_format.md` says these are compound cells, i.e. they accept the same
interpolation prefixes (`>` `*` `~` `^` `/` `\` `_`) as `freq`/`bright`/`vol`:

> `freq duty bright R G B` (LED) and `freq pan vol mod freqR duty phase attack`
> (audio) and LED `phase attack` are **compound cells**
>
> So **"modulate pulse duty 10→80% over 5 s"** is just `~10:80:5000` in the `duty` field.

The device does not do this. Every v2 pulse field is parsed through:

```c
static inline float parse_v2_value(const char *t) {
    config_interpolation_t dummy;
    return parse_value_with_interpolation(t, &dummy);   // interp parsed, then dropped
}
```

The prefix is parsed and thrown away, so `~10:80:5000` on a duty field plays as
a flat 10%. The core fields instead keep it:

```c
led_entry->duty_cycle = parse_value_with_interpolation(tokens[2], &led_entry->duty_interp);
```

`config_led_entry_t` / `config_audio_entry_t` carry `*_interp` (+ `*_mod_end`,
`*_mod_period_ms`) for the core fields only — there is no `phase_interp`,
`attack_interp` or audio `duty_interp` to write into.

### Where each layer stands

| Field | `ledc_format.md` | web parse/model/serialize | firmware struct | firmware runtime |
|---|---|---|---|---|
| LED `freq duty bright R G B`, audio `freq pan vol mod` | compound | cell | `*_interp` + mod extras | sweeps + mod_engine |
| LED/audio `phase`, `attack` | compound | **cell — round-trips today** | none | interp discarded |
| audio `duty` | compound | **cell — round-trips today** | none | interp discarded |
| audio `freqR` | compound | **plain number** | plain `atof` | "not an `AUDIO_PARAM_*` sweep pair" |
| `jitter` amp/period | **not** listed as compound | `{amp, period}` object | amp only | interp discarded |

The web stack is already ahead of the firmware for `phase`/`attack`/audio
`duty`: a ramp authored in the Text view parses, round-trips and re-serializes
intact. Only the device ignores it. That is why the table view deliberately
shows plain number boxes — an editor there would author ramps the device
silently flattens.

## Scope

**In:** LED `phase`, LED `attack`, audio `duty`, audio `phase`, audio `attack`.
These are already compound cells end-to-end in the web stack, so the work is
firmware-only until the last step.

**Out — `freqR`.** The format doc lists it as compound but nothing implements
it: the web parser stores a plain clamped float (no cell), and the firmware
comment states it is deliberately *not* an `AUDIO_PARAM_*` sweep pair. Making it
animatable means changing the web model, parser and serializer as well as the
firmware. Separate job.

**Out — `jitter`.** The format does not describe it as a compound cell; it is
`<amp>[:<period_ms>]`. Animating it is a format extension, not an
implementation gap — and jitter is itself a slow anti-habituation wander, so
modulating it is modulating a modulation. Worth a design discussion first.

## Steps

### 1. `config_parser` — stop discarding the interp

- Add to `config_led_entry_t`: `phase_interp`, `attack_interp`, plus
  `phase_mod_end` / `phase_mod_period_ms`, `attack_mod_end` /
  `attack_mod_period_ms`.
- Add to `config_audio_entry_t`: `duty_interp`, `phase_interp`,
  `attack_interp` plus matching mod extras.
- Replace the `parse_v2_value(tokens[i])` calls for those fields with the
  core-field pattern already used a few lines above: capture into the entry's
  `*_interp`, then `parse_mod_extras()` when `config_interp_is_modulation()`.
- `parse_v2_value` stays for `env` (an enum — explicitly not ramped).

**Watch:** `sizeof(config_entry_t)` grows, and the timeline entry pool is
pre-allocated (`timeline_entry_pool = 100 entries, 10400 bytes` at boot). Check
the pool still sizes correctly and log the new per-entry cost.

### 2. `audio_generator` — three new sweepable params

- Extend `audio_param_t` with `AUDIO_PARAM_ISO_DUTY`, `AUDIO_PARAM_ISO_PHASE`,
  `AUDIO_PARAM_ISO_ATTACK` before `AUDIO_PARAM_COUNT`. Both `sweeps[]` and
  `mods[]` are sized by that count, so the per-channel state widens
  automatically — **measure the RAM cost across 16 channels.**
- Extend the sweep-start snapshot switch (`audio_generator.c` ~line 517) with
  the three new cases, reading back `ch->iso_duty * 100`, the phase held in
  `ch->mod_phase_offset_q32`, and `ch->iso_attack_ms`.
- Evaluate the new sweeps/mods **once per `fill_buffer` block, not per sample**,
  writing into `ch->iso_duty` / `ch->mod_phase_offset_q32` / `ch->iso_attack_ms`.

The per-block choice is the critical design decision. These are pulse *shape*
parameters that change over seconds; the existing per-sample stepping exists for
audio-rate params (frequency, amplitude). `fill_buffer` runs from IRAM — commit
`5e57c13` was specifically about freeing IRAM — so adding per-sample work to it
costs both cycles and IRAM for no audible benefit.

### 3. `mod_engine` — periodic modulation on the new fields

- Add `MOD_AUDIO_ISO_DUTY`, `MOD_AUDIO_ISO_PHASE`, `MOD_AUDIO_ISO_ATTACK` to
  `mod_audio_field_t`.
- Extend the dispatch in the `fields[]` loop in `config_parser.c` (~line 983)
  that currently maps `AUDIO_PARAM_*` → `mod_audio_field_t`.

### 4. LED — `phase` / `attack` curves

- Add `phase_curve` / `attack_curve` (and start/target values) to
  `led_sweep_spec_t`, alongside the existing `freq_curve` / `duty_curve` /
  `bright_curve` / `r,g,b_curve`.
- Interpolate them in the LED flicker engine's per-tick update.
- Wire them in the spec built at `config_parser.c` ~line 1065, using the same
  `patch_field_curve_led()` helper.

### 5. Web UI — enable the editor

Only after 1–4 are on hardware:

- Swap `pulseCellInput` for `createCompoundCell` on phase / attack / audio duty
  in `web/src/js/gen/views/table.js`. They are already cells in the model, so
  this is small.
- Confirm `ledRampTarget` / `audioRampTarget` resolve targets for these field
  names (they take the field name already — verify, don't assume).
- Drop the "Ramps ON pulse fields are a deferred device feature" comment.

### 6. Validation

- **Web unit tests:** `>` and `~` on phase/attack/duty survive
  parse → serialize. The byte-identical library round-trip gate
  (`format_v2.test.js`) must stay green.
- **Firmware:** a fixture that ramps audio duty 10→80% over 5 s and sweeps LED
  phase 0→180°; confirm the shape actually changes rather than holding the
  start value — the exact symptom this plan fixes.
- **Hardware:** flash + monitor is the orchestrator's job, not a subagent's
  (see the hardware policy in `CLAUDE.md`). Check IRAM headroom and free heap
  before/after.
- **Timing:** the project targets ±100 µs audio-LED sync. Re-run the timing
  precision test; per-block evaluation should leave it untouched, but confirm.

## Risks

- **IRAM.** `fill_buffer` is IRAM-resident and IRAM has been tight enough to
  warrant its own commit. Measure before/after; per-block evaluation is the
  mitigation.
- **Per-channel RAM.** Widening `AUDIO_PARAM_COUNT` grows `sweeps[]` and
  `mods[]` for all 16 channels.
- **Entry pool.** Three new interps plus mod extras per entry, ×100 pooled
  entries.
- **Silent partial support.** If step 5 ships before steps 1–4 are verified on
  hardware, the UI advertises ramps the device flattens — the precise failure
  this plan exists to remove. Do not reorder.
