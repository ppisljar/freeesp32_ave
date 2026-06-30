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
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "settings";

#define SETTINGS_VERSION   ((uint16_t)5)   // v5: button_gpio added
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
    s->button_gpio = 5;

    // ---- Network
#ifdef CONFIG_GENERATOR_SERVER_URL
    strlcpy(s->generator_url, CONFIG_GENERATOR_SERVER_URL, sizeof(s->generator_url));
#else
    strlcpy(s->generator_url, "http://192.168.1.100:8000", sizeof(s->generator_url));
#endif

    // ---- WiFi credentials (Phase 2): seed from the former hardcoded defaults.
    strlcpy(s->wifi_ssid, DEFAULT_WIFI_SSID, sizeof(s->wifi_ssid));
    strlcpy(s->wifi_password, DEFAULT_WIFI_PASSWORD, sizeof(s->wifi_password));

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

/* ------------------------------------------------------------------ init ---- */

esp_err_t settings_init(void)
{
    bool loaded = false;

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
    return ESP_OK;
}

const device_settings_t *settings_get(void)
{
    return &s_settings;
}

/* ------------------------------------------------------------- JSON apply --- */

static int clamp_pin(int v)
{
    if (v < -1) return -1;
    if (v > 39) return 39;
    return v;
}

static void apply_pin(const cJSON *root, const char *key, int *field)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) {
        *field = clamp_pin((int)it->valuedouble);
    }
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

    apply_pin(root, "led_data_pin", &cur.led_data_pin);
    apply_pin(root, "led_clock_pin", &cur.led_clock_pin);
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
            if (cJSON_IsNumber(e)) cur.led_direct_pins[i] = clamp_pin((int)e->valuedouble);
        }
    }
    apply_int(root, "led_direct_active_low_mask", &cur.led_direct_active_low_mask, 0, 255);
    apply_int(root, "led_dotstar_spi_clock_hz", &cur.led_dotstar_spi_clock_hz, 100000, 40000000);

    apply_pin(root, "i2s_bck_pin", &cur.i2s_bck_pin);
    apply_pin(root, "i2s_ws_pin", &cur.i2s_ws_pin);
    apply_pin(root, "i2s_data_pin", &cur.i2s_data_pin);
    apply_pin(root, "i2s_mclk_pin", &cur.i2s_mclk_pin);
    apply_pin(root, "i2s_din_pin", &cur.i2s_din_pin);
    apply_pin(root, "amp_enable_pin", &cur.amp_enable_pin);

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
        }
    }
    apply_int(root, "codec_i2c_port", &cur.codec_i2c_port, 0, 1);
    apply_pin(root, "codec_i2c_sda", &cur.codec_i2c_sda);
    apply_pin(root, "codec_i2c_scl", &cur.codec_i2c_scl);
    apply_int(root, "codec_i2c_freq_hz", &cur.codec_i2c_freq_hz, 10000, 1000000);

    apply_pin(root, "sd_cs", &cur.sd_cs);
    apply_pin(root, "sd_mosi", &cur.sd_mosi);
    apply_pin(root, "sd_miso", &cur.sd_miso);
    apply_pin(root, "sd_clk", &cur.sd_clk);

    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(root, "default_volume");
    if (cJSON_IsNumber(vol)) {
        float v = (float)vol->valuedouble;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        cur.default_volume = v;
    }
    apply_int(root, "audio_max_volume", &cur.audio_max_volume, 0, 100);
    apply_pin(root, "button_gpio", &cur.button_gpio);

    apply_str(root, "generator_url", cur.generator_url, sizeof(cur.generator_url));

    // WiFi: SSID updates whenever present. Password updates ONLY when a
    // non-empty value is supplied, so saving the form without re-typing the
    // password keeps the stored one (the GET never echoes it back).
    apply_str(root, "wifi_ssid", cur.wifi_ssid, sizeof(cur.wifi_ssid));
    const cJSON *wp = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
    if (cJSON_IsString(wp) && wp->valuestring && wp->valuestring[0] != '\0') {
        strlcpy(cur.wifi_password, wp->valuestring, sizeof(cur.wifi_password));
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
        case AUDIO_CODEC_AC101:  return "ac101";
        case AUDIO_CODEC_ES8388: return "es8388";
        case AUDIO_CODEC_NONE:
        default:                 return "none";
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
    return settings_persist();
}
