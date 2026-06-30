# Runtime Settings Refactor — Plan & Contract

> **STATUS (2026-06-29): All 3 phases implemented, build clean (web + firmware),
> NOT yet flashed/verified on hardware.** App binary 0x113d00 (~26% free).
> Phase 1 ✅ settings core + value migration + Settings web page. Phase 2 ✅
> WiFi runtime + SoftAP (`ESP32-AVE-Setup`/`entrain123` @ 192.168.4.1).
> Phase 3 ✅ all LED backends + codecs compiled in, runtime-selected. NVS
> SETTINGS_VERSION=2. Awaiting orchestrator full flash + user hardware verify.

Goal: move runtime-configurable options out of compile-time `sdkconfig` into a
runtime settings store (NVS-backed), editable via a **Settings** section in the
web UI. `sdkconfig` keeps (a) feature toggles that `#ifdef` whole code paths in
or out (default: everything ON) and (b) the **default values** that seed the
runtime store on first boot. A single firmware ships with all backends compiled
in and is configured entirely over the web — no rebuild needed.

Decisions (from user):
- **Apply model:** save to NVS, reboot to apply hardware settings. Live-apply
  only where trivial (generator URL, default volume). A `POST /api/reboot`
  endpoint + "Save & Reboot" button.
- **WiFi:** credentials are runtime settings, with a **SoftAP fallback** when
  the device can't join the configured network.
- **Sequencing:** three phases, each implemented + built clean (no flash) before
  the next.

The active project dir is `freeesp32_ave` (build: `source ./activate.sh && idf.py
build`; web: `cd web && npm run build`). Subagents must NOT flash.

---

## Settings schema (shared contract — all phases)

New module `main/settings.h` / `main/settings.c`.

```c
typedef enum { LED_BACKEND_NEOPIXEL = 0, LED_BACKEND_DOTSTAR = 1, LED_BACKEND_DIRECT = 2 } led_backend_t;
typedef enum { AUDIO_CODEC_NONE = 0, AUDIO_CODEC_AC101 = 1, AUDIO_CODEC_ES8388 = 2 } audio_codec_t;

typedef struct {
    // LED
    led_backend_t led_backend;          // active backend (runtime select; acted on in Phase 3)
    int  led_data_pin;                  // neopixel/dotstar data
    int  led_clock_pin;                 // dotstar clock
    int  led_count;
    char led_channel_map[256];
    int  led_grid_width, led_grid_height;
    int  led_direct_pins[8];
    int  led_direct_active_low_mask;
    int  led_dotstar_spi_clock_hz;
    // Audio I2S output
    int  i2s_bck_pin, i2s_ws_pin, i2s_data_pin, i2s_mclk_pin, i2s_din_pin, amp_enable_pin;
    // Audio codec
    audio_codec_t audio_codec;          // active codec (runtime select; acted on in Phase 3)
    int  codec_i2c_port, codec_i2c_sda, codec_i2c_scl, codec_i2c_freq_hz;
    // SD card (BG audio)
    int  sd_cs, sd_mosi, sd_miso, sd_clk;
    // Audio misc
    float default_volume;               // 0..1
    // Network
    char generator_url[128];
    char wifi_ssid[33];                 // Phase 2
    char wifi_password[65];             // Phase 2
} device_settings_t;

esp_err_t settings_init(void);                 // nvs_open; load blob or seed from CONFIG_* defaults
const device_settings_t *settings_get(void);   // read-only live struct
esp_err_t settings_apply_json(const char *body, int len); // parse (cJSON), validate, store, commit
int       settings_to_json(char *buf, int cap);           // serialize current settings
esp_err_t settings_reset_defaults(void);       // erase namespace + reseed
```

Persistence: a **versioned blob** in NVS namespace `"devcfg"`, key `"blob"`,
prefixed with `uint16_t SETTINGS_VERSION`. On missing key or version mismatch,
seed the struct from the `CONFIG_*` defaults (see mapping) and write it. Bump
`SETTINGS_VERSION` whenever the struct layout changes (Phase 2 adds wifi → bump).

JSON: use **cJSON** (bundled in IDF). Field names in JSON mirror struct fields
(e.g. `led_backend` as a lowercase string "neopixel"/"dotstar"/"direct";
`audio_codec` as "none"/"ac101"/"es8388"; everything else as numbers/strings).
`settings_apply_json` ignores unknown keys and keeps current value for omitted
keys (partial updates OK). Validate pin ranges (-1..39), clamp volume 0..1.

`settings_init()` is called from `app_main` in `esp32_audioplayer.c` right after
`nvs_flash_init()` (~line 240), BEFORE any driver init reads settings.

### CONFIG_* → settings field seed mapping
| settings field | seed from CONFIG_ |
|---|---|
| led_backend | LED_TYPE_NEOPIXEL?0 : LED_TYPE_DOTSTAR?1 : 2 |
| led_data_pin | LED_DATA_PIN (12) |
| led_clock_pin | LED_CLOCK_PIN (14) |
| led_count | LED_COUNT (48) |
| led_channel_map | LED_CHANNEL_MAP |
| led_grid_width/height | LED_GRID_WIDTH/HEIGHT (12/4) |
| led_direct_pins[0..7] | LED_DIRECT_PIN_CH1..CH8 (12,13,14,15,2,4,16,17) |
| led_direct_active_low_mask | LED_DIRECT_ACTIVE_LOW_MASK (0) |
| led_dotstar_spi_clock_hz | LED_DOTSTAR_SPI_CLOCK_HZ (10000000) |
| i2s_bck/ws/data/mclk/din_pin | AUDIO_I2S_BCK/WS/DATA/MCLK/DIN_GPIO (26/25/22/-1/-1) |
| amp_enable_pin | AUDIO_AMP_ENABLE_GPIO (-1) |
| audio_codec | AUDIO_DRIVER_NONE?0 : AC101?1 : 2 |
| codec_i2c_port/sda/scl/freq_hz | AUDIO_CODEC_I2C_PORT/SDA/SCL/FREQ_HZ (0/33/32/100000) |
| sd_cs/mosi/miso/clk | BG_SDCARD_CS/MOSI/MISO/CLK_GPIO (5/23/19/18) — guard: only meaningful if BG_SDCARD_ENABLED, but still store/seed |
| default_volume | AUDIO_DEFAULT_VOLUME (0.5) |
| generator_url | GENERATOR_SERVER_URL ("http://192.168.1.100:8000") |
| wifi_ssid/password | (Phase 2) currently hardcoded #defines in esp32_audioplayer.c |

Defaults for fields whose CONFIG_ is `depends on` a disabled option (e.g. SD
pins when BG_SDCARD_ENABLED=n, direct pins when not DIRECT) must still compile:
use `#ifdef CONFIG_X ... #else <hardcoded fallback> #endif` in the seed code so
the seed always has a value regardless of the current compile config.

---

## Phase 1 — Settings core + plain-value migration + Settings web page

Scope: build the settings module and migrate every **value-type** CONFIG_ read
(Section "easily runtime-movable" of the inventory) to `settings_get()`. Do NOT
change the compile-time backend selection yet (`#if CONFIG_LED_TYPE_*` and
`#if CONFIG_AUDIO_DRIVER_*` stay). Include `led_backend`/`audio_codec` enum
fields in the struct + JSON (seeded from the compile choice) but don't branch on
them at runtime yet.

1. `settings.c/.h` per the contract (blob+version, cJSON, seed mapping).
2. Call `settings_init()` after `nvs_flash_init()` in `esp32_audioplayer.c`.
3. Replace value reads with `settings_get()->field` at the init call sites:
   - I2S pins: wherever `CONFIG_AUDIO_I2S_*`/`AUDIO_I2S_*` macros configure I2S
     (audio_manager.c / audio_driver.c). Keep `audio_config.h` macros as-is (they
     remain the seed defaults via CONFIG_); only the runtime init reads settings.
   - amp enable pin: `audio_driver.c:145-159` (keep `>=0` guard but read setting).
   - codec I2C: `audio_driver_ac101.c:78-94`, `audio_driver_es8388.c:79-98`,
     `audio_manager.c:136-159`.
   - LED data/clock/count/map/dotstar-clock/direct pins/active-low/grid:
     `led_strip.c:178-206, 976,1070,1108`, `led_matrix_example.c:156,161`.
   - SD pins: `bg_player.c` / `audio_config.h:102-105` (behind BG_SDCARD gate).
   - default volume: wherever AUDIO_DEFAULT_VOLUME is applied.
   - generator URL: `web_server.c:344` appconfig handler → `settings_get()->generator_url`.
   Watch for static-initializer contexts — `settings_get()` is a runtime call, so
   only use it in function bodies, not file-scope initializers. If a CONFIG_ is
   used in a static initializer, convert that init to runtime or leave it.
4. web_server.c: add `GET /api/settings` (returns `settings_to_json`), `POST
   /api/settings` (calls `settings_apply_json`), `POST /api/reboot` (esp_restart
   after a short delay so the HTTP response flushes). Register as exact routes
   before the wildcard.
5. Web UI: new `web/src/js/settings.js` rendering a Settings section (grouped:
   LED, Audio I2S, Audio Codec, SD, Network) from `GET /api/settings`, with a
   "Save" (POST /api/settings) and "Save & Reboot" (POST then /api/reboot)
   button, and a "Restore defaults" action. Add a `<div class="section"
   id="settings">` + heading to `web/src/index.html`, import+init from `main.js`.
   Backend selector fields can render but note "(applied in a later update)".
   Run `npm run build`.
6. Build clean: `cd web && npm run build` then `source ./activate.sh && idf.py
   build`. NO FLASH.

## Phase 2 — WiFi runtime config + SoftAP fallback

1. Add wifi_ssid/password to seed (replace `#define WIFI_SSID/PASSWORD`; seed
   from those values as defaults). Bump SETTINGS_VERSION.
2. `esp32_audioplayer.c:375` connect call reads `settings_get()->wifi_ssid/password`.
3. SoftAP fallback in `wifi_manager.c`: if STA connect fails / times out (or
   ssid empty), start AP mode (SSID e.g. "ESP32-AVE-Setup", known password or
   open) so the user can reach the web UI and set credentials. Web server must
   bind in AP mode too. Document the AP SSID/IP.
4. Settings page: WiFi SSID + password fields. Build clean. NO FLASH.

## Phase 3 — Runtime backend selection (compile all backends in)

1. Kconfig: replace `choice LED_TYPE` with bools `LED_SUPPORT_NEOPIXEL/DOTSTAR/
   DIRECT` (default y) + a `choice LED_DEFAULT_BACKEND` (seeds settings only).
   Same for audio: `AUDIO_SUPPORT_AC101/ES8388` (default y; "none" is always
   available) + `choice AUDIO_DEFAULT_CODEC`. Update sdkconfig/sdkconfig.defaults.
2. `led_strip.c`: compile all supported backends (`#if CONFIG_LED_SUPPORT_*`) and
   branch the init on `settings_get()->led_backend` at runtime instead of the old
   `#if defined(CONFIG_LED_TYPE_*)` chain. Ensure RMT (neopixel), SPI (dotstar),
   LEDC (direct) drivers coexist.
3. `audio_driver.c` (+ ac101/es8388): compile all supported codecs and dispatch
   on `settings_get()->audio_codec` at runtime.
4. Settings page: enable the LED backend + audio codec selectors; grey out
   backends whose `CONFIG_*_SUPPORT_*` is compiled out.
5. Build clean (verify each backend compiles). NO FLASH.

After all three: orchestrator flashes (full flash — partition table unchanged,
so app+storage) and the user verifies on hardware.
