# BG WebSocket Raw-PCM Push — Implementation Report

**Plan:** `plans/bg_websocket_pcm_push_plan.md`
**Date:** 2026-07-18
**Status:** Steps 1–8 done, firmware + web build clean, 187 web tests green.
Step 9 (on-device hardware verify) pending — orchestrator/user.

## Why

The MP3-over-POST push (now reverted) produced audible **clicks/artifacts** on
hardware — suspected on-device minimp3 decode issues and/or the unproven 8 KB
async-worker stack running the decoder. Raw PCM over a WebSocket has **no
on-device decoder at all**, so it cannot produce decode artifacts, and WS adds a
bidirectional back-channel a single HTTP POST cannot. This is additive — the
WAV-POST ingest stays as the fallback.

## What changed

### Firmware
- **`sdkconfig` + `sdkconfig.defaults`:** `CONFIG_HTTPD_WS_SUPPORT=y`.
- **`web_server.c`:** new `GET /api/bg-ws` (`.is_websocket = true`). `bg_ws_handler`
  is a thin entry that `async_dispatch`es the session to the worker pool (the
  single server task stays free, mirroring `bg_stream_handler`). `bg_ws_work`:
  - first **TEXT** frame = handshake JSON `{"pan","loudness","rate","bits","ch"}`
    → clamps + `bg_player_start_push`;
  - every **BINARY** frame = raw int16 stereo LE PCM → the **existing**
    `bg_stream_feed_pcm` (0–3 byte frame carry preserved across WS frames) →
    `bg_player_push_pcm` → ring;
  - periodic `{"consumed","ring_ms"}` back-channel (`bg_ws_send_progress`);
  - `s_bg_stream_busy` single-ingest guard reused; teardown mirrors the POST path
    (`bg_player_end_push` on clean end, `bg_player_stop` on error); PSRAM
    recv/stage buffers freed on every exit; bounded recv-failure tolerance
    (`BG_STREAM_MAX_TIMEOUTS`).
- **`bg_player.c/.h`:** new `bg_player_push_bytes_streamed()` getter for the
  back-channel. Ring, mix, prime-sync gate, `bg_player_push_pcm` — all unchanged.

### Browser (`web/src/js/gen/`)
- **`transport.js`:** `pushBgWs(pcmBytes, {pan,loudness,onProgress})` — opens
  same-origin `ws(s)://…/api/bg-ws`, sends the handshake, pumps 16 KB PCM chunks
  paced by `ws.bufferedAmount`, closes on end-of-stream; `stopBgWs`; `stopBg` now
  also closes the WS. Pure helpers `wsHandshakeMsg` / `wsUrl` / `WS_CHUNK_BYTES`.
- **`play.js`:** the speech-bounce Play path strips the 44-byte WAV header from the
  bounce and pushes **raw PCM via `pushBgWs`** instead of the WAV `pushBg`.
- **`test/ws_transport.test.js`:** +4 tests for the pure helpers (handshake
  contract, ws/wss URL, chunk-size bound).

## Build & test

| Build | Result |
|---|---|
| Firmware `(PUSH=y, WS=y)` | Clean — `httpd_ws.c` linked |
| Web `npm run build` | OK — 57 KB gz app.js (no lamejs; WS client is tiny) |
| Web `npm test` | 187 / 187 pass |

(The `ota_1` overflow warning is the pre-existing asymmetric-OTA layout.)

## Screen-lock keep-alive (added)

A likely root cause of the reported clicks is **not** decode at all but **mobile
screen-lock starvation**: the phone isn't playing audio locally (the ESP32 is),
so the OS has no reason to keep the page alive on lock — it freezes the JS pump
and the device ring starves → underrun/recovery clicks. This affects *any*
browser-push transport (and WS, which needs active JS per frame, is more exposed
than a single Blob-POST the network stack can drain on its own). New
`web/src/js/gen/keepalive.js` plays a looping **silent `<audio>` element** during
the push to earn the OS background-audio exemption (works in an insecure context,
unlike Wake Lock). Wired into `play.js` around the push (start in the Play
gesture, stop in `finally`). **First test to run: replay with the screen kept ON**
— if the clicks vanish, screen-lock was the cause and this keep-alive is the fix,
independent of MP3-vs-WS.

## Honest scope / risks

- **✅ Resolved (cost one hardware round): frame-driven, not async.** The first cut
  offloaded the WS recv loop to the async worker (`httpd_req_async_handler_begin`)
  → **corrupted framing** ("WS frame is not properly masked" / "WS Message too
  long" from frame 1). That async facility detaches a whole HTTP request and is NOT
  for WebSockets. Rewrote to the **frame-driven** model (server task invokes the
  handler per frame on the real req; per-socket state in `req->sess_ctx`, teardown
  via `req->free_ctx`) — matches the official `ws_echo_server` example (whose
  "async" is `httpd_queue_work`/`httpd_ws_send_frame_async`, for *sending* only).
  **Hardware-verified working.** Tradeoff: `bg_stream_feed_pcm` runs on the server
  task and can block it ~<0.74 s when the ring is full (bounded by drain; never a
  session-length wedge; the prime gate can't deadlock — releases at ~176 KB « 1 MB
  ring).
- **Data volume unchanged.** Still ~1.4 Mbps / ~286 MB for a 27-min session in
  real time — WS does not shrink it. v1 has pacing + stall detection but **no
  resume-from-offset** (deferred v2); a long-enough WiFi stall still ends the push.
- Only 7 sockets, one held for the whole session; WS keepalive relies on the
  stack's auto-PONG.

## Step 9 — hardware verification (pending)

- Play a speech session → **no clicks/artifacts** (the core hypothesis). A/B mentally
  against the reverted MP3 behaviour.
- Prime-sync: timeline defers t=0 until ~500 ms buffered; speech aligned.
- SoftAP-only (`ws://192.168.4.1/api/bg-ws`) with no internet.
- Server stays responsive during the stream (`/api/stop`, `/api/state` answer) —
  confirms the async offload; watch `/api/logs` for `bg-ws:` lines + no wedge.
- Stop closes cleanly; a second push supersedes the first.
- Watch worker-task stack high-water mark (8 KB) — though no decoder runs here now.
