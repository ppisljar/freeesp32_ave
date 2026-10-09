#ifndef SETTINGS_H
#define SETTINGS_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime device settings (Plan: runtime_settings_plan.md, Phase 1).
 *
 * Persisted as a versioned blob in NVS namespace "devcfg", key "blob":
 *   [ uint16_t SETTINGS_VERSION ][ device_settings_t ]
 * On a missing key or a version mismatch the struct is (re)seeded from the
 * compile-time CONFIG_* defaults and written back.
 *
 * Phase 1 migrates plain value reads (pins, counts, volume, URL) to the runtime
 * store. led_backend / audio_codec are stored and serialized but NOT branched on
 * at runtime yet — the compile-time backend selection (#if CONFIG_LED_TYPE_* /
 * #if CONFIG_AUDIO_DRIVER_*) is unchanged. Runtime backend selection is Phase 3.
 */

#define SETTINGS_LED_DIRECT_CHANNELS 8

typedef enum {
    LED_BACKEND_NEOPIXEL = 0,
    LED_BACKEND_DOTSTAR  = 1,
    LED_BACKEND_DIRECT   = 2,
} led_backend_t;

typedef enum {
    AUDIO_CODEC_NONE           = 0,
    AUDIO_CODEC_AC101          = 1,
    AUDIO_CODEC_ES8388         = 2,
    AUDIO_CODEC_TLV320DAC3101  = 3,
} audio_codec_t;

typedef struct {
    // LED
    led_backend_t led_backend;          // active backend (runtime select; acted on in Phase 3)
    int  led_data_pin;                  // neopixel/dotstar data
    int  led_clock_pin;                 // dotstar clock
    int  led_count;
    char led_channel_map[256];
    int  led_grid_width, led_grid_height;
    int  led_direct_pins[SETTINGS_LED_DIRECT_CHANNELS];
    int  led_direct_active_low_mask;
    int  led_dotstar_spi_clock_hz;
    // Audio I2S output
    int  i2s_bck_pin, i2s_ws_pin, i2s_data_pin, i2s_mclk_pin, i2s_din_pin, amp_enable_pin;
    // Audio codec
    audio_codec_t audio_codec;          // active codec (runtime select; acted on in Phase 3)
    int  codec_i2c_port, codec_i2c_sda, codec_i2c_scl, codec_i2c_freq_hz;
    int  codec_reset_pin;               // codec ~RESET (active low); -1 = not wired
    // SD card (BG audio)
    int  sd_cs, sd_mosi, sd_miso, sd_clk;
    // Audio misc
    float default_volume;               // 0..1
    int   audio_max_volume;             // master output cap 0..100 (%); scales all audio
    // Controls
    int   button_gpio;                  // momentary snapshot/stop button GPIO; -1 = none
    // Network
    char generator_url[128];
    char wifi_ssid[33];                 // Phase 2 (seeded, not yet in JSON/UI)
    char wifi_password[65];             // Phase 2 (seeded, not yet in JSON/UI)
    char mdns_hostname[32];             // label before ".local" (sanitized to [a-z0-9-]); empty => "esp32-ave"
    // Reports: where the web app persists session reports, in addition to
    // always uploading to the generator when generator_url is set.
    char report_storage[8];             // "none" | "local" | "spiffs"
} device_settings_t;

/** nvs_open; load the blob or seed from CONFIG_* defaults. */
esp_err_t settings_init(void);

/** Read-only pointer to the live settings struct (always valid after init). */
const device_settings_t *settings_get(void);

/** Parse a JSON body (cJSON), validate, store onto the live struct, commit to NVS.
 *  Unknown keys are ignored; omitted keys keep their current value. */
esp_err_t settings_apply_json(const char *body, int len);

/** Serialize the current settings to JSON into buf (cap bytes). Returns the
 *  number of bytes written (excluding NUL), or -1 on error. */
int settings_to_json(char *buf, int cap);

/** Erase the namespace and reseed from CONFIG_* defaults. */
esp_err_t settings_reset_defaults(void);

#ifdef __cplusplus
}
#endif

#endif // SETTINGS_H
