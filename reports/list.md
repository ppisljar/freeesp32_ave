# Reports

Implementation reports. Reports not tied to a numbered plan live under
`non_planned_reports/`.

- `non_planned_reports/bg_browser_push_report.md` — browser-push BG audio +
  session-bounce-to-WAV (plan `bg_browser_push_plan.md`); Phases 1–3.5 built,
  build + web tests clean, hardware verification pending.
- `non_planned_reports/patent_landscape_ave.md` — AVE/mind-machine patent & company
  FTO-awareness landscape (3-agent web research, 2026-07-02): core techniques are
  expired/public-domain; only real active risk is MIT US 10,682,490 (35–45 Hz gamma).
- `non_planned_reports/ledc_format_v2_report.md` — `.ledc` format v2 (shared pulse fields
  env/phase/attack/jitter + audio duty + `-` sentinel). Spec locked (`ledc_format.md`); web
  parse/serialize/model done + 163 tests incl. 25-session fixpoint; device parser tolerant;
  per-channel wiring + missing caps are follow-up (task 25).
- `non_planned_reports/entrainment_firmware_report.md` — 5/6 engine enhancements built
  (B2 sine flicker, A2 trapezoid isochronic, V-E1 finer tick, V-E2 phase offset =
  cool/warm invisible flicker, A3 beat jitter); build clean, hardware verify pending;
  B3 safety clamp skipped; per-entry `.ledc` authoring deferred to a consolidated pass.
- `non_planned_reports/entrainment_authoring_report.md` — authoring effort for
  `entrainment_authoring_plan.md` (2026-07-03): 6 new + 5 refined sessions (31 total,
  0-error), new `macros.js` presets/macros (monaural, harmonic stack, breath/rotating-pan
  LFO, flicker pairs, research carriers + SSVEP colours), browser-only epilepsy opt-in
  (`safety.js`), and a rewritten `SESSION_DESIGN_GUIDELINES.md`. 183 web tests green.
  Firmware v2 engine build-clean/pending-flash; `safety_mode` clamp cut.
- `non_planned_reports/entrainment_techniques_playbook.md` — learning-oriented digest
  (3-agent research, 2026-07-02) of audio/visual/session techniques mined from patents +
  literature, each mapped to a concrete engine change. Flagships: monaural beats,
  trapezoid isochronic envelope, invisible spectral flicker, anti-habituation dither.
- `non_planned_reports/audio_mix_sum_aware_normalization.md` — replaced the count-based
  `1/N_active` mix scaling with sum-aware `gain=min(1,1/Σamp)`. Fixes user-reported
  loudness duck in `09_lucid_hypnagogic` at t=8:00 (enabling ch2/ch3 dropped ch1 ~9.5 dB)
  while keeping the 8 multi-carrier Gateway sessions (Σ up to 5.12×) clip-safe. Build
  clean; hardware verify pending.
