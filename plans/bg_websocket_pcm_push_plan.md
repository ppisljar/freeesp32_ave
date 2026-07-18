# BG WebSocket Raw-PCM Push — Plan & Contract

> **STATUS (2026-07-18): IMPLEMENTED + HARDWARE-VERIFIED WORKING (Steps 1–9).**
> Firmware: `CONFIG_HTTPD_WS_SUPPORT=y` (sdkconfig + sdkconfig.defaults); new
> `GET /api/bg-ws` **frame-driven** handler in `web_server.c` — the server task
> invokes `bg_ws_handler` once per WS frame on the REAL req (per-socket state in
> `req->sess_ctx` = `bg_ws_ctx_t`, torn down by `req->free_ctx` on socket close).
> A handshake JSON text frame arms `bg_player_start_push`; binary frames → the
> existing `bg_stream_feed_pcm`→`bg_player_push_pcm` ring path; periodic
> `{consumed,ring_ms}` back-channel via synchronous `httpd_ws_send_frame`;
> `s_bg_stream_busy` guard reused; new `bg_player_push_bytes_streamed()` getter.
> Browser: `transport.js` `pushBgWs` (handshake + `ws.bufferedAmount`-paced 16 KB
> chunks) + `stopBgWs`; `stopBg` closes the WS too; `play.js` speech path strips
> the 44-byte WAV header and pushes raw PCM via `pushBgWs`. WAV-POST kept as the
> coexisting fallback. 187 web tests green.
>
> **⚠️ CRITICAL LESSON (cost a hardware round):** the FIRST cut offloaded the WS
> recv loop to the async worker via `httpd_req_async_handler_begin` — this
> **corrupts WS framing** ("WS frame is not properly masked" / "WS Message too
> long" from frame 1), because that async facility detaches a whole HTTP request
> and is NOT for WebSockets. Per the official `ws_echo_server` example, WS **recv
> must be frame-driven on the real req**; the example's "async" is `httpd_queue_work`
> + `httpd_ws_send_frame_async` for **sending** only. Tradeoff of frame-driven:
> `bg_stream_feed_pcm` runs on the server task and can block it briefly (~<0.74 s,
> bounded by ring drain at the 256 KB-free watermark) when the ring is full — never
> a session-length wedge (server is free between frames); the prime gate can't
> deadlock (releases at ~176 KB « the 1 MB ring). Resume-on-drop still v2/deferred.
>
> **ORIGINAL PLAN BELOW.** Alternative to the
> MP3-over-POST browser-push path (`bg_mp3_push_plan.md`), motivated by audible
> clicks/artifacts in the MP3 push (suspected minimp3 decode issues + the 8 KB
> async-worker stack running the decoder). This path makes the ESP32 a
> **WebSocket server** and streams **RAW PCM (no codec)** from the browser over a
> binary WS. RAW PCM has NO on-device decode step, so it *cannot* produce decode
> artifacts, and WS gives message framing + a bidirectional back-channel that a
> single fixed-length HTTP POST cannot. This is purely additive: the MP3-POST and
> the original WAV-POST ingest paths are KEPT. Resume-on-drop (WS's one real
> reliability edge over a POST) is scoped as a v2 follow-up; v1 ships pacing +
> stall detection.

## Goal

Today `POST /api/bg-stream` (`web_server.c` `bg_stream_work`) accepts a WAV
or MP3 body, sniffs magic bytes, and feeds the 1 MB PSRAM ring the mix drains.
The WAV branch feeds raw PCM into `bg_player_push_pcm` via `bg_stream_feed_pcm`;
the MP3 branch decodes through minimp3 first. The MP3 branch is where the clicks
appear.

We want a new **WebSocket ingest** — `GET /api/bg-ws` (Upgrade → WebSocket) —
that carries **raw 44.1 kHz / 16-bit / stereo little-endian PCM** in binary WS
frames and feeds them into the **same** `bg_player_push_pcm` path the WAV-POST
branch already uses. Everything downstream of the ring — the prime-sync gate, the
additive mix, the fades, the I2S sink — is byte-for-byte unchanged. No decoder is
involved, so no decode artifacts are possible on this path.

This is explicitly NOT a switch from push to pull, and NOT a change to the ring
or mix. It is a new *transport* for the *already-existing, decoder-free* raw-PCM
producer, plus a back-channel that a POST structurally cannot offer.

## Design decisions

- **Transport = WebSocket binary frames; enable `CONFIG_HTTPD_WS_SUPPORT`.**
  `sdkconfig` currently has `# CONFIG_HTTPD_WS_SUPPORT is not set`. WS must be
  turned on (add `CONFIG_HTTPD_WS_SUPPORT=y` to `sdkconfig.defaults` so it
  survives regenerations, and to `sdkconfig`). A WS URI handler is registered like
  any other, but with `.is_websocket = true`; esp_http_server performs the
  RFC6455 upgrade handshake internally, then delivers each WS frame to the handler
  via `httpd_ws_recv_frame`.

- **The WS handler MUST run on the async worker pool — non-negotiable.**
  esp_http_server is a single select-based task. A WS session that lives for a
  27-min push would park that one task and wedge EVERY other request —
  `/api/stop`, `/api/state`, live control — for the whole session. This project
  already hit exactly this failure mode (the unbounded recv-timeout wedge that
  `BG_STREAM_MAX_TIMEOUTS` was added to bound). So the WS receive loop offloads to
  the existing `async_dispatch` / `async_worker_task` pool, identical to how
  `bg_stream_handler` already dispatches `bg_stream_work`. The async handle
  (`httpd_req_async_handler_begin`) works for WS handlers too. The same
  bounded-timeout escape (`recv_timeouts > MAX`) protects the loop.

- **Reuse the raw-PCM producer verbatim — NO new decode code.** The WS steady-state
  simply calls the existing `bg_stream_feed_pcm(buf, len, stage, carry,
  &carry_len)`, which maintains the 0–3 byte stereo-frame carry and calls
  `bg_player_push_pcm`. Same ring, same `bg_convert_to_stereo_float`, same
  watermark/backpressure, same prime-sync gate (`bg_player_push_buffered_ms` →
  timeline defers t=0 until ≥500 ms buffered). The MP3 `bg_player_push_mp3*` path
  is NOT touched and NOT used here.

- **Framing protocol: one JSON handshake text frame, then binary PCM frames.**
  The first WS message is a small **text** frame `{"pan":<-100..100>,
  "loudness":<0..100>,"rate":44100,"bits":16,"ch":2}` — this replaces the current
  `?pan=&loudness=` query string. On receipt the handler calls
  `bg_player_start_push(pan, loudness)`. Every subsequent **binary** frame is raw
  interleaved 16-bit stereo LE PCM, fed straight to `bg_stream_feed_pcm`. Frames
  need NOT align to 4-byte boundaries: a binary message may end mid-stereo-frame;
  the existing carry buffer (`carry[4]`, `carry_len`) absorbs the 0–3 leftover
  bytes across messages — the *exact same mechanism* the POST path relies on. No
  RIFF header is sent (format is fixed by the handshake), so no header-straddle
  handling is needed either.

- **Data-volume reality — WS does NOT reduce bytes.** Raw 44.1 kHz/16/stereo =
  176 400 B/s ≈ 1.41 Mbps → ~286 MB for a 27-min session, streamed in real time.
  This is *identical* to the original WAV-POST volume and ~11× the MP3-POST. WS
  buys reliability NOT by shrinking data but via the **back-channel**: the device
  periodically sends a text frame `{"consumed":<frames>,"ring_ms":<buffered_ms>}`
  (derived from `s_bg.bytes_streamed` / `bg_player_push_buffered_ms`). The browser
  uses this to (a) **pace** — hold sends when `ws.bufferedAmount` is high or
  `ring_ms` is near-full; (b) **detect stalls** — no progress for N seconds → warn
  / reconnect; (c) **RESUME** — after a socket drop, reconnect and resume sending
  from the last consumed offset (v2). Resume is the ONLY thing that makes a 27-min
  real-time push genuinely robust against WiFi hiccups; raw PCM alone does not.

- **Resume is DEFERRED to v2.** v1 ships the WS transport + handshake + PCM
  framing + back-channel *reporting* + browser pacing + stall detection — enough
  to validate that the artifact problem is gone and that WS pacing works. Full
  gapless resume-from-offset (reconnect, re-arm push at a byte offset, skip already
  consumed audio) is a distinct hardening pass, called out honestly so no one
  assumes v1 is drop-proof. v1's drop behaviour is: back-channel stall detected →
  browser tears down and restarts the clip from the top (same UX as today's
  client-driven loop), OR surfaces an error — no silent audio loss.

- **Single-producer + supersede reuse the existing guards.** `s_bg_stream_busy`
  still enforces one ingest at a time across POST and WS — a WS open while a POST
  push is active (or vice-versa) gets a clean 409 / close. `bg_player_start_push`
  already stops any active pull OR push first (guaranteeing single-producer on the
  FreeRTOS StreamBuffer). A WS **close** (client sends CLOSE, or socket drops →
  `httpd_ws_recv_frame` returns error/0) is the natural-completion signal →
  `bg_player_end_push` (drain + fade). A **new** WS connection superseding an old
  one relies on `bg_player_start_push` tearing down the prior session; the old
  handler's `bg_player_push_pcm` then returns short (`s_bg.streaming` went false)
  and the old WS loop exits and closes.

- **SoftAP-only friendly.** WS is same-origin `ws://<device>/api/bg-ws`, no
  internet, no CDN, no TLS. Works at `ws://192.168.4.1/api/bg-ws` in SoftAP mode
  exactly like the current push. (No `wss://` — the device has no cert; the browser
  allows insecure `ws://` from an insecure `http://` origin, which is what the
  device serves.)

- **Coexist behind the existing `CONFIG_BG_SUPPORT_PUSH` flag, gated by a new
  `CONFIG_HTTPD_WS_SUPPORT=y`; do NOT remove the MP3-POST or WAV-POST paths.** WS
  is a third, additive ingest at `/api/bg-ws`. This lets the WS path be A/B-tested
  against the click-prone MP3 path on real hardware before any removal decision.
  The browser picks the path via a feature flag / capability probe (try WS, fall
  back to MP3-POST).

Active project dir: `freeesp32_ave` (build: `source ./activate.sh && idf.py
build`). Platform: dual-core ESP32 @ 240 MHz, **PSRAM enabled**. Web UI:
vanilla-JS esbuild bundle under `web/` (`npm run build`, tests `npm test`).
Subagents may build (`idf.py build` / `npm run build` + `npm test`) but **MUST
NOT flash** — flashing/hardware verification is the orchestrator's/user's job.

---

## Key integration points (verified against current source)

| Concern | Location |
|---|---|
| WS support currently OFF (must enable) | `sdkconfig` `# CONFIG_HTTPD_WS_SUPPORT is not set` |
| LWIP socket ceiling (max_open_sockets = 10−3 = 7) | `sdkconfig` `CONFIG_LWIP_MAX_SOCKETS=10`; `web_server.c` ~252–256 |
| Async worker pool (WS handler MUST use it) | `async_dispatch` `web_server.c:149`; `async_worker_task` ~134; `ASYNC_WORKERS 1`, `ASYNC_WORKER_STACK 8192` ~126–128 |
| Single-threaded-server wedge precedent + bounded escape | `web_server.c` recv-timeout wedge note; `BG_STREAM_MAX_TIMEOUTS` :1542 |
| Current POST handler to mirror (thin dispatch + real body) | `bg_stream_handler` `web_server.c:1576`; `bg_stream_work` :1581 |
| Raw-PCM frame carry (reuse verbatim for WS) | `bg_stream_feed_pcm` `web_server.c:1550` (0–3 byte carry :1567) |
| Single-ingest guard (extend to cover WS) | `s_bg_stream_busy` `web_server.c:1543,1609` |
| URI handler registration table + counts | `web_server.c` ~280–518; `max_uri_handlers 40`; `max_open_sockets 7` |
| Raw-PCM producer arm (reuse; stops any prior producer) | `bg_player_start_push` `bg_player.c:1526` (approx after edits) |
| Raw-PCM feed + watermark backpressure (reuse) | `bg_player_push_pcm` `bg_player.c` (watermark loop) |
| Natural-completion drain + fade (reuse) | `bg_player_end_push` `bg_player.c` |
| Shared decode tail (reuse) | `bg_convert_to_stereo_float` `bg_player.c:370` |
| Prime-sync gate (reuse; counts decoded PCM in ring) | `bg_player_push_hold/release`; `bg_player_push_buffered_ms` |
| Producer teardown / single-producer branch | `bg_player_stop_impl`; producer_kind reset |
| Ring size / cushion | `BG_RING_BYTES 1048576` `bg_player.c:145` (~3 s PCM at 1.4 Mbps) |
| Bytes-streamed counter (back-channel source) | `s_bg.bytes_streamed` `bg_player.c` |
| Browser push transport (add WS client) | `web/src/js/gen/transport.js` `pushBg` :86, `stopBg` :113 |
| Browser bounce (produces MP3 today; add raw-PCM for WS) | `web/src/js/gen/bounce.js` `encodeMp3` + return |
| Browser play wiring (pushBg after playDoc) | `web/src/js/gen/play.js` ~100–101 |
| Raw-PCM/WAV encoder already present (reuse int16 interleave) | `web/src/js/gen/bgaudio.js` `encodeWav16`, `interleaveInt16` |
| Feature flag | `main/Kconfig.projbuild` `CONFIG_BG_SUPPORT_PUSH` |

**Latent detail to preserve:** the raw-PCM path drops nothing per frame ONLY
because `bg_stream_feed_pcm` carries the 0–3 trailing bytes. A WS binary message
can end mid-4-byte-frame → the WS loop MUST keep the *same* `carry`/`carry_len`
state across `httpd_ws_recv_frame` calls, exactly as the POST loop keeps it across
`httpd_req_recv` calls. Do not reset carry per WS message.

---

## Steps

### Step 1 — Enable WS + register the endpoint
- Add `CONFIG_HTTPD_WS_SUPPORT=y` to `sdkconfig.defaults` AND `sdkconfig`. Confirm
  `idf.py build` picks it up (esp_http_server WS symbols compile in).
- Register a new URI handler in `web_server.c`, guarded by
  `#if CONFIG_BG_SUPPORT_PUSH && CONFIG_HTTPD_WS_SUPPORT`:
  ```c
  httpd_uri_t bg_ws_uri = {
      .uri = "/api/bg-ws", .method = HTTP_GET,
      .handler = bg_ws_handler, .user_ctx = NULL,
      .is_websocket = true, .handle_ws_control_frames = false,
  };
  ```
  `max_uri_handlers` is 40 with ~28 in use — headroom OK; no bump needed.
- **Success:** builds clean with WS on and (guarded out) with `CONFIG_BG_SUPPORT_PUSH=n`.

### Step 2 — WS handler skeleton on the async pool
- `bg_ws_handler`: on the FIRST call for a connection the upgrade has already
  completed inside esp_http_server; do NOT block here. Offload the receive loop to
  the worker: mirror `bg_stream_handler` → `return async_dispatch(req, bg_ws_work);`.
- **Rationale:** keeps the single server task free (see the wedge note). The async
  handle from `httpd_req_async_handler_begin` is what the worker uses for
  `httpd_ws_recv_frame`/`httpd_ws_send_frame`.
- Guard overlap with `s_bg_stream_busy`: CAS 0→1; on failure close the WS with a
  close frame + reason "bg-stream busy" (mirror the 409).

### Step 3 — `bg_ws_work`: handshake frame → arm push
- Allocate the SAME PSRAM scratch as the POST path: `stage`
  (`BG_STREAM_RECV_BYTES+4`) + a recv buffer, `MALLOC_CAP_SPIRAM`. Reuse `carry[4]`,
  `carry_len`.
- Recv the first WS frame (`httpd_ws_recv_frame` with len=0 to get size, then the
  payload). Expect `HTTPD_WS_TYPE_TEXT` JSON handshake. Parse `pan`/`loudness`
  (clamp), validate `rate=44100,bits=16,ch=2`; reject others with a close + reason.
- `bg_player_start_push(pan, loudness)` — arms the decoder-free producer, stops any
  prior producer, resets the ring.
- **Success:** a WS client that sends only the handshake arms push mode (verify via
  logs); no PCM yet → prime gate holds the timeline.

### Step 4 — `bg_ws_work`: PCM receive loop (reuse the carry feeder)
- Loop:
  ```
  recv_timeouts = 0
  loop:
    ws_frame = httpd_ws_recv_frame(...)         // binary PCM (or CLOSE / PING)
    if timeout:  if ++recv_timeouts > BG_STREAM_MAX_TIMEOUTS: break; else continue
    recv_timeouts = 0
    if CLOSE or recv error / 0-len peer close:  natural-end → break
    if BINARY:
        if !bg_stream_feed_pcm(payload, len, stage, carry, &carry_len): ended=break
    (PING handled by stack if handle_ws_control_frames=false)
  ```
  This is the POST loop with `httpd_req_recv` swapped for `httpd_ws_recv_frame` and
  the codec-sniff/WAV-header branches DELETED (format is fixed by the handshake).
  `bg_stream_feed_pcm` and its carry are reused unchanged — that is the whole point.
- Large WS messages: cap per-frame payload to the recv buffer; if a client sends a
  message larger than the buffer, receive it in fragments (esp_http_server exposes
  `final`/fragment flags) and feed each fragment through `bg_stream_feed_pcm` (carry
  handles the split). Recommend the browser cap frames at ~16–32 KB anyway.
- **Success:** a WS client streaming raw PCM plays clean BG audio (hardware, Step 9),
  with zero decode step → no clicks attributable to decode.

### Step 5 — Back-channel: device → browser progress frames
- In `bg_ws_work`, roughly once per second (or every M recv iterations), send a
  TEXT frame `{"consumed":<bytes_streamed>,"ring_ms":<buffered_ms>}` using
  `s_bg.bytes_streamed` (expose via a getter) and `bg_player_push_buffered_ms()`.
  `httpd_ws_send_frame_async` from the worker (has the async handle).
- Keep it cheap: a fixed-format ~48-byte string; do not allocate per send.
- **Success:** browser console logs periodic progress; `ring_ms` tracks ~500–3000 ms.

### Step 6 — Teardown / supersede / stop
- Natural end (CLOSE frame or peer close): `bg_player_end_push()` — drain + fade.
  Error/timeout: `bg_player_stop()` (no drain). Superseded (a new push arms via
  `bg_player_start_push`): `bg_stream_feed_pcm` returns false → loop exits, close the
  WS cleanly.
- Free `stage`/recv buffers on EVERY exit path; clear `s_bg_stream_busy` (mirror the
  POST handler's Finish block).
- `/api/stop` and the existing `stopBg` (`?stop=1` POST) still tear down BG via
  `bg_player_stop` — the WS loop notices (`s_bg.streaming` false) and exits.
- **Success:** Stop silences immediately; a second WS connection cleanly supersedes
  the first (no dual-producer, no ring corruption).

### Step 7 — Browser: WS client in `transport.js`
- Add `pushBgWs(getChunk, { pan, loudness, onProgress }) => Promise`:
  - `ws = new WebSocket('ws://' + location.host + '/api/bg-ws')` (same-origin →
    SoftAP-safe). `ws.binaryType = 'arraybuffer'`.
  - `onopen`: send handshake JSON, then start pumping PCM chunks.
  - **Backpressure:** before each `ws.send(chunk)`, if `ws.bufferedAmount >
    HIGH_WATERMARK` (e.g. 512 KB) OR the last back-channel `ring_ms` is near-full,
    `await` a short delay / the next drain tick. This is the pacing the POST path got
    for free from TCP; over WS the app must implement it via `bufferedAmount`.
  - `onmessage`: parse back-channel JSON → update `onProgress` / pacing state / stall
    watchdog (no `consumed` progress for N s → reject/reconnect).
  - `onclose`/`onerror`: resolve on clean close (server sent CLOSE after drain);
    reject on abnormal close so the caller can surface it.
- Keep `pushBg` (MP3-POST) and `stopBg` as-is. `stopBg` also closes the WS if open.
- **Unit-testable** piece: the handshake-JSON builder + chunker + backpressure
  decision (pure functions), tested in Node with a fake WS.

### Step 8 — Browser: produce raw PCM (adapt `bounce.js` / `play.js`)
- `bounce.js` currently returns MP3. Add a raw-PCM producer path: reuse
  `interleaveInt16` / `encodeWav16` internals to yield **headerless raw int16 stereo
  LE PCM** (the WS handshake carries the format, so NO RIFF header — send
  `encodeWav16` output minus the 44-byte header, or a new `encodePcm16` that skips
  the header). Chunk it (~16–32 KB) so the WS pump can pace.
- `play.js`: when the WS path is selected, call `pushBgWs(chunkIter,
  {pan,loudness})` instead of `pushBg(blob,...)`. The prime-sync flow (playDoc arms
  the device to wait, then push fills the ring) is unchanged — same `push://<name>`
  BG semantics.
- Selection: a feature flag / capability probe (attempt WS; on failure fall back to
  the existing MP3-POST `pushBg`). Keep it SoftAP-only friendly (no internet).
- **Success:** `npm run build` + `npm test` green; bundle-size delta noted (WS client
  is tiny; keep lamejs for the MP3-POST path — do NOT drop it).

### Step 9 — Build + hardware verification (orchestrator/user — subagents MUST NOT flash)
- `idf.py build` clean with `CONFIG_HTTPD_WS_SUPPORT=y` + `CONFIG_BG_SUPPORT_PUSH=y`,
  and clean with `CONFIG_BG_SUPPORT_PUSH=n` (WS handler compiled out). Record app-size
  delta.
- `cd web && npm run build && npm test` green.
- **Orchestrator flashes** and verifies:
  - WS raw-PCM BG plays with **no clicks/artifacts** (the core hypothesis: no decode
    = no decode artifacts). A/B against the same session via MP3-POST.
  - Prime-sync: timeline defers t=0 until ~500 ms buffered; speech stays aligned.
  - SoftAP-only (`ws://192.168.4.1/api/bg-ws`) works with no internet.
  - Single-ingest: WS while a POST push is active → clean 409/close; new WS supersedes
    old cleanly.
  - Server stays responsive during a long WS session (`/api/stop`, `/api/state`
    answer) — confirms async-worker offload works and nothing wedges.
  - Back-channel frames arrive; browser pacing keeps `ring_ms` in a healthy band with
    no underruns over a multi-minute stream.
  - Regression: MP3-POST and WAV-POST + existing `BG http(s)://` pull all unchanged.

### Step 10 — Docs + report + list
- Update `README.md` BG section (third ingest: WS raw-PCM), the `/api/bg-ws` contract
  (handshake JSON + binary PCM + back-channel), the `CONFIG_HTTPD_WS_SUPPORT`
  requirement, SoftAP support, and the resume-deferred note. Update this plan's
  STATUS.
- Add the `plans/list.md` entry; write a report under `reports/`.

---

## Comparison: MP3-POST vs WAV-POST vs WS-raw-PCM

| Dimension | MP3-POST (current) | WAV-POST (original) | WS-raw-PCM (proposed) |
|---|---|---|---|
| Decode artifacts | **Possible** — minimp3 on 8 KB async stack (the click source) | None (raw PCM) | **None** (raw PCM, no decoder) |
| Data volume (27 min) | ~26 MB (~128 kbps) | ~286 MB (~1.4 Mbps) | ~286 MB (~1.4 Mbps) — WS does NOT shrink it |
| Ring jitter cushion | ~20× (1 MB holds ~60 s of MP3-fed audio) | ~3 s (1 MB PCM) | ~3 s (1 MB PCM) — same as WAV |
| Reliability / resume | Single POST; long stall trips `MAX_TIMEOUTS` → abort, no resume | Same — single POST, no resume | Stall detected via back-channel; **resume-from-offset possible (v2)** |
| Back-channel | None (POST is one-way) | None | **Yes** — device reports consumed/ring level |
| Pacing | Automatic (TCP backpressure) | Automatic (TCP) | App-level via `ws.bufferedAmount` + back-channel (more code) |
| Complexity (new code) | Low (decoder reused) | Lowest (baseline) | **Medium** — WS handshake, framing, back-channel, browser pacing |
| SoftAP support | Yes | Yes | Yes (`ws://`, same-origin) |
| On-device CPU/stack | minimp3 decode on 8 KB stack | trivial convert only | trivial convert only |

**Takeaway:** WS-raw-PCM trades MP3's data-efficiency + deep cushion for
*artifact-freedom* (no decode) and a *back-channel that enables true resume*. It
does NOT reduce the 27-min bandwidth need; only resume (v2) makes it genuinely
robust. If the clicks are confirmed to be minimp3, WS-raw-PCM removes their root
cause outright.

---

## Risks & notes

- **WS on the 8 KB async worker (`ASYNC_WORKER_STACK`).** The WS receive path
  (`httpd_ws_recv_frame` + `bg_stream_feed_pcm` + `bg_convert_to_stereo_float`) is
  shallow and uses PSRAM scratch, so it fits — but minimp3 is NOT on this path
  (that's the win). Verify high-water-mark on the worker task after a long WS
  session; do not add stack-resident large buffers.
- **Only 7 sockets, held for the whole session.** A WS push holds ONE socket for
  ~27 min. With `ASYNC_WORKERS 1`, a concurrent long request (a `tts` HTTPS proxy)
  plus UI traffic must fit in the remaining 6. The `s_bg_stream_busy` guard prevents
  two concurrent pushes. Monitor for socket exhaustion if the browser opens many
  short-lived UI requests while the WS is up.
- **WS keepalive / ping-pong.** Over a 27-min upload, NAT/AP timeouts or a half-dead
  browser can silently kill the socket. Rely on the stack's control-frame handling
  (`handle_ws_control_frames=false` auto-PONGs PINGs); have the browser send periodic
  activity and the device's back-channel act as a liveness signal both ways. The
  `BG_STREAM_MAX_TIMEOUTS` bound still frees the worker on a truly dead socket.
- **Raw PCM does NOT fix the fundamental 27-min real-time bandwidth need.** WS
  reframes and adds a back-channel, but 1.4 Mbps for 27 min over SoftAP is the same
  demand as the original WAV-POST. A long-enough WiFi stall still starves the ring.
  **Only resume (v2) makes it robust** — v1 must not be presented as drop-proof.
- **App-level backpressure is a new failure surface.** Unlike TCP's automatic POST
  pacing, WS pacing depends on the browser correctly honoring `ws.bufferedAmount` and
  the back-channel. A bug → either browser memory blowup (send too fast) or ring
  underrun (send too slow). Unit-test the pacing decision; validate on hardware.
- **Headerless framing must match the ring format exactly.** 44.1 kHz / 16-bit /
  stereo LE, or `bg_convert_to_stereo_float` produces garbage. The handshake locks
  it; reject any other rate/bits/ch at Step 3 rather than silently mis-decoding.
- **Do NOT remove MP3-POST / WAV-POST in this plan.** WS is additive and
  A/B-testable. A default-switch or removal is a separate change after WS is
  hardware-verified artifact-free.
