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
