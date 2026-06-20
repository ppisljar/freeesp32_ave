# freeesp32_ave

**Audio-Visual Entrainment (AVE) firmware** for the
[`freeesp32_audioplayer`](https://github.com/ppisljar/freeesp32_audioplayer)
ESP32 board.

A **low-cost, open-source alternative** to commercial AVE / mind machines such as:

- **Pandora Star**
- Mind Alive **David Delight Pro**
- MindPlace **Kasina**
- **roXiva lamp**
- Lumenate **Nova**

## What it does

- Generates **binaural beats**, isochronic tones, frequency sweeps, and other
  brainwave-entrainment audio in real time on the ESP32 (ESP-DSP based)
- Drives **LED flicker visuals synchronized to the audio** with
  sub-millisecond timing precision
- Plays timeline scripts (`.led` files) that choreograph audio + light into
  full entrainment sessions
- Built-in Wi-Fi web UI for uploading sessions and controlling playback

## Supported LED backends

Pick one at build time via `idf.py menuconfig`:

| Backend | Driver | Typical use |
|---|---|---|
| **NeoPixel** | WS2812 / SK6812 via RMT | RGB LED strips and glasses |
| **DotStar**  | APA102 via SPI3        | High-refresh-rate RGB |
| **Direct LEDC** | 8× PWM @ 5 kHz       | Plain LEDs, lamps, high-power drivers |

**8 logical flicker channels** × **16 audio channels**, with per-pixel
channel mapping configurable from `menuconfig`.

## Hardware

Designed for the
[`freeesp32_audioplayer`](https://github.com/ppisljar/freeesp32_audioplayer)
open-hardware board (XIAO ESP32-S3 + CS4344 DAC + APA2068 speaker amp +
microSD + configurable LED headers).

Runs on bare XIAO ESP32-S3 dev boards too, with externally wired I²S DAC and
LED strip.

## Build & flash

```bash
source ./activate.sh        # activate ESP-IDF v5.5.2 environment
idf.py menuconfig           # pick LED backend, set GPIO mapping
idf.py build flash monitor
```

## Timeline format

Sessions are described in plain-text `.led` files. Use
[`freeesp32_ave_generator`](https://github.com/ppisljar/freeesp32_ave_generator)
for a browser-based graphical editor that produces them.

Quick example:

```
# time(ms)  freq(Hz)  duty(%)  brightness(%)  channel
1000        10.0      50       75             0
2000        >40.0     50       100            1     # > = linear ramp
A1500       binaural  40.0     6.0                  # 40 Hz base, 6 Hz beat
```

## Related projects

| Repo | Role |
|---|---|
| [`freeesp32_audioplayer`](https://github.com/ppisljar/freeesp32_audioplayer) | Open-hardware ESP32 board (KiCad) |
| [`freeesp32_ave`](https://github.com/ppisljar/freeesp32_ave) | **This repo** — firmware |
| [`freeesp32_ave_generator`](https://github.com/ppisljar/freeesp32_ave_generator) | Web session editor / timeline generator |

## Status

Active development. See `plans/` for the implementation roadmap and
`INTEGRATION_STATUS.md` for the current state.

## License

TBD.
