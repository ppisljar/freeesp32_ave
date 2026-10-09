# SD card: fully offline sessions

**Goal:** play a complete session — entrainment tones, LED flicker, background
music *and* speech — with no internet and no browser attached. Everything the
session needs lives on the device's SD card.

Today a speech session is only playable while a browser is driving it: speech is
synthesized in the browser (Puter TTS), the whole session is bounced to one WAV
in the browser, and that WAV is streamed to the device over WebSocket for the
entire length of the session (`web/src/js/gen/play.js`). Close the laptop and
the session stops. That is the gap this plan closes.

## Decisions taken (user, 2026-10-09)

1. **Speech: pre-render in the browser, store PER PHRASE on the card.** Keep
   using Puter TTS while online; the device just plays files — **no on-device
   TTS**.

   NOT one mixed WAV per session. The user's model: *"every speech line we have
   should be uploaded to the sd card and few sample background music files. the
   logic in the webapp should be to upload any missing speech files and bg files
   to the sd card."* So the card holds a **library**, synced incrementally:

   ```
   /sdcard/speech/sp_<hash>.wav   one file per unique phrase
   /sdcard/bg/<name>.wav          a handful of shared background tracks
   ```

   Phrases are shared across sessions, so this costs far less space than a mixed
   WAV per session and makes adding a session nearly free.

   **Filename derivation.** `sp_<8 hex>.wav` where the hash is FNV-1a 32 over
   `voice + "|" + text`. Both the browser and the firmware compute it from the
   same inputs, so no manifest file and no `.ledc` format change is needed — the
   device resolves an `S` row to a filename on its own. The two implementations
   MUST stay byte-identical; a unit test should pin several known strings.

   **Architectural consequence — the one real cost of this model.** Speech and
   background are now separate streams, so the DEVICE has to mix them, where
   previously the browser delivered a single pre-mixed WAV. The output task in
   `main/audio_test.c` already mixes in stages:

   ```
   audio_generator_fill_buffer()   16 tone channels
   bg_player_mix_into()            background stream
   speech_player_mix_into()        <-- NEW, third stage
   ```

   A `speech_player` mirroring `bg_player` (ring buffer, prime gate, gain/pan)
   is the clean way in; it is additive and does not disturb the existing paths.
   This is the largest single piece of work in the plan.
2. **Transfer: both web upload and direct card access.** Upload over WiFi from
   the web UI for convenience; pull the card out for bulk background music.
3. **Fall back silently to the browser** when a session's audio is not on the
   card, so nothing breaks if a session was never prepared.
4. Written now; implementation waits on the codec bring-up (there is no point
   testing an audio path while the codec is silent — see
   `esp32s3_yb_dac_port_plan.md`).

## Current state

| | |
|---|---|
| Card | formatted, single FAT32 MBR partition, ~15 GB, write-verified |
| Board wiring | microSD on SPI: CS=10, MOSI=11, SCK=12, MISO=13 (seeded in `sdkconfig.defaults.esp32s3`) |
| `CONFIG_BG_SDCARD_ENABLED` | **off** by default |
| `sdcard://` handling | **a stub** — `main/bg_player.c:338` logs `"BG dispatch: sdcard:// '%s' (stub)"` and does nothing |
| Mount code | does not exist; only a ~30-line comment sketch at `main/bg_player.c:949-982`, whose pin examples are classic-ESP32 SDMMC and do **not** match this board |

`main/config_parser.c:1637` already accepts `sdcard://` as a valid BG URL scheme,
so the parser side needs no work.

## Steps

### 1. Mount the card
New `main/sdcard.c` / `sdcard.h`: `sdcard_mount()` / `sdcard_unmount()` /
`sdcard_is_mounted()`, using `esp_vfs_fat_sdspi_mount("/sdcard", ...)` on the
runtime pins already in settings (`sd_cs/sd_mosi/sd_miso/sd_clk`).

- Mount **lazily and non-fatally**: a missing or unreadable card must degrade
  exactly like the codec failure did — log it, mark the subsystem DEGRADED, and
  let the rest of the device boot. Never block startup on removable media.
- The defaults currently in NVS on the S3 board are the *classic* board's SD
  pins (`sd_cs=5, sd_mosi=23, sd_miso=19, sd_clk=18`) because the settings blob
  predates the S3 seed. Step 1 must either migrate them or the user must set
  them in the web UI — **verify the live values before blaming the hardware.**
- The vendor documents SD CS as a solder bridge and MOSI/SCK/MISO as not broken
  out to headers; confirm the bridge is closed before debugging in software.

### 2. Make `sdcard://` real
Replace the stub at `main/bg_player.c:333-338`. Translate `sdcard://rain.wav` →
`/sdcard/rain.wav` and feed it to the existing streaming path — the WAV and MP3
decoders, the ring buffer and the prime-gate already exist and are
format-agnostic about where bytes come from. This should be a reader shim, not a
second pipeline.

Turn `CONFIG_BG_SDCARD_ENABLED` on for the S3 board only.

### 3. Speech player on the device (the big one)
Speech and BG are separate files now, so the device must mix them. Add
`main/speech_player.c/.h` mirroring `bg_player`: open `/sdcard/speech/sp_<hash>.wav`,
decode into a ring buffer, and expose `speech_player_mix_into()` as a THIRD mix
stage in `main/audio_test.c`, after `bg_player_mix_into()`.

The timeline executor triggers it: an `S` row at time T calls
`speech_player_play(voice, text)`, which derives the filename by hash and starts
playback. Short phrases, so a modest ring is fine — but the prime-gate pattern
from `bg_player` still applies so the first samples aren't ragged.

If the file is missing, log and continue: a missing phrase must never abort a
session.

### 3b. Filename hash, shared by both sides
FNV-1a 32 over `voice + "|" + text`, rendered `sp_%08x.wav`. Implement in
`main/speech_player.c` and in `web/src/js/gen/sdsync.js`, and pin the two
together with a unit test over known strings — a silent divergence here means
the device looks for files the browser never uploaded.

### 4. File transfer over WiFi
Mirror the existing `/api/configs` REST shape (`main/web_server.c:557-570`),
which already does list/get/put/delete against cfgfs:

- `GET    /api/sd`         — list files (name, size), plus free/total space
- `GET    /api/sd/*`       — download
- `PUT    /api/sd/*`       — upload (streamed to the card, not buffered in RAM)
- `DELETE /api/sd/*`       — delete

Upload must stream in chunks — a background music file is far larger than
available heap. Reuse the chunked-receive approach the OTA updater already uses
rather than inventing a third one.

Web UI: a simple file manager (list / upload / delete / free space) next to the
existing Background-audio panel.

### 5. Incremental sync from the browser
New `web/src/js/gen/sdsync.js`:
1. `GET /api/sd` to list what the card already has.
2. Walk every session (reuse `collectPhrases()` from `gen/ttspreload.js`, which
   already dedups speech across the whole library) to build the needed set.
3. Upload only what is missing — speech from the existing IndexedDB phrase cache
   when present, synthesizing only genuine gaps; plus any referenced BG tracks.
4. Report progress like the existing "Preload all speech" button does.

Then teach `playSession()` (`web/src/js/gen/play.js`) that when every phrase a
session needs is already on the card, it should skip the bounce and the
WebSocket push entirely and just send the timeline — that is what delivers
"close the laptop and it keeps playing". **Fall back silently** to the current
browser-streamed path when anything is missing.

### 6. Verification
1. Card absent → device boots normally, SD marked degraded, everything else works.
2. Card present → mounts, file list over `/api/sd` matches what the Mac wrote.
3. Upload a WAV over WiFi, verify byte-identical on the Mac.
4. Play a `sdcard://` BG clip — no browser connected.
5. Full offline session: speech + BG + tones + LEDs, **WiFi off**, laptop closed.
6. Long-run: a full-length session from the card with no underruns
   (`/api/audiostats`).

## Risks / open questions

- **SPI contention.** The SD card and the DotStar LED backend both want SPI. The
  S3 board uses the NEOPIXEL (RMT) backend so they do not collide today, but if
  anyone selects DotStar at runtime with SD enabled, that needs resolving.
- **Throughput.** SDSPI at ~20 MHz is comfortably enough for 44.1 kHz stereo
  WAV, but the existing ring buffer sizing assumes a network source; re-check
  prime depth with a card source, which has very different latency behaviour.
- **MP3 vs WAV on the card.** WAV is simplest and the decoder path already
  exists for both; MP3 saves a lot of space for background music. Storage is not
  scarce on a 15 GB card, so prefer WAV for anything timing-sensitive.
- **cfgfs vs SD.** Sessions (`.ledc`, small text) stay in cfgfs; only audio goes
  on the card. Do not split session state across two filesystems.
