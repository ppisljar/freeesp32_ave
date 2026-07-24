# WebSocket BG-Push Reliability — Investigation & Fix Plan

**Date:** 2026-07-24
**Status:** Investigated (6 subagents), NOT yet implemented. LED-fade fix (separate issue) is being done first per user.
**Device:** 10.0.0.162 (non-glasses: direct LED, es8388), observed on `33_ego_dissolution_gamma` (30 min speech session → full raw-PCM bounce pushed over WS).

## Symptom
Browser-captured session log (new client-log feature) shows the device BG ring buffer chronically low: `ws: device ring low: 46–69 ms (underrun risk)` for essentially the whole 30-minute session (warn threshold <200 ms; ring capacity ~2.97 s). Audio is unreliable/glitchy. Two regimes in the log:
- **+155 s .. +676 s:** near-continuous warnings ~every 1.3 s (foregrounded tab).
- **after ~+676 s:** ~49 s gap, then **tight clusters of ~4 warnings once per ~60 s** (backgrounded tab).

Device log also shows batch execution ballooning from <1 ms early to **~17 ms** late, and recurring `wifi:[ADDBA]RX DELBA, reason:39` (block-ACK teardown) roughly every 60 s.

## System recap
Browser bounces the whole session (speech+BG merged) to one raw-PCM WAV (~283 MB / 297,172,224 B for 30 min), streams it over a WebSocket in 16 KB frames to the device, which buffers into a **1 MB PSRAM ring (~2.97 s)** and drains at 44100×2×2 = **176,400 B/s** (1.41 Mbit/s), mixing it as background audio. Device reports `ring_ms` back over the WS (advisory only today).

## Root causes (converged across 3 WS subagents)

### The 46/69 ms oscillation is exact — the ring runs one frame deep
Producer writes 2048-frame batches (`BG_PUSH_BATCH_FRAMES`, 16384 B = 46.4 ms); consumer drains 1024-frame chunks (`AUDIO_GEN_BUFFER_SIZE`, 8192 B = 23.2 ms). Ring ping-pongs between "2 batches − 1 drain" = 69.7 ms and "1 batch" = 46.4 ms. This is not jitter — it's the ring sitting essentially empty.

### Regime 1 (steady low, foregrounded) — STRUCTURAL
1. **32 KB device TCP receive window** (`CONFIG_LWIP_TCP_WND_DEFAULT=32768`, sdkconfig) caps browser lookahead to ~186 ms *regardless of WiFi speed*. `WS_HIGH_WATER=512 KB` in the browser pump is irrelevant — the OS TCP layer stalls the browser at the device's 32 KB window.
2. **WS handler runs inline on the httpd server task at priority 5** (`web_server.c` `bg_ws_handler` — it does NOT use the async worker; the "offloads to async" comment ~line 368 is misleading), starved on core 0 by LWIP (prio 22) and WiFi (prio 23). So even received frames drain slowly. Net production ≈ real-time consumption, zero accumulation.

### Regime 2 (once-per-60 s clusters, screen locked) — BROWSER THROTTLING
- The pump's only re-arm is `setTimeout(pump, 30)` (`transport.js`). Chrome/iOS **intensive background-tab throttling** slows this to ~60 s.
- The **silent keepalive WAV** (`keepalive.js`, all-zero samples) is treated as "not audible," so it fails to keep the tab foregrounded-equivalent. Each throttled 60 s wake bursts 512 KB → device plays ~2.97 s (4 back-channel frames = the 4-warning cluster), then 57 s of starvation.
- No `visibilitychange` listener; `ring_ms` back-channel is read but its handler body is empty (`/* hook for UI */`) — no adaptive flow control.

### Systemic ceiling
- Even with a perfect producer/consumer, WiFi has near-zero headroom at 1.41 Mbit/s, and **DELBA reason:39** (AP tearing down block-ACK aggregation) every ~60 s drops throughput for 50–200 ms → with only 46 ms buffered, instant underrun.
- **CPU is at 160 MHz, not 240** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`) — 33% less headroom.
- The 17 ms late-session batch times = `timeline_exec` (prio 4, core 0) starved by `bg_streamer`/httpd work on core 0. Affects timeline dispatch jitter, not the LED ramp (which runs in a hardware ISR).
- Compounding: the browser polls `/api/state` every 1 s (`livecontrol.js`) and `/api/logs` every 3 s (`logcapture.js`) DURING the push, holding sockets (`max_open_sockets=7`, FIN_WAIT 20 s) — socket-slot contention risk.

## Fix plan

### Tier 1 — cheap, keeps raw PCM (recommended first; ~90% of the win)
1. **TCP window** `CONFIG_LWIP_TCP_WND_DEFAULT` 32768 → 131072 (also raise `TCP_SND_BUF_DEFAULT`, check `TCP_RECVMBOX_SIZE`). ⚠️ DRAM cost ≈ window × max_open_sockets(7); may need 64 KB instead of 128 KB, or enable `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`. Test heap on boot. Raises ring floor from ~46 ms to ~400–600 ms.
2. **httpd priority**: add `config.task_priority = 18;` in `web_server_init` (`web_server.c`) so the receiver isn't starved by LWIP/WiFi.
3. **Deeper prebuffer**: `BG_PRIME_THRESHOLD_MS` 500 → 2000 (`config_parser.c`) — enter playback with ~2 s buffered (only helps the `push://` timeline-sync path).
4. **Browser throttle-immune pump**: replace `setTimeout(pump, 30)` with a `MessageChannel.postMessage` re-arm (not subject to 60 s background throttling), keeping a 10 ms floor sleep at the high-water boundary; or move the pump to a Web Worker. Eliminates Regime 2.
5. **Adaptive flow control**: in `transport.js` `ws.onmessage`, when `ring_ms < 200` kick the pump immediately (safety net; back-channel arrives ~every 743 ms — raise `BG_WS_BACKCHAN_EVERY` cadence if needed).
6. Optional: CPU to 240 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`); pause `/api/logs`/`/api/state` pollers during an active WS push.
7. Optional WiFi: static IP, 802.11n-only on AP, investigate the ~60 s AP-side BA idle-timeout causing DELBA.

### Tier 2 — durable (bigger rewrite)
- **Compress the BG stream (MP3/Opus over WS)** → ~11× less bandwidth (128 kbps ≈ 16 KB/s). The 1 MB ring then holds ~60 s → rides out any DELBA dropout with huge margin. minimp3 is already integrated with PSRAM scratch. NOTE: a prior MP3-over-POST push was reverted for "audible clicks" blamed on minimp3 decode — but that was likely the same **screen-lock starvation** (Regime 2), now separately fixable; re-test with screen ON before blaming the decoder.
- Alternative durable path: device-PULL of a compressed file (the project's proven reliable path) — but the browser can't be an HTTP server, so this needs the bounce served from somewhere the device can pull.

### Recommended sequence
Tier-1 #1 + #2 + #4 together are the minimum effective fix (structural floor + throttle immunity). Add #3/#5 for robustness. Escalate to Tier 2 only if DELBA dropouts still exceed the deepened ring cushion.

## Key file:line references
- `web/src/js/gen/transport.js` — `pushBgWs`, `WS_CHUNK_BYTES=16384`, `WS_HIGH_WATER=512K`, `setTimeout(pump,30)`, empty `ring_ms` handler.
- `web/src/js/gen/keepalive.js` — silent (zero-sample) WAV keepalive.
- `web/src/js/gen/play.js` — `startKeepAlive`/`pushBgWs`/`stopKeepAlive`.
- `main/web_server.c` — `bg_ws_handler` (frame-driven on httpd task), `BG_WS_RECV_BYTES=16384`, `BG_WS_BACKCHAN_EVERY=8`, `bg_ws_send_progress` (ring_ms snapshot), no `config.task_priority` override.
- `main/bg_player.c` — `BG_RING_BYTES=1048576`, `BG_PUSH_BATCH_FRAMES=2048`, `bg_player_push_buffered_ms`, watermark `WATERMARK_FREE_BYTES=262144` (never fires when ring is empty).
- `main/audio_generator.h` — `AUDIO_GEN_BUFFER_SIZE=1024`.
- `sdkconfig` — `CONFIG_LWIP_TCP_WND_DEFAULT=32768`, `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`, `CONFIG_LWIP_TCPIP_TASK_PRIO=22`, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`.
- Task map: `led_flicker` prio 24/core1, `audio_output` 23/core1, `timing_dispatch` 22/core1, `bg_streamer` 18/core0, httpd+async 5, `timeline_exec` 4/core0.
