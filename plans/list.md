# Plans

Chronological list of execution plans. Newest last. Mark `[DONE]` when complete.

- `precision_meditation_device_execution_plan.md` — parent 6-week roadmap (Phase 1 ✅, Phase 2 ✅, Phase 3 ⏳, Phase 4 partial, Phase 5 pending)
- `generator_section_plan.md` — visual `.led` editor in the device web UI [DONE — all 5 phases]
- `runtime_settings_plan.md` — settings moved from sdkconfig to runtime NVS store [DONE — build clean, not yet flashed]
- `ota_dual_boot_plan.md` — field OTA via dedicated minimal updater [DONE — needs one-time wired full flash]
- `mp3_support_plan.md` — BG MP3 decode via vendored minimp3 [Steps 1–7 done, hardware verify pending]
- `bg_browser_push_plan.md` — browser-generated/loaded BG audio pushed to device via `/api/bg-stream` (additive; keeps existing pull path) + opt-in "bounce whole session to WAV" [IMPLEMENTED Phases 1–3.5, build+tests clean, hardware verify pending]
- `diagnostics_over_wifi_plan.md` — serial-parity over WiFi: reset reason + live log tail + crash core dumps + Diagnostics page (Layers 1-2 OTA-able, Layer 3 needs one wired flash) [PLANNED]
- `entrainment_firmware_plan.md` — 6 engine enhancements from the technique research: sine/gamma flicker carrier, trapezoid isochronic envelope, per-channel flicker phase offset (invisible cool/warm flicker), finer flicker tick, photosensitivity safety clamp, binaural beat jitter [PLANNED]
- `entrainment_authoring_plan.md` — no-firmware companion: carrier/colour/noise preset tables, harmonic-stack + breath-LFO + monaural macros, GENUS/Ganzfeld/split-field/starter-arc sessions, epilepsy opt-in UI, refreshed design guidelines [PLANNED]
