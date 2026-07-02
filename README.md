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

## Timeline format (`.ledc` files)

Sessions are described in plain-text `.ledc` files (legacy `.led` also
accepted). Each line is one of: a comment, an LED command, an audio
command, or a background-audio command. The web UI on the ESP32 and
the [`freeesp32_ave_generator`](https://github.com/ppisljar/freeesp32_ave_generator)
graphical editor both produce this format.

### General syntax rules

- One command per line. Fields are whitespace-separated (spaces or tabs).
- Lines starting with `#` are comments. Inline `# ...` after a command
  is also a comment.
- Blank lines are ignored.
- Numeric values are decimal — integer or floating-point as appropriate.
- Timestamps are absolute milliseconds from session start (t=0).
- Commands within a file do **not** need to be sorted by time — the
  parser orders them. Multiple commands sharing the same timestamp fire
  in the same dispatch cycle.

### LED commands (no prefix)

LED commands change the flicker / brightness / color of one or more LED
channels at a specific time. Two formats are accepted:

**Canonical 8-token form:**

```
time  freq  duty  bright  R  G  B  mask
```

| Field | Range | Meaning |
|---|---|---|
| `time` | 0..N ms | Absolute timestamp |
| `freq` | 0.01..500 Hz | LED flicker frequency. `0` = LED stays at solid `bright` level (no flicker). |
| `duty` | 0..100 % | Duty cycle within each flicker period. 50 = symmetric on/off. |
| `bright` | 0..100 % | Peak brightness during the "on" phase of the flicker. |
| `R` `G` `B` | 0..255 each | LED color, used by NeoPixel / DotStar backends. Ignored by the DIRECT backend (monochrome PWM). |
| `mask` | 0..255 | Bitmask of which logical channels (CH1..CH8) to update. `bit N = channel N+1`. `mask=1` = CH1 only; `mask=3` = CH1+CH2; `mask=255` = all 8 channels. |

**Legacy 5-token form** (no color, single channel):

```
time  freq  duty  bright  channel
```

`channel` is the 1-indexed channel number (1..8). RGB defaults to white
(255,255,255). The mask is derived as `1 << (channel - 1)`. Useful for
quick monochrome LED scripts; for richer behavior use the 8-token form.

### Audio commands (`A` prefix)

Audio commands start a tone / noise on a synthesis channel, or update
parameters of a channel already running. Format is positional with
optional trailing fields:

```
A  time  freq  pan  vol  mod  channel  [freq_r]  [wave_type]
```

| Field | Range | Meaning |
|---|---|---|
| `time` | 0..N ms | Absolute timestamp |
| `freq` | 0..20000 Hz | Carrier frequency (left ear for binaural; ignored for noise types). `0` stops the channel. |
| `pan` | -100..+100 | Stereo position. -100 = full left, 0 = center, +100 = full right. |
| `vol` | 0..100 % | Channel volume. |
| `mod` | 0..N Hz | Amplitude-modulation rate (isochronic tone). `0` = continuous tone. |
| `channel` | 1..16 | Audio channel index. Each channel is an independent oscillator. |
| `freq_r` | 0 or 1..20000 Hz | **Optional.** Right-ear carrier frequency for **binaural beats**. `0` = same as `freq` (mono). When `freq_r != 0`, the channel becomes binaural: left ear gets `freq`, right ear gets `freq_r`, and the brain perceives a beat at `|freq - freq_r|` Hz. |
| `wave_type` | 0..6 | **Optional.** Waveform: `0`=sine (default), `1`=square, `2`=triangle, `3`=sawtooth, `4`=white noise, `5`=pink noise, `6`=brown noise. Noise types ignore `freq` and `freq_r`. |

**Important quirk:** the parser is positional, so to specify `wave_type`
you must also include `freq_r` (use `0` if not binaural). Example for
20 % white noise on channel 9:

```
A  0  0  0  20  0  9  0  4
```

### BG command (`BG` prefix)

Plays a background audio file continuously for the entire session,
mixed underneath the synthesized channels. Not time-scheduled — it's
a global timeline property applied from t=0 to the end. Only one `BG`
line per file is honored (last one wins, with a warning).

```
BG  <url>  <pan>  <loudness>
```

| Field | Range | Meaning |
|---|---|---|
| `url` | string | Source location. Three schemes: `http://host:port/path`, `https://...`, or `sdcard://path`. The stream may be **WAV or MP3** — the container is detected from the stream's magic bytes, not the file extension. Both must be **44.1 kHz (or 22.05 kHz) stereo or mono**; other sample rates are rejected with a log message. WAV must be 16-bit PCM. MP3 support requires `CONFIG_BG_SUPPORT_MP3=y` (default on; uses the built-in minimp3 decoder). The generator server can also auto-transcode MP3 → WAV when you request a `.wav` URL whose source is an `.mp3` file (i.e. `wav/river.wav` works even if only `wav/river.mp3` exists on disk). |
| `pan` | -100..+100 | Stereo position. |
| `loudness` | 0..100 % | Background mix level. Typical values: 20-40 for ambient sound under binaural tracks. |

Example:

```
BG  http://10.0.0.213:8000/wav/river.wav  0  30
```

### Interpolation prefixes — animate-on-start convention

Any numeric field may be prefixed with a special character. **The prefix
lives on the START entry of a transition, not the target entry** — i.e.
the prefix tells the engine "starting at this entry, do X going forward".
There are two families:

**One-shot ramps** — transition smoothly from this entry's value to the
NEXT entry's value (for the same channel/mask), over the time window
between the two entries:

| Prefix | Curve | Example |
|---|---|---|
| `>` | Linear | `>20` then `30` at +1000ms = linear ramp 20→30 over 1 second |
| `*` | Quadratic | `*20` then `30` at +1000ms = quadratic ease 20→30 over 1 second |
| (none) | Step | Instant step to the value at the entry's timestamp. |

**Periodic modulation** — self-contained oscillation that ignores the
next entry and runs continuously until a new entry preempts it. All
five share the same `prefix start:end:period_ms` syntax. The wave runs
its first half-cycle from `start` to `end` over `period_ms/2`, then
back to `start`, repeating:

| Prefix | Wave | Mnemonic | Example |
|---|---|---|---|
| `^` | Triangle (linear up/down) | Caret = triangle peak | `^50:75:1000` = triangle 50↔75 with 1s period |
| `~` | Sine (smooth) | Tilde = sine wave shape | `~50:75:1000` = smooth oscillation 50↔75 |
| `/` | Sawtooth (ramp up) | Forward slash = ramp up | `/0:100:2000` = ramp 0→100 over 2s then jump back to 0 |
| `\` | Reverse sawtooth | Backslash = ramp down | `\100:0:2000` = ramp 100→0 over 2s then jump back to 100 |
| `_` | Square | Underscore = flat | `_50:75:1000` = half period at 50, half at 75 |

Example — fade brightness from 100 to 0 over 60 seconds, then oscillate
LED brightness 50↔75 with sine wave at 500 ms period:

```
0       1.0  50  >100  255 50 0  255       # ramp starts here, target = 0
60000   1.0  50  0     255 50 0  255       # ramp ends here at brightness 0
60500   1.0  50  ~50:75:500  255 50 0  255 # sine modulation begins here
```

**Runtime support status:** every modulation-capable field supports all
5 wave shapes. The engine updates values at 100 Hz via per-field setters
on the LED matrix and audio_generator layers.

| Field | Backed by |
|---|---|
| LED brightness | `led_matrix_update_brightness_masked` |
| LED frequency  | `led_matrix_update_frequency_masked` |
| LED duty       | `led_matrix_update_duty_masked` |
| LED R / G / B  | `led_matrix_update_color_{r,g,b}_masked` |
| Audio frequency | `audio_generator_set_param(ch, AUDIO_PARAM_FREQUENCY, val)` |
| Audio pan       | `audio_generator_set_param(ch, AUDIO_PARAM_PAN, val/100)` |
| Audio volume    | `audio_generator_set_param(ch, AUDIO_PARAM_AMPLITUDE, val/100)` |
| Audio mod freq  | `audio_generator_set_param(ch, AUDIO_PARAM_MOD_FREQ, val)` |

**Implementation note**: 100 Hz update rate gives ~10ms timing jitter on
modulation transitions. For modulation periods under ~200ms this becomes
visible — the rising/falling edges of a square wave or the cusps of a
triangle look "stepped" rather than smooth. Periods ≥ 500ms produce
smooth-looking modulation. For audio modulation specifically, the 100 Hz
update rate is fine for sub-Hz oscillations of any carrier; modulating
a carrier at audio-rate frequencies isn't the intended use.

### A complete, runnable example

A 10-minute session with a binaural pair drifting from 12 Hz beat to
7.83 Hz, amber LED flicker locked to the beat frequency on all
channels, white noise underlay through channel 9, river-sound
background:

```
# Background ambience
BG http://10.0.0.213:8000/wav/river.wav  0  30

# t = 0: start at 12 Hz beat, LED amber at 70 % duty / 60 % bright
0        12     70  60  255 50 0  255
A 0      105   -100  60  0  1            # left ear, binaural
A 0      117    100  60  0  2            # right ear, beat = 12 Hz
A 0        0      0  20  0  9  0  4      # white noise underlay

# t = 10 min: ramp to 7.83 Hz (Schumann resonance)
600000   >7.83  70  60  255 50 0  255
A 600000  >107.085  -100  60  0  1
A 600000  >114.915   100  60  0  2
A 600000        0      0  20  0  9  0  4
```

Real-world session files (45-minute deep-meditation, etc.) live in
`freeesp32_ave_generator/ledc/` — read those for richer examples.

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
