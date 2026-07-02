# BG MP3 Support — Plan & Contract

> **STATUS (2026-07-02): Steps 1–7 implemented, build clean both flag states
> (`CONFIG_BG_SUPPORT_MP3=y` and `=n`). NOT yet flashed / hardware-verified
> (Step 8 pending).** Decoder = **minimp3** (vendored `main/minimp3.h`, impl in
> `main/mp3_decoder.c`). App binary grew ~33 KB with MP3 on (`0x113d00` →
> `0x11bdc0`); ~22 KB of that is the decoder (compiles out when flag=n). The
> pre-existing `ota_1` partition-size warning is unrelated (asymmetric OTA
> layout from the OTA dual-boot plan). Files touched: `bg_player.c` (dispatch +
> `bg_convert_to_stereo_float` helper + `bg_stream_http_mp3` + stack 4→12 KB),
> `Kconfig.projbuild`, `CMakeLists.txt`, `README.md`. Build-only per step;
> hardware verification is the orchestrator's/user's job (subagents must NOT
> flash).

## Goal

Today the BG player streams **16-bit stereo PCM WAV only**, fetched live over
HTTP/HTTPS (`sdcard://` is a compile-gated stub, out of scope here). A `.led`/
`.ledc` config line `BG <url> <pan> <loudness>` names the source; the URL is
opened, the WAV header parsed, then raw PCM is int16→float converted and pushed
into a 1 MB PSRAM ring buffer that the audio mix task drains additively on top of
the binaural generator.

We want the same `BG <url> ...` command to also accept **MP3** URLs, decoding
them to the same 44.1 kHz stereo float ring format so **everything downstream is
unchanged**.

## Design decisions

- **Decoder: minimp3** — single public-domain header (`minimp3.h`), no ESP-IDF
  component manager needed (the project has none wired up). Vendored into
  `main/`. Patent pool expired 2017; use is clear.
- **Format detection by magic bytes, not URL extension.** The HTTP path already
  reads the first 512 bytes before parsing; sniff those: `RIFF` → WAV, `ID3` tag
  or MPEG frame sync (`0xFF 0xEx/0xFx`) → MP3. The `.mp3`/`.wav` suffix is never
  consulted today and content sniffing is more robust.
- **No decoder vtable (yet).** WAV (fixed-frame streaming) and MP3
  (decode-frame-at-a-time) have genuinely different loop structures — a vtable
  would abstract over nothing. Instead: one magic-byte dispatch + a **shared
  `bg_push_pcm_frames()` helper** for the int16→float→ring tail both use. Revisit
  a vtable only if a third format (FLAC/AAC) is added.
- **Sample-rate policy (v1): accept 44100 and 22050 Hz only** (22050 reuses the
  existing 2× sample-and-hold upsampler), stereo **or mono** (mono is duplicated
  L=R — cheap and common for MP3). Any other rate → reject with a clear log,
  exactly as `wav_parser` already rejects off-spec WAV. General resampling is a
  separate future plan.
- **Feature flag:** `CONFIG_BG_SUPPORT_MP3` (default `y`) in
  `main/Kconfig.projbuild`, mirroring the `CONFIG_AUDIO_SUPPORT_*` precedent, so
  the decoder can be `#if`-compiled out to save flash on constrained builds.

The active project dir is `freeesp32_ave` (build: `source ./activate.sh &&
idf.py build`). Platform: dual-core ESP32 classic @ 240 MHz, **PSRAM enabled**
(`CONFIG_SPIRAM=y`) — ample working RAM for the decoder.

---

## Key integration points (verified against current source)

| Concern | Location |
|---|---|
| HTTP open + 512 B header read + WAV parse | `bg_player.c:617-638` (parse call `:630`) |
| Leftover-past-header handling (carry precedent) | `bg_player.c:366-393` |
| WAV streaming loop + int16→float tail | `bg_player.c:404-478` (convert `:465-478`) |
| Producer task stack (`BG_STREAMER_STACK_BYTES`) | `bg_player.c:154` — **currently 4 KB, too small for MP3** |
| Watermark pacing (reuse as-is) | `bg_player.c:434-439` |
| Ring buffer = float stereo 44.1k, 1 MB PSRAM | `bg_player.c:141`, `:851-858` |
| Consumer mix (NO CHANGE) | `bg_player.c:1163-1237` |
| Output sink float→int16→I2S (NO CHANGE) | `audio_test.c:104-173` |
| WAV parser (NO CHANGE, WAV path keeps using it) | `wav_parser.c/.h` |

**Latent detail to preserve:** the WAV loop drops partial trailing bytes each
chunk (`frames = bytes_read / frame_bytes`). Harmless for fixed 4-byte frames;
**fatal for MP3**, where compressed frames are variable-length and straddle 16 KB
reads. The MP3 loop MUST carry unconsumed bytes across reads (Step 4).

---

## Step 1 — Vendor minimp3 + build wiring + feature flag

1. Add `main/minimp3.h` (upstream single-header, unmodified) and a thin
   `main/mp3_decoder.c` that owns the implementation:
   ```c
   #define MINIMP3_IMPLEMENTATION
   #define MINIMP3_NO_STDIO
   #define MINIMP3_ONLY_MP3        /* drop MP1/MP2 tables — smaller flash */
   #include "minimp3.h"
   ```
2. Add `mp3_decoder.c` to `main/CMakeLists.txt` `SRCS`, gated so it compiles
   even when the flag is off (the `#if CONFIG_BG_SUPPORT_MP3` lives inside the
   file / at call sites, so the object is empty-but-present when disabled — or
   conditionally append to SRCS; pick whichever matches project style).
3. Add to `main/Kconfig.projbuild`:
   ```
   config BG_SUPPORT_MP3
       bool "Compile in MP3 decoder (minimp3) for BG audio"
       default y
       help
         Compile the minimp3 decoder so the BG player can stream MP3 URLs in
         addition to WAV. Adds ~20-30 KB flash. Disable to save space if only
         WAV BG sources are used.
   ```
4. **Success:** `idf.py build` clean with the flag on and off. No behavior change
   yet (nothing calls the decoder).

## Step 2 — Extract shared `bg_push_pcm_frames()` helper

Factor the int16→float→(upsample)→ring tail (currently duplicated at
`bg_player.c:371-392` and `:465-478`) into one static helper:
```c
/* Convert `frames` interleaved int16 samples (channels 1 or 2) to 44.1k stereo
 * float and push to the ring. When upsample_2x, each frame is emitted twice
 * (22050→44100 sample-and-hold). Mono is duplicated L=R. flt_buf must hold
 * frames * (upsample_2x?2:1) * 2 floats. Returns bytes sent. */
static size_t bg_push_pcm_frames(const int16_t *pcm, size_t frames,
                                 unsigned channels, bool upsample_2x,
                                 float *flt_buf);
```
Rewrite the WAV path (both leftover block and main loop) to call it. **Pure
refactor — WAV behavior must be byte-identical.**

**Success:** build clean; WAV BG playback unchanged (regression-verify on
hardware at the end).

## Step 3 — Magic-byte format detection & dispatch

In `bg_stream_from_http`, after the 512 B header read (`bg_player.c:617-626`),
replace the unconditional `wav_parse_header` with a sniff:
```c
if (hdr_bytes >= 12 && memcmp(hdr_buf, "RIFF", 4) == 0) {
    /* existing WAV path → wav_parse_header + bg_stream_http_wav(...) */
#if CONFIG_BG_SUPPORT_MP3
} else if (bg_looks_like_mp3(hdr_buf, hdr_bytes)) {  /* "ID3" or 0xFF 0xEx/0xFx */
    bg_stream_http_mp3(client, hdr_buf, hdr_bytes);
#endif
} else {
    ESP_LOGE(TAG, "BG HTTP: unrecognized container (not RIFF/WAV or MP3)");
    /* close + cleanup + return */
}
```
Rename the existing `bg_stream_http_pcm` → `bg_stream_http_wav` for clarity.
`bg_looks_like_mp3`: true if starts with `"ID3"`, or first byte `0xFF` and
`(second & 0xE0) == 0xE0` (frame sync).

**Success:** build clean; WAV still dispatches correctly; MP3 URLs reach the new
(stub-at-this-step) `bg_stream_http_mp3`.

## Step 4 — MP3 streaming loop with carry buffer (the core)

Implement `bg_stream_http_mp3(client, seed, seed_len)`:

- Allocate a **carry buffer** in INTERNAL DRAM (e.g. `BG_MP3_CARRY_BYTES` = 32 KB
  — must exceed one max MP3 frame ~1441 bytes plus a full HTTP chunk headroom),
  seeded with the `seed` bytes already read for detection.
- Allocate `mp3dec_t` and the PCM output buffer
  (`int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]` = 1152*2) — decoder state on the
  heap (PSRAM ok), pcm scratch in INTERNAL DRAM (tight convert loop).
- `flt_buf` sized for worst case: `1152 frames * 2 (upsample) * 2 ch * float`.
- Loop (reuse the **watermark pacing** verbatim from `bg_player.c:434-439`):
  ```
  while streaming:
      watermark-pace (yield if ring >80% full)
      refill: read up to (CARRY_CAP - carry_len) bytes from HTTP into carry tail
              track eof on read <= 0
      progress = false
      while carry_len > 0:
          n = mp3dec_decode_frame(&dec, carry, carry_len, pcm, &info)
          if info.frame_bytes == 0: break        // need more input → refill
          consume info.frame_bytes (memmove tail to front, carry_len -= )
          progress = true
          if n > 0:
              validate/lock format on first decoded frame (Step 6)
              bg_push_pcm_frames(pcm, n, info.channels, upsample_2x, flt_buf)
      if eof and (!progress or carry_len < 4): break
  free buffers
  ```
- The `info.frame_bytes==0 && n==0` case with junk at the head (e.g. leftover ID3
  payload) — minimp3 skips it and reports `frame_bytes` consumed; trust its
  return to advance. Guard against an infinite no-progress loop when `eof`.

**Success:** build clean. (Playback verified on hardware in Step 8.)

## Step 5 — Producer stack & memory sizing

- Raise `BG_STREAMER_STACK_BYTES` (`bg_player.c:154`) from 4 KB to **12 KB**
  (minimp3 uses a few KB of stack in `mp3dec_decode_frame`; the WAV path was
  tuned for 4 KB and must not starve).
- Confirm all large buffers use `heap_caps_malloc`: carry + pcm scratch =
  `MALLOC_CAP_INTERNAL`, `flt_buf` + `mp3dec_t` = `MALLOC_CAP_SPIRAM`, with the
  existing PSRAM-exhaustion fallback pattern (`bg_player.c:344-348`).
- Verify free heap / PSRAM headroom in the build's static analysis and at boot
  log.

**Success:** build clean; no stack-overflow / malloc-fail logs during Step 8.

## Step 6 — Format validation & rejection

On the **first** decoded MP3 frame, capture `info.hz` and `info.channels`:
- Accept `hz == 44100` (upsample_2x=false) or `hz == 22050` (upsample_2x=true).
- Accept `channels == 1` (mono → duplicate) or `2`.
- Anything else → `ESP_LOGE` with the actual rate/channels ("re-encode to 44.1
  kHz stereo MP3") and abort the stream cleanly (close/cleanup, ring drains to
  silence). Mirror `wav_parser`'s descriptive-rejection ethos.

**Success:** a 48 kHz MP3 is rejected with a clear log, not garbled playback.

## Step 7 — Docs & config

- No `config_parser.c` change needed — `BG <url>` already accepts any URL; the
  content, not the extension, selects the decoder.
- Update `CLAUDE.md` / relevant `.md`: BG now supports WAV **and** MP3 (44.1/22.05
  kHz, mono/stereo) over HTTP(S); note the `CONFIG_BG_SUPPORT_MP3` flag.
- Note in the web UI help text (if BG URLs are surfaced there) that `.mp3` is
  accepted.

**Success:** docs reflect MP3 support and its constraints.

## Step 8 — Build clean + hardware verification (orchestrator/user)

1. `idf.py build` clean, both `CONFIG_BG_SUPPORT_MP3=y` and `=n`.
2. **Orchestrator flashes** (subagents do not). Verify:
   - A 44.1 kHz stereo MP3 URL plays cleanly, loops on EOF, mixes under the
     generator, honors pan/loudness.
   - A 22.05 kHz MP3 plays (upsampled). A mono MP3 plays (L=R).
   - A 48 kHz MP3 is rejected with the expected log.
   - **Regression:** an existing WAV BG still plays identically.
   - Sustained play (>2 min) with no TCP-degradation stall (watermark pacing
     preserved) and no heap/stack warnings.

**Success metrics:** MP3 BG plays without dropouts; WAV regression clean; clear
rejection of unsupported rates; stable >2 min; build clean both flag states.

---

## Risks & notes

- **Frame straddling** is the #1 bug source — Step 4's carry+memmove is the fix;
  never compute frames as `bytes/const` for MP3.
- **No-progress guard:** if minimp3 reports `frame_bytes==0` on a full carry
  buffer at EOF (truncated final frame), break rather than spin.
- **CPU:** MP3 decode is ~25-40% of one core at 44.1k; producer runs on core 0
  (prio 18). Watch it doesn't starve LWIP — the watermark pacing already yields,
  but confirm no audio underruns under WiFi load.
- **Flash budget:** minimp3 + tables ≈ 20-30 KB. The runtime-settings plan noted
  ~26% app free, so there's room; confirm post-build.
- **Report:** on completion, write `reports/mp3_support_report.md` (or under
  `reports/non_planned_reports/` if no list.md entry exists).
