#include "led_strip.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

static const char* TAG = "led_strip";

// WS2812 timing configuration (all in nanoseconds; RMT clock is 10 MHz).
#define WS2812_T0H_NS    350    // 0 bit high time (ns)
#define WS2812_T0L_NS    900    // 0 bit low time (ns)
#define WS2812_T1H_NS    900    // 1 bit high time (ns)
#define WS2812_T1L_NS    350    // 1 bit low time (ns)
// Reset/latch low time in NANOSECONDS. WS2812B (V1-V3) requires >=50 us;
// WS2812B-V4+ requires >=280 us. We use 280 us to cover all silicon variants.
// BUG HISTORY: previously defined as WS2812_RESET_US = 50 and used as a RAW
// TICK count (i.e. 50 ticks x 100 ns/tick = 5 us), 10x too short to latch.
// That caused frames to merge in the cascade shift register and produce
// random-color flicker across all LEDs.
#define WS2812_RESET_NS  280000

// Convert nanoseconds to RMT ticks (10MHz RMT clock: 1 tick = 100ns)
#define NS_TO_RMT_TICKS(ns) ((ns) / 100)

// Number of RMT symbols per LED (24 bits for RGB)
#define SYMBOLS_PER_LED 24

// LED strip timing profiles
typedef struct {
    uint32_t t0h_ticks;
    uint32_t t0l_ticks;
    uint32_t t1h_ticks;
    uint32_t t1l_ticks;
    uint32_t reset_ticks;
} led_timing_t;

// Timing configuration for the NEOPIXEL backend (WS2812/SK6812/APA106 timings
// are identical so we collapse to a single entry).  DOTSTAR and DIRECT don't
// use this table -- their refresh paths don't call s_neopixel_encode_data().
static const led_timing_t s_neopixel_timing = {
    .t0h_ticks  = NS_TO_RMT_TICKS(WS2812_T0H_NS),
    .t0l_ticks  = NS_TO_RMT_TICKS(WS2812_T0L_NS),
    .t1h_ticks  = NS_TO_RMT_TICKS(WS2812_T1H_NS),
    .t1l_ticks  = NS_TO_RMT_TICKS(WS2812_T1L_NS),
    .reset_ticks = NS_TO_RMT_TICKS(WS2812_RESET_NS),
};

/* =========================================================================
 * Forward declarations for internal helpers
 * ========================================================================= */

static esp_err_t s_neopixel_setup_rmt(led_strip_handle_t *handle);
static esp_err_t s_neopixel_encode_data(led_strip_handle_t *handle, rmt_symbol_word_t *symbols);

/* Shared addressable-backend helpers (used by both NEOPIXEL and DOTSTAR) */
static esp_err_t s_addressable_set_channel(led_strip_handle_t *handle,
                                            uint8_t channel_idx, uint8_t brightness,
                                            uint8_t red, uint8_t green, uint8_t blue);
static esp_err_t s_addressable_set_pixel_rgb(led_strip_handle_t *handle,
                                              uint32_t pixel_num,
                                              uint8_t red, uint8_t green, uint8_t blue);
static esp_err_t s_addressable_get_pixel_color(led_strip_handle_t *handle,
                                                uint32_t pixel_num, led_color_t *color);

/* DotStar-specific helpers */
static esp_err_t s_dotstar_init(led_strip_handle_t *handle, gpio_num_t data_pin,
                                 gpio_num_t clock_pin, uint32_t pixel_count,
                                 uint32_t spi_clock_hz);
static void      s_dotstar_encode(led_strip_handle_t *handle);
static esp_err_t s_dotstar_refresh(led_strip_handle_t *handle);
static esp_err_t s_dotstar_clear(led_strip_handle_t *handle);
static esp_err_t s_dotstar_set_all(led_strip_handle_t *handle,
                                    uint8_t red, uint8_t green, uint8_t blue);
static esp_err_t s_dotstar_deinit(led_strip_handle_t *handle);

/* Direct (LEDC PWM) backend helpers — Step 6 */
static esp_err_t s_direct_init(led_strip_handle_t *handle, const gpio_num_t pin_ch[NUM_LED_CHANNELS]);
static esp_err_t s_direct_set_channel(led_strip_handle_t *handle,
                                       uint8_t ch, uint8_t brightness,
                                       uint8_t r, uint8_t g, uint8_t b);
static esp_err_t s_direct_set_pixel_rgb(led_strip_handle_t *handle,
                                         uint32_t pixel_num,
                                         uint8_t red, uint8_t green, uint8_t blue);
static esp_err_t s_direct_refresh(led_strip_handle_t *handle);
static esp_err_t s_direct_clear(led_strip_handle_t *handle);
static esp_err_t s_direct_deinit(led_strip_handle_t *handle);

/* =========================================================================
 * 3.1  s_parse_channel_map
 *
 * Parses a comma-separated string of channel indices (0-(NUM_LED_CHANNELS-1))
 * into `out`.  Clamps out-of-range values to 0; logs a WARN if any token is
 * out of range.  Pads missing entries with 0; truncates extras.
 * Does NOT fail -- a misconfigured strip is better than a brick.
 * ========================================================================= */
static void s_parse_channel_map(const char *str, uint8_t *out, uint32_t expected_n)
{
    if (!str || !out || expected_n == 0) {
        return;
    }

    // Work on a mutable copy because strtok_r modifies the string.
    char *buf = strdup(str);
    if (!buf) {
        ESP_LOGW(TAG, "channel_map: strdup failed, defaulting to channel 0");
        memset(out, 0, expected_n);
        return;
    }

    uint32_t count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);

    while (tok && count < expected_n) {
        int val = atoi(tok);
        if (val < 0 || val >= NUM_LED_CHANNELS) {
            ESP_LOGW(TAG, "channel_map[%lu]: value %d out of range (0-%d), clamping to 0",
                     count, val, NUM_LED_CHANNELS - 1);
            val = 0;
        }
        out[count] = (uint8_t)val;
        count++;
        tok = strtok_r(NULL, ",", &saveptr);
    }

    // Pad any missing entries
    if (count < expected_n) {
        ESP_LOGW(TAG, "channel_map: got %lu tokens, expected %lu -- padding remaining with channel 0",
                 count, expected_n);
        memset(&out[count], 0, expected_n - count);
    }

    // Warn if there are extra tokens beyond expected_n
    if (tok) {
        ESP_LOGW(TAG, "channel_map: more tokens than LED_COUNT=%lu -- truncating extras", expected_n);
    }

    free(buf);
}

/* =========================================================================
 * 3.2  led_strip_init  --  menuconfig pull + backend dispatch
 * ========================================================================= */

esp_err_t led_strip_init(const led_strip_config_t *config, led_strip_handle_t **handle)
{
    if (!config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    /* -----------------------------------------------------------------
     * Resolve backend and parameters.
     * When backend == LED_STRIP_BACKEND_FROM_MENUCONFIG we pull everything
     * from CONFIG_LED_TYPE_* / CONFIG_LED_DATA_PIN etc.
     * ----------------------------------------------------------------- */
    led_strip_backend_t backend        = config->backend;
    uint32_t            length         = config->length;
    gpio_num_t          gpio_pin       = config->gpio_pin;
    gpio_num_t          clock_pin      = config->clock_pin;
    uint32_t            spi_clock_hz   = config->spi_clock_hz;
    const char         *channel_map_str = config->channel_map_str;
    gpio_num_t          direct_pins[NUM_LED_CHANNELS];
    memcpy(direct_pins, config->direct_pins, sizeof(direct_pins));

    if (backend == LED_STRIP_BACKEND_FROM_MENUCONFIG) {
#if defined(CONFIG_LED_TYPE_NEOPIXEL)
        backend        = LED_STRIP_BACKEND_NEOPIXEL;
        length         = CONFIG_LED_COUNT;
        gpio_pin       = (gpio_num_t)CONFIG_LED_DATA_PIN;
        clock_pin      = GPIO_NUM_NC;
        spi_clock_hz   = 0;
        channel_map_str = CONFIG_LED_CHANNEL_MAP;
        (void)clock_pin;
        (void)spi_clock_hz;
#elif defined(CONFIG_LED_TYPE_DOTSTAR)
        backend        = LED_STRIP_BACKEND_DOTSTAR;
        length         = CONFIG_LED_COUNT;
        gpio_pin       = (gpio_num_t)CONFIG_LED_DATA_PIN;
        clock_pin      = (gpio_num_t)CONFIG_LED_CLOCK_PIN;
        spi_clock_hz   = CONFIG_LED_DOTSTAR_SPI_CLOCK_HZ;
        channel_map_str = CONFIG_LED_CHANNEL_MAP;
#elif defined(CONFIG_LED_TYPE_DIRECT)
        backend        = LED_STRIP_BACKEND_DIRECT;
        length         = NUM_LED_CHANNELS; /* logical channels only */
        gpio_pin       = GPIO_NUM_NC;
        clock_pin      = GPIO_NUM_NC;
        spi_clock_hz   = 0;
        channel_map_str = NULL;
        direct_pins[0] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH1;
        direct_pins[1] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH2;
        direct_pins[2] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH3;
        direct_pins[3] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH4;
        direct_pins[4] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH5;
        direct_pins[5] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH6;
        direct_pins[6] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH7;
        direct_pins[7] = (gpio_num_t)CONFIG_LED_DIRECT_PIN_CH8;
#else
        ESP_LOGE(TAG, "LED_STRIP_CONFIG_FROM_MENUCONFIG: no LED_TYPE selected in menuconfig");
        return ESP_ERR_INVALID_ARG;
#endif
    }

    /* Validate backend */
    if (backend != LED_STRIP_BACKEND_NEOPIXEL &&
        backend != LED_STRIP_BACKEND_DOTSTAR  &&
        backend != LED_STRIP_BACKEND_DIRECT) {
        ESP_LOGE(TAG, "Invalid LED backend: %d", (int)backend);
        return ESP_ERR_INVALID_ARG;
    }

    /* For addressable backends enforce a sane pixel count. */
    if (backend != LED_STRIP_BACKEND_DIRECT) {
        if (length == 0 || length > 1000) {
            ESP_LOGE(TAG, "Invalid LED count: %lu (must be 1-1000)", length);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* -----------------------------------------------------------------
     * Allocate handle
     * ----------------------------------------------------------------- */
    led_strip_handle_t *strip = calloc(1, sizeof(led_strip_handle_t));
    if (!strip) {
        ESP_LOGE(TAG, "Failed to allocate LED strip handle");
        return ESP_ERR_NO_MEM;
    }

    strip->backend  = backend;
    strip->length   = length;
    strip->gpio_pin = gpio_pin;

    /* Create mutex for thread safety */
    strip->access_mutex = xSemaphoreCreateMutex();
    if (!strip->access_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(strip);
        return ESP_ERR_NO_MEM;
    }

    /* -----------------------------------------------------------------
     * Backend-specific allocation and setup
     * ----------------------------------------------------------------- */

    if (backend == LED_STRIP_BACKEND_NEOPIXEL) {
        /* Pixel working + display buffers */
        strip->working_buffer = calloc(length, sizeof(led_color_t));
        strip->display_buffer = calloc(length, sizeof(led_color_t));
        if (!strip->working_buffer || !strip->display_buffer) {
            ESP_LOGE(TAG, "Failed to allocate LED pixel buffers");
            led_strip_deinit(strip);
            return ESP_ERR_NO_MEM;
        }

        /* Pre-allocate RMT symbol buffer (+1 for reset pulse) */
        strip->symbol_buffer_size = length * SYMBOLS_PER_LED + 1;
        strip->symbol_buffer = malloc(strip->symbol_buffer_size * sizeof(rmt_symbol_word_t));
        if (!strip->symbol_buffer) {
            ESP_LOGE(TAG, "Failed to allocate RMT symbol buffer (%zu symbols)",
                     strip->symbol_buffer_size);
            led_strip_deinit(strip);
            return ESP_ERR_NO_MEM;
        }

        /* Channel map */
        strip->channel_map = calloc(length, 1);
        if (!strip->channel_map) {
            ESP_LOGE(TAG, "Failed to allocate channel_map");
            led_strip_deinit(strip);
            return ESP_ERR_NO_MEM;
        }
        s_parse_channel_map(channel_map_str ? channel_map_str : "", strip->channel_map, length);

        /* RMT hardware setup */
        esp_err_t ret = s_neopixel_setup_rmt(strip);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to setup RMT: %s", esp_err_to_name(ret));
            led_strip_deinit(strip);
            return ret;
        }

        ESP_LOGI(TAG, "LED strip [NEOPIXEL] initialised: length=%lu, gpio=%d",
                 strip->length, strip->gpio_pin);

    } else if (backend == LED_STRIP_BACKEND_DOTSTAR) {
        /* Pixel working + display buffers */
        strip->working_buffer = calloc(length, sizeof(led_color_t));
        strip->display_buffer = calloc(length, sizeof(led_color_t));
        if (!strip->working_buffer || !strip->display_buffer) {
            ESP_LOGE(TAG, "Failed to allocate LED pixel buffers");
            led_strip_deinit(strip);
            return ESP_ERR_NO_MEM;
        }

        /* Channel map */
        strip->channel_map = calloc(length, 1);
        if (!strip->channel_map) {
            ESP_LOGE(TAG, "Failed to allocate channel_map");
            led_strip_deinit(strip);
            return ESP_ERR_NO_MEM;
        }
        s_parse_channel_map(channel_map_str ? channel_map_str : "", strip->channel_map, length);

        /* DotStar SPI backend — Step 5 */
        esp_err_t ret = s_dotstar_init(strip, gpio_pin, clock_pin, length, spi_clock_hz);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialise DotStar SPI backend: %s", esp_err_to_name(ret));
            /* s_dotstar_init frees its own partial state; we still own the
             * buffers and channel_map that were allocated above. */
            free(strip->working_buffer);
            free(strip->display_buffer);
            free(strip->channel_map);
            strip->working_buffer = NULL;
            strip->display_buffer = NULL;
            strip->channel_map    = NULL;
            vSemaphoreDelete(strip->access_mutex);
            free(strip);
            return ret;
        }

        ESP_LOGI(TAG, "LED strip [DOTSTAR] initialised: length=%lu, data=%d, clk=%d, spi_hz=%lu",
                 strip->length, gpio_pin, clock_pin, spi_clock_hz);

    } else { /* LED_STRIP_BACKEND_DIRECT */
        /* Direct mode: NUM_LED_CHANNELS (8) logical channels, no pixel buffers,
         * no channel_map.  We do NOT allocate working_buffer / display_buffer /
         * symbol_buffer / channel_map — those are addressable-strip concepts
         * irrelevant here.  s_direct_init owns all LEDC resource allocation. */
        strip->length      = NUM_LED_CHANNELS;
        strip->channel_map = NULL;

        esp_err_t ret = s_direct_init(strip, direct_pins);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialise Direct LEDC backend: %s", esp_err_to_name(ret));
            vSemaphoreDelete(strip->access_mutex);
            free(strip);
            return ret;
        }

        ESP_LOGI(TAG, "LED strip [DIRECT] initialised: pins=%d,%d,%d,%d,%d,%d,%d,%d",
                 direct_pins[0], direct_pins[1], direct_pins[2], direct_pins[3],
                 direct_pins[4], direct_pins[5], direct_pins[6], direct_pins[7]);
    }

    strip->initialized = true;
    *handle = strip;

    return ESP_OK;
}

/* =========================================================================
 * 5.4 / 4.2  Shared addressable-backend helpers
 *
 * Both NEOPIXEL and DOTSTAR share the same working_buffer layout (3-byte
 * led_color_t) and the same brightness-scale + channel-map walk.  Factor
 * these into shared helpers so neither backend duplicates the logic.
 * ========================================================================= */

/**
 * @brief Write a single pixel into working_buffer (no mutex — caller holds it
 *        or knows single-threaded context).  Shared by both addressable backends.
 */
static esp_err_t s_addressable_set_pixel_rgb(led_strip_handle_t *handle,
                                              uint32_t pixel_num,
                                              uint8_t red, uint8_t green, uint8_t blue)
{
    if (pixel_num >= handle->length) {
        ESP_LOGE(TAG, "Pixel %lu out of range (max %lu)", pixel_num, handle->length - 1);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    handle->working_buffer[pixel_num].red   = red;
    handle->working_buffer[pixel_num].green = green;
    handle->working_buffer[pixel_num].blue  = blue;
    xSemaphoreGive(handle->access_mutex);

    return ESP_OK;
}

/**
 * @brief Read back last-refreshed pixel colour from display_buffer.
 *        Shared by both addressable backends.
 */
static esp_err_t s_addressable_get_pixel_color(led_strip_handle_t *handle,
                                                uint32_t pixel_num, led_color_t *color)
{
    if (pixel_num >= handle->length) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    *color = handle->display_buffer[pixel_num];
    xSemaphoreGive(handle->access_mutex);

    return ESP_OK;
}

/**
 * @brief Set one logical channel's colour across all pixels in working_buffer.
 *
 * Computes r'=(r*brightness)/100 etc., then walks channel_map and writes
 * working_buffer[i] for every pixel owned by channel_idx.
 *
 * Shared by NEOPIXEL (s_neopixel_set_channel) and DOTSTAR (s_dotstar_set_channel).
 * Both backends use the same 3-byte led_color_t buffer layout.
 */
static esp_err_t s_addressable_set_channel(led_strip_handle_t *handle,
                                            uint8_t channel_idx, uint8_t brightness,
                                            uint8_t red, uint8_t green, uint8_t blue)
{
    if (channel_idx >= NUM_LED_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t r = (uint8_t)(((uint32_t)red   * brightness) / 100);
    uint8_t g = (uint8_t)(((uint32_t)green * brightness) / 100);
    uint8_t b = (uint8_t)(((uint32_t)blue  * brightness) / 100);

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    for (uint32_t i = 0; i < handle->length; i++) {
        if (handle->channel_map && handle->channel_map[i] == channel_idx) {
            handle->working_buffer[i].red   = r;
            handle->working_buffer[i].green = g;
            handle->working_buffer[i].blue  = b;
        }
    }

    xSemaphoreGive(handle->access_mutex);

    return ESP_OK;
}

/* =========================================================================
 * Neopixel backend static helpers
 * (bodies moved from the old public functions; no behaviour change)
 * ========================================================================= */

/* Thin wrappers that delegate to the shared addressable helpers. */

static inline esp_err_t s_neopixel_set_pixel_rgb(led_strip_handle_t *handle,
                                                   uint32_t pixel_num,
                                                   uint8_t red, uint8_t green, uint8_t blue)
{
    return s_addressable_set_pixel_rgb(handle, pixel_num, red, green, blue);
}

static inline esp_err_t s_neopixel_get_pixel_color(led_strip_handle_t *handle,
                                                     uint32_t pixel_num, led_color_t *color)
{
    return s_addressable_get_pixel_color(handle, pixel_num, color);
}

/* 3.3 + 4.2: set_channel for the neopixel backend (delegates to shared helper). */
static inline esp_err_t s_neopixel_set_channel(led_strip_handle_t *handle,
                                                uint8_t channel_idx, uint8_t brightness,
                                                uint8_t red, uint8_t green, uint8_t blue)
{
    return s_addressable_set_channel(handle, channel_idx, brightness, red, green, blue);
}

static esp_err_t s_neopixel_refresh(led_strip_handle_t *handle)
{
    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    // Wait for the previous RMT TX to drain before we overwrite symbol_buffer.
    // rmt_transmit() is asynchronous -- without this barrier, the next encode
    // would write fresh symbols on top of bytes the DMA is still reading,
    // producing GRB-byte-axis splicing (visible as green-tinted random pixels).
    rmt_tx_wait_all_done(handle->rmt_tx_channel, portMAX_DELAY);

    // Copy working buffer to display buffer
    memcpy(handle->display_buffer, handle->working_buffer,
           handle->length * sizeof(led_color_t));

    size_t symbol_count = handle->length * SYMBOLS_PER_LED + 1; // +1 for reset
    rmt_symbol_word_t *symbols = handle->symbol_buffer;

    if (!symbols || symbol_count > handle->symbol_buffer_size) {
        xSemaphoreGive(handle->access_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = s_neopixel_encode_data(handle, symbols);
    if (ret != ESP_OK) {
        xSemaphoreGive(handle->access_mutex);
        return ret;
    }

    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    esp_err_t tx_ret = rmt_transmit(handle->rmt_tx_channel, handle->rmt_encoder,
                                    symbols, symbol_count * sizeof(rmt_symbol_word_t),
                                    &tx_config);
    if (tx_ret != ESP_OK) {
        ret = tx_ret;
    }

    xSemaphoreGive(handle->access_mutex);
    return ESP_OK;
}

static esp_err_t s_neopixel_clear(led_strip_handle_t *handle)
{
    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    memset(handle->working_buffer, 0, handle->length * sizeof(led_color_t));
    xSemaphoreGive(handle->access_mutex);
    return s_neopixel_refresh(handle);
}

static esp_err_t s_neopixel_set_all(led_strip_handle_t *handle,
                                     uint8_t red, uint8_t green, uint8_t blue)
{
    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    for (uint32_t i = 0; i < handle->length; i++) {
        handle->working_buffer[i].red   = red;
        handle->working_buffer[i].green = green;
        handle->working_buffer[i].blue  = blue;
    }
    xSemaphoreGive(handle->access_mutex);
    return s_neopixel_refresh(handle);
}

/**
 * @brief Fill the working_buffer with a stereo VU meter pattern (backend-agnostic).
 *
 * Left channel occupies the first half of the strip (red).
 * Right channel occupies the second half (green).
 * Acquires access_mutex internally.  Does NOT call refresh.
 */
static esp_err_t s_addressable_vu_meter_fill(led_strip_handle_t *handle,
                                              uint8_t level_left, uint8_t level_right,
                                              uint8_t brightness)
{
    if (level_left > 100 || level_right > 100 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    memset(handle->working_buffer, 0, handle->length * sizeof(led_color_t));

    uint32_t half_length = handle->length / 2;
    uint8_t  intensity   = (uint8_t)(((uint32_t)brightness * 255) / 100);

    uint32_t left_pixels = (level_left * half_length) / 100;
    for (uint32_t i = 0; i < left_pixels; i++) {
        handle->working_buffer[i].red   = intensity;
        handle->working_buffer[i].green = 0;
        handle->working_buffer[i].blue  = 0;
    }

    uint32_t right_pixels = (level_right * half_length) / 100;
    for (uint32_t i = 0; i < right_pixels; i++) {
        uint32_t pixel_idx = half_length + i;
        if (pixel_idx < handle->length) {
            handle->working_buffer[pixel_idx].red   = 0;
            handle->working_buffer[pixel_idx].green = intensity;
            handle->working_buffer[pixel_idx].blue  = 0;
        }
    }

    xSemaphoreGive(handle->access_mutex);
    return ESP_OK;
}

/**
 * @brief Fill the working_buffer with a spectrum analyser pattern (backend-agnostic).
 *
 * Maps spectrum_data onto the full strip with a red→green→blue gradient.
 * Acquires access_mutex internally.  Does NOT call refresh.
 */
static esp_err_t s_addressable_spectrum_fill(led_strip_handle_t *handle,
                                              uint8_t *spectrum_data, size_t data_length,
                                              uint8_t brightness)
{
    if (!spectrum_data || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    memset(handle->working_buffer, 0, handle->length * sizeof(led_color_t));

    for (uint32_t i = 0; i < handle->length; i++) {
        uint32_t data_idx = (i * data_length) / handle->length;
        if (data_idx < data_length) {
            uint8_t level     = spectrum_data[data_idx];
            uint8_t intensity = (uint8_t)(((uint32_t)level * brightness) / 100);

            if (i < handle->length / 3) {
                handle->working_buffer[i].red   = intensity;
                handle->working_buffer[i].green = (uint8_t)((intensity * i * 3) / handle->length);
                handle->working_buffer[i].blue  = 0;
            } else if (i < 2 * handle->length / 3) {
                uint32_t seg = i - handle->length / 3;
                handle->working_buffer[i].red   = intensity - (uint8_t)((intensity * seg * 3) / handle->length);
                handle->working_buffer[i].green = intensity;
                handle->working_buffer[i].blue  = 0;
            } else {
                uint32_t seg = i - 2 * handle->length / 3;
                handle->working_buffer[i].red   = 0;
                handle->working_buffer[i].green = intensity - (uint8_t)((intensity * seg * 3) / handle->length);
                handle->working_buffer[i].blue  = intensity;
            }
        }
    }

    xSemaphoreGive(handle->access_mutex);
    return ESP_OK;
}

static esp_err_t s_neopixel_vu_meter(led_strip_handle_t *handle,
                                      uint8_t level_left, uint8_t level_right,
                                      uint8_t brightness, uint8_t color_mode)
{
    (void)color_mode;
    esp_err_t ret = s_addressable_vu_meter_fill(handle, level_left, level_right, brightness);
    if (ret != ESP_OK) return ret;
    return s_neopixel_refresh(handle);
}

static esp_err_t s_neopixel_spectrum(led_strip_handle_t *handle,
                                      uint8_t *spectrum_data, size_t data_length,
                                      uint8_t brightness, uint8_t style)
{
    (void)style;
    esp_err_t ret = s_addressable_spectrum_fill(handle, spectrum_data, data_length, brightness);
    if (ret != ESP_OK) return ret;
    return s_neopixel_refresh(handle);
}

static esp_err_t s_neopixel_deinit(led_strip_handle_t *handle)
{
    /* Turn off LEDs before freeing hardware resources */
    if (handle->initialized) {
        /* Best effort: ignore return value -- we're tearing down anyway. */
        s_neopixel_clear(handle);
    }

    if (handle->rmt_encoder) {
        rmt_del_encoder(handle->rmt_encoder);
        handle->rmt_encoder = NULL;
    }
    if (handle->rmt_tx_channel) {
        rmt_disable(handle->rmt_tx_channel);
        rmt_del_channel(handle->rmt_tx_channel);
        handle->rmt_tx_channel = NULL;
    }
    if (handle->symbol_buffer) {
        free(handle->symbol_buffer);
        handle->symbol_buffer = NULL;
    }
    if (handle->working_buffer) {
        free(handle->working_buffer);
        handle->working_buffer = NULL;
    }
    if (handle->display_buffer) {
        free(handle->display_buffer);
        handle->display_buffer = NULL;
    }
    if (handle->channel_map) {
        free(handle->channel_map);
        handle->channel_map = NULL;
    }
    return ESP_OK;
}

/* =========================================================================
 * 5.1  s_dotstar_init — SPI3 bus + device + pre-allocated DMA buffer
 *
 * APA102 frame layout:
 *   [4 bytes 0x00 start] [4 bytes/pixel: 0xE0|0x1F, B, G, R] [end frame]
 *
 * End-frame length = ceil(pixel_count / 2 / 8) bytes, each byte = 0xFF.
 * The ceiling formula is (pixel_count + 15) / 16 (integer division).
 *
 * Full frame size = 4 + 4*pixel_count + (pixel_count + 15) / 16.
 * ========================================================================= */

static esp_err_t s_dotstar_init(led_strip_handle_t *handle, gpio_num_t data_pin,
                                 gpio_num_t clock_pin, uint32_t pixel_count,
                                 uint32_t spi_clock_hz)
{
    /* Compute frame size:
     *   4 bytes  — start frame (all 0x00)
     *   4*N bytes — pixel data: {0xE0|brightness, B, G, R} per pixel
     *   ceil(N/16) bytes — end frame (all 0xFF)
     *
     * The end-frame requirement for APA102 is ceil(N/2) bits = ceil(N/16) bytes.
     * At 48 LEDs: 4 + 192 + 3 = 199 bytes.  At 1000 LEDs: 4 + 4000 + 63 = 4067 bytes.
     * Both fit within the default SPI DMA limit (4092 bytes).
     */
    size_t end_frame_bytes = (pixel_count + 15) / 16;
    size_t frame_size      = 4 + 4 * pixel_count + end_frame_bytes;

    /* --- SPI bus --- */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = data_pin,
        .sclk_io_num     = clock_pin,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = (int)frame_size,
    };

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "s_dotstar_init: spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- SPI device --- */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (int)spi_clock_hz,
        .mode           = 0,       /* APA102 clocks on rising edge, idle low */
        .spics_io_num   = -1,      /* no chip-select; DotStar uses continuous clocking */
        .queue_size     = 1,
        .command_bits   = 0,
        .address_bits   = 0,
    };

    ret = spi_bus_add_device(SPI3_HOST, &dev_cfg, &handle->spi_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "s_dotstar_init: spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI3_HOST);
        return ret;
    }

    /* --- DMA-capable frame buffer --- */
    handle->dma_buffer = (uint8_t *)heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
    if (!handle->dma_buffer) {
        ESP_LOGE(TAG, "s_dotstar_init: failed to allocate %zu-byte DMA buffer", frame_size);
        spi_bus_remove_device(handle->spi_device);
        handle->spi_device = NULL;
        spi_bus_free(SPI3_HOST);
        return ESP_ERR_NO_MEM;
    }
    handle->dma_buffer_size = frame_size;

    /* Pre-initialise the constant parts of the frame:
     *   bytes [0..3]         = 0x00  (start frame)
     *   bytes [4+4*N .. end] = 0xFF  (end frame)
     *
     * Per-pixel bytes [4..4+4*N-1] will be written by s_dotstar_encode()
     * on each refresh — no need to initialise them here.
     */
    memset(handle->dma_buffer, 0x00, 4);                           /* start frame */
    memset(handle->dma_buffer + 4 + 4 * pixel_count, 0xFF,        /* end frame   */
           end_frame_bytes);

    return ESP_OK;
}

/* =========================================================================
 * 5.2  s_dotstar_encode — APA102 per-pixel frame builder
 *
 * For each pixel i: write 4 bytes at offset (4 + 4*i):
 *   byte 0: 0xFF  = 0xE0 | 0x1F  (top 3 bits "111" + 5-bit global brightness max)
 *   byte 1: blue  (APA102 wire order is BGR after the brightness byte)
 *   byte 2: green
 *   byte 3: red
 *
 * Per-pixel brightness is already scaled into RGB by s_addressable_set_channel.
 * Global APA102 brightness is fixed at 0x1F (maximum) so no extra scale stage.
 *
 * MUST be called with access_mutex held (called from s_dotstar_refresh).
 * ========================================================================= */

static void s_dotstar_encode(led_strip_handle_t *handle)
{
    uint8_t *buf = handle->dma_buffer + 4;  /* skip the 4-byte start frame */

    for (uint32_t i = 0; i < handle->length; i++) {
        buf[4 * i + 0] = 0xE0U | 0x1FU;                         /* 111 + 5-bit brightness max */
        buf[4 * i + 1] = handle->display_buffer[i].blue;
        buf[4 * i + 2] = handle->display_buffer[i].green;
        buf[4 * i + 3] = handle->display_buffer[i].red;
    }
    /* End-frame bytes were pre-written to 0xFF in s_dotstar_init and never change. */
}

/* =========================================================================
 * 5.3  s_dotstar_refresh — SPI DMA transmit
 * ========================================================================= */

static esp_err_t s_dotstar_refresh(led_strip_handle_t *handle)
{
    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    /* Mirror working buffer to display buffer (same pattern as neopixel). */
    memcpy(handle->display_buffer, handle->working_buffer,
           handle->length * sizeof(led_color_t));

    /* Encode per-pixel data into the DMA buffer. */
    s_dotstar_encode(handle);

    /* Transmit synchronously — blocks until the entire frame is sent.
     * SPI queue_size == 1, so at most one transaction is in flight. */
    spi_transaction_t t = {
        .length    = handle->dma_buffer_size * 8,  /* length in bits */
        .tx_buffer = handle->dma_buffer,
    };

    esp_err_t ret = spi_device_transmit(handle->spi_device, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "s_dotstar_refresh: spi_device_transmit failed: %s",
                 esp_err_to_name(ret));
    }

    xSemaphoreGive(handle->access_mutex);
    return ret;
}

/* Convenience wrappers for DotStar that delegate to shared addressable helpers
 * (same working_buffer layout, same per-pixel channel semantics). */

static inline esp_err_t s_dotstar_set_channel(led_strip_handle_t *handle,
                                               uint8_t channel_idx, uint8_t brightness,
                                               uint8_t red, uint8_t green, uint8_t blue)
{
    return s_addressable_set_channel(handle, channel_idx, brightness, red, green, blue);
}

static inline esp_err_t s_dotstar_set_pixel_rgb(led_strip_handle_t *handle,
                                                  uint32_t pixel_num,
                                                  uint8_t red, uint8_t green, uint8_t blue)
{
    return s_addressable_set_pixel_rgb(handle, pixel_num, red, green, blue);
}

static inline esp_err_t s_dotstar_get_pixel_color(led_strip_handle_t *handle,
                                                    uint32_t pixel_num, led_color_t *color)
{
    return s_addressable_get_pixel_color(handle, pixel_num, color);
}

static esp_err_t s_dotstar_clear(led_strip_handle_t *handle)
{
    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    memset(handle->working_buffer, 0, handle->length * sizeof(led_color_t));
    xSemaphoreGive(handle->access_mutex);
    return s_dotstar_refresh(handle);
}

static esp_err_t s_dotstar_set_all(led_strip_handle_t *handle,
                                    uint8_t red, uint8_t green, uint8_t blue)
{
    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    for (uint32_t i = 0; i < handle->length; i++) {
        handle->working_buffer[i].red   = red;
        handle->working_buffer[i].green = green;
        handle->working_buffer[i].blue  = blue;
    }
    xSemaphoreGive(handle->access_mutex);
    return s_dotstar_refresh(handle);
}

/* =========================================================================
 * 5.5  s_dotstar_deinit — release SPI device, bus, and DMA buffer
 * ========================================================================= */

static esp_err_t s_dotstar_deinit(led_strip_handle_t *handle)
{
    /* Best-effort blank all LEDs before tearing down the SPI bus. */
    if (handle->initialized && handle->dma_buffer) {
        s_dotstar_clear(handle);
    }

    if (handle->spi_device) {
        spi_bus_remove_device(handle->spi_device);
        handle->spi_device = NULL;
    }

    spi_bus_free(SPI3_HOST);

    if (handle->dma_buffer) {
        free(handle->dma_buffer);
        handle->dma_buffer = NULL;
    }

    if (handle->working_buffer) {
        free(handle->working_buffer);
        handle->working_buffer = NULL;
    }
    if (handle->display_buffer) {
        free(handle->display_buffer);
        handle->display_buffer = NULL;
    }
    if (handle->channel_map) {
        free(handle->channel_map);
        handle->channel_map = NULL;
    }

    return ESP_OK;
}

/* =========================================================================
 * Step 6 — Direct (LEDC PWM) backend implementation
 *
 * Drives 4 discrete LEDs via the ESP32 LEDC peripheral (low-speed mode,
 * 8-bit resolution, 5 kHz carrier).  One LEDC channel per logical LED channel.
 *
 * Contract: R, G, B arguments passed to s_direct_set_channel are silently
 * discarded.  Only `brightness` (0-100) drives the PWM duty cycle.  This
 * makes a single .led timeline file portable across all three backends —
 * R/G/B are wire-protocol details for addressable strips; they are irrelevant
 * for discrete LEDs whose physical colour is fixed by the LED itself.
 * ========================================================================= */

/**
 * 6.1  s_direct_init
 *
 * Configure one shared LEDC timer and 4 LEDC channels on the supplied GPIO
 * pins.  Stores the channel IDs and pins in the handle; initialises
 * direct_channel_brightness[] to 0.
 */
static esp_err_t s_direct_init(led_strip_handle_t *handle, const gpio_num_t pin_ch[NUM_LED_CHANNELS])
{
    /* Shared LEDC timer — low-speed mode, 8-bit resolution, 25 kHz.
     * 5 kHz was clearly audible as a whine through nearby speaker wiring
     * on the AI-Thinker A1S board; 25 kHz is comfortably above the human
     * hearing limit (~20 kHz) and well within LEDC's range. Higher
     * frequencies (40-80 kHz) work too but provide no additional benefit
     * for visual smoothness — the eye can't perceive flicker above ~80 Hz
     * to begin with. 25 kHz × 256 (8-bit) = 6.4 MHz LEDC clock, well within
     * the APB clock range. */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 25000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* NUM_LED_CHANNELS (8) LEDC channels, one per logical LED channel.
     * LEDC_CHANNEL_0 + 7 == LEDC_CHANNEL_7 is the hardware maximum for
     * low-speed mode on ESP32 (classic). Channels with pin == GPIO_NUM_NC
     * (set via CONFIG_LED_DIRECT_PIN_CHn = -1) are skipped — no LEDC slot
     * allocated, no pin claimed, no PWM output. set_channel / refresh /
     * deinit also short-circuit for those channels.
     *
     * LEDC duty scale: we use 0..LED_DIRECT_DUTY_MAX where MAX = 1<<resolution.
     * For 8-bit that's 256, which the ESP-IDF LEDC treats as a special case
     * meaning "constant HIGH with no LOW pulses at all". Using the more
     * intuitive 0..255 range leaves a 1/256 LOW pulse every cycle at "max",
     * which is enough to leak measurable current through an active-low LED
     * and leave it visibly glowing in the off state. duty=256 → truly off
     * for active-low; duty=0 → truly off for active-high.
     *
     * For active-low channels (bit set in CONFIG_LED_DIRECT_ACTIVE_LOW_MASK)
     * the initial duty is LED_DIRECT_DUTY_MAX so the pin is constantly HIGH
     * at boot (LED off), not blasting on until the first refresh. */
    const uint8_t active_low_mask = (uint8_t)CONFIG_LED_DIRECT_ACTIVE_LOW_MASK;
    for (int i = 0; i < NUM_LED_CHANNELS; i++) {
        handle->ledc_channels[i] = (ledc_channel_t)(LEDC_CHANNEL_0 + i);
        handle->direct_pins[i]   = pin_ch[i];

        if (pin_ch[i] == GPIO_NUM_NC) {
            ESP_LOGI(TAG, "s_direct_init: channel %d disabled (pin -1)", i);
            continue;
        }

        ledc_channel_config_t ch_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = handle->ledc_channels[i],
            .timer_sel  = LEDC_TIMER_0,
            .intr_type  = LEDC_INTR_DISABLE,
            .gpio_num   = pin_ch[i],
            .duty       = (active_low_mask & (1u << i)) ? 256 : 0,
            .hpoint     = 0,
        };
        esp_err_t ret = ledc_channel_config(&ch_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "s_direct_init: ledc_channel_config ch%d failed: %s",
                     i, esp_err_to_name(ret));
            return ret;
        }
    }

    memset(handle->direct_channel_brightness, 0, sizeof(handle->direct_channel_brightness));
    return ESP_OK;
}

/**
 * 6.2  s_direct_set_channel
 *
 * Store brightness for one logical channel.  R, G, B are silently discarded
 * (documented contract — no log to avoid UART flooding at flicker frequencies).
 */
static esp_err_t s_direct_set_channel(led_strip_handle_t *handle,
                                       uint8_t ch, uint8_t brightness,
                                       uint8_t r, uint8_t g, uint8_t b)
{
    (void)r; (void)g; (void)b;  /* silently discard — documented contract */

    if (ch >= NUM_LED_CHANNELS || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    handle->direct_channel_brightness[ch] = brightness;
    xSemaphoreGive(handle->access_mutex);

    return ESP_OK;
}

/**
 * 6.3  s_direct_set_pixel_rgb
 *
 * Direct mode has no individually addressable pixels — only 4 logical
 * channels driven by 4 physical pins.  Always returns NOT_SUPPORTED.
 */
static esp_err_t s_direct_set_pixel_rgb(led_strip_handle_t *handle,
                                         uint32_t pixel_num,
                                         uint8_t red, uint8_t green, uint8_t blue)
{
    (void)handle; (void)pixel_num; (void)red; (void)green; (void)blue;
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * 6.4  s_direct_refresh
 *
 * Snapshot all 4 channel brightnesses under the mutex, then write LEDC duty
 * registers outside the lock (LEDC register writes are not time-critical and
 * the registers are per-channel so no shared-state risk).
 *
 * duty = (brightness * 255) / 100  for 8-bit (0-255) resolution.
 */
static esp_err_t s_direct_refresh(led_strip_handle_t *handle)
{
    uint8_t brightness[NUM_LED_CHANNELS];

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    memcpy(brightness, handle->direct_channel_brightness, sizeof(brightness));
    xSemaphoreGive(handle->access_mutex);

    /* Active-low channels (boards where the LED's anode is on Vcc and the
     * GPIO sinks current) need their PWM duty inverted so brightness=0
     * actually turns the LED off. Bit N of CONFIG_LED_DIRECT_ACTIVE_LOW_MASK
     * marks channel N as active-low.
     *
     * Duty scale: 0..256 (not 0..255). 256 is the ESP-IDF LEDC special value
     * meaning "constant HIGH, no LOW pulse" for an 8-bit timer — required to
     * fully extinguish active-low LEDs. Using 255 leaves a 1/256 LOW pulse
     * per cycle that visibly lights active-low LEDs in the supposed off state. */
    const uint8_t active_low_mask = (uint8_t)CONFIG_LED_DIRECT_ACTIVE_LOW_MASK;
    for (int ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (handle->direct_pins[ch] == GPIO_NUM_NC) continue;
        uint32_t duty = ((uint32_t)brightness[ch] * 256U) / 100U;
        if (active_low_mask & (1u << ch)) duty = 256U - duty;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, handle->ledc_channels[ch], duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, handle->ledc_channels[ch]);
    }

    return ESP_OK;
}

/**
 * 6.5  s_direct_clear
 *
 * Set all 4 channel brightnesses to 0 and push to hardware immediately.
 */
static esp_err_t s_direct_clear(led_strip_handle_t *handle)
{
    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);
    memset(handle->direct_channel_brightness, 0, sizeof(handle->direct_channel_brightness));
    xSemaphoreGive(handle->access_mutex);

    return s_direct_refresh(handle);
}

/**
 * 6.6  s_direct_deinit
 *
 * Stop all 4 LEDC channels (output goes low / idle level 0).  No explicit
 * timer teardown — LEDC_TIMER_0 is a shared hardware resource and stopping
 * the channels is sufficient to silence the outputs.
 */
static esp_err_t s_direct_deinit(led_strip_handle_t *handle)
{
    /* idle_level on stop: 1 for active-low channels (HIGH = LED OFF),
     * 0 for active-high (LOW = LED OFF). Either way, ensure the LED is
     * dark after shutdown. */
    const uint8_t active_low_mask = (uint8_t)CONFIG_LED_DIRECT_ACTIVE_LOW_MASK;
    for (int ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (handle->direct_pins[ch] == GPIO_NUM_NC) continue;
        uint32_t idle = (active_low_mask & (1u << ch)) ? 1 : 0;
        ledc_stop(LEDC_LOW_SPEED_MODE, handle->ledc_channels[ch], idle);
    }
    return ESP_OK;
}

/* =========================================================================
 * 3.3  Public API dispatchers
 * ========================================================================= */

esp_err_t led_strip_set_pixel_rgb(led_strip_handle_t *handle, uint32_t pixel_num,
                                   uint8_t red, uint8_t green, uint8_t blue)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Invalid handle or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_set_pixel_rgb(handle, pixel_num, red, green, blue);
    case LED_STRIP_BACKEND_DOTSTAR:
        return s_dotstar_set_pixel_rgb(handle, pixel_num, red, green, blue);
    case LED_STRIP_BACKEND_DIRECT:
        return s_direct_set_pixel_rgb(handle, pixel_num, red, green, blue);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_strip_set_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                     const led_color_t *color)
{
    if (!color) {
        return ESP_ERR_INVALID_ARG;
    }
    return led_strip_set_pixel_rgb(handle, pixel_num, color->red, color->green, color->blue);
}

esp_err_t led_strip_get_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                     led_color_t *color)
{
    if (!handle || !handle->initialized || !color) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_get_pixel_color(handle, pixel_num, color);
    case LED_STRIP_BACKEND_DOTSTAR:
        return s_dotstar_get_pixel_color(handle, pixel_num, color);
    case LED_STRIP_BACKEND_DIRECT:
        return ESP_ERR_NOT_SUPPORTED; /* no pixel addressing */
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_strip_set_channel(led_strip_handle_t *handle,
                                 uint8_t channel_idx,
                                 uint8_t brightness,
                                 uint8_t red, uint8_t green, uint8_t blue)
{
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel_idx >= NUM_LED_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_set_channel(handle, channel_idx, brightness, red, green, blue);
    case LED_STRIP_BACKEND_DOTSTAR:
        return s_dotstar_set_channel(handle, channel_idx, brightness, red, green, blue);
    case LED_STRIP_BACKEND_DIRECT:
        return s_direct_set_channel(handle, channel_idx, brightness, red, green, blue);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_strip_refresh(led_strip_handle_t *handle)
{
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_refresh(handle);
    case LED_STRIP_BACKEND_DOTSTAR:
        return s_dotstar_refresh(handle);
    case LED_STRIP_BACKEND_DIRECT:
        return s_direct_refresh(handle);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_strip_clear(led_strip_handle_t *handle)
{
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_clear(handle);
    case LED_STRIP_BACKEND_DOTSTAR:
        return s_dotstar_clear(handle);
    case LED_STRIP_BACKEND_DIRECT:
        return s_direct_clear(handle);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_strip_set_all(led_strip_handle_t *handle,
                             uint8_t red, uint8_t green, uint8_t blue)
{
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_set_all(handle, red, green, blue);
    case LED_STRIP_BACKEND_DOTSTAR:
        return s_dotstar_set_all(handle, red, green, blue);
    case LED_STRIP_BACKEND_DIRECT:
        return ESP_ERR_NOT_SUPPORTED; /* no pixel concept in direct mode */
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* 3.5  Gate vu_meter / spectrum for direct mode */

esp_err_t led_strip_vu_meter(led_strip_handle_t *handle,
                              uint8_t level_left, uint8_t level_right,
                              uint8_t brightness, uint8_t color_mode)
{
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_vu_meter(handle, level_left, level_right, brightness, color_mode);
    case LED_STRIP_BACKEND_DOTSTAR:
        /* vu_meter pixel-fill logic is backend-agnostic (works on working_buffer).
         * Use the shared fill helper then the DotStar-specific refresh. */
        {
            esp_err_t _ret = s_addressable_vu_meter_fill(handle, level_left, level_right,
                                                          brightness);
            if (_ret != ESP_OK) return _ret;
            return s_dotstar_refresh(handle);
        }
    case LED_STRIP_BACKEND_DIRECT: {
        static bool s_vu_warned = false;
        if (!s_vu_warned) {
            ESP_LOGW(TAG, "vu_meter not supported in DIRECT mode (no per-pixel addressing)");
            s_vu_warned = true;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_strip_spectrum(led_strip_handle_t *handle,
                              uint8_t *spectrum_data, size_t data_length,
                              uint8_t brightness, uint8_t style)
{
    if (!handle || !handle->initialized || !spectrum_data) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        return s_neopixel_spectrum(handle, spectrum_data, data_length, brightness, style);
    case LED_STRIP_BACKEND_DOTSTAR:
        /* spectrum pixel-fill logic is backend-agnostic; use shared fill + dotstar refresh. */
        {
            esp_err_t _ret = s_addressable_spectrum_fill(handle, spectrum_data,
                                                          data_length, brightness);
            if (_ret != ESP_OK) return _ret;
            return s_dotstar_refresh(handle);
        }
    case LED_STRIP_BACKEND_DIRECT: {
        static bool s_spectrum_warned = false;
        if (!s_spectrum_warned) {
            ESP_LOGW(TAG, "spectrum not supported in DIRECT mode (no per-pixel addressing)");
            s_spectrum_warned = true;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t led_strip_deinit(led_strip_handle_t *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (handle->backend) {
    case LED_STRIP_BACKEND_NEOPIXEL:
        s_neopixel_deinit(handle);
        break;
    case LED_STRIP_BACKEND_DOTSTAR:
        s_dotstar_deinit(handle);
        break;
    case LED_STRIP_BACKEND_DIRECT:
        s_direct_deinit(handle);
        break;
    default:
        break;
    }

    /* Common teardown — only the mutex and the handle allocation itself remain
     * (backend deinit functions free their own working/display/channel_map buffers). */
    if (handle->access_mutex) {
        vSemaphoreDelete(handle->access_mutex);
        handle->access_mutex = NULL;
    }

    free(handle);

    ESP_LOGI(TAG, "LED strip deinitialized");
    return ESP_OK;
}

/* =========================================================================
 * 3.4  Public getters  (correct from Step 2; verified here)
 * ========================================================================= */

led_strip_backend_t led_strip_get_backend(const led_strip_handle_t *handle)
{
    if (!handle) {
        return LED_STRIP_BACKEND_NEOPIXEL; /* safe default */
    }
    return handle->backend;
}

uint32_t led_strip_get_pixel_count(const led_strip_handle_t *handle)
{
    if (!handle) {
        return 0;
    }
    /* DIRECT mode always exposes exactly NUM_LED_CHANNELS logical channels as pixel count */
    if (handle->backend == LED_STRIP_BACKEND_DIRECT) {
        return NUM_LED_CHANNELS;
    }
    return handle->length;
}

bool led_strip_supports_pixel_addressing(const led_strip_handle_t *handle)
{
    if (!handle) {
        return false;
    }
    return (handle->backend != LED_STRIP_BACKEND_DIRECT);
}

/* =========================================================================
 * Internal RMT helpers (neopixel only)
 * ========================================================================= */

static esp_err_t s_neopixel_setup_rmt(led_strip_handle_t *handle)
{
    ESP_LOGI(TAG, "Setting up RMT TX channel for GPIO %d", handle->gpio_pin);

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = handle->gpio_pin,
        // Claim all 8 ESP32 RMT memory blocks for this channel (8 x 64 = 512).
        // For a 48-LED strip we need 1152+1 symbols per frame; bigger FIFO drops
        // the CPU-driven refill count from ~18 to ~2 per frame, with each refill
        // window now ~320 us (was ~40 us) -- comfortably above WiFi interrupt
        // bursts. No other channel uses RMT in this project, so claiming the
        // whole pool is free. ESP32 classic doesn't support `with_dma = true`
        // (lacks GDMA hardware); this is the strongest software-only mitigation.
        .mem_block_symbols = 512,
        .resolution_hz = 10000000, // 10MHz resolution (100ns per tick)
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_chan_config, &handle->rmt_tx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_enable(handle->rmt_tx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(ret));
        return ret;
    }

    rmt_copy_encoder_config_t encoder_config = {};
    ret = rmt_new_copy_encoder(&encoder_config, &handle->rmt_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT copy encoder: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "RMT TX channel and encoder created successfully");
    return ESP_OK;
}

static esp_err_t s_neopixel_encode_data(led_strip_handle_t *handle,
                                         rmt_symbol_word_t *symbols)
{
    const led_timing_t *timing = &s_neopixel_timing;
    size_t symbol_idx = 0;

    for (uint32_t led = 0; led < handle->length; led++) {
        led_color_t *pixel = &handle->display_buffer[led];

        /* WS2812 protocol transmits in GRB order */
        uint8_t color_data[3] = { pixel->green, pixel->red, pixel->blue };

        for (int byte_idx = 0; byte_idx < 3; byte_idx++) {
            uint8_t byte = color_data[byte_idx];

            for (int bit = 7; bit >= 0; bit--) {
                if (byte & (1 << bit)) {
                    symbols[symbol_idx].level0    = 1;
                    symbols[symbol_idx].duration0 = timing->t1h_ticks;
                    symbols[symbol_idx].level1    = 0;
                    symbols[symbol_idx].duration1 = timing->t1l_ticks;
                } else {
                    symbols[symbol_idx].level0    = 1;
                    symbols[symbol_idx].duration0 = timing->t0h_ticks;
                    symbols[symbol_idx].level1    = 0;
                    symbols[symbol_idx].duration1 = timing->t0l_ticks;
                }
                symbol_idx++;
            }
        }
    }

    /* Add reset pulse -- 280 us low */
    symbols[symbol_idx].level0    = 0;
    symbols[symbol_idx].duration0 = timing->reset_ticks;
    symbols[symbol_idx].level1    = 0;
    symbols[symbol_idx].duration1 = 0;

    return ESP_OK;
}
