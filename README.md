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

The firmware supports a configurable set of off-the-shelf ESP32 audio
boards (selected at build time via `idf.py menuconfig` and the per-board
`sdkconfig.<board>` snapshots in this directory). Two end-product
configurations cover most use cases:

### Setup A — AVE Glasses (wearable)

Lightweight, battery-powered, LEDs mounted in front of the eyes through
diffuser fabric. Audio is delivered via headphones plugged into the
board's headphone jack.

| Component | Recommendation | Notes |
|---|---|---|
| ESP32 board | **HiFi-ESP32** or **Sonatino** (recommended) | Smaller form factor, modern audio chain, easy battery integration. Both have onboard headphone amp + ES8388-class codec. |
| ESP32 board (alt) | AI-Thinker **ESP32-A1S** ([AliExpress](https://www.aliexpress.com/item/4000130915903.html)) | Cheap and widely available, but bulkier; AC101 or ES8388 variants both work (`sdkconfig.ac101` / `sdkconfig.es8388`). |
| LED visuals | **4×12 NeoPixel RGB matrix** (48 WS2812 LEDs) — [AliExpress](https://www.aliexpress.com/item/1005007218748810.html) | Matches the default `CONFIG_LED_COUNT=48` and `CONFIG_LED_CHANNEL_MAP` frame topology. |
| Diffuser | White ping-pong-ball halves, opal acrylic, or 200-gsm tracing paper | Critical for uniform light spread — bare LEDs are uncomfortably bright and produce visible "dots" instead of a soft flicker field. |
| Frames | 3D-printed or modified safety/swim goggles | Must fully block external light. Keep LED-to-eye distance ≥ 25 mm to avoid focal discomfort. |
| Power | 1 × 18650 Li-ion or 2 × AAA via boost converter | A1S has a JST-PH battery connector and onboard charging; HiFi-ESP32 and Sonatino vary — check vendor datasheets. |
| Audio out | 3.5 mm TRS headphone jack (onboard) | All three boards above expose it directly. |

LED backend for this setup: **NeoPixel** (`CONFIG_LED_TYPE_NEOPIXEL=y`)
on a single data pin (default GPIO 12 in `sdkconfig.glasses`).

**Assembly steps:**

1. **Build the frame.** Either 3D-print one (search Thingiverse/Printables
   for "AVE goggles" or "mind machine glasses" — many free designs) or
   start from cheap swim goggles / safety goggles and remove the lenses.
   The frame must completely block external light and hold the LED matrix
   ~25-35 mm from your closed eyelids.
2. **Wire the LED matrix to the ESP32 board.** Three wires:
   - 5 V → matrix 5 V (red wire)
   - GND → matrix GND (black/white wire)
   - GPIO 12 (or whichever pin `CONFIG_LED_DATA_PIN` is set to) → matrix
     DIN (green/data wire). The matrix is shipped with `IN` and `OUT`
     ends — use the `IN` end. A 470 Ω resistor in series with DIN is
     optional but helps with signal integrity on long leads (>10 cm).
3. **Flash the firmware** before final assembly while the board is still
   accessible: `./switch_board.sh glasses && idf.py flash monitor`.
   Verify the LED matrix lights up on boot.
4. **Add the diffuser** between the LED matrix and the eye-side opening.
   White ping-pong ball halves are a classic hack; thin opal acrylic or
   2-3 layers of tracing paper give a more uniform field.
5. **Glue everything in place.** Hot glue is forgiving and removable
   later. Make sure the USB port on the ESP32 board remains accessible
   for re-flashing — leave a cutout in the frame.
6. **Power via USB.** Either from a 5 V USB battery bank (1000+ mAh
   gets you 6-8 hours) or wall-plug USB charger. For a fully wearable
   setup with onboard battery, add a 18650 cell + protection board on
   boards that support it (A1S has a JST-PH battery input + charging
   IC).

### Setup B — Standalone Lamp + Audio (room device)

High-power continuous LED for room illumination (think "therapy lamp"
class device — comparable to RoXiva or Lumenate Nova hardware) plus
speaker output. Mains-powered, fixed location.

| Component | Recommendation | Notes |
|---|---|---|
| ESP32 board | **A1S** ([AliExpress](https://www.aliexpress.com/item/4000130915903.html)) or HiFi-ESP32 / Sonatino | A1S has onboard speaker amplifier, no extra amp needed for low-volume room use. |
| LED light source | **100 W warm-white COB LED** — [AliExpress](https://www.aliexpress.com/item/1005011883784272.html) | Single chip-on-board emitter, ~30 V Vf at full current. Use lower-power emitters (~10-50 W) for desk-class devices to reduce heatsink size and PSU rating. |
| LED driver | **Mean Well NLDD-1400HW** constant-current driver | 1400 mA output, 2-52 V output range, accepts 12-54 V input. For LEDs running lower current, use NLDD-700H, NLDD-350H, or similar — match driver current to LED rated current. |
| LED dimming | PWM input on the NLDD driver, wired to one ESP32 GPIO | Use **DIRECT LED backend** (`CONFIG_LED_TYPE_DIRECT=y`) with `CONFIG_LED_DIRECT_PIN_CH1` set to the GPIO going to the driver's PWM/DIM pin. The driver does the high-current switching; the ESP32 only supplies a 0-100% PWM signal at 25 kHz. |
| AC adapter | **220 V → 12 V DC, 200-400 W** — [AliExpress](https://www.aliexpress.com/item/1005011942752725.html) | Sized to cover the LED + ESP32 + speakers with comfortable headroom. A 200 W supply handles a 100 W COB + everything else; a 400 W supply gives margin for two COBs or higher-power audio amps. Quality matters — a low-noise adapter avoids audible hum in the speakers. |
| ESP32 supply | **12 V → 5 V buck converter** — [AliExpress](https://www.aliexpress.com/item/1005003141428190.html) (LM2596 / MP1584 / similar) | Step the 12 V LED rail down to 5 V for the ESP32 board's USB/VBAT input. A 1 A buck is more than enough. Avoid 12 V → 3.3 V direct unless your specific board has a 3.3 V-only input (most ESP32 dev boards include onboard 3.3 V regulators expecting 5 V upstream). |
| Heatsink + fan | **Active heatsink with fan for 100 W COB** — [AliExpress](https://www.aliexpress.com/item/1005007983357418.html) | Required at 100 W; passive cooling is feasible up to ~50 W with a large finned heatsink, but active (fan) cooling buys margin and lets you run the LED hard without thermal throttling or color shift. |
| Audio | Onboard speakers (A1S) or external 3.5 mm output | A1S's onboard amp drives small ~1-3 W speakers directly. For louder output add a Class-D amp module fed from the headphone-out signal. |

LED backend for this setup: **Direct LEDC PWM**
(`CONFIG_LED_TYPE_DIRECT=y`) on up to 4 GPIOs going to 4 separate LED
driver dimming inputs. The COB LEDs above are dual-channel (warm+cold
white per emitter, two independent PWM inputs each), and the linked
cooler holds two LEDs — so the full build uses **4 driver-LED channels
total**, one per `CONFIG_LED_DIRECT_PIN_CH1..CH4`. The constant-current
drivers handle all the high-current switching; the ESP32 just emits
0-100% PWM control signals at 25 kHz.

**Assembly steps:**

1. **Solder leads to the COB LEDs.** Each COB has 4 solder pads — two
   for the warm-white channel (+/-) and two for the cold-white channel
   (+/-). Use silicone-insulated wire (resists heat) — 22 AWG is
   adequate for 1.4 A per channel. Tin both pad and wire, then join
   quickly to avoid heating the LED. **The cooler holds 2 LEDs**, so
   repeat for the second emitter — total 4 channels (2 warm + 2 cold
   across both LEDs).
2. **Wire each LED channel to its own constant-current driver.**
   Four NLDD-1400HW drivers, one per channel: the driver's `+OUT`/`-OUT`
   goes to the matching channel pair on the COB. Keep wire runs short
   (<20 cm) to minimize voltage drop. **Important:** all 4 drivers
   share the same +12 V input rail, but each has its own dimming line.
3. **Wire each driver's dimming/PWM input to a separate ESP32 GPIO.**
   For a default `sdkconfig.glasses` or `sdkconfig.a1s` build with
   `CONFIG_LED_TYPE_DIRECT=y`:
   - Driver 1 PWM → GPIO `CONFIG_LED_DIRECT_PIN_CH1` (default 12)
   - Driver 2 PWM → GPIO `CONFIG_LED_DIRECT_PIN_CH2` (default 13)
   - Driver 3 PWM → GPIO `CONFIG_LED_DIRECT_PIN_CH3` (default 14)
   - Driver 4 PWM → GPIO `CONFIG_LED_DIRECT_PIN_CH4` (default 15)
   Plus a shared GND between the driver dimming line and the ESP32 GND.
4. **Assemble the optics.** Stack: heatsink (with fan facing out) →
   thermal paste → COB LED → diffuser lens. The cooler kit linked above
   typically includes mounting hardware for both LEDs and the diffuser
   lenses; mount them so the two LEDs are spaced for even light
   distribution at the intended viewing distance.
5. **Wire the 12 V → 5 V buck converter** between the 12 V rail (shared
   with the LED drivers) and the ESP32 board's USB or VBAT input.
   Trim the buck's output potentiometer to exactly 5.0 V before
   connecting to the ESP32 — over-voltage will fry the board.
6. **Connect mains.** 220 V AC adapter → 12 V DC rail → splits to (a)
   all 4 LED drivers, (b) the 12 V → 5 V buck → ESP32. Add an inline
   fuse on the 12 V side rated for ~1.5× your expected total current
   draw (for a 100 W LED load: ~10 A fuse).
7. **Flash the firmware** before final case assembly:
   `./switch_board.sh a1s && idf.py flash monitor`. Verify all 4 LED
   channels respond to a test `.led` file that exercises each channel
   individually (`mask=1`, `mask=2`, `mask=4`, `mask=8`).
8. **Power on via the 220 V cord.** No USB cable needed during normal
   operation — the buck converter feeds the ESP32 from the same 12 V
   rail as the LEDs.

**Safety notes:**
- The 220 V AC side is mains voltage — house it in an insulated
  enclosure with strain relief on the mains cord. Don't expose any
  uninsulated mains conductors.
- COB LEDs at 100 W produce intense light — never look directly at the
  bare emitter without the diffuser in place. Even briefly. Eye damage
  is real.
- The heatsink runs hot (60-90 °C in active operation). Keep it away
  from skin contact, fabric, and other heat-sensitive items.

### Wiring summary (per board family)

The board-specific `sdkconfig.<board>` files in this directory hold the
exact pin assignments. To switch between boards: `./switch_board.sh
{glasses|ac101|es8388}` then `idf.py build flash`.

### Original reference design

The project was originally designed for the
[`freeesp32_audioplayer`](https://github.com/ppisljar/freeesp32_audioplayer)
open-hardware board (XIAO ESP32-S3 + CS4344 DAC + APA2068 speaker amp +
microSD + configurable LED headers). The firmware still runs there
unchanged; the pin map for it lives in `sdkconfig.glasses`.

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
