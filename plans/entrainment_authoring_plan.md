# Entrainment Techniques — Authoring, Presets & Web-UI Plan

> **STATUS (2026-07-03): DONE (all 10 steps).** Sessions (5 refined + 6 new, all
> `validate_session.mjs` 0-error), web macros/presets, epilepsy opt-in UI, and the
> guidelines/report all landed; 183 web tests pass, bundle builds. Independently
> verified (0 critical / 0 high). Step 9's "surface the `safety_mode` device setting"
> was documented rather than wired because the firmware clamp (B3) was cut — the opt-in
> is browser-only. Report: `reports/non_planned_reports/entrainment_authoring_report.md`.
> NOTE: sessions `07/27/28/29` use v2 pulse fields — flash the v2 firmware before pushing
> the library or those trailing fields won't render.
> The techniques from the 3-agent
> research (see `reports/non_planned_reports/entrainment_techniques_playbook.md`)
> that need **NO firmware** — they're achievable with existing `.ledc` features and
> the existing web generator. This is mostly preset tables, session files, editor
> macros, and UI. The firmware-only items are in the sibling
> `entrainment_firmware_plan.md`; a few items here become *better* once that lands
> (flagged as cross-deps).

## Goal

Turn "already possible but undocumented / not surfaced" techniques into real,
discoverable presets and UI:
- **A4** per-band carrier tables, **A5** harmonic-stack macro, **A6** AM-noise sleep
  presets, **A8** rotating-pan + **breath LFO** (0.1 Hz), **B4** GENUS-faithful 40 Hz,
  **B5** split-field per-eye presets, **B7** Ganzfeld eyes-closed presets, **B8**
  colour/wavelength SSVEP presets, the **5 starter session arcs**, plus an epilepsy
  **opt-in** UI and refreshed design guidelines.

Everything here rides on existing engine features: `.ledc` linear/quadratic ramps,
periodic LFO mods (`~ ^ / \ _` with `start:end:period_ms`), per-channel volume, the
16 audio / 8 LED channels, per-pixel `channel_map`, and centre-panned monaural beats.

## What already exists (build on it, don't recreate)

- `sessions/library/*.ledc` — **25 curated sessions** already (incl. `01_sleep_onset`,
  `04_meditation_theta`, `05_focus_smr`, `07_genus_40hz`, `15_ganzflicker_imagery`).
- `sessions/SESSION_DESIGN_GUIDELINES.md`, `sessions/validate_session.mjs`.
- Web editor `web/src/js/gen/macros.js` — `BRAINWAVE_PRESETS`, `COLOR_PRESETS`,
  `NOISE_PRESETS`, `SEGMENT_TEMPLATES` (wizard building blocks).

So most steps = **enrich existing tables/files with the research numbers**, add a few
new sessions, add editor macros, and add the safety opt-in.

---

## Step 1 — Carrier-frequency tables (A4) + monaural-beat helper

- Update `macros.js` `BRAINWAVE_PRESETS` with research carriers: δ~200, θ~250, α~370,
  β~420, γ~340 Hz; beat perception peaks ~400 Hz carrier, 340 Hz for 40 Hz gamma.
- Wizard helper: "band + beat → fill `freq`/`freqR`" (binaural) and a **monaural**
  option (two centre-panned channels `freq`, `freq+beat`, `pan=0`) — document that
  monaural works on speakers/one ear. No engine change.
- Note in guidelines: carrier ≤ ~400 Hz, beat ≤ 35 Hz for good perception.

**Success:** picking a band in the wizard yields research-backed carriers; a
"monaural" toggle emits the two-channel pattern; docs updated.

## Step 2 — Harmonic-stack macro (A5)

- Add a `SEGMENT_TEMPLATES`/editor macro "harmonic stack": emit N audio channels sharing
  the beat Δf on octave carriers (e.g. 100/106, 200/206, 400/406 @ Δf6), upper octaves
  −3…−6 dB via volume. Optional 20 Hz subharmonic layer for 40 Hz gamma.
- Uses existing 16 channels + `1/N` auto-scaling.

**Success:** one click emits a 3–4 channel octave stack; renders/plays richer without
clipping; ~1.5% CPU/3 ch confirmed on device (in the firmware plan's HW pass).

## Step 3 — AM-noise sleep presets (A6)

- Add `NOISE_PRESETS` entries + a `sessions/library/` file: pink/brown noise channel with
  `mod` at the target (0.8–1.0 Hz for sleep SO, 10 Hz alpha, 40 Hz gamma), deep depth.
- Works today (sine AM); becomes crisp bursts once the firmware **A2 trapezoid envelope**
  lands (cross-dep — author both a "smooth" now and a "pulsed" variant later).

**Success:** a "pink-noise sleep bed" preset plays and masks pleasantly; documented.

## Step 4 — Rotating pan (A8) + breath LFO (0.1 Hz) presets

- **Rotating pan:** document + preset `set_mod(ch, PAN, sine, -1, 1, 15000)` (8–30 s) —
  already works.
- **Breath LFO:** a `~50:100:10000` sine mod on **brightness** (LED) and/or **volume**
  (audio) = 6 breaths/min (0.1 Hz) coherence pacing, independent of the entrainment
  frequency. Add as an editor macro "breath layer" (period from breaths/min; default
  10 s). Asymmetric 4 s:6 s inhale:exhale would want a new LFO shape (firmware) — ship
  symmetric now, note the async variant as future.

**Success:** a "breath layer" macro adds a 0.1 Hz brightness swell; a rotating-pan
preset audibly circles; both from existing mod syntax.

## Step 5 — GENUS-faithful 40 Hz (B4) refresh

- Update `07_genus_40hz.ledc` to the exact GENUS numbers: 40 Hz light square **50% duty**
  ~390–400 lux-equivalent brightness; **40 Hz audio click 1 ms / 25 ms = 4% duty** (set the
  audio channel `mod` duty ≈4% — pairs with firmware A2, but a short-duty approximation is
  authorable now); combined, in-phase, steady 60 min, fade out. Add a **dim comfort variant**.
- Label experimental (amyloid claim contested; entrainment solid). Best delivered via the
  invisible-flicker pair once V-E2 lands (cross-dep).

**Success:** `07_genus_40hz.ledc` matches the published protocol; comfort variant added;
guideline note on evidence + safety.

## Step 6 — Split-field / per-eye presets (B5)

- Add example sessions + a `channel_map` recipe: left-eye pixels → ch0, right-eye → ch1
  (or 4 channels for L/R half-fields), then two `.ledc` LED lines with different `freq` +
  `mask` (e.g. lucid 16 Hz | 6 Hz; depression β-left/α-right).
- Different-frequency split-field works **today**. Phase-locked (same-freq, offset) split-
  field is a cross-dep on firmware **V-E2**.

**Success:** a "split-field lucid" session drives each eye at a different rate on the
user's glasses; `channel_map` recipe documented.

## Step 7 — Ganzfeld eyes-closed presets (B7) + colour/SSVEP (B8)

- **Ganzfeld:** refresh `15_ganzflicker_imagery` and add an eyes-closed template that
  **biases amber/red ≥620 nm** (eyelid passes ~14.5% at 700 nm vs ≤3% blue), long stable
  hold (15–25 min), ≥10 s ease-in.
- **Colour/SSVEP:** update `COLOR_PRESETS` with the strong-response pairs (amber/red 8.06 dB,
  blue/red 6.82; avoid green/lime for drive). Feed the invisible-flicker pair choice
  (amber↔blue = strongest SSVEP *and* luminance-complementary).

**Success:** a closed-eye Ganzfeld preset uses warm/amber; `COLOR_PRESETS` reflect SSVEP
strength; documented.

## Step 8 — The 5 starter arcs (refine existing, add missing)

Apply the playbook §C ramp/dwell/ending numbers to the library:
- **Sleep Descent** — refine `01_sleep_onset`: α10 hold 2 min → ramp 10→2 Hz (~0.44 Hz/min)
  → fade to dark, **no wake-up**.
- **Theta Dive** — refine `04_meditation_theta`: 10→6 Hz (0.5 Hz/min), hold 6 min w/ L6.0/R6.3,
  return to alert.
- **Alpha + Breath** — NEW: α10 hold + 0.1 Hz breath LFO → gentle up-ramp end.
- **Focus/SMR** — refine `05_focus_smr`: 14 Hz, **brightness ≤25% (photosensitivity), beat in
  AUDIO**; end bright/alert.
- **Gamma-40** — from Step 5.

Run `validate_session.mjs` on all; keep the 4-phase arc + goal-specific endings
(sleep = fade dark, focus = ramp up).

**Success:** the 5 arcs match research ramp rates/endings and pass the validator.

## Step 9 — Web UI: epilepsy opt-in + macro surfacing

- **Epilepsy warning + one-time opt-in** modal (reuse `util.chooseModal`) before first
  play/flash of any flicker session; store acceptance in localStorage; surface the
  `safety_mode` device setting (from the firmware plan) with an explanation. Ship
  contraindication text (epilepsy/photosensitivity, pregnancy, pacemaker caution,
  don't-drive) in the UI + each session header comment.
- Surface the new macros (harmonic stack, breath layer, monaural, split-field recipe)
  as wizard/table buttons.

**Success:** first flicker play shows the warning + opt-in; macros are one-click;
contraindications visible.

## Step 10 — Docs

- Rewrite `sessions/SESSION_DESIGN_GUIDELINES.md` around the playbook: band→state table,
  the 4-phase arc, ramp-slope guidance (0.25–2 Hz/min, warn >3), goal-specific endings,
  AV-sync ("ms budget, µs is margin"), breath layer, carriers, colour/SSVEP, and the
  safety rules. Cross-link the playbook report.
- Note the firmware cross-deps (invisible flicker, phase-locked split-field, trapezoid
  pulse, async breath) so authors know which presets get better after the firmware plan.

**Success:** guidelines reflect the research; a new author can build a safe, effective
session from them.

---

## Risks & notes

- **Cross-deps on the firmware plan:** invisible-flicker pairs (V-E2), phase-locked
  split-field (V-E2), crisp isochronic/AM-noise pulses (A2), and asymmetric breath (new
  LFO shape) are *better or only-possible* after firmware. Ship the "works-today" version
  now, and a "+firmware" variant later — don't block this plan on firmware.
- **Evidence framing:** keep everything experiential, not medical (band→state is largely
  convention; 40 Hz amyloid claim contested). Say so in the guidelines.
- **Don't regress the 25 existing sessions** — refine in place, re-validate with
  `validate_session.mjs`, and keep filenames stable (they're referenced by `push_library.sh`).
- Web changes need `npm test` green + `npm run build`; no firmware flash required for this
  plan (SPIFFS/web only), except that the epilepsy opt-in surfaces the firmware `safety_mode`.
- On completion write `reports/non_planned_reports/entrainment_authoring_report.md`.
