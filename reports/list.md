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
- `non_planned_reports/entrainment_techniques_playbook.md` — learning-oriented digest
  (3-agent research, 2026-07-02) of audio/visual/session techniques mined from patents +
  literature, each mapped to a concrete engine change. Flagships: monaural beats,
  trapezoid isochronic envelope, invisible spectral flicker, anti-habituation dither.
