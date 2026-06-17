#ifndef LED_STRIP_H
#define LED_STRIP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <driver/rmt_tx.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <driver/ledc.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Number of logical LED channels supported by the DIRECT backend.
 *
 * The ESP32 (classic) LEDC peripheral has exactly 8 low-speed channels
 * (LEDC_CHANNEL_0 through LEDC_CHANNEL_7), so 8 is the hardware ceiling for
 * DIRECT mode on this chip.
 *
 * This macro is defined here (at the strip layer, the lower-level component)
 * rather than in led_matrix_example.h so that both led_strip.c and
 * led_matrix_example.c can see it without a layering inversion.  Files that
 * include led_strip.h (including led_matrix_example.h transitively) get it
 * automatically.
 */
#define NUM_LED_CHANNELS 8

/**
 * @brief LED Strip Control Component
 *
 * Provides a unified API for three physical LED backends:
 *
 *  - NEOPIXEL  — addressable single-wire strip (WS2812 / SK6812) driven via RMT.
 *  - DOTSTAR   — addressable two-wire strip (APA102) driven via hardware SPI (SPI3_HOST).
 *  - DIRECT    — NUM_LED_CHANNELS (8) discrete LEDs driven by LEDC PWM, one GPIO per
 *                logical channel.
 *
 * The backend is selected at compile time through menuconfig (CONFIG_LED_TYPE_*) and
 * cannot be changed at runtime.
 *
 * Per-pixel effects (set_pixel_rgb, set_pixel_color, get_pixel_color, set_all,
 * vu_meter, spectrum) return ESP_ERR_NOT_SUPPORTED when the handle was
 * initialised with LED_STRIP_BACKEND_DIRECT because direct mode has no concept
 * of individually addressable pixels — it exposes exactly NUM_LED_CHANNELS logical
 * channels.
 *
 * Use led_strip_set_channel() as the primary write API; it is portable across
 * all three backends.  In DIRECT mode the R/G/B arguments are silently ignored
 * and only `brightness` drives the LEDC duty cycle.  This makes a single .led
 * timeline file portable across all three backends.
 */

/**
 * @brief Hardware backend selector.
 *
 * LED_STRIP_BACKEND_NEOPIXEL (0) — single-wire addressable strip via RMT.
 * LED_STRIP_BACKEND_DOTSTAR  (1) — two-wire (CLK+DATA) APA102 strip via SPI3.
 * LED_STRIP_BACKEND_DIRECT   (2) — NUM_LED_CHANNELS (8) discrete GPIOs driven
 *                                   by LEDC PWM.
 *
 * The value LED_STRIP_BACKEND_FROM_MENUCONFIG (-1 cast to the enum) is a
 * sentinel used internally by LED_STRIP_CONFIG_FROM_MENUCONFIG; callers must
 * not read or store this value after led_strip_init() returns.
 */
typedef enum {
    LED_STRIP_BACKEND_NEOPIXEL         = 0,
    LED_STRIP_BACKEND_DOTSTAR          = 1,
    LED_STRIP_BACKEND_DIRECT           = 2,
    LED_STRIP_BACKEND_FROM_MENUCONFIG  = -1,  /**< Sentinel: read from CONFIG_LED_TYPE_* */
} led_strip_backend_t;

/**
 * @brief RGB colour tuple (one pixel / one logical channel colour).
 */
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color_t;

/**
 * @brief Internal LED strip state (opaque to callers beyond led_strip.c).
 *
 * Fields are public only because callers in led_matrix_example.c need to pass
 * the pointer around.  Do NOT access fields directly — use the API functions.
 */
typedef struct {
    /* --- Common fields --- */
    led_strip_backend_t backend;     /**< Active backend selected at init time */
    uint32_t length;                 /**< Total pixel count (0 for DIRECT mode) */
    gpio_num_t gpio_pin;             /**< Data / single-wire GPIO (NEOPIXEL/DOTSTAR) */

    led_color_t *working_buffer;     /**< Pixel staging buffer (addressable backends) */
    led_color_t *display_buffer;     /**< Last-refreshed pixel state (addressable backends) */

    /**
     * Per-pixel channel map (addressable backends only).
     * channel_map[i] == 0..3 indicates which logical channel owns pixel i.
     * NULL for DIRECT mode.  Length == length.
     */
    uint8_t *channel_map;

    SemaphoreHandle_t access_mutex;
    bool initialized;

    /* --- NEOPIXEL backend fields --- */
    rmt_channel_handle_t rmt_tx_channel;  /**< ESP-IDF 5.x RMT TX handle */
    rmt_encoder_handle_t rmt_encoder;     /**< RMT copy encoder */

    /** Pre-allocated RMT symbol buffer (avoids malloc/free per refresh). */
    rmt_symbol_word_t *symbol_buffer;
    size_t symbol_buffer_size;

    /* --- DOTSTAR (APA102) backend fields --- */
    spi_device_handle_t spi_device;   /**< SPI3_HOST device handle */
    uint8_t *dma_buffer;              /**< Pre-allocated DMA-capable frame buffer */
    size_t dma_buffer_size;           /**< Total byte size of dma_buffer */

    /* --- DIRECT (LEDC PWM) backend fields --- */
    ledc_channel_t ledc_channels[NUM_LED_CHANNELS];          /**< LEDC channel numbers for ch 0..7 */
    uint8_t direct_channel_brightness[NUM_LED_CHANNELS];     /**< Current brightness (0-100) per channel */
    gpio_num_t direct_pins[NUM_LED_CHANNELS];                /**< GPIO per logical channel */
} led_strip_handle_t;

/**
 * @brief Configuration passed to led_strip_init().
 *
 * For the compile-time-default configuration derived from menuconfig, use the
 * LED_STRIP_CONFIG_FROM_MENUCONFIG macro instead of filling this struct by hand.
 */
typedef struct {
    /**
     * Backend to initialise.  Set to LED_STRIP_BACKEND_FROM_MENUCONFIG (via
     * LED_STRIP_CONFIG_FROM_MENUCONFIG) to auto-detect from CONFIG_LED_TYPE_*.
     */
    led_strip_backend_t backend;

    /** Number of pixels.  Ignored for DIRECT mode (always 4 logical channels). */
    uint32_t length;

    /** Data GPIO pin (NEOPIXEL) or MOSI pin (DOTSTAR).  Ignored for DIRECT. */
    gpio_num_t gpio_pin;

    /** Clock GPIO pin (DOTSTAR only).  Ignored for NEOPIXEL and DIRECT. */
    gpio_num_t clock_pin;

    /**
     * SPI clock speed in Hz for the DOTSTAR backend (e.g. 10000000 for 10 MHz).
     * Ignored for NEOPIXEL and DIRECT.
     */
    uint32_t spi_clock_hz;

    /**
     * GPIO pins for logical channels 0-(NUM_LED_CHANNELS-1) (DIRECT only).
     * Ignored for NEOPIXEL and DOTSTAR.
     */
    gpio_num_t direct_pins[NUM_LED_CHANNELS];

    /**
     * Per-pixel channel map string (comma-separated integers 0-3, length must
     * equal `length`).  NULL or empty string selects the menuconfig default
     * (CONFIG_LED_CHANNEL_MAP).  Ignored for DIRECT mode.
     */
    const char *channel_map_str;
} led_strip_config_t;

/**
 * @brief Initialiser macro that tells led_strip_init() to pull every parameter
 *        from the matching CONFIG_LED_* menuconfig symbol.
 *
 * Usage:
 *   led_strip_config_t cfg = LED_STRIP_CONFIG_FROM_MENUCONFIG;
 *   led_strip_handle_t *handle;
 *   ESP_ERROR_CHECK(led_strip_init(&cfg, &handle));
 */
#define LED_STRIP_CONFIG_FROM_MENUCONFIG \
    { \
        .backend       = LED_STRIP_BACKEND_FROM_MENUCONFIG, \
        .length        = 0, \
        .gpio_pin      = GPIO_NUM_NC, \
        .clock_pin     = GPIO_NUM_NC, \
        .spi_clock_hz  = 0, \
        .direct_pins   = { \
            GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, \
            GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC  \
        }, \
        .channel_map_str = NULL, \
    }

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialise the LED strip backend.
 *
 * When config->backend == LED_STRIP_BACKEND_FROM_MENUCONFIG the function reads
 * all parameters from CONFIG_LED_TYPE_*, CONFIG_LED_DATA_PIN, CONFIG_LED_COUNT,
 * CONFIG_LED_CHANNEL_MAP, etc.
 *
 * @param config  Configuration.  Use &LED_STRIP_CONFIG_FROM_MENUCONFIG (via the
 *                macro) for the compile-time default.
 * @param handle  Output handle.
 * @return ESP_OK on success.
 */
esp_err_t led_strip_init(const led_strip_config_t *config, led_strip_handle_t **handle);

/**
 * @brief Set one logical channel's full parameter set.
 *
 * This is the primary write API and is portable across all three backends:
 *
 *   - NEOPIXEL / DOTSTAR: brightness scales R/G/B to r'=(R*brightness)/100 etc.,
 *     then writes r'/g'/b' into working_buffer for every pixel whose channel_map
 *     entry equals channel_idx.
 *   - DIRECT: brightness is stored in direct_channel_brightness[channel_idx].
 *     R, G, B are **silently ignored** — the LEDC duty cycle is brightness-only.
 *
 * The silent discard of R/G/B in direct mode is intentional: it allows a single
 * .led timeline file to drive all three backends without modification.
 *
 * @note In direct mode, R/G/B are **silently ignored**; only `brightness` drives
 *       the LEDC PWM duty.  This makes a single .led timeline file portable
 *       across all three backends — R/G/B settings are wire-protocol details
 *       for addressable strips, irrelevant for discrete LEDs.
 *
 * @param handle       LED strip handle.
 * @param channel_idx  Logical channel index (0 to NUM_LED_CHANNELS-1).
 * @param brightness   Channel brightness 0-100 percent.
 * @param red          Red component 0-255 (ignored in DIRECT mode).
 * @param green        Green component 0-255 (ignored in DIRECT mode).
 * @param blue         Blue component 0-255 (ignored in DIRECT mode).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if channel_idx >= NUM_LED_CHANNELS.
 */
esp_err_t led_strip_set_channel(led_strip_handle_t *handle,
                                uint8_t channel_idx,
                                uint8_t brightness,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue);

/**
 * @brief Return the backend type that was selected at initialisation.
 *
 * @param handle  LED strip handle (must not be NULL).
 * @return LED_STRIP_BACKEND_NEOPIXEL, _DOTSTAR, or _DIRECT.
 */
led_strip_backend_t led_strip_get_backend(const led_strip_handle_t *handle);

/**
 * @brief Return the total pixel count for this strip.
 *
 * For NEOPIXEL and DOTSTAR this equals the CONFIG_LED_COUNT value passed at
 * init time.  For DIRECT mode this always returns NUM_LED_CHANNELS (one per
 * logical channel).
 *
 * @param handle  LED strip handle.
 * @return Pixel count.
 */
uint32_t led_strip_get_pixel_count(const led_strip_handle_t *handle);

/**
 * @brief Return true if the backend supports individual pixel addressing.
 *
 * Returns true for NEOPIXEL and DOTSTAR; false for DIRECT.
 * When this function returns false, the following APIs will return
 * ESP_ERR_NOT_SUPPORTED:
 *   set_pixel_rgb, set_pixel_color, get_pixel_color, set_all, vu_meter, spectrum.
 *
 * @param handle  LED strip handle.
 * @return true if pixel addressing is available.
 */
bool led_strip_supports_pixel_addressing(const led_strip_handle_t *handle);

/**
 * @brief Set a single pixel by RGB values.
 *
 * @note Returns ESP_ERR_NOT_SUPPORTED for DIRECT mode (no pixel addressing).
 *
 * @param handle     LED strip handle.
 * @param pixel_num  Pixel index (0-based, must be < pixel_count).
 * @param red        Red component 0-255.
 * @param green      Green component 0-255.
 * @param blue       Blue component 0-255.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_strip_set_pixel_rgb(led_strip_handle_t *handle, uint32_t pixel_num,
                                  uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Set a single pixel by led_color_t.
 *
 * @note Returns ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 *
 * @param handle     LED strip handle.
 * @param pixel_num  Pixel index (0-based).
 * @param color      Colour to write.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_strip_set_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                    const led_color_t *color);

/**
 * @brief Read back the last-refreshed pixel colour.
 *
 * @note Returns ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 *
 * @param handle     LED strip handle.
 * @param pixel_num  Pixel index (0-based).
 * @param color      Output colour.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_strip_get_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                    led_color_t *color);

/**
 * @brief Commit the working buffer to the physical LEDs.
 *
 * For NEOPIXEL: transmits via RMT (asynchronous, waits for previous frame).
 * For DOTSTAR:  transmits the APA102 frame via SPI DMA.
 * For DIRECT:   writes LEDC duty registers for all 4 channels.
 *
 * @param handle  LED strip handle.
 * @return ESP_OK on success.
 */
esp_err_t led_strip_refresh(led_strip_handle_t *handle);

/**
 * @brief Set all pixels to black and refresh.
 *
 * @param handle  LED strip handle.
 * @return ESP_OK on success.
 */
esp_err_t led_strip_clear(led_strip_handle_t *handle);

/**
 * @brief Set every pixel to the same colour and refresh.
 *
 * @note Returns ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 *
 * @param handle  LED strip handle.
 * @param red     Red component 0-255.
 * @param green   Green component 0-255.
 * @param blue    Blue component 0-255.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_strip_set_all(led_strip_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Render a stereo VU meter across the strip.
 *
 * @note Returns ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 *
 * @param handle       LED strip handle.
 * @param level_left   Left channel level 0-100.
 * @param level_right  Right channel level 0-100.
 * @param brightness   Overall brightness 0-100.
 * @param color_mode   Colour mode selector (implementation-defined).
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_strip_vu_meter(led_strip_handle_t *handle, uint8_t level_left,
                             uint8_t level_right, uint8_t brightness, uint8_t color_mode);

/**
 * @brief Render a spectrum analyser visualisation across the strip.
 *
 * @note Returns ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 *
 * @param handle        LED strip handle.
 * @param spectrum_data Frequency magnitude array.
 * @param data_length   Length of spectrum_data.
 * @param brightness    Overall brightness 0-100.
 * @param style         Visual style selector (implementation-defined).
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_strip_spectrum(led_strip_handle_t *handle, uint8_t *spectrum_data,
                            size_t data_length, uint8_t brightness, uint8_t style);

/**
 * @brief Release all resources owned by this handle.
 *
 * Turns off all LEDs before freeing hardware resources.
 *
 * @param handle  LED strip handle.
 * @return ESP_OK on success.
 */
esp_err_t led_strip_deinit(led_strip_handle_t *handle);

/* -------------------------------------------------------------------------
 * Convenience colour literals
 * ---------------------------------------------------------------------- */
#define LED_COLOR_RED     {.red = 255, .green = 0,   .blue = 0}
#define LED_COLOR_GREEN   {.red = 0,   .green = 255, .blue = 0}
#define LED_COLOR_BLUE    {.red = 0,   .green = 0,   .blue = 255}
#define LED_COLOR_WHITE   {.red = 255, .green = 255, .blue = 255}
#define LED_COLOR_YELLOW  {.red = 255, .green = 255, .blue = 0}
#define LED_COLOR_CYAN    {.red = 0,   .green = 255, .blue = 255}
#define LED_COLOR_MAGENTA {.red = 255, .green = 0,   .blue = 255}
#define LED_COLOR_BLACK   {.red = 0,   .green = 0,   .blue = 0}

#ifdef __cplusplus
}
#endif

#endif // LED_STRIP_H
