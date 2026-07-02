# BG Browser-Push Audio — Implementation Report

**Plan:** `plans/bg_browser_push_plan.md`
**Date:** 2026-07-02
**Status:** Phases 1–3.5 implemented. Firmware builds clean with
`CONFIG_BG_SUPPORT_PUSH` both `y` and `n`; web tests green (141); web bundle
builds. **NOT yet flashed / hardware-verified** — that is the orchestrator's/
user's step (subagents must not flash; see CLAUDE.md hardware policy).

## What was built

Additive feature: the browser can generate / load / bounce audio and **push** it
into the device over HTTP, instead of the device pulling a URL. The existing
`BG http(s)://` / `sdcard://` pull path is unchanged.

### Firmware (ESP32)

- **`main/Kconfig.projbuild`** — new `CONFIG_BG_SUPPORT_PUSH` (default `y`).
- **`main/bg_player.c` / `.h`** — new push producer that reuses the existing
  1 MB PSRAM ring, `bg_convert_to_stereo_float`, mixer, and I2S sink verbatim:
  - `bg_player_start_push(pan, loudness)` — stops any active BG first
    (single-producer invariant), resets ring, arms fade-in, marks active WITHOUT
    spawning `bg_streamer_task`, skips the WiFi-connected gate (works on SoftAP).
  - `bg_player_push_pcm(pcm, frames, channels, upsample_2x)` — converts + pushes
    to the ring, paced by the same high watermark as the pull path so TCP flow
    control throttles the upload to playback rate. Returns frames consumed
    (`< frames` ⇒ push ended).
  - `bg_player_end_push()` — drains the buffered ring, then clean fade-out.
  - `producer_kind` (PULL/PUSH) added to state; `bg_player_stop_impl` frees the
    push scratch and reverts to PULL (no task join needed — push has no task).
- **`main/web_server.c`** — `POST /api/bg-stream` handler: parses
  `?pan&loudness&stop`, streams the WAV body via an incremental `httpd_req_recv`
  loop, parses the header (reusing `wav_parser`), carries 0–3 leftover PCM bytes
  across reads (frame-straddle safety), feeds `bg_player_push_pcm` through an
  aligned stage buffer, and guards against concurrent streams (409).
- **`main/config_parser.c`** — `parse_bg_line` accepts `push://`; `bg_player_start`
  short-circuits `push://` as a no-op pull (never fetches, never spawns a task).

### Web UI (browser)

- **`web/src/js/gen/bgaudio.js`** — pure `encodeWav16` / `interleaveInt16` /
  `floatToInt16` (device-matching /32768 scaling), `conform` (resample→44100/
  stereo via OfflineAudioContext), `decodeFile` (any browser format), and
  `generateNoise` (white/pink/brown) / `generateDrone` generators.
- **`web/src/js/gen/bgstore.js`** — IndexedDB clip library (list/get/put/remove).
- **`web/src/js/gen/synth.js`** — offline session bounce: ports the device
  synthesis (Q32 phase accumulators, binaural L=freq/R=freqR, sine-AM iso at 0.1
  depth, equal-power pan, LFSR/Kellet/leaky noise, `>`/`*` sweeps to the next
  same-channel entry, periodic `^~/\_` mods) into a direct sample loop →
  one 44100/16/stereo WAV.
- **`web/src/js/gen/views/bg_panel.js`** — "Background audio" panel: generate,
  load-from-disk, library CRUD, Set-as-BG (`push://<name>` row), Push (with loop),
  Stop BG, and the opt-in **Bounce session → WAV** (download + save + push).
- **`web/src/js/gen/transport.js`** — `pushBg` / `stopBg`.
- Wired into `generator.js` (`initBgPanel`, push-after-play in `onPlay`),
  `index.html` (`#genBgPanel` section), `style.css` (panel styles).

## Verification done

- `idf.py build` clean with `CONFIG_BG_SUPPORT_PUSH=y` (app grew ~3.5 KB, the
  push code genuinely compiled in) and with `=n` (all `#if` guards hold, code
  compiled out cleanly).
- `npm test` → 141/141 pass (added `bgaudio.test.js` + `synth.test.js`: WAV
  header/PCM round-trip, interleave, float→int16 clamps, waveform shapes,
  evalField linear/quad/mod, extract+render length/range/clamp).
- `npm run build` → bundle builds; `app.js` grew ~129 KB → ~145 KB with the new
  modules.
- Pre-existing `ota_1` partition-overflow warning is unrelated (asymmetric OTA
  layout, noted in the OTA + MP3 plans).

## Speech (`S`) rows + TTS (added 2026-07-02)

New `S <time> <voice> <volume> "text"` entry, browser-only:
- **Model/parse/serialize**: `speechRow` (`model.js`), `S` line parse with quoted
  text that may contain `#`/spaces (`parse.js`), `serializeSpeech` +
  `serializeForDevice` (strips `S`) (`serialize.js`). `transport.playDoc` sends
  `serializeForDevice` so the firmware never sees `S`; config storage keeps it.
- **TTS engines** (`tts.js`, both free/no-key, panel-selectable):
  **Puter.js** default (`puter.ai.txt2speech`, AWS Polly neural, lazy CDN load,
  browser-only) and **Google via device** (new firmware `GET /api/tts?tl=&q=`
  proxy in `web_server.c` → esp_http_client + esp_crt_bundle → Google Translate
  TTS MP3; browser splits ≤180 chars). `previewSpeech` uses `speechSynthesis`
  for local audition. edge-tts was rejected (WSS + rotating token + NTP too heavy
  for the ESP32).
- **Bounce scope choice**: *BG + Speech* (leave A to device) vs *All* (BG + Speech
  + A). Speech synthesized then mixed at each line's time; BG push:// clip mixed
  (looped, panned) via `synth.js` `mixInto`/`zeroBuffers`/`clampBuffers`.
- **Table view**: `S` row type (`+ S`, voice/volume/text + 🔊 preview).
- Build: firmware app grew to 0x132f10 (~+90 KB vs the push-only build) — the
  TTS proxy links the mbedTLS CA bundle. Web tests 147 green; bundle builds.

## Design notes / caveats

- **Single-producer** ring is protected: push stops any pull first and never
  spawns the streamer task; a second concurrent `/api/bg-stream` gets 409.
- **HTTP/1.1**: uses finite-Content-Length `Blob` POSTs (not `fetch` streaming
  bodies, which need HTTP/2). Looping = client re-POST (tiny seam gap).
- **Bandwidth / tab**: raw PCM ≈ 176 KB/s; the tab must stay connected for the
  session. Resident-PSRAM / gapless-loop mode was explicitly ruled out of scope.
- **Bounce fidelity** is perceptual, not bit-exact (sine via `Math.sin` vs the
  device's 4096-entry LUT; 5 ms de-click ramps approximated). The long
  synchronous render can briefly block the tab for long sessions.

## Remaining (hardware — orchestrator/user)

Flash and verify on-device: generate/load → Set as BG → Play → audible under the
generator with correct pan/loudness; SoftAP-only push at `http://192.168.4.1`;
`BG http(s)://` pull regression unchanged; single-producer (push while pull
active) is glitch-free; sustained >2 min no underruns; Stop silences; bounce a
known session and confirm it sounds equivalent to live device synthesis.
