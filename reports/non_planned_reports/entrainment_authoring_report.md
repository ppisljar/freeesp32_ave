# Entrainment Authoring — Implementation Report

**Date:** 2026-07-03
**Plan:** `plans/entrainment_authoring_plan.md`
**Source research:** `reports/non_planned_reports/entrainment_techniques_playbook.md`

Turned the "already possible but undocumented / not surfaced" entrainment techniques
from the 3-pass techniques research into shipped presets, sessions, editor macros, a
safety opt-in, and rewritten authoring docs. **No firmware and no `.ledc` engine change**
— everything rides on existing `.ledc` features (incl. the v2 pulse fields) and the web
generator. Four parallel work-streams; all landed.

## 1. Sessions — `sessions/library/*.ledc`

**Refined in place** (filenames stable — referenced by `push_library.sh`):
- `01_sleep_onset` — Sleep Descent arc: 10 Hz alpha entry → continuous 10→2 Hz glide at
  ~0.44 Hz/min → simultaneous Soft Off, no wake-up.
- `04_meditation_theta` — Theta Dive: 10→6 Hz at 0.5 Hz/min, 6 min flat theta hold with a
  dual-frequency **L6.0 / R6.3 Hz** deepening offset, Return to alert alpha.
- `05_focus_smr` — 14 Hz SMR carried **100 % by audio isochronic**, LED held steady at
  `freq 0` (dim), ends **bright/alert** at 12 Hz / 40 %.
- `07_genus_40hz` — faithful GENUS: 40 Hz light 50 % duty + 40 Hz audio **4 %-duty click**
  (10 kHz carrier, `env=0` square gate), fixed 60 min, fade out.
- `15_ganzflicker_imagery` — 10 Hz eyes-closed imagery with a **sine gate** (`env=1`) and a
  ±0.5 Hz floating drift.

**New** (research techniques that had no session):
- `26_alpha_breath` — 10 Hz alpha + **0.1 Hz brightness breath LFO** (coherence pacing),
  up-ramp to 12 Hz + brighten to end alert.
- `27_genus_40hz_dim` — dim/comfort GENUS via an **invisible-flicker antiphase pair**
  (amber `phase 0` / blue `phase 180`), with a `channel_map` interleave recipe.
- `28_noise_sleep_bed` — **AM-noise** pink bed, smooth 0.85 Hz sine AM + brown warmth bed.
- `29_noise_sleep_pulsed` — the pulsed variant: pink noise **trapezoid gate** (`env=3`,
  40 % duty, 50 ms attack).
- `30_splitfield_lucid` — **split-field per-eye** (LEFT 16 Hz / RIGHT 6 Hz via masks +
  `channel_map`), dim, soft-off ending.
- `31_ganzfeld_amber` — **eyes-closed Ganzfeld**, steady amber-red field (≥620 nm eyelid
  bias) + pink field + 7 Hz theta underlay.

`26`/`27` form the invisible-flicker antiphase pair called for in the plan.

| # | Session | Technique | Playbook ref | Status |
|---|---------|-----------|--------------|--------|
| 01 | Sleep Onset | 4-phase descent + Soft Off | §C arc A | refined |
| 04 | Meditation Theta | Theta Dive + L/R offset | §C arc B | refined |
| 05 | Focus SMR | audio-carried beat, steady dim light | §C arc D | refined |
| 07 | GENUS 40 Hz | faithful GENUS click + light | §C GENUS / B4 | refined |
| 15 | Ganzflicker | 10 Hz sine-gate imagery | B2 / B7 | refined |
| 26 | Alpha + Breath | 0.1 Hz breath LFO | §C arc C / breath | new |
| 27 | GENUS dim | invisible antiphase pair | B1 | new |
| 28 | AM-Noise (smooth) | AM-noise carrier | A6 | new |
| 29 | AM-Noise (pulsed) | trapezoid AM burst | A6 + A2 | new |
| 30 | Split-Field Lucid | per-eye split field | B5 | new |
| 31 | Ganzfeld (amber) | eyes-closed red bias | B7 / B8 | new |

## 2. Macros — `web/src/js/gen/macros.js`

New authoring macros + research-backed preset tables surfaced as wizard buttons:
- **`monaural`** — two centre-panned channels (`pan=0`) at `base` / `base+beat`; the beat
  forms acoustically → works on a single speaker (companion to `binaural`).
- **`harmonicStack` / `harmonicCarriers`** — N octave carriers sharing one Δf, −4.5 dB/oct
  roll-off, 1/N auto-scaled, optional 20 Hz subharmonic.
- **`breathMod` / `breathPeriodMs`** — 0.1 Hz (6 bpm default) coherence-breath LFO.
- **`rotatingPanMod`** — sine pan LFO (8–30 s, default 15 s).
- **`FLICKER_PAIRS`** — luminance-complementary invisible-flicker pairs (`amberBlue`,
  `redCyan`).
- **`BRAINWAVE_PRESETS`** — research carriers: δ 200 / θ 250 / α 370 / β 420 / γ 340 Hz.
- **`COLOR_PRESETS`** — SSVEP-weighted: amber/red 8.06 dB, blue/cyan 6.82 dB, green 2.85 dB.

## 3. Epilepsy opt-in — `web/src/js/safety.js`

One-time, **browser-side** photosensitivity opt-in gating the play path
(`ensureSafetyAccepted`). Only prompts when the doc actually flickers an LED
(`docHasFlicker` / `ledRowFlickers`); acceptance stored in `localStorage`
(`ave:safetyAccepted`, version-tagged so wording changes can re-prompt), resettable from
Settings. Experiential (non-medical) contraindication copy. **Deliberately no device
wiring** — the firmware `safety_mode` clamp was cut (see §Cross-deps), so this is a no-op
on the device.

## 4. Docs

- **Rewrote `sessions/SESSION_DESIGN_GUIDELINES.md`** around the playbook: 4-phase arc +
  goal-specific endings, band→state table, ramp-slope guidance (≤2 Hz/min, warn >3),
  carrier + SSVEP-colour tables, monaural vs binaural, the 0.1 Hz breath layer, AV-sync
  ("ms budget, µs is margin"), the full safety section, a `.ledc` field-mapping table with
  gotchas, worked examples (`01`, `05`), and a dedicated **Firmware cross-deps** section.
- This report; `reports/list.md` updated.
- `sessions/README.md` was updated by the sessions work-stream (adds rows 26–31).

## Research numbers applied

Carriers δ200/θ250/α370/β420/γ340 Hz; beat perception peak ~400 Hz carrier, 340 Hz for
40 Hz gamma; beat ≤35 Hz. SSVEP: amber/red 8.06 dB, blue/cyan 6.82, green 2.85.
Eyelid transmission ~14.5 % @700 nm vs ≤3 % <580 nm → amber/red ≥620 nm eyes-closed.
Ramp slope 0.25–2 Hz/min (warn >3). Breath 0.1 Hz (6 bpm). GENUS: 40 Hz 50 % light duty,
1 ms/25 ms = 4 % audio click. AV sync tolerance ~20–40 ms. Photosensitive danger band
15–25 Hz (peak 16–20).

## Validation

- **Sessions:** all **31** pass `node validate_session.mjs library/<file>.ledc` with
  **0 errors**.
- **Web:** `node --test test/*.test.js` → **183 tests, 183 pass, 0 fail**.

## Deviations / notes

- **`30_splitfield_lucid` emits 2 intentional warnings** — its LEFT eye runs 16 Hz (in the
  15–25 Hz photosensitive band). Kept dim (≤18 %) and single-eye; the validator warns on
  those two rows by design and the header documents the intent. Every other session is
  0-warning.
- **Fixed ~0.1 AM depth:** the audio AM/isochronic depth is hard-fixed in this build, so
  the GENUS click and AM-noise bursts (`07`, `28`, `29`) are gentle-by-design; their
  headers note this pending a firmware depth control.

## Remaining firmware cross-deps (status corrected 2026-07-03)

The v2 engine work (`entrainment_firmware_plan.md`) is **build-clean and DONE**, pending
the user's one-time hardware flash. **Available after that flash:** sine/trapezoid flicker
carriers, per-channel flicker `phase` (invisible antiphase), trapezoid isochronic
`env`+`attack`, GENUS audio `duty`, beat `jitter`, and full per-channel `.ledc` v2 wiring
with the `-` skip sentinel. Sessions `27`/`29` (and the `env`/`duty` columns in `07`/`15`)
render fully only after that flash and degrade gracefully before it.

**Genuinely still future:**
- **(a)** Photosensitivity clamp `safety_mode` — **cut**; the epilepsy interlock is
  browser-only.
- **(b)** Asymmetric breath LFO shape (4 s:6 s inhale:exhale) — needs a new LFO shape;
  symmetric sine ships now.
- **(c)** Ramps/modulation ON the v2 pulse fields — `env`/`phase`/`attack` are step-only
  on-device for now.
- Plus the fixed ~0.1 AM depth noted above.
