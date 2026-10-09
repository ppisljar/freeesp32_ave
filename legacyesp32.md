# Legacy ESP32 (classic) — where supporting it costs us something

This firmware targets **ESP32-S3** going forward, but still builds and runs on the
**classic ESP32** boards (glasses / ac101 / es8388). This file is the running
record of every place where classic-ESP32 support makes us do something
*differently or worse* than an S3-only codebase would.

**The rule:** when the S3 can do something materially better, we do NOT make
every target take the slow path. We ship a **separate module per target**,
selected in `main/CMakeLists.txt` by `IDF_TARGET`, and log the divergence here.

Read this before:
- deciding whether a workaround still needs to exist,
- dropping classic-ESP32 support (this is the removal checklist),
- adding a new peripheral that the two chips expose differently.

---

## 1. Neopixel RMT: CPU-refilled FIFO vs GDMA

| | classic ESP32 | ESP32-S3 |
|---|---|---|
| RMT words per channel | 64 | 48 |
| TX-capable channels | 8 | 4 |
| Total TX FIFO | **512 symbols** | 192 symbols |
| GDMA on RMT (`SOC_RMT_SUPPORT_DMA`) | **no** | **yes** |

**Files:** `main/led_rmt.h`, `main/led_rmt_esp32.c`, `main/led_rmt_esp32s3.c`

A 48-LED frame is 1152+1 RMT symbols — far more than either chip's FIFO — so
the frame has to be streamed while it transmits. If the stream falls behind,
the strip latches a half-written colour and visibly flickers.

- **Classic ESP32** has no DMA on RMT, so the CPU must refill the FIFO
  mid-transmission. The only defence is a *big* FIFO: we claim all 8 memory
  blocks for the single channel, which cuts refills from ~18 to ~2 per frame
  and widens each refill window from ~40 µs to ~320 µs — enough to ride out a
  WiFi interrupt burst. It is a mitigation, not a fix: a long enough strip or a
  busy enough radio can still starve it.
- **ESP32-S3** streams the frame over GDMA. There is no CPU refill to starve,
  so the failure mode is gone rather than mitigated.

Note the S3's *non-DMA* path would be **worse** than the classic ESP32's (192 vs
512 symbols of FIFO). DMA is what makes the S3 better here, not FIFO size — which
is exactly why this is two modules and not one tunable constant. `led_rmt_esp32s3.c`
keeps a non-DMA fallback only for the case where something else already claimed
the one DMA-capable TX channel.

**If classic support is dropped:** delete `led_rmt_esp32.c` and the `IDF_TARGET`
branch in `main/CMakeLists.txt`; `led_rmt_esp32s3.c` can lose its non-DMA fallback
if nothing else in the project ever takes an RMT TX channel.

---

## 2. GPIO numbering: 0–39 with holes vs 0–48 with different holes

| | classic ESP32 | ESP32-S3 |
|---|---|---|
| Valid GPIOs | 0–39, no 24 / 28–31 | 0–48, **no 22–25** |
| Input-only | 34–39 | — |

**Files:** `main/Kconfig.projbuild`, `main/settings.c` (`pin_is_valid`)

Every GPIO option in `Kconfig.projbuild` carries a target-conditional range:

```kconfig
range -1 48 if IDF_TARGET_ESP32S3
range -1 39
```

This is pure legacy tax — an S3-only tree would have one `range` line per option.

`main/settings.c` previously clamped any pin arriving over `/api/settings` to a
hard `39`, which on the S3 would silently turn a requested GPIO47 into GPIO39
and drive the wrong pin. It now validates with `GPIO_IS_VALID_GPIO()`, which is
per-target and also rejects each chip's numbering holes — something a numeric
clamp cannot express. That change is strictly better on *both* targets and
should survive a classic-support removal.

**If classic support is dropped:** collapse each Kconfig option to a single
`range -1 48`. Keep `pin_is_valid()`.

---

## 3. I2S clock source: APLL vs fractional divider

**File:** `main/audio_manager.c` (guarded by `SOC_I2S_SUPPORTS_APLL`)

The **classic ESP32** needs its APLL to produce a usable MCLK. Its default
160 MHz PLL cannot integer-divide to audio rates × 256 — for
44100 × 256 = 11.2896 MHz you get ~11.43 MHz, which is far enough off that
MCLK-driven codecs misbehave. APLL's fractional-N synthesis hits audio rates
exactly, so the code switches to it whenever MCLK is wired.

The **ESP32-S3 has no APLL on I2S at all** (`SOC_I2S_SUPPORTS_APLL` is
undefined; sources are PLL_F160M / PLL_D2 / XTAL). It doesn't need one — its
I2S clock divider does true fractional division, so the driver synthesises
exact audio rates from PLL_F160M by itself.

Guarded on the SoC capability rather than `IDF_TARGET`, so any future chip
gets the right branch automatically. This is a one-line `#if`, not a module
split, because the S3 path is simply "don't do the workaround".

---

## 4. I2S MCLK pin routing

**Files:** `main/Kconfig.projbuild` (`AUDIO_I2S_MCLK_GPIO` help text),
`main/audio_driver_es8388.c` (header comment)

On the **classic ESP32**, MCLK can only be routed to GPIO 0, 1 or 3 — a hard
pin-mux constraint, which is why the ES8388 boards are stuck with MCLK on GPIO0.
The **ESP32-S3** has no such restriction; the YB-ESP32-S3-DAC uses GPIO4.

This costs us nothing in code (the pin is already a runtime setting), but the
constraint is baked into the classic boards' configs and documentation, so it is
recorded here.

---

## 5. Flash size and partition layout

| | classic boards | YB-ESP32-S3-DAC |
|---|---|---|
| Flash | 4 MB | 16 MB |
| PSRAM | none / 4 MB | 8 MB OPI |

**Files:** `partitions.csv`, `partitions_16mb.csv`, `sdkconfig.*`

Both tables use the same dual-boot model: `ota_0` = main app, `ota_1` = the
standalone minimal OTA updater, *not* a second copy of the app. So `idf.py
build` prints a standing `Part 'ota_1' ... too small for binary` warning on
**both** targets — `ota_1` is deliberately sized for the updater. Expected, not
a broken build.

What the S3's extra flash buys is filesystem room rather than a second app
slot: `storage` 0.5 → 2 MB and `cfgfs` 0.5 → 8 MB, with ~2 MB left unallocated
for a future dedicated audio partition.

---

## 6. Dropped: `components/esp-dsp`

**File:** `CMakeLists.txt` (`EXCLUDE_COMPONENTS esp-dsp`)

The vendored `components/esp-dsp` predates esp-dsp's multi-target support: its
`CMakeLists.txt` unconditionally assembles ESP32-classic LX6 sources (`*_ae32.S`),
which the S3's LX7 toolchain cannot build.

Nothing in `main/` ever called a `dsps_*` / `dspm_*` function — there was a single
vestigial `#include "dsps_tone_gen.h"` in `main/audio_generator.h` and no uses.
Rather than carry a target-split of code we never call, the component is excluded
from the build entirely. The directory is untracked and still on disk; delete it
if you are sure. If ESP-DSP is ever genuinely wanted, pull a current upstream
release (which selects sources per target) instead of un-excluding this copy.
