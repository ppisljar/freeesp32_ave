/*
 * Runtime device settings store — Plan runtime_settings_plan.md, Phase 1.
 *
 * Persistence: a versioned blob in NVS namespace "devcfg", key "blob", laid out
 * as [uint16_t SETTINGS_VERSION][device_settings_t]. On a missing key or a
 * version mismatch the struct is seeded from the compile-time CONFIG_* defaults
 * and written back. All CONFIG_* reads in the seed are wrapped in
 * #ifdef ... #else <hardcoded default> #endif so the seed compiles regardless of
 * which LED backend / audio codec / SD option is currently selected (a CONFIG_
 * that `depends on` a disabled option is simply not #defined).
 */

#include "settings.h"
#include "sdkconfig.h"
#include "driver/gpio.h"   /* GPIO_IS_VALID_GPIO — per-target GPIO validity mask */
#include "esp_bit_defs.h"  /* BIT64 */
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
/* Not a public header, but the only target-accurate account of which pads the
 * memory bus owns — see the unsafe-pin section below. Reached from main/
 * because esp_hw_support exports its whole include/ (as esp_private/periph_ctrl.h
 * in led_strip.c already relies on). Wrapped by exactly one function here so an
 * IDF bump has one place to break. */
#include "esp_private/esp_gpio_reserve.h"
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM_MODE_OCT
#include "soc/spi_pins.h"  /* MSPI_IOMUX_PIN_NUM_* */
#endif
#include <string.h>
#include <stdlib.h>

static const char *TAG = "settings";

#define SETTINGS_VERSION   ((uint16_t)7)   // v7: codec_reset_pin added
#define SETTINGS_NS        "devcfg"
#define SETTINGS_KEY       "blob"

// WiFi credential seed defaults. These were previously hardcoded #defines in
// esp32_audioplayer.c (WIFI_SSID/WIFI_PASSWORD); they now seed the runtime
// store on first boot (or version bump) and are no longer the runtime source.
#define DEFAULT_WIFI_SSID     "Teltonika_Router"
#define DEFAULT_WIFI_PASSWORD "secpass123"

// Default per-pixel channel map (12x4 frame+inner-rect topology). Mirrors the
// CONFIG_LED_CHANNEL_MAP default in Kconfig.projbuild for builds where that
// symbol is not #defined (DIRECT mode).
#define DEFAULT_LED_CHANNEL_MAP \
    "1,1,1,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,1,1,1,2,2,2,2,2,3,3,2,2,3,3,2,2,3,3,2,2,3,3,2,2,2,2,2"

static device_settings_t s_settings;

/* ------------------------------------------------------------------ seed ---- */

static void settings_seed_defaults(device_settings_t *s)
{
    memset(s, 0, sizeof(*s));

    // ---- LED backend (Phase 3: all backends compiled in; this only seeds the
    // runtime default from the CONFIG_LED_DEFAULT_BACKEND_* choice).
#if defined(CONFIG_LED_DEFAULT_BACKEND_NEOPIXEL)
    s->led_backend = LED_BACKEND_NEOPIXEL;
#elif defined(CONFIG_LED_DEFAULT_BACKEND_DOTSTAR)
    s->led_backend = LED_BACKEND_DOTSTAR;
#else
    s->led_backend = LED_BACKEND_DIRECT;
#endif

#ifdef CONFIG_LED_DATA_PIN
    s->led_data_pin = CONFIG_LED_DATA_PIN;
#else
    s->led_data_pin = 12;
#endif

#ifdef CONFIG_LED_CLOCK_PIN
    s->led_clock_pin = CONFIG_LED_CLOCK_PIN;
#else
    s->led_clock_pin = 14;
#endif

#ifdef CONFIG_LED_COUNT
    s->led_count = CONFIG_LED_COUNT;
#else
    s->led_count = 48;
#endif

#ifdef CONFIG_LED_CHANNEL_MAP
    strlcpy(s->led_channel_map, CONFIG_LED_CHANNEL_MAP, sizeof(s->led_channel_map));
#else
    strlcpy(s->led_channel_map, DEFAULT_LED_CHANNEL_MAP, sizeof(s->led_channel_map));
#endif

#ifdef CONFIG_LED_GRID_WIDTH
    s->led_grid_width = CONFIG_LED_GRID_WIDTH;
#else
    s->led_grid_width = 12;
#endif
#ifdef CONFIG_LED_GRID_HEIGHT
    s->led_grid_height = CONFIG_LED_GRID_HEIGHT;
#else
    s->led_grid_height = 4;
#endif

    // Direct-mode LEDC pins (default layout 12,13,14,15,2,4,16,17).
    {
        static const int dflt_direct[SETTINGS_LED_DIRECT_CHANNELS] = {12, 13, 14, 15, 2, 4, 16, 17};
        for (int i = 0; i < SETTINGS_LED_DIRECT_CHANNELS; i++) {
            s->led_direct_pins[i] = dflt_direct[i];
        }
    }
#ifdef CONFIG_LED_DIRECT_PIN_CH1
    s->led_direct_pins[0] = CONFIG_LED_DIRECT_PIN_CH1;
#endif
#ifdef CONFIG_LED_DIRECT_PIN_CH2
    s->led_direct_pins[1] = CONFIG_LED_DIRECT_PIN_CH2;
#endif
#ifdef CONFIG_LED_DIRECT_PIN_CH3
    s->led_direct_pins[2] = CONFIG_LED_DIRECT_PIN_CH3;
#endif
#ifdef CONFIG_LED_DIRECT_PIN_CH4
    s->led_direct_pins[3] = CONFIG_LED_DIRECT_PIN_CH4;
#endif
#ifdef CONFIG_LED_DIRECT_PIN_CH5
    s->led_direct_pins[4] = CONFIG_LED_DIRECT_PIN_CH5;
#endif
#ifdef CONFIG_LED_DIRECT_PIN_CH6
    s->led_direct_pins[5] = CONFIG_LED_DIRECT_PIN_CH6;
#endif
#ifdef CONFIG_LED_DIRECT_PIN_CH7
    s->led_direct_pins[6] = CONFIG_LED_DIRECT_PIN_CH7;
#endif
#ifdef CONFIG_LED_DIRECT_PIN_CH8
    s->led_direct_pins[7] = CONFIG_LED_DIRECT_PIN_CH8;
#endif

#ifdef CONFIG_LED_DIRECT_ACTIVE_LOW_MASK
    s->led_direct_active_low_mask = CONFIG_LED_DIRECT_ACTIVE_LOW_MASK;
#else
    s->led_direct_active_low_mask = 0;
#endif

#ifdef CONFIG_LED_DOTSTAR_SPI_CLOCK_HZ
    s->led_dotstar_spi_clock_hz = CONFIG_LED_DOTSTAR_SPI_CLOCK_HZ;
#else
    s->led_dotstar_spi_clock_hz = 10000000;
#endif

    // ---- Audio I2S output pins (always #defined; no Kconfig depends)
#ifdef CONFIG_AUDIO_I2S_BCK_GPIO
    s->i2s_bck_pin = CONFIG_AUDIO_I2S_BCK_GPIO;
#else
    s->i2s_bck_pin = 26;
#endif
#ifdef CONFIG_AUDIO_I2S_WS_GPIO
    s->i2s_ws_pin = CONFIG_AUDIO_I2S_WS_GPIO;
#else
    s->i2s_ws_pin = 25;
#endif
#ifdef CONFIG_AUDIO_I2S_DATA_GPIO
    s->i2s_data_pin = CONFIG_AUDIO_I2S_DATA_GPIO;
#else
    s->i2s_data_pin = 22;
#endif
#ifdef CONFIG_AUDIO_I2S_MCLK_GPIO
    s->i2s_mclk_pin = CONFIG_AUDIO_I2S_MCLK_GPIO;
#else
    s->i2s_mclk_pin = -1;
#endif
#ifdef CONFIG_AUDIO_I2S_DIN_GPIO
    s->i2s_din_pin = CONFIG_AUDIO_I2S_DIN_GPIO;
#else
    s->i2s_din_pin = -1;
#endif
#ifdef CONFIG_AUDIO_AMP_ENABLE_GPIO
    s->amp_enable_pin = CONFIG_AUDIO_AMP_ENABLE_GPIO;
#else
    s->amp_enable_pin = -1;
#endif

    // ---- Audio codec (Phase 3: all codecs compiled in; this only seeds the
    // runtime default from the CONFIG_AUDIO_DEFAULT_CODEC_* choice).
#if defined(CONFIG_AUDIO_DEFAULT_CODEC_AC101)
    s->audio_codec = AUDIO_CODEC_AC101;
#elif defined(CONFIG_AUDIO_DEFAULT_CODEC_ES8388)
    s->audio_codec = AUDIO_CODEC_ES8388;
#elif defined(CONFIG_AUDIO_DEFAULT_CODEC_TLV320DAC3101)
    s->audio_codec = AUDIO_CODEC_TLV320DAC3101;
#else
    s->audio_codec = AUDIO_CODEC_NONE;
#endif

#ifdef CONFIG_AUDIO_CODEC_I2C_PORT
    s->codec_i2c_port = CONFIG_AUDIO_CODEC_I2C_PORT;
#else
    s->codec_i2c_port = 0;
#endif
#ifdef CONFIG_AUDIO_CODEC_I2C_SDA_GPIO
    s->codec_i2c_sda = CONFIG_AUDIO_CODEC_I2C_SDA_GPIO;
#else
    s->codec_i2c_sda = 33;
#endif
#ifdef CONFIG_AUDIO_CODEC_I2C_SCL_GPIO
    s->codec_i2c_scl = CONFIG_AUDIO_CODEC_I2C_SCL_GPIO;
#else
    s->codec_i2c_scl = 32;
#endif
#ifdef CONFIG_AUDIO_CODEC_I2C_FREQ_HZ
    s->codec_i2c_freq_hz = CONFIG_AUDIO_CODEC_I2C_FREQ_HZ;
#else
    s->codec_i2c_freq_hz = 100000;
#endif
#ifdef CONFIG_AUDIO_CODEC_RESET_GPIO
    s->codec_reset_pin = CONFIG_AUDIO_CODEC_RESET_GPIO;
#else
    s->codec_reset_pin = -1;
#endif

    // ---- SD card pins (depends on BG_SDCARD_ENABLED; seed regardless)
#ifdef CONFIG_BG_SDCARD_CS_GPIO
    s->sd_cs = CONFIG_BG_SDCARD_CS_GPIO;
#else
    s->sd_cs = 5;
#endif
#ifdef CONFIG_BG_SDCARD_MOSI_GPIO
    s->sd_mosi = CONFIG_BG_SDCARD_MOSI_GPIO;
#else
    s->sd_mosi = 23;
#endif
#ifdef CONFIG_BG_SDCARD_MISO_GPIO
    s->sd_miso = CONFIG_BG_SDCARD_MISO_GPIO;
#else
    s->sd_miso = 19;
#endif
#ifdef CONFIG_BG_SDCARD_CLK_GPIO
    s->sd_clk = CONFIG_BG_SDCARD_CLK_GPIO;
#else
    s->sd_clk = 18;
#endif

    // ---- Audio misc
    s->default_volume = 0.5f;  // mirrors AUDIO_DEFAULT_VOLUME
    s->audio_max_volume = 100; // no attenuation by default (full scale)

    // ---- Controls: momentary snapshot/stop button (former #define GPIO 5).
#ifdef CONFIG_BUTTON_GPIO
    s->button_gpio = CONFIG_BUTTON_GPIO;
#else
    s->button_gpio = 5;
#endif

    // ---- Network
#ifdef CONFIG_GENERATOR_SERVER_URL
    strlcpy(s->generator_url, CONFIG_GENERATOR_SERVER_URL, sizeof(s->generator_url));
#else
    strlcpy(s->generator_url, "http://192.168.1.100:8000", sizeof(s->generator_url));
#endif

    // ---- WiFi credentials (Phase 2): seed from the former hardcoded defaults.
    strlcpy(s->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(s->wifi_ssid));
    strlcpy(s->wifi_password, DEFAULT_WIFI_PASSWORD, sizeof(s->wifi_password));

    // ---- mDNS hostname: reachable at http://<hostname>.local on any network.
    // Seeded per board from CONFIG_MDNS_HOSTNAME so two devices on the same
    // network don't both claim the same .local name and knock each other out.
#ifdef CONFIG_MDNS_HOSTNAME
    strlcpy(s->mdns_hostname, CONFIG_MDNS_HOSTNAME, sizeof(s->mdns_hostname));
#else
    strlcpy(s->mdns_hostname, "esp32-ave", sizeof(s->mdns_hostname));
#endif

    // ---- Reports: default to browser localStorage (no device flash writes).
    strlcpy(s->report_storage, "local", sizeof(s->report_storage));
}

/* --------------------------------------------------------------- persist ---- */

static esp_err_t settings_persist(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", SETTINGS_NS, esp_err_to_name(err));
        return err;
    }

    size_t blob_len = sizeof(uint16_t) + sizeof(device_settings_t);
    uint8_t *buf = malloc(blob_len);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    uint16_t ver = SETTINGS_VERSION;
    memcpy(buf, &ver, sizeof(ver));
    memcpy(buf + sizeof(ver), &s_settings, sizeof(device_settings_t));

    err = nvs_set_blob(h, SETTINGS_KEY, buf, blob_len);
    free(buf);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ----------------------------------------------------------- unsafe pins ---- */

/* Pins the device must never be talked into driving.
 *
 * GPIO_IS_VALID_GPIO answers "does this pad exist on this die", not "is this pad
 * free" — and the pads bonded to the SPI flash and the PSRAM very much exist. On
 * an unauthenticated POST /api/settings that difference is a remote brick rather
 * than a bad setting: the value reaches NVS before the reboot that applies it,
 * no driver will stop it (RMT, LEDC, I2S and I2C all log a warning about a
 * reserved pin and then mux it anyway), and a board that cannot read its own
 * flash cannot be talked out of it over the air — it needs USB and a boot
 * button. Nothing legitimate wants these pins, so there is no trade-off to
 * balance: they are refused outright.
 *
 * IDF already tracks them, but in the same global mask drivers use to record "I
 * own this pad now", so the mask only means flash/PSRAM for as long as no driver
 * has started. By the time the web server is up it also holds our own LED, I2S
 * and I2C pins, and validating against it live would reject the very pin map the
 * device is currently running on. Hence a snapshot, taken in settings_init()
 * which app_main calls ahead of every pin consumer.
 *
 * Seeded with the static supplement so we are never weaker than it, even if
 * this is somehow consulted before the snapshot. */
#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM_MODE_OCT
/* The snapshot has a hole on exactly this board. esp_mspi_pin_reserve() adds the
 * MSPI data lines only when the *flash* is octal (it reads the FLASH_TYPE
 * efuse), and here DIO flash sits next to OPI PSRAM, whose driver reserves only
 * its own CS. D4..DQS are driven on every PSRAM access yet absent from the mask,
 * so patch them in from the SoC's own pin header rather than a typed-out list.
 *
 * No classic-ESP32 equivalent on purpose: there the quad-PSRAM driver reserves
 * the full set at runtime and resolves CLK/CS from the chip package, which a
 * hardcoded list gets wrong in both directions — sdkconfig.es8388 legitimately
 * drives LEDs on GPIO 16/17 precisely because that board has no PSRAM. */
#define UNSAFE_PINS_STATIC \
    (BIT64(MSPI_IOMUX_PIN_NUM_D4) | BIT64(MSPI_IOMUX_PIN_NUM_D5) | \
     BIT64(MSPI_IOMUX_PIN_NUM_D6) | BIT64(MSPI_IOMUX_PIN_NUM_D7) | \
     BIT64(MSPI_IOMUX_PIN_NUM_DQS))
#else
#define UNSAFE_PINS_STATIC 0ULL
#endif

static uint64_t s_unsafe_pins = UNSAFE_PINS_STATIC;

/* Why the last settings_apply_json() refused, for the HTTP body. The whole
 * request is rejected atomically, so without naming the field the user sees
 * "HTTP 400" against a form with 24 pin boxes and no clue which one — and
 * loses anything else they typed, including a WiFi password the GET never
 * echoes back. Names the FIRST offender; the log lists them all. */
static char s_apply_error[96];

const char *settings_last_apply_error(void)
{
    return s_apply_error[0] ? s_apply_error : NULL;
}

static void apply_error_set(const char *field, int pin, const char *why)
{
    if (s_apply_error[0]) return;            /* keep the first, it is the one to fix */
    snprintf(s_apply_error, sizeof(s_apply_error), "%s = GPIO %d: %s", field, pin, why);
}

/* Snapshot IDF's reserved-pin mask while it still describes only the memory bus
 * and the target's missing pads — see the call site in settings_init(), which
 * MUST stay ahead of every driver init for this to mean what we want.
 *
 * Queried one pin at a time through the documented esp_gpio_is_reserved()
 * rather than in one shot. A single esp_gpio_reserve(0) returns the same word
 * in one call, but it gets there by exploiting that a fetch_or of nothing
 * happens to be a read — undocumented behaviour an IDF rewrite could change
 * without anyone noticing, in a guard whose failure mode is a remotely
 * triggered brick. 64 atomic loads, once, at boot. */
static void settings_snapshot_unsafe_pins(void)
{
    for (int i = 0; i < 64; i++) {
        if (esp_gpio_is_reserved(BIT64(i))) s_unsafe_pins |= BIT64(i);
    }
    ESP_LOGI(TAG, "unsafe GPIO mask 0x%016llx (flash/PSRAM + pads this chip lacks)",
             (unsigned long long)s_unsafe_pins);
}

/* NULL when the pin is safe to store, else a short reason fit for a log line.
 * -1 stays the "not wired" sentinel everywhere. */
static const char *pin_reject_reason(int v)
{
    if (v == -1) return NULL;
    /* > 63 first: the mask is 64 bits and BIT64 of anything larger is undefined.
     * Keeping GPIO_IS_VALID_GPIO as well means an IDF change that empties the
     * snapshot degrades to the old check instead of to no check. */
    if (v < 0 || v > 63 || !GPIO_IS_VALID_GPIO(v)) return "not a GPIO on this chip";
    if (s_unsafe_pins & BIT64(v)) return "wired to the SPI flash or PSRAM";
    return NULL;
}

/* Disarm stored pins we would refuse over HTTP.
 *
 * The CONFIG_* seed and any blob written by an older build never went through
 * that check, so a flash/PSRAM pin can already be latched before the first
 * request arrives. -1 is the right landing: the feature goes quiet instead of
 * fighting the memory bus, and since GET /api/settings then never hands back a
 * value its own POST would reject, the Settings page stays savable. */
static int settings_sanitize_pins(device_settings_t *s)
{
    struct { const char *name; int *pin; } fields[] = {
        { "led_data_pin",    &s->led_data_pin },
        { "led_clock_pin",   &s->led_clock_pin },
        { "i2s_bck_pin",     &s->i2s_bck_pin },
        { "i2s_ws_pin",      &s->i2s_ws_pin },
        { "i2s_data_pin",    &s->i2s_data_pin },
        { "i2s_mclk_pin",    &s->i2s_mclk_pin },
        { "i2s_din_pin",     &s->i2s_din_pin },
        { "amp_enable_pin",  &s->amp_enable_pin },
        { "codec_i2c_sda",   &s->codec_i2c_sda },
        { "codec_i2c_scl",   &s->codec_i2c_scl },
        { "codec_reset_pin", &s->codec_reset_pin },
        { "sd_cs",           &s->sd_cs },
        { "sd_mosi",         &s->sd_mosi },
        { "sd_miso",         &s->sd_miso },
        { "sd_clk",          &s->sd_clk },
        { "button_gpio",     &s->button_gpio },
    };
    int fixed = 0;

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const char *why = pin_reject_reason(*fields[i].pin);
        if (!why) continue;
        ESP_LOGE(TAG, "stored %s = GPIO %d is %s — forcing -1 (feature disabled)",
                 fields[i].name, *fields[i].pin, why);
        *fields[i].pin = -1;
        fixed++;
    }
    for (int k = 0; k < SETTINGS_LED_DIRECT_CHANNELS; k++) {
        const char *why = pin_reject_reason(s->led_direct_pins[k]);
        if (!why) continue;
        ESP_LOGE(TAG, "stored led_direct_pins[%d] = GPIO %d is %s — forcing -1",
                 k, s->led_direct_pins[k], why);
        s->led_direct_pins[k] = -1;
        fixed++;
    }
    return fixed;
}

/* ------------------------------------------------------------------ init ---- */

/* Warn at boot when two settings claim the same GPIO.
 *
 * This has bitten twice on the S3 board and both times presented as a baffling
 * hardware fault rather than a config error:
 *   - button_gpio defaulted to 5, which is the I2S BIT CLOCK there. Claiming it
 *     as a button input degraded BCLK just enough that the codec still locked
 *     its PLL but never latched a sample: perfect clocks, total silence.
 *   - sd_cs defaulted to 5 as well, for the same reason (the SD pin Kconfig
 *     symbols did not exist while the SD feature was off, so the seed fell back
 *     to classic-board pins).
 *
 * A conflict is not always fatal — an inactive backend's pins are harmless — so
 * this warns rather than refusing to boot. But it turns a multi-hour hardware
 * hunt into one line in the log.
 */
static void settings_warn_pin_conflicts(void)
{
    const device_settings_t *s = &s_settings;
    struct { const char *name; int pin; bool active; } claims[] = {
        { "i2s_bck",      s->i2s_bck_pin,   true },
        { "i2s_ws",       s->i2s_ws_pin,    true },
        { "i2s_data",     s->i2s_data_pin,  true },
        { "i2s_mclk",     s->i2s_mclk_pin,  true },
        { "i2s_din",      s->i2s_din_pin,   true },
        { "codec_i2c_sda",s->codec_i2c_sda, s->audio_codec != AUDIO_CODEC_NONE },
        { "codec_i2c_scl",s->codec_i2c_scl, s->audio_codec != AUDIO_CODEC_NONE },
        { "codec_reset",  s->codec_reset_pin, s->audio_codec != AUDIO_CODEC_NONE },
        { "amp_enable",   s->amp_enable_pin, true },
        { "button",       s->button_gpio,   true },
        { "led_data",     s->led_data_pin,
              s->led_backend == LED_BACKEND_NEOPIXEL || s->led_backend == LED_BACKEND_DOTSTAR },
        { "led_clock",    s->led_clock_pin, s->led_backend == LED_BACKEND_DOTSTAR },
#if CONFIG_BG_SDCARD_ENABLED
        { "sd_cs",        s->sd_cs,   true },
        { "sd_mosi",      s->sd_mosi, true },
        { "sd_miso",      s->sd_miso, true },
        { "sd_clk",       s->sd_clk,  true },
#endif
    };
    const size_t n = sizeof(claims) / sizeof(claims[0]);

    for (size_t i = 0; i < n; i++) {
        if (!claims[i].active || claims[i].pin < 0) continue;
        for (size_t j = i + 1; j < n; j++) {
            if (!claims[j].active || claims[j].pin != claims[i].pin) continue;
            ESP_LOGE(TAG, "GPIO %d claimed by BOTH '%s' and '%s' — peripherals "
                     "will interfere; fix one in Settings",
                     claims[i].pin, claims[i].name, claims[j].name);
        }
    }

    /* DIRECT LED pins only matter when that backend is the active one. */
    if (s->led_backend == LED_BACKEND_DIRECT) {
        for (int k = 0; k < SETTINGS_LED_DIRECT_CHANNELS; k++) {
            int p = s->led_direct_pins[k];
            if (p < 0) continue;
            for (size_t i = 0; i < n; i++) {
                if (claims[i].active && claims[i].pin == p) {
                    ESP_LOGE(TAG, "GPIO %d claimed by BOTH 'led_direct[%d]' and "
                             "'%s'", p, k, claims[i].name);
                }
            }
        }
    }
}

esp_err_t settings_init(void)
{
    bool loaded = false;

    /* Before anything can consult it, and before any driver has polluted it. */
    settings_snapshot_unsafe_pins();

    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        size_t needed = 0;
        err = nvs_get_blob(h, SETTINGS_KEY, NULL, &needed);
        if (err == ESP_OK && needed == sizeof(uint16_t) + sizeof(device_settings_t)) {
            uint8_t *buf = malloc(needed);
            if (buf && nvs_get_blob(h, SETTINGS_KEY, buf, &needed) == ESP_OK) {
                uint16_t ver = 0;
                memcpy(&ver, buf, sizeof(ver));
                if (ver == SETTINGS_VERSION) {
                    memcpy(&s_settings, buf + sizeof(uint16_t), sizeof(device_settings_t));
                    loaded = true;
                    ESP_LOGI(TAG, "loaded settings blob (v%u)", (unsigned)ver);
                } else {
                    ESP_LOGW(TAG, "settings version mismatch (stored %u != %u) — reseeding",
                             (unsigned)ver, (unsigned)SETTINGS_VERSION);
                }
            }
            free(buf);
        }
        nvs_close(h);
    } else {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s — using defaults in RAM only",
                 SETTINGS_NS, esp_err_to_name(err));
    }

    if (!loaded) {
        settings_seed_defaults(&s_settings);
        esp_err_t perr = settings_persist();
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "could not persist seeded settings: %s", esp_err_to_name(perr));
        } else {
            ESP_LOGI(TAG, "seeded settings from CONFIG_* defaults (v%u)",
                     (unsigned)SETTINGS_VERSION);
        }
    }

    /* Write the corrected map straight back, so a value that predates this check
     * stops reappearing on every boot. */
    if (settings_sanitize_pins(&s_settings) > 0) {
        esp_err_t perr = settings_persist();
        if (perr != ESP_OK) {
            ESP_LOGE(TAG, "could not persist sanitized pins: %s — the stored blob "
                     "still holds them, RAM does not", esp_err_to_name(perr));
        }
    }

    settings_warn_pin_conflicts();
    return ESP_OK;
}

const device_settings_t *settings_get(void)
{
    return &s_settings;
}

/* ------------------------------------------------------------- JSON apply --- */

/* Validate against the SoC's real GPIO map and the memory bus (see
 * pin_reject_reason) rather than a hardcoded ceiling. Clamping is the wrong
 * remedy for a pin we won't accept: silently turning a requested GPIO47 into
 * GPIO39 drives a completely different pin, and turning a flash pin into its
 * nearest legal neighbour hides the fact that someone asked for a flash pin.
 *
 * Returns false and leaves the caller's field untouched; the caller is expected
 * to fail the whole request, because a half-applied pin map is how you end up
 * debugging a board that is wired to neither the old config nor the new one. */
static bool apply_pin(const cJSON *root, const char *key, int *field)
    __attribute__((warn_unused_result));
static bool apply_pin(const cJSON *root, const char *key, int *field)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) {
        /* valueint, not valuedouble: cJSON clamps valueint to INT_MIN/INT_MAX,
         * whereas casting an out-of-range double to int is undefined. The range
         * check below would catch the saturated result either way, but a guard
         * against bricking should not rest on what UB happens to do here. */
        int v = it->valueint;
        const char *why = pin_reject_reason(v);
        if (why) {
            ESP_LOGE(TAG, "refusing %s = GPIO %d: %s — keeping %d",
                     key, v, why, *field);
            apply_error_set(key, v, why);
            return false;
        }
        *field = v;
    }
    return true;
}

static void apply_int(const cJSON *root, const char *key, int *field, int lo, int hi)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) {
        int v = (int)it->valuedouble;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        *field = v;
    }
}

static void apply_str(const cJSON *root, const char *key, char *field, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strlcpy(field, it->valuestring, cap);
    }
}

esp_err_t settings_apply_json(const char *body, int len)
{
    s_apply_error[0] = '\0';          /* per-request; stale text would misdirect */

    if (!body || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_ParseWithLength(body, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "settings_apply_json: parse error");
        return ESP_ERR_INVALID_ARG;
    }

    // Work on a copy so a malformed partial update can't leave us half-applied.
    device_settings_t cur = s_settings;

    // Set by any refused pin. Accumulated rather than returned early so one
    // request's log names every bad field, not just the first.
    bool pins_ok = true;

    // LED backend (string -> enum). Phase 3: validate the requested backend is
    // actually compiled in (CONFIG_LED_SUPPORT_*); ignore + warn otherwise.
    const cJSON *be = cJSON_GetObjectItemCaseSensitive(root, "led_backend");
    if (cJSON_IsString(be) && be->valuestring) {
        if (!strcmp(be->valuestring, "neopixel")) {
#if CONFIG_LED_SUPPORT_NEOPIXEL
            cur.led_backend = LED_BACKEND_NEOPIXEL;
#else
            ESP_LOGW(TAG, "led_backend 'neopixel' not compiled in — ignoring");
#endif
        } else if (!strcmp(be->valuestring, "dotstar")) {
#if CONFIG_LED_SUPPORT_DOTSTAR
            cur.led_backend = LED_BACKEND_DOTSTAR;
#else
            ESP_LOGW(TAG, "led_backend 'dotstar' not compiled in — ignoring");
#endif
        } else if (!strcmp(be->valuestring, "direct")) {
#if CONFIG_LED_SUPPORT_DIRECT
            cur.led_backend = LED_BACKEND_DIRECT;
#else
            ESP_LOGW(TAG, "led_backend 'direct' not compiled in — ignoring");
#endif
        }
    }

    pins_ok &= apply_pin(root, "led_data_pin", &cur.led_data_pin);
    pins_ok &= apply_pin(root, "led_clock_pin", &cur.led_clock_pin);
    apply_int(root, "led_count", &cur.led_count, 1, 1000);
    apply_str(root, "led_channel_map", cur.led_channel_map, sizeof(cur.led_channel_map));
    apply_int(root, "led_grid_width", &cur.led_grid_width, 1, 64);
    apply_int(root, "led_grid_height", &cur.led_grid_height, 1, 64);

    const cJSON *dp = cJSON_GetObjectItemCaseSensitive(root, "led_direct_pins");
    if (cJSON_IsArray(dp)) {
        int n = cJSON_GetArraySize(dp);
        if (n > SETTINGS_LED_DIRECT_CHANNELS) n = SETTINGS_LED_DIRECT_CHANNELS;
        for (int i = 0; i < n; i++) {
            const cJSON *e = cJSON_GetArrayItem(dp, i);
            if (!cJSON_IsNumber(e)) {
                /* Fail rather than skip. Every other rejection path fails the
                 * request, and a silent skip here would answer 200 with the
                 * unchanged value echoed back — exactly the indistinguishable-
                 * from-success behaviour this change exists to remove. */
                ESP_LOGE(TAG, "refusing led_direct_pins[%d]: not a number", i);
                apply_error_set("led_direct_pins", -1, "not a number");
                pins_ok = false;
                continue;
            }
            int v = e->valueint;
            /* The array is the one pin field that doesn't go through apply_pin,
             * so the check has to be repeated here. */
            const char *why = pin_reject_reason(v);
            if (why) {
                ESP_LOGE(TAG, "refusing led_direct_pins[%d] = GPIO %d: %s — keeping %d",
                         i, v, why, cur.led_direct_pins[i]);
                apply_error_set("led_direct_pins", v, why);
                pins_ok = false;
                continue;
            }
            cur.led_direct_pins[i] = v;
        }
    }
    apply_int(root, "led_direct_active_low_mask", &cur.led_direct_active_low_mask, 0, 255);
    apply_int(root, "led_dotstar_spi_clock_hz", &cur.led_dotstar_spi_clock_hz, 100000, 40000000);

    pins_ok &= apply_pin(root, "i2s_bck_pin", &cur.i2s_bck_pin);
    pins_ok &= apply_pin(root, "i2s_ws_pin", &cur.i2s_ws_pin);
    pins_ok &= apply_pin(root, "i2s_data_pin", &cur.i2s_data_pin);
    pins_ok &= apply_pin(root, "i2s_mclk_pin", &cur.i2s_mclk_pin);
    pins_ok &= apply_pin(root, "i2s_din_pin", &cur.i2s_din_pin);
    pins_ok &= apply_pin(root, "amp_enable_pin", &cur.amp_enable_pin);

    // Audio codec (string -> enum). Phase 3: "none" is always valid; ac101 /
    // es8388 are accepted only when compiled in (CONFIG_AUDIO_SUPPORT_*).
    const cJSON *cd = cJSON_GetObjectItemCaseSensitive(root, "audio_codec");
    if (cJSON_IsString(cd) && cd->valuestring) {
        if (!strcmp(cd->valuestring, "none")) {
            cur.audio_codec = AUDIO_CODEC_NONE;
        } else if (!strcmp(cd->valuestring, "ac101")) {
#if CONFIG_AUDIO_SUPPORT_AC101
            cur.audio_codec = AUDIO_CODEC_AC101;
#else
            ESP_LOGW(TAG, "audio_codec 'ac101' not compiled in — ignoring");
#endif
        } else if (!strcmp(cd->valuestring, "es8388")) {
#if CONFIG_AUDIO_SUPPORT_ES8388
            cur.audio_codec = AUDIO_CODEC_ES8388;
#else
            ESP_LOGW(TAG, "audio_codec 'es8388' not compiled in — ignoring");
#endif
        } else if (!strcmp(cd->valuestring, "tlv320dac3101")) {
#if CONFIG_AUDIO_SUPPORT_TLV320DAC3101
            cur.audio_codec = AUDIO_CODEC_TLV320DAC3101;
#else
            ESP_LOGW(TAG, "audio_codec 'tlv320dac3101' not compiled in — ignoring");
#endif
        }
    }
    apply_int(root, "codec_i2c_port", &cur.codec_i2c_port, 0, 1);
    pins_ok &= apply_pin(root, "codec_i2c_sda", &cur.codec_i2c_sda);
    pins_ok &= apply_pin(root, "codec_i2c_scl", &cur.codec_i2c_scl);
    pins_ok &= apply_pin(root, "codec_reset_pin", &cur.codec_reset_pin);
    apply_int(root, "codec_i2c_freq_hz", &cur.codec_i2c_freq_hz, 10000, 1000000);

    pins_ok &= apply_pin(root, "sd_cs", &cur.sd_cs);
    pins_ok &= apply_pin(root, "sd_mosi", &cur.sd_mosi);
    pins_ok &= apply_pin(root, "sd_miso", &cur.sd_miso);
    pins_ok &= apply_pin(root, "sd_clk", &cur.sd_clk);

    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(root, "default_volume");
    if (cJSON_IsNumber(vol)) {
        float v = (float)vol->valuedouble;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        cur.default_volume = v;
    }
    // Up to 200%: >100 is a genuine gain boost (the mix is divided by
    // NUM_AUDIO_CHANNELS=16 for headroom, so output is quiet on some headphones).
    // Peaks past full-scale are caught by the ±1.0 clamp in audio_test.c.
    apply_int(root, "audio_max_volume", &cur.audio_max_volume, 0, 200);
    pins_ok &= apply_pin(root, "button_gpio", &cur.button_gpio);

    apply_str(root, "generator_url", cur.generator_url, sizeof(cur.generator_url));

    // WiFi: SSID updates whenever present. Password updates ONLY when a
    // non-empty value is supplied, so saving the form without re-typing the
    // password keeps the stored one (the GET never echoes it back).
    apply_str(root, "wifi_ssid", cur.wifi_ssid, sizeof(cur.wifi_ssid));
    const cJSON *wp = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
    if (cJSON_IsString(wp) && wp->valuestring && wp->valuestring[0] != '\0') {
        strlcpy(cur.wifi_password, wp->valuestring, sizeof(cur.wifi_password));
    }

    // mDNS hostname: accept then sanitize to a valid DNS label ([a-z0-9-],
    // lowercased, no leading/trailing '-'); empty result falls back to the
    // default so mdns_hostname_set() never gets a broken name.
    const cJSON *mh = cJSON_GetObjectItemCaseSensitive(root, "mdns_hostname");
    if (cJSON_IsString(mh) && mh->valuestring) {
        char clean[sizeof(cur.mdns_hostname)];
        size_t o = 0;
        for (const char *p = mh->valuestring; *p && o < sizeof(clean) - 1; p++) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                (c == '-' && o > 0)) {      // no leading hyphen
                clean[o++] = c;
            }
        }
        while (o > 0 && clean[o - 1] == '-') o--;   // trim trailing hyphens
        clean[o] = '\0';
        if (o == 0) strlcpy(clean, "esp32-ave", sizeof(clean));
        strlcpy(cur.mdns_hostname, clean, sizeof(cur.mdns_hostname));
    }

    // Reports: only accept the known destinations.
    const cJSON *rs = cJSON_GetObjectItemCaseSensitive(root, "report_storage");
    if (cJSON_IsString(rs) && rs->valuestring) {
        if (!strcmp(rs->valuestring, "none") || !strcmp(rs->valuestring, "local") ||
            !strcmp(rs->valuestring, "spiffs")) {
            strlcpy(cur.report_storage, rs->valuestring, sizeof(cur.report_storage));
        }
    }

    cJSON_Delete(root);

    /* A refused pin fails the request instead of being quietly dropped: the old
     * behaviour answered 200 with the unchanged value in the body, which looks
     * identical to success unless you diff it, and on a GPIO this is the one
     * mistake worth being loud about. INVALID_ARG is what the caller already
     * turns into a 400. */
    if (!pins_ok) {
        ESP_LOGE(TAG, "settings not applied — one or more GPIOs were refused "
                 "(see above); nothing was written");
        return ESP_ERR_INVALID_ARG;
    }

    s_settings = cur;
    return settings_persist();
}

/* ------------------------------------------------------------ JSON build ---- */

static const char *led_backend_str(led_backend_t b)
{
    switch (b) {
        case LED_BACKEND_DOTSTAR: return "dotstar";
        case LED_BACKEND_DIRECT:  return "direct";
        case LED_BACKEND_NEOPIXEL:
        default:                  return "neopixel";
    }
}

static const char *audio_codec_str(audio_codec_t c)
{
    switch (c) {
        case AUDIO_CODEC_AC101:          return "ac101";
        case AUDIO_CODEC_ES8388:         return "es8388";
        case AUDIO_CODEC_TLV320DAC3101:  return "tlv320dac3101";
        case AUDIO_CODEC_NONE:
        default:                         return "none";
    }
}

int settings_to_json(char *buf, int cap)
{
    if (!buf || cap <= 0) return -1;

    const device_settings_t *s = &s_settings;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON_AddStringToObject(root, "led_backend", led_backend_str(s->led_backend));
    cJSON_AddNumberToObject(root, "led_data_pin", s->led_data_pin);
    cJSON_AddNumberToObject(root, "led_clock_pin", s->led_clock_pin);
    cJSON_AddNumberToObject(root, "led_count", s->led_count);
    cJSON_AddStringToObject(root, "led_channel_map", s->led_channel_map);
    cJSON_AddNumberToObject(root, "led_grid_width", s->led_grid_width);
    cJSON_AddNumberToObject(root, "led_grid_height", s->led_grid_height);

    cJSON *dp = cJSON_AddArrayToObject(root, "led_direct_pins");
    if (dp) {
        for (int i = 0; i < SETTINGS_LED_DIRECT_CHANNELS; i++) {
            cJSON_AddItemToArray(dp, cJSON_CreateNumber(s->led_direct_pins[i]));
        }
    }
    cJSON_AddNumberToObject(root, "led_direct_active_low_mask", s->led_direct_active_low_mask);
    cJSON_AddNumberToObject(root, "led_dotstar_spi_clock_hz", s->led_dotstar_spi_clock_hz);

    cJSON_AddNumberToObject(root, "i2s_bck_pin", s->i2s_bck_pin);
    cJSON_AddNumberToObject(root, "i2s_ws_pin", s->i2s_ws_pin);
    cJSON_AddNumberToObject(root, "i2s_data_pin", s->i2s_data_pin);
    cJSON_AddNumberToObject(root, "i2s_mclk_pin", s->i2s_mclk_pin);
    cJSON_AddNumberToObject(root, "i2s_din_pin", s->i2s_din_pin);
    cJSON_AddNumberToObject(root, "amp_enable_pin", s->amp_enable_pin);

    cJSON_AddStringToObject(root, "audio_codec", audio_codec_str(s->audio_codec));
    cJSON_AddNumberToObject(root, "codec_i2c_port", s->codec_i2c_port);
    cJSON_AddNumberToObject(root, "codec_i2c_sda", s->codec_i2c_sda);
    cJSON_AddNumberToObject(root, "codec_i2c_scl", s->codec_i2c_scl);
    cJSON_AddNumberToObject(root, "codec_i2c_freq_hz", s->codec_i2c_freq_hz);
    cJSON_AddNumberToObject(root, "codec_reset_pin", s->codec_reset_pin);

    cJSON_AddNumberToObject(root, "sd_cs", s->sd_cs);
    cJSON_AddNumberToObject(root, "sd_mosi", s->sd_mosi);
    cJSON_AddNumberToObject(root, "sd_miso", s->sd_miso);
    cJSON_AddNumberToObject(root, "sd_clk", s->sd_clk);

    cJSON_AddNumberToObject(root, "default_volume", s->default_volume);
    cJSON_AddNumberToObject(root, "audio_max_volume", s->audio_max_volume);
    cJSON_AddNumberToObject(root, "button_gpio", s->button_gpio);
    cJSON_AddStringToObject(root, "generator_url", s->generator_url);

    // WiFi: expose the SSID but NEVER the stored password. A boolean tells the
    // UI whether a password is set so it can show "unchanged" vs "not set".
    cJSON_AddStringToObject(root, "wifi_ssid", s->wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_password_set", s->wifi_password[0] != '\0');

    cJSON_AddStringToObject(root, "mdns_hostname", s->mdns_hostname);

    cJSON_AddStringToObject(root, "report_storage", s->report_storage);

    // Capabilities (Phase 3): which backends/codecs are compiled into THIS
    // firmware. The web UI uses these to enable/grey the selector options.
    // Codec list always includes "none" (passive DAC needs no driver).
    cJSON *supported = cJSON_AddObjectToObject(root, "supported");
    if (supported) {
        cJSON *led = cJSON_AddArrayToObject(supported, "led");
        if (led) {
#if CONFIG_LED_SUPPORT_NEOPIXEL
            cJSON_AddItemToArray(led, cJSON_CreateString("neopixel"));
#endif
#if CONFIG_LED_SUPPORT_DOTSTAR
            cJSON_AddItemToArray(led, cJSON_CreateString("dotstar"));
#endif
#if CONFIG_LED_SUPPORT_DIRECT
            cJSON_AddItemToArray(led, cJSON_CreateString("direct"));
#endif
        }
        cJSON *codec = cJSON_AddArrayToObject(supported, "codec");
        if (codec) {
            cJSON_AddItemToArray(codec, cJSON_CreateString("none"));
#if CONFIG_AUDIO_SUPPORT_AC101
            cJSON_AddItemToArray(codec, cJSON_CreateString("ac101"));
#endif
#if CONFIG_AUDIO_SUPPORT_ES8388
            cJSON_AddItemToArray(codec, cJSON_CreateString("es8388"));
#endif
#if CONFIG_AUDIO_SUPPORT_TLV320DAC3101
            cJSON_AddItemToArray(codec, cJSON_CreateString("tlv320dac3101"));
#endif
        }
        // SD card support is a compile-time feature (CONFIG_BG_SDCARD_ENABLED);
        // the UI hides the SD pin group entirely when it isn't built in.
#if CONFIG_BG_SDCARD_ENABLED
        cJSON_AddBoolToObject(supported, "sd", true);
#else
        cJSON_AddBoolToObject(supported, "sd", false);
#endif
    }

    bool ok = cJSON_PrintPreallocated(root, buf, cap, false);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "settings_to_json: buffer too small (cap=%d)", cap);
        return -1;
    }
    return (int)strlen(buf);
}

/* --------------------------------------------------------------- reset ------ */

esp_err_t settings_reset_defaults(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_key(h, SETTINGS_KEY);  // ignore ESP_ERR_NVS_NOT_FOUND
        nvs_commit(h);
        nvs_close(h);
    }
    settings_seed_defaults(&s_settings);
    /* The seed reads CONFIG_* straight into the struct, so a board config that
     * names a flash/PSRAM pin would otherwise reach NVS through this endpoint
     * without ever passing the JSON check. */
    settings_sanitize_pins(&s_settings);
    return settings_persist();
}
