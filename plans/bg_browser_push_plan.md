# BG Browser-Push Audio — Plan & Contract

> **STATUS (2026-07-02): IMPLEMENTED (Phases 1–3.5), build clean both flag
> states + web tests green (141) + web bundle builds. NOT yet flashed / hardware-
> verified — that step is the orchestrator's/user's.** Firmware: `bg_player.c/.h`
> (start_push/push_pcm/end_push + producer_kind + push:// short-circuit),
> `web_server.c` (`POST /api/bg-stream` streaming handler + `?stop=1`),
> `config_parser.c` (push:// accepted), `Kconfig.projbuild`
> (`CONFIG_BG_SUPPORT_PUSH`, default y; app +~3.5 KB when on). Web: `gen/bgaudio.js`
> (WAV encoder + resample + noise/drone gen + decode), `gen/bgstore.js` (IndexedDB
> library), `gen/synth.js` (offline session bounce), `gen/views/bg_panel.js` (UI),
> `gen/transport.js` (pushBg/stopBg), wired into `generator.js` + `index.html` +
> `style.css`. See report `reports/non_planned_reports/bg_browser_push_report.md`.
>
> **Original status: PLANNED, not started.** New feature, purely additive.
> Lets the **browser** generate, load-from-disk, store, and **push** background
> audio directly into the device instead of the device pulling it from an
> external HTTP(S) URL. The existing `BG <http(s)://…>` pull path is **kept
> intact** — this adds a second, parallel source. Big wins: (a) works in
> **SoftAP-only mode** (no internet needed — the current pull path can't fetch
> anything without a station uplink), and (b) the browser does all decoding /
> resampling, so the device only ever receives its canonical 44.1 kHz 16-bit
> stereo PCM contract and never needs an on-device MP3/OGG decoder for these
> sources.

## Goal

Today the BG player is a **PULL client**: a `BG <url> <pan> <loudness>` line in a
`.led`/`.ledc` names an `http://`/`https://` URL, and `bg_streamer_task` opens an
outbound `esp_http_client` GET, sniffs the container, decodes to 44.1 kHz stereo
float, and pushes into a 1 MB PSRAM ring that the mix task drains additively
(`bg_player.c`). `sdcard://` is a compile-gated stub.

We want a new, additive **PUSH** source: the browser (already talking to the
device's `esp_http_server` over `/api/*`) sends audio bytes *into* the device
over a new endpoint, feeding the **same ring** through the **same decode tail**.
The browser gains a small BG audio toolkit: **generate** (noise/drone/tones),
**load from disk** (any format the browser can decode), **store** clips in the
browser (IndexedDB) and **save to disk**, and **select** any stored/loaded clip
as the BG source for a session.

Everything downstream of the ring is unchanged. Nothing in the existing pull
path is removed or altered in behavior.

## Design decisions

- **Invert producer, reuse everything else.** The ring
  (`xStreamBufferCreateStatic`, 1 MB PSRAM, `bg_player.c:145,1097`), the decode
  tail (`bg_convert_to_stereo_float`, `bg_player.c:336`), the consumer
  (`bg_player_mix_into`, `bg_player.c:1406`-ish), the pan/loudness/fade ramps,
  and the I2S sink (`audio_test.c`) are all untouched. Push mode only supplies a
  **different producer**: the HTTP-server request thread feeding the ring via a
  new public `bg_player_push_*` API.
- **Single-producer safety is non-negotiable.** FreeRTOS `StreamBuffer` is
  strictly single-producer/single-consumer. Push mode MUST (a) `bg_player_stop()`
  any active pull first, and (b) **never spawn `bg_streamer_task`**. The push
  handler thread is the sole producer for the duration of the POST.
- **Browser conforms the format; device stays codec-blind.** The browser
  resamples to **44100 Hz, stereo, 16-bit PCM** and wraps it in a canonical WAV
  header, so the device reuses the *existing* `wav_parser` + `bg_convert_to_stereo_float`
  path verbatim. No new on-device decoder. (This makes the separate
  `mp3_support_plan.md` orthogonal — both can coexist; neither needs the other.)
- **Transport = one long `POST` + TCP backpressure (MVP).** `httpd_req_recv` is
  pull-based on the firmware side: decode a chunk → block on `xStreamBufferSend`
  until the ring drains → only then read the next socket chunk. TCP flow control
  throttles the browser's upload to real-time playback rate automatically — no
  app-level pacing. This avoids the `fetch()` streaming-request-body limitation
  (Chrome only allows `ReadableStream` bodies over HTTP/2; the device is
  HTTP/1.1), because the browser sends a normal fixed-`Content-Length` `Blob`.
- **Stream-through only (one mode).** The clip streams through the ring while the
  tab stays connected — unbounded length, low RAM. **Looping** is client-driven:
  the browser re-POSTs the clip when the previous POST completes (a tiny gap at
  the seam is acceptable for ambient beds). A resident-in-PSRAM / gapless-loop
  mode was explicitly ruled out of scope for this plan.
- **`push://` scheme for round-trip, not for fetching.** A `BG push://<name>`
  line lets a saved `.ledc` remember "BG is a browser clip named `<name>`" so the
  session reopens losslessly and the browser can re-push on load. The device
  config parser accepts `push://` as valid but bg_player treats it as a **no-op
  pull** (never fetches, never errors) — the actual bytes arrive via
  `/api/bg-stream`. The existing `http(s)://`/`sdcard://` schemes are unchanged.
- **Feature flag:** `CONFIG_BG_SUPPORT_PUSH` (default `y`) in
  `main/Kconfig.projbuild`, mirroring `CONFIG_BG_SUPPORT_MP3`, so the endpoint +
  push API compile out on constrained builds.

Active project dir: `freeesp32_ave` (build: `source ./activate.sh && idf.py
build`). Platform: dual-core ESP32 @ 240 MHz, **PSRAM enabled** (`CONFIG_SPIRAM=y`).
Web UI: vanilla-JS esbuild bundle under `web/` (build: `./build_web.sh` or
`web/ && npm run build`; tests `npm test`). Web assets pack into SPIFFS.

---

## Key integration points (verified against current source)

| Concern | Location |
|---|---|
| Ring = float stereo 44.1k, 1 MB PSRAM, created once at boot | `bg_player.c:145`, `:1097`-`:1109` |
| Shared decode tail (int16→float, mono dup, 2× upsample) — **reuse** | `bg_convert_to_stereo_float`, `bg_player.c:336`-`:355` |
| WAV streaming loop + watermark pacing (pattern to mirror) | `bg_player.c:446`-`:539`, watermark `:674` |
| WAV header parse (browser sends WAV → reuse) | `wav_parser.c/.h` |
| URL-scheme dispatch (add `push://` no-op branch) | `bg_dispatch_url`, `bg_player.c:292`-`:298` |
| Producer task lifecycle (push must NOT spawn it) | `bg_streamer_task`, `bg_player.c:986`; spawn at `:1202` |
| `bg_player_start` WiFi-connected gate (pull-only; push skips it) | `bg_player.c:1158` |
| `bg_player_stop` joins producer task (needs push-aware branch) | `bg_player_stop_impl`, `bg_player.c:1256`-`:1347` |
| Consumer mix (NO CHANGE) | `bg_player_mix_into` |
| Output sink float→int16→I2S (NO CHANGE) | `audio_test.c:104`-`:173` |
| Public BG API (add push entries) | `bg_player.h:41`-`:121` |
| HTTP server + URI table + wildcard match | `web_server.c:151`, handlers `:164`-`:318` |
| Incremental body-recv precedent (mirror this, not play-config's buffer-all) | `configs_put_handler`, `web_server.c:~691` (`httpd_req_recv` loop → sink) |
| BG config struct (url/pan/loudness) | `config_bg_entry_t`, `config_parser.h:122`-`:125` |
| BG line parse + scheme validation (add `push://`) | `parse_bg_line`, `config_parser.c:1367`-`:1416` |
| Web transport (add `pushBg`/`stopBg`) | `web/src/js/gen/transport.js` |
| Web generator page shell (add BG panel) | `web/index.html:49`-`:110`, `web/src/js/gen/generator.js` |
| Web local store precedent (localStorage; audio needs IndexedDB) | `configstore.js`, `reportstore.js` |

**Latent detail to preserve:** the WAV loop drops partial trailing bytes per
chunk (`frames = bytes / frame_bytes`); for 16-bit stereo the frame is 4 bytes,
so a chunk boundary can split a frame. The push handler MUST **carry** the 0–3
leftover bytes across `httpd_req_recv` reads (same class of bug as the MP3
carry-buffer). The WAV RIFF header (44 bytes) may likewise straddle the first
read — buffer until ≥ header size before parsing.

---

# Phase 1 — ESP32 push producer + endpoint (core, MVP)

## Step 1 — Feature flag + build wiring

1. Add to `main/Kconfig.projbuild`:
   ```
   config BG_SUPPORT_PUSH
       bool "Accept browser-pushed BG audio via /api/bg-stream"
       default y
       help
         Compile in the HTTP endpoint + bg_player push API so the web UI can
         stream/generate BG audio into the device instead of the device pulling
         from an external URL. Works in SoftAP-only mode. Adds ~a few KB flash.
   ```
2. All new code guarded by `#if CONFIG_BG_SUPPORT_PUSH`. Object files stay
   present-but-empty when disabled (project style, per `mp3_support_plan.md` Step 1).
3. **Success:** `idf.py build` clean with flag `y` and `n`. No behavior change yet.

## Step 2 — `bg_player` push-mode API + single-producer state

Add to `bg_player.h` / `bg_player.c` (all `#if CONFIG_BG_SUPPORT_PUSH`):

```c
/* Arm push mode: stop any active pull, reset+drain ring, mark active+streaming,
 * set pan/loudness/fade like bg_player_start, but DO NOT spawn bg_streamer_task.
 * The caller's thread (HTTP handler) becomes the sole producer. WiFi-state gate
 * is skipped (works on SoftAP). Returns ESP_ERR_INVALID_STATE if not init'd. */
esp_err_t bg_player_start_push(float pan, float loudness);

/* Feed one buffer of interleaved 16-bit PCM (channels 1 or 2, hz 44100 or 22050)
 * into the ring, reusing bg_convert_to_stereo_float. Blocks on ring-full
 * (bounded timeout) → provides backpressure. Returns bytes pushed, or 0 if push
 * mode was ended (streaming went false) so the handler can stop reading. */
size_t bg_player_push_pcm(const int16_t *pcm, size_t frames,
                          unsigned channels, bool upsample_2x);

/* End push mode: streaming=false, active=false, ring drains to silence via the
 * consumer's zero-fill. Idempotent. NOT a task-join (no producer task exists). */
esp_err_t bg_player_end_push(void);
```

Internals:
- Add a `producer_kind` to the state struct (`bg_player.c:215`): `PULL` (existing)
  vs `PUSH`. `bg_player_start_push` sets `PUSH`; `bg_player_start` sets `PULL`.
- `bg_player_start_push`: take the state mutex; if `s_bg.active` →
  `bg_player_stop()` (joins/kills any pull task first — guarantees single
  producer); reset ring (`xStreamBufferReset`); set ramps/pan/loudness exactly as
  `bg_player_start` does (`bg_player.c:1179`-`:1197`); `s_bg.streaming = true`,
  `s_bg.active = true`, `producer_kind = PUSH`; **no `xTaskCreatePinnedToCore`.**
- `bg_player_push_pcm`: bail early returning 0 if `!s_bg.streaming`; else
  `bg_convert_to_stereo_float(...)` into a caller-or-static `flt_buf`, then
  `xStreamBufferSend(s_bg.ring, …, timeout)`. Mirror the watermark check
  (`bg_player.c:674`) so a full ring yields rather than busy-waits.
- **`bg_player_stop_impl` push-awareness (`bg_player.c:1256`):** when
  `producer_kind == PUSH`, skip the producer-task join block (`:1316`-`:1332`) —
  there is no task we own; just set `streaming=false`, `active=false`, log, done.
  This is the one edit to an existing function; guard it so PULL behavior is
  byte-identical.

**Success:** build clean; existing pull BG unchanged (regression-verify Step 8).

## Step 3 — `/api/bg-stream` streaming POST handler

New handler in `web_server.c` (register in the URI table, `:164`-`:318`):

- **`POST /api/bg-stream?pan=<-100..100>&loudness=<0..100>`**,
  body = canonical WAV (44100 Hz, 16-bit, stereo — what the browser sends).
  (No `loop` flag: looping is client-driven by re-POSTing, so the firmware just
  plays the body once and ends.)
- Parse query params (default pan 0, loudness 50). Call
  `bg_player_start_push(pan, loudness)`.
- **Incremental recv loop** (mirror `configs_put_handler`'s `httpd_req_recv`
  pattern, `web_server.c:~691` — NOT the buffer-all `/api/play-config` path):
  ```
  carry[HDR+3] ; parse WAV header once enough bytes buffered (reuse wav_parser)
  loop:
      r = httpd_req_recv(req, buf, BUF)      // BUF ~ 4-8 KB internal DRAM
      if r <= 0: break (0 = client closed, <0 = err/timeout)
      prepend carried 0..3 leftover bytes; frames = whole 16-bit-stereo frames
      bg_player_push_pcm(frames…)  // BLOCKS on ring-full → TCP backpressure
      carry the 0..3 trailing bytes
      if bg_player returned 0 (push ended by a concurrent stop): break
  bg_player_end_push()
  respond 200 {"ok":true,"bytes":N}   // browser re-POSTs here if looping
  ```
- **Concurrency:** only one `/api/bg-stream` in flight at a time — a second POST
  calls `bg_player_start_push` which stops the first (its handler's
  `push_pcm`/`recv` then returns and it exits cleanly). Guard with a small
  `s_bg_push_busy` atomic to reject overlap with `409` cleanly rather than racing.
- **DRAM:** recv buffer + `flt_buf` in `MALLOC_CAP_INTERNAL`; nothing large held
  (streaming). No 32 KB cap (that's config-only).

**Success:** build clean; a WAV POSTed by `curl --data-binary @clip.wav` plays and
mixes under the generator (hardware, Step 8).

## Step 4 — `push://` scheme (config round-trip, no fetch)

- `parse_bg_line` (`config_parser.c:1381`): add `push://` to the accepted scheme
  prefixes alongside `http(s)://`/`sdcard://`. Store the URL as-is (`push://name`).
- `bg_dispatch_url` (`bg_player.c:292`): add a `push://` branch that returns
  `ESP_OK` **without** opening any connection (a no-op "pull"), so a timeline that
  pre-rolls a `push://` BG neither fetches nor errors — the audio is expected to
  arrive via `/api/bg-stream`. `bg_player_start` for a `push://` url must also
  **skip the WiFi-connected gate** (`bg_player.c:1158`) and **not spawn** the
  streamer (or spawn one that immediately clean-exits on the no-op). Cleanest:
  `bg_player_start` detects `push://` and internally routes to
  `bg_player_start_push`-like arming without a task.
- **No removal:** `http(s)://`/`sdcard://` handling is untouched.

**Success:** a `.ledc` with `BG push://ambient 0 60` parses, plays the timeline,
and silently waits for a browser push (no fetch error, no crash). Round-trips
through serialize/parse losslessly.

---

# Phase 2 — Browser audio engine (generate / decode / conform / store)

New module `web/src/js/gen/bgaudio.js` (pure, unit-testable helpers + Web Audio):

## Step 5 — WAV encoder + resampler (the device contract)

- `encodeWav16(audioBuffer) → Blob` — write a 44-byte canonical RIFF/`fmt `/`data`
  header (PCM, 2 ch, 44100, 16-bit) + interleaved little-endian int16 from the
  float channels (clamp ±1.0 → ±32767). ~30 lines, no deps. **Unit-tested**:
  header field offsets, sample count, round-trip a known ramp.
- `conform(audioBuffer) → AudioBuffer @44100/stereo` — resample via
  `OfflineAudioContext(2, ceil(dur*44100), 44100)` + a `BufferSource` → render.
  Mono is upmixed to stereo by the graph. This guarantees the device only ever
  sees its accepted rate/channels, so the firmware never rejects.

## Step 6 — Generators (Web Audio, offline-rendered)

`generate(kind, opts) → Promise<AudioBuffer>` via `OfflineAudioContext`:
- **Noise**: white / pink / brown (pink via Voss-McCartney or a biquad-shaped
  white; brown via integrated white). Duration + gain.
- **Drone / tone bed**: N oscillators (sine/triangle) + detune + slow LFO on gain
  for movement. Optional stereo width.
- **Isochronic/amplitude-mod bed** (optional): carrier × pulse envelope — but note
  the *device* already does binaural/iso on the audio channels; BG generators are
  for ambient texture, kept distinct to avoid confusing the two systems.
- All return a rendered `AudioBuffer` → `conform` → `encodeWav16`.

## Step 7 — File load + browser library (IndexedDB) + save-to-disk

- **Load from disk**: `<input type="file" accept="audio/*">` →
  `arrayBuffer` → `AudioContext.decodeAudioData` (browser decodes mp3/wav/ogg/m4a
  per platform support) → `conform` → store.
- **Library store** `web/src/js/gen/bgstore.js`: **IndexedDB** (localStorage is
  too small — clips are MBs; `configstore.js`/`reportstore.js` use localStorage
  only because configs are tiny text). One object store `bgclips`:
  `{ name, createdAt, durationMs, sourceKind, wavBlob }`. CRUD:
  `list()/get(name)/put(clip)/delete(name)`. Small hand-rolled idb wrapper
  (no new npm dep) — open/upgrade/txn helpers. **Unit-tested** against a fake-idb
  shim or via a thin interface so logic is testable in Node.
- **Save to disk**: `URL.createObjectURL(wavBlob)` + a download anchor →
  user gets the `.wav`. Also allow re-import of a saved wav (round-trip).

**Success:** `npm test` green for `encodeWav16` + store logic; manual: generate →
appears in library → download → re-load.

---

# Phase 3 — Browser BG panel UI + wiring

## Step 8 — Transport push

Add to `web/src/js/gen/transport.js`:
```js
// POST a WAV Blob to the device; resolves when the device finished consuming
// (or the stream was superseded). Backpressure is handled by TCP.
export function pushBg(wavBlob, { pan = 0, loudness = 50 } = {}) {
  const q = `?pan=${pan}&loudness=${loudness}`;
  return fetch('/api/bg-stream' + q, {
    method: 'POST',
    headers: { 'Content-Type': 'audio/wav' },
    body: wavBlob,
  }).then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); });
}
export function stopBg() { return fetch('/api/bg-stream?stop=1', { method:'POST' }); }
```
Looping is a panel-level concern: the BG panel re-invokes `pushBg` when the prior
promise resolves (and a "loop" toggle is on), until the user stops.
(`stopBg` → handler calls `bg_player_end_push`; or reuse `/api/stop` which already
tears down BG. Pick one; document it.)

## Step 9 — BG panel in the generator page

- Add a **"Background audio"** section to `web/index.html` (near the generator
  toolbar, `:49`-`:80`) + logic in a new `web/src/js/gen/views/bg_panel.js` wired
  from `generator.js`.
- UI: library list (from `bgstore`), **[Generate ▾]** (kind + duration + params),
  **[Load file…]**, per-clip **[Play as BG] [Download] [Delete]**, and
  **pan / loudness** sliders + a **loop** toggle (client-driven re-POST).
- **Select-as-BG for a session**: choosing a clip sets the doc's BG to
  `BG push://<name> <pan> <loudness>` (via the existing model BG row / wizard
  `session.bg`), so it serializes into the `.ledc` and round-trips. On **Play**
  (`transport.playDoc`) the panel first `pushBg`es the selected clip's WAV, then
  plays the config — BG and timeline run concurrently (BG is session-level, matching
  current semantics). On **load** of a saved `.ledc` referencing `push://<name>`,
  if the named clip exists in the library, offer to re-push it; if not, show a
  clear "clip not in this browser's library" note (graceful, non-fatal).
- **Preview locally**: optional — play the clip through the browser's own
  `AudioContext` so the user can audition before pushing (no device round-trip).

**Success:** end-to-end in a browser against a running device (SoftAP or STA):
generate/load → select → Play → BG audible under the generator, pan/loudness
honored, Stop silences it. Existing `BG http(s)://` configs still play unchanged.

---

# Phase 3.5 — "Bounce session to single WAV" (opt-in) — ADDED 2026-07-02

User request: a button (NOT automatic) that renders the **whole session's audio**
into one WAV, then can **download / save-to-library / push-to-device** (all
three). "Whole session audio" = the **full entrainment mix** the device would
synthesize — binaural beats, isochronic/amplitude mod, frequency sweeps, noise,
per-channel waveform + pan + volume — for the entire timeline, mixed to one
44.1 kHz/16-bit/stereo stream. The device then just plays it as BG; the LED
timeline still runs on-device for sync.

## Step 9.5 — Browser synth engine (`web/src/js/gen/synth.js`)

Reproduce the device's audio path (`audio_generator.c`) offline with Web Audio
(`OfflineAudioContext`), matching it perceptually (not bit-exactly):
- Per audio channel: carrier oscillator; binaural = L/R offset by `freqR`;
  waveform per `wave` (sine/square/tri/saw — match device LUT/table choices);
  isochronic/amplitude mod per `mod`; frequency **sweeps** via
  `setValueCurveAtTime`/linear+quad ramps mirroring the parser's `>`/`*` interp.
- Noise channel: white/pink matching the device's noise.
- Per-channel **pan law** and **volume** matching `audio_generator.c` (equal-power
  vs linear — verify against source).
- Mix all channels + optional BG, clamp/limit as the device does, render to an
  `AudioBuffer` for the full timeline duration → `encodeWav16`.
- **Fidelity caveat**: documented as "approximate, matches perceptually"; the
  exact device constants (waveform tables, mod depth law, pan law, mix headroom)
  are mirrored from `audio_generator.c` and cited in comments.

## Step 9.6 — Bounce UI + outputs

- A **"Bounce session → WAV"** button in the BG panel (clearly opt-in; never
  runs automatically). Shows progress (offline render can take a few seconds).
- On completion offer all three: **Download** (.wav), **Save to library**
  (IndexedDB, as a normal clip), **Push to device** (`pushBg`, becomes active BG).
- The bounced clip behaves like any other library clip afterward (selectable as
  a session's `push://<name>` BG).

**Success:** bouncing a known session produces a WAV that, played in the browser
and pushed to the device, sounds equivalent to the device synthesizing it live.

---

# Phase 3.6 — Speech (`S`) rows + TTS + bounce-scope choice — ADDED 2026-07-02

User request: a new `S <time> <voice> <volume> "text"` entry type. TTS-synthesized
in the browser and mixed into the bounced WAV; **filtered out before the device**
(firmware can't parse `S`). Bounce gains a scope choice: *BG + Speech* (leave A to
the device) vs *All* (BG + Speech + A).

## Implemented
- **Model/parse/serialize** (`model.js` `speechRow`, `parse.js` `S` line w/ quoted
  text incl. `#`, `serialize.js` `serializeSpeech` + `serializeForDevice` which
  strips `S`). `transport.playDoc` uses `serializeForDevice`. Config *storage*
  keeps `S` for round-trip.
- **TTS** (`tts.js`), two free/no-key engines, selectable:
  - **Puter.js** (default) — `puter.ai.txt2speech` (AWS Polly neural), lazy-loaded
    from `js.puter.com`, browser-only. May prompt Puter sign-in.
  - **Google via device** — new firmware `GET /api/tts?tl=&q=` proxy
    (`web_server.c`, under `CONFIG_BG_SUPPORT_PUSH`) does the outbound HTTPS GET to
    `translate.google.com/translate_tts` (esp_http_client + esp_crt_bundle),
    returns MP3; browser splits text ≤180 chars, decodes+concatenates.
  - `previewSpeech` uses the browser's `speechSynthesis` for local audition only.
- **Synth mixing** (`synth.js` `mixInto`/`zeroBuffers`/`clampBuffers`) + panel
  `onBounce`: synth each `S` line, mix at its time; scope `bgspeech` (silence base
  + speech + BG clip) vs `all` (A render + speech + BG clip); 3 outputs.
- **Table view** (`table.js`): `S` row type (add `+ S`, voice/volume/text + 🔊
  preview), grid + card branches.
- Firmware note: TTS proxy pulls the mbedTLS CA bundle into the app (~+90 KB app).

**Decision:** ESP32 proxies **Google** TTS (edge-tts needs WSS + rotating token +
NTP — too heavy for the device); **Puter.js** covers browser-native neural voices.

---

# Phase 3.7 — UI reorganization — ADDED 2026-07-02

Per user feedback the browser UI was restructured:
- **"Background Audio" is now its own top-level tab** (`#page-bgaudio`, nav +
  `nav.js` TABS). It holds ONLY the TTS engine config (Puter/Google, persisted via
  `tts.js` getEngine/setEngine → localStorage) and the clip **library**
  (generate / load-file to populate; download / delete). `bg_panel.js` rewritten
  to this slim form; `#genBgPanel` removed from the Generator page.
- **Generator "Bounce" button** next to Apply Live (`#btnGenBounce`) — bounces the
  whole session (scope 'all') → saves to library + downloads.
- **Play with speech**: if the session has `S` rows, a modal (`util.chooseModal`)
  asks: *Bounce BG+Speech (device plays A)* / *Bounce all (device plays LEDs only)*
  / *Ignore speech*. 'bgspeech' → playDoc (A kept) + push the BG+speech WAV;
  'all' → play LED-only config (`serializeForDevice` of non-audio rows via new
  `transport.playConfigText`) + push the full-mix WAV.
- **BG row** (`table.js`) now offers URL **or** a "📁" local-file load (decodes →
  library → sets `push://name`) **or** a library dropdown.
- Bounce/push orchestration extracted to `gen/bounce.js` (`bounceSession`,
  `pushSessionBg`) so the Generator owns it (not the BG tab). `parse.js` now
  accepts the `push://` BG scheme.

---

# Phase 3.8 — Simplify: no BG page, auto-merge, hidden phrase cache — ADDED 2026-07-02

Per user (after weighing IndexedDB limits — large on desktop, but WAV is
~10.6 MB/min so only full 'all' bounces are heavy):
- **TTS engine config moved to the Settings page** ("Browser preferences" block,
  `#ttsEngineSetting`, bound to `tts.getEngine/setEngine`, browser-local).
- **Background Audio tab REMOVED** (`bg_panel.js` deleted, nav/index/generator
  references removed). No user-facing library UI.
- **Hidden LRU TTS phrase cache** (`bgstore.js` v2 `ttsphrases` store, cap 200,
  evict oldest by lastUsed). `tts.synthSpeech` checks it (key engine|voice|text)
  → decode on hit, store on miss. Gives fast replay without caching big WAVs.
- **Play with `S` = no prompt.** Auto-merge BG+Speech (`bounceSession` scope
  'bgspeech'), `playDoc` (A kept, device synths A live), `pushBg` the merged WAV.
- **http/https BG is now fetched in-browser** to bake speech onto it; on CORS/
  fetch failure `bounceSession` throws a clear error suggesting the user download
  the file and upload it via the BG row 📁 (sdcard:// similarly can't be baked).
  For plain playback (no speech) an http BG is still device-pulled, untouched.
- **Generator "Bounce"** (scope 'all') still saves to the hidden clip store
  (selectable later via the BG row library dropdown) + downloads.

---

# Phase 4 — Docs, build, hardware verification

## Step 10 — Docs
- Update `CLAUDE.md` (both), `README.md`, and web help: BG now has two sources —
  **pull** (`BG http(s)://…`, unchanged) and **browser push** (`BG push://<name>`,
  new), the `/api/bg-stream` contract (WAV 44100/16/stereo, query params), the
  `CONFIG_BG_SUPPORT_PUSH` flag, the SoftAP capability, and that push is
  stream-through (tab stays connected; looping = client re-POST).
- Add `plans/list.md` entry; on completion write `reports/bg_browser_push_report.md`.

## Step 11 — Build clean + hardware verification (orchestrator/user)
1. `idf.py build` clean, `CONFIG_BG_SUPPORT_PUSH` `y` and `n`. `npm test` + `npm run build` green.
2. **Orchestrator flashes** (subagents must NOT flash — see CLAUDE.md hardware policy). Verify:
   - Generate noise/drone in-browser → Play → audible BG under the generator; pan/loudness honored.
   - Load an MP3 from disk → decodes in browser → plays as clean BG (device did no MP3 decode).
   - **SoftAP-only** (no internet): browser push BG works at `http://192.168.4.1`.
   - **Regression:** an existing `BG https://…` pull config still plays identically; `sdcard://` stub behavior unchanged.
   - Single-producer: starting a push while a pull is active cleanly stops the pull (no ring corruption / dual-producer glitch).
   - Sustained stream (>2 min) with no underruns (watermark pacing preserved); Stop silences immediately.
   - Client-driven loop (browser re-POST on completion) restarts the clip cleanly.

**Success metrics:** browser-generated + disk-loaded BG plays without dropouts;
works on SoftAP with no internet; pull-path regression clean; single-producer
invariant holds; build clean both flag states; web tests green.

---

## Risks & notes

- **Single-producer is the #1 hazard.** Any path where both `bg_streamer_task`
  and the push handler feed the ring = corruption. Enforce via
  `bg_player_start_push` stopping pull first + the `producer_kind` branch in
  `bg_player_stop_impl`. Add a defensive assert/log if a push arrives while a
  pull task handle is non-NULL.
- **`fetch` streaming bodies are out.** Do NOT try a `ReadableStream` request
  body for live/infinite audio — Chrome requires HTTP/2. Use finite `Blob` POSTs.
  WebSocket is a viable *future* path for gapless live streaming (esp_http_server
  supports WS) but is out of scope here.
- **Bandwidth / tab lifetime:** raw PCM ≈ 176 KB/s (~10.6 MB/min). Fine over
  local WiFi, but the tab must stay open & connected for the whole session, and
  looping re-POSTs the clip (tiny seam gap). A resident-PSRAM / gapless mode was
  ruled out of scope — revisit only if seamless, tab-free loops become a need.
- **Frame/header straddling:** carry 0–3 trailing PCM bytes and buffer the RIFF
  header across `httpd_req_recv` reads (see "Latent detail" above). This is the
  most likely subtle bug.
- **CPU:** decode is trivial (no compression on device); the convert+push tail is
  the same cost as the WAV pull path. The httpd task priority vs the audio output
  task (prio 23, core 1) — confirm the push recv loop (httpd default prio, core 0)
  doesn't starve LWIP; the ring-full block yields naturally.
- **Flash budget:** endpoint + push API is a few KB; browser bundle grows by the
  audio engine (~a few KB JS). Confirm post-build both stay within headroom
  (runtime-settings plan noted ~26% app free; web assets pack to SPIFFS).
- **Library scope:** IndexedDB is per-origin/per-browser — clips saved on a phone
  aren't on a laptop. The `push://<name>` round-trip degrades gracefully when the
  named clip is absent (prompt to re-load/generate); document this.
