# freeesp32_ave

**Audio-Visual Entrainment (AVE) firmware for off-the-shelf ESP32 audio boards.**
Generates binaural and monaural beats, isochronic tones and synchronised LED
photic stimulation in real time, driven from one plain-text `.ledc` session
script. Developed and measured on the **YelloByte YB-ESP32-S3-DAC**; also runs
on classic ESP32 audio boards (AI-Thinker ESP32-A1S, generic ESP32 + I2S DAC).

### 📖 Documentation → [ppisljar.github.io/freeesp32_ave](https://ppisljar.github.io/freeesp32_ave/)

**This README is for people and agents working on the code.** Everything
user-facing lives on the site, and is not repeated here:

| If you want | Go to |
|---|---|
| To **use** the device — connecting, the web UI, playing sessions, OTA, troubleshooting | [User guide](https://ppisljar.github.io/freeesp32_ave/user-guide/) |
| To **build** one — parts, shopping list, wiring diagrams, LED topology, power, hazards | [Hardware](https://ppisljar.github.io/freeesp32_ave/hardware/) |
| To **write a session** — every `.ledc` line type, column, default and range | [`.ledc` reference](https://ppisljar.github.io/freeesp32_ave/sessions/reference.html) |
| The **entrainment techniques** the firmware implements, and what it does *not* | [Techniques](https://ppisljar.github.io/freeesp32_ave/sessions/techniques.html) |

> ⚠️ This device deliberately flashes light in the 3–30 Hz band and ships **no
> firmware safety interlock**. Read the
> [safety section](https://ppisljar.github.io/freeesp32_ave/#safety) before
> running anything that flickers. Not a medical device; no clinical validation.

---

## Sources of truth

Read this before changing or documenting behaviour — most of the traps in this
project are disagreements between a document and the code.

| Subject | Authority | Derived from it |
|---|---|---|
| `.ledc` semantics | **`main/config_parser.c`** (device) | `ledc_format.md`, then the site's reference page |
| `.ledc` in the browser | `web/src/js/gen/parse.js` | — |
| Measured numbers | the capture files under `reports/` | everything that quotes them |

Two rules that follow:

1. **Where the device parser and the browser parser disagree, the device is
   right and the browser is a bug.** Never "fix" the device to match the editor.
2. **`ledc_format.md` is derived from the C, not the other way round.** It is
   the long-form spec; the site's reference page is derived from *it*. Change
   the code, then the spec, then the site — in that order, in one change.

`docs/` is the site's source (hand-written static HTML, no build step). See
`docs/README.md` before editing it.

## Repository layout

```
main/                 firmware (28 .c files — see "Architecture" below)
web/                  device web UI: vanilla JS, bundled into a SPIFFS partition
docs/                 the project site (GitHub Pages, served from /docs)
sessions/
  library/            86 shipped .ledc sessions
  test/               15 instrumented .ledc files for hardware measurement
  SESSION_DESIGN_GUIDELINES.md
tools/avmeasure/      audio/LED capture + analysis harness (Python)
plans/                execution plans; plans/list.md is the index
reports/              measurement + implementation reports; reports/list.md is the index
ledc_format.md        authoritative long-form .ledc spec (derived from the C)
legacyesp32.md        every classic-ESP32 / ESP32-S3 divergence + removal checklist
sdkconfig.<board>     per-board config snapshots (see "Board profiles")
```

## Architecture

Two paths dispatched from **one clock origin**, which is why audio and light
stay together. `config_parser.c` parses a `.ledc` into a timeline and executes
it; each entry drives the audio generator, the LED flicker engine, or both.

| Area | Files |
|---|---|
| Timeline parse + execute | `config_parser.c`, `timing_engine.c` |
| Audio synthesis | `audio_generator.c`, `mod_engine.c`, `audio_manager.c` |
| Codec drivers | `audio_driver.c`, `audio_driver_{ac101,es8388,tlv320dac3101}.c` |
| LED flicker | `led_matrix_example.c` (the flicker ISR), `led_strip.c`, `led_rmt_esp32{,s3}.c` |
| Background audio / speech | `bg_player.c`, `speech_player.c`, `mp3_decoder.c`, `wav_parser.c` |
| HTTP API + web UI serving | `web_server.c` |
| Settings / NVS | `settings.c` |
| Diagnostics | `diagnostics.c`, `isr_profiling.c`, `log_ctrl.c` |
| Plumbing | `lock_free_comm.c`, `memory_pool.c`, `sdcard.c`, `wifi_manager.c` |

**8 logical flicker channels × 16 audio channels.** LED backend is a build-time
choice:

| Backend | Driver | Typical use |
|---|---|---|
| **NeoPixel** | WS2812 / SK6812 via RMT | RGB strips, glasses, the NeoHEX build |
| **DotStar** | APA102 via SPI3 | High-refresh-rate RGB |
| **Direct LEDC** | 8× PWM | Plain LEDs and high-power constant-current drivers (the lamp build) |

Per-channel audio `vol` is **absolute, not a share of a mix**: every channel is
scaled by a fixed 1/16 regardless of how many are running. See
[the mixing model](https://ppisljar.github.io/freeesp32_ave/sessions/reference.html#mixing).

## Build & flash

```bash
source ./activate.sh        # activate ESP-IDF v5.5.2 environment
idf.py menuconfig           # pick LED backend, set GPIO mapping
idf.py build flash monitor
```

| Script | What it does |
|---|---|
| `./switch_board.sh <board>` | Swap in that board's `sdkconfig` snapshot. |
| `./flash_all.sh` | Full USB flash: bootloader + app + both filesystems. Chip and offsets come from the build, so it follows the selected board. |
| `./build_web.sh` | Build the web bundle (and firmware) without flashing. |
| `./flash_web.sh` | Rebuild the web UI and flash **only** the `storage` partition over USB. The app keeps running. |
| `./ota_flash.sh <ip> [--part app\|storage\|cfgfs]` | Push over WiFi via the minimal OTA updater. |

> **The web UI lives in a SPIFFS partition, not in the app image.** After
> changing anything under `web/`, flash it explicitly — `./flash_web.sh` over
> USB, or `./ota_flash.sh <ip> --part storage` over WiFi. An app-only update
> leaves the device serving the old bundle.

The build prints `Part 'ota_1' … too small for binary`. **That warning is
expected**: `ota_1` holds the minimal standalone OTA updater, not a second copy
of the app. See the header of `partitions_16mb.csv`.

### Board profiles

`./switch_board.sh {glasses|ac101|es8388|yb_s3_dac}` swaps in that board's
`sdkconfig`, then `idf.py build flash`. Targets differ in more than pins — the
classic ESP32 and the ESP32-S3 need genuinely different code in places (RMT
channel layout, I2S clocking, flash size). Every such divergence is catalogued
in **`legacyesp32.md`**, which doubles as the removal checklist for the day
classic-ESP32 support is dropped.

Pin maps and the board-specific gotchas (the TLV320 codec held in reset by a
solder bridge, the unconnected MCLK, the framing difference) are on the
[hardware page](https://ppisljar.github.io/freeesp32_ave/hardware/#s3).

### Reaching the device while developing

mDNS: `http://esp32-ave.local`, or **`http://esp32-ave-mobile.local`** for the
ESP32-S3 profile — the hostname is a per-board setting, and the S3 does not use
the default. If the configured WiFi fails the device raises a SoftAP named
`ESP32-AVE-Setup` (password `entrain123`) at `http://192.168.4.1`; holding the
button during power-on forces that path. Full story:
[Connecting](https://ppisljar.github.io/freeesp32_ave/user-guide/connecting.html).

## Working on the web UI

Run `npm test` in `web/` before flashing; the suite is fast (269 tests) and
covers the parser, the serializer and the device-traffic gate. `web/README.md`
has the theming rules.

**All device requests must go through `deviceFetch()`**
(`web/src/js/devicefetch.js`), and all background polling through
`startPolling()` (`web/src/js/poll.js`). A bare `fetch('/api/...')` is a bug.
The device is a microcontroller with **seven HTTP sockets and one task**, and
Chrome will happily open six connections per host: a handful of unguarded
`setInterval` pollers is enough to occupy every connection, after which the
whole page hangs with no error and no timeout — while `curl` from a terminal
still answers in 40 ms, because curl isn't inside Chrome's exhausted pool. That
asymmetry makes it look like a device fault, and it is not. `deviceFetch` caps
in-flight requests, shares identical GETs and puts a deadline on everything;
`startPolling` schedules the next run only after the previous one finishes.

Before adding a poller, check whether the data is already being fetched —
`livecontrol.js` polls `/api/state` once a second and broadcasts it as a
`devicestate` event precisely so other panels do not open a second one.

## Testing and measurement

| What | Where |
|---|---|
| Web UI unit tests | `cd web && npm test` |
| Session linting | `sessions/validate_session.mjs` |
| Instrumented sessions for hardware capture | `sessions/test/` (see its `README.md`) |
| Capture + analysis harness | `tools/avmeasure/` (see its `README.md`) |

Claims about timing, harmonics or levels belong in `reports/` with the capture
that produced them. Numbers quoted on the site and in `ledc_format.md` are
traceable back to those files — keep it that way rather than citing a figure
from memory.

## Related projects

| Repo | Role |
|---|---|
| [`freeesp32_ave`](https://github.com/ppisljar/freeesp32_ave) | **This repo** — firmware |
| [`freeesp32_ave_generator`](https://github.com/ppisljar/freeesp32_ave_generator) | Web session editor / timeline generator |
| [`freeesp32_audioplayer`](https://github.com/ppisljar/freeesp32_audioplayer) | Open-hardware ESP32 board (KiCad) — **designed, never fabricated**; not a supported target |

## Status

Active development. `plans/list.md` is the roadmap index, `reports/list.md` the
report index, and `INTEGRATION_STATUS.md` the current state.

## License

TBD.
