# ESP32-S3 / YB-ESP32-S3-DAC port

Bring the firmware up on the YelloByte **YB-ESP32-S3-DAC** (ESP32-S3-WROOM-1
N16R8: 16 MB flash, 8 MB OPI PSRAM, TI TLV320DAC3101 codec, microSD, WS2812
strip) while keeping the three classic-ESP32 boards building and flashable.

**Architectural rule for this port** (user directive): where the S3 can do
something materially better, do NOT make both targets take the slow path —
ship a **separate module per target**, selected by `IDF_TARGET` in
`main/CMakeLists.txt`. Every place classic-ESP32 support costs us something is
recorded in **`legacyesp32.md`**, which doubles as the removal checklist if
classic support is ever dropped.

## Board facts (verified, not assumed)

Confirmed by probing the attached board (`esptool chip_id`/`flash_id`) and by
reading the vendor repo, schematic and TI datasheet SLAS666B:

| | |
|---|---|
| Chip | ESP32-S3 (QFN56) rev v0.2, dual-core, 8 MB embedded PSRAM |
| Flash | 16 MB, quad |
| Codec | TLV320DAC3101 @ I2C `0x18` — headphone jack + **stereo** class-D (1.3 W/8R) |
| I2S | MCLK=4, BCLK=5, LRCLK=6, DIN=7 |
| I2C | SDA=8, SCL=9 |
| microSD | SPI: CS=10, MOSI=11, SCK=12, MISO=13 |
| Status LED | GPIO47 |

Two board traps that drove design decisions:

1. **MCLK is not connected out of the box.** GPIO4 reaches the codec only
   through solder bridge **JP2, which ships OPEN**. The vendor's own shipping
   example therefore clocks the codec PLL from **BCLK**, not MCLK — and so do
   we (`CONFIG_AUDIO_I2S_MCLK_GPIO=-1`). Closing JP2 and setting the pin to 4
   switches the driver to the MCLK=256xFs clock tree.
2. **GPIO21 is the codec's ~RESET**, and firmware is *expected* to drive it.
   The 3-pad jumper's **default-closed** side is GPIO21 ↔ codec ~RESET; the
   ESP32 EN pin is the *optional* side. Confirmed by
   `espressif/arduino-esp32` `variants/yb_esp32s3_dac/pins_arduino.h`:
   `TLV_RESET = 21 // if resp. solder bridge is closed (default closed)`.

   > **Corrected 2026-10-09.** This previously said GPIO21 was tied to EN and
   > must never be driven. That was backwards, and it is why the first firmware
   > left the pin floating — which held the codec in reset and produced a
   > completely silent I2C bus on first boot. Driving GPIO21 is safe and does
   > **not** reset the ESP32.

   It is still not a free GPIO, so the LED strip correctly uses **GPIO14**
   (GPIO12, the glasses board's LED pin, is the SD clock here).

## Steps

### 1. Drop the vendored esp-dsp — DONE
`components/esp-dsp` is a pre-multi-target copy that unconditionally assembles
ESP32-classic LX6 `*_ae32.S`; the S3's LX7 toolchain cannot build it. It is also
entirely unused (zero `dsps_*`/`dspm_*` calls; one vestigial include in
`main/audio_generator.h`). Excluded via `EXCLUDE_COMPONENTS` rather than deleted,
since the directory is untracked and the exclusion is reversible.

### 2. Target-aware GPIO validation — DONE
- `main/Kconfig.projbuild`: all 22 `range … 39` directives gained a
  `range … 48 if IDF_TARGET_ESP32S3` companion.
- `main/settings.c`: replaced the hard `clamp_pin()` ceiling of 39 with
  `GPIO_IS_VALID_GPIO()`. The old code would silently turn a requested GPIO47
  into GPIO39 — driving a different pin. Now invalid pins are rejected with a
  warning and the previous value kept. Strictly better on both targets.

### 3. Per-target neopixel RMT — DONE
`rmt_new_tx_channel()` was asking for 512 symbol words; the S3 has only
8 x 48 = 384 total and just 4 TX channels, so it would fail outright. Split into
`main/led_rmt.h` + `led_rmt_esp32.c` (big CPU-refilled FIFO) and
`led_rmt_esp32s3.c` (GDMA, no CPU refill — strictly better). Also derived
`NS_TO_RMT_TICKS` from a single `RMT_RESOLUTION_HZ` so tick timings and the
channel's resolution cannot drift apart.

### 4. TLV320DAC3101 driver — DONE (needs hardware verification)
`main/audio_driver_tlv320dac3101.c`, following the existing AC101/ES8388 shape:
paged-register I2C, probe-with-readback, per-sample-rate clock tables for both
the BCLK and MCLK PLL inputs, digital volume in 0.5 dB steps, pop-free deinit.
Register sequence taken from TI's own worked example in SLAS666B plus the
vendor library's divider table — not derived from scratch.

Wired through `audio_driver.c` dispatch, `audio_codec_t`, settings JSON
parse/serialize, the `supported.codec` capability list, Kconfig, and the web UI
codec dropdown.

Two datasheet inconsistencies are noted inline in the driver (TI's prose labels
disagree with its own bit tables on class-D gain and de-pop time); the register
*values* are TI's and are not in dispute, only the human-readable labels.

### 5. Target defaults + 16 MB partitions — DONE
- `sdkconfig.defaults.esp32s3` — flash/PSRAM/partition table plus the board pin
  map (auto-loaded by IDF for the S3 target).
- `sdkconfig.defaults.esp32` — classic-only symbols, including the 4 MB flash
  size that was previously only in the live sdkconfig (a regenerated classic
  config failed the partition step without it).
- `partitions_16mb.csv` — keeps the existing dual-boot model (ota_0 = main app
  **3 MB**, ota_1 = the minimal OTA updater **704 KB**, unchanged from the 4 MB
  table). Full A/B was considered and rejected: it would need a different OTA
  mechanism for no real gain. The extra flash goes to the filesystems instead —
  **storage 2 MB**, **cfgfs 8 MB**, ~2 MB left unallocated for a future
  dedicated audio partition.
- `switch_board.sh` learned `yb_s3_dac`, and its board detection was fixed — it
  tested `CONFIG_AUDIO_DRIVER_*`, which stopped existing after the runtime
  settings refactor, so detection always fell through to "glasses".

### 6. Build verification — DONE
Both targets build clean from a removed `sdkconfig`, each selecting its own RMT
module (`led_rmt_esp32s3.c.obj` / `led_rmt_esp32.c.obj`). 259 web tests green.
S3 app: 1,323,440 bytes — 42% of its 3 MB slot.

### 7. Hardware bring-up — PENDING (orchestrator only)
Not yet flashed. Order of verification on the real board:
1. Boot + serial log; confirm PSRAM and 16 MB flash detected.
2. `/api/state` reachable over WiFi; web UI loads from the 2 MB storage partition.
3. **I2C scan** — confirm the codec ACKs at `0x18` before trusting the driver
   (`audio_driver_i2c_scan` already logs a hint for that address).
4. Audio: tone out of the headphone jack, then the speaker outputs.
5. LEDs on GPIO14 via the GDMA RMT path; watch for flicker under WiFi load.

## Deferred

**SD card offline storage.** The SD card is formatted (single FAT32, 15 GB) and
the board's SPI pins are seeded, but `bg_player.c`'s `sdcard://` handler is
still a stub that only logs — the mount logic exists solely as a comment block.
Implementing it (`esp_vfs_fat_sdspi_mount`, path translation, wiring into the
WAV/MP3 stream path, `CONFIG_BG_SDCARD_ENABLED`) is its own task, to be done
after the board is confirmed working. Goal: fully offline sessions with speech
samples and background music on device.
