# Reports

Implementation reports. Reports not tied to a numbered plan live under
`non_planned_reports/`.

- `non_planned_reports/websocket_bg_push_reliability_investigation.md` — 6-agent
  investigation (2026-07-24) of the raw-PCM-over-WS BG push starving the device ring
  (46–69 ms). Root causes: 32 KB TCP window + httpd prio 5 (structural floor),
  background-tab timer throttling (silent keepalive fails), DELBA/WiFi ceiling.
  Tier-1 fixes (TCP window, httpd priority, deeper prebuffer, MessageChannel pump,
  ring_ms flow control) vs Tier-2 (compress MP3/Opus over WS). NOT yet implemented.
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
- `bg_websocket_pcm_push_report.md` — ESP32-as-WebSocket-server raw-PCM BG ingest
  (`GET /api/bg-ws`), the artifact-free alternative to the reverted MP3-POST push.
  No on-device decode; reuses the existing bg_player_push_pcm ring + prime gate;
  adds a consumed/ring_ms back-channel. Build clean, 187 web tests; hardware verify
  pending. Key unverified risk: httpd_ws_recv_frame in a blocking loop on the async worker.
- `non_planned_reports/av_sync_offset_measured_2026-10-12.md` — optical measurement of
  audio-vs-LED sync (MiniFuse 4, solar cells + TRS splitter; `tools/avmeasure/avsync.py`).
  Found a RANDOM per-session offset uniform over the 23.22 ms audio generation chunk
  (14 runs, +1.2..+21.1 ms); proved the mechanism by halving the chunk (range 21.9→11.4 ms);
  fixed it with a start-latency phase catch-up (range → 1.30 ms, sd 0.38). STILL OPEN:
  entries on an already-active channel bypass both the catch-up and the DMA-lag
  pre-advance and run ~48 ms early (= −AUDIO_DMA_PIPELINE_LAG_US) — the dominant
  real-world case. Also: the +44 dB LED→audio crosstalk was a USB ground loop, not a bug.
- `non_planned_reports/led_audio_bug_sweep_2026-10-12.md` — bug sweep after the A/V sync
  work. Fixed an out-of-bounds write from an unchecked audio channel number (the three
  `_locked` generator entry points; reachable from any uploaded .ledc) and the `uint8_t`
  bitmask truncation that made audio channels 8..15 silently ignore EVERY `>`/`*` ramp —
  that one affected 14 shipped Gateway sessions. Also: 11-minute capture shows no clock
  drift (<0.2 ppm) and no per-cycle glitches; and a percycle.py false-positive mode on
  long windows that nearly produced a bogus "-36.8 ms glitch" report.
