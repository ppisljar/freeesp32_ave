#include "led_strip.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include <stdlib.h>
#include <string.h>

static const char* TAG = "led_strip";

// WS2812 timing configuration (all in nanoseconds; RMT clock is 10 MHz).
#define WS2812_T0H_NS    350    // 0 bit high time (ns)
#define WS2812_T0L_NS    900    // 0 bit low time (ns)
#define WS2812_T1H_NS    900    // 1 bit high time (ns)
#define WS2812_T1L_NS    350    // 1 bit low time (ns)
// Reset/latch low time in NANOSECONDS. WS2812B (V1-V3) requires ≥50 µs;
// WS2812B-V4+ requires ≥280 µs. We use 280 µs to cover all silicon variants.
// BUG HISTORY: previously defined as WS2812_RESET_US = 50 and used as a RAW
// TICK count (i.e. 50 ticks × 100 ns/tick = 5 µs), 10× too short to latch.
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

// Timing configurations for different LED types
static const led_timing_t led_timings[] = {
    [LED_STRIP_WS2812] = {
        .t0h_ticks = NS_TO_RMT_TICKS(WS2812_T0H_NS),
        .t0l_ticks = NS_TO_RMT_TICKS(WS2812_T0L_NS),
        .t1h_ticks = NS_TO_RMT_TICKS(WS2812_T1H_NS),
        .t1l_ticks = NS_TO_RMT_TICKS(WS2812_T1L_NS),
        .reset_ticks = NS_TO_RMT_TICKS(WS2812_RESET_NS)
    },
    // SK6812 and APA106 have similar timing to WS2812
    [LED_STRIP_SK6812] = {
        .t0h_ticks = NS_TO_RMT_TICKS(WS2812_T0H_NS),
        .t0l_ticks = NS_TO_RMT_TICKS(WS2812_T0L_NS),
        .t1h_ticks = NS_TO_RMT_TICKS(WS2812_T1H_NS),
        .t1l_ticks = NS_TO_RMT_TICKS(WS2812_T1L_NS),
        .reset_ticks = NS_TO_RMT_TICKS(WS2812_RESET_NS)
    },
    [LED_STRIP_APA106] = {
        .t0h_ticks = NS_TO_RMT_TICKS(WS2812_T0H_NS),
        .t0l_ticks = NS_TO_RMT_TICKS(WS2812_T0L_NS),
        .t1h_ticks = NS_TO_RMT_TICKS(WS2812_T1H_NS),
        .t1l_ticks = NS_TO_RMT_TICKS(WS2812_T1L_NS),
        .reset_ticks = NS_TO_RMT_TICKS(WS2812_RESET_NS)
    }
};

// Forward declarations
static esp_err_t led_strip_setup_rmt(led_strip_handle_t *handle);
static esp_err_t led_strip_encode_data(led_strip_handle_t *handle, rmt_symbol_word_t *symbols);

esp_err_t led_strip_init(const led_strip_config_t *config, led_strip_handle_t **handle) {
    if (!config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->type >= LED_STRIP_MAX_TYPES) {
        ESP_LOGE(TAG, "Invalid LED strip type: %d", config->type);
        return ESP_ERR_INVALID_ARG;
    }

    if (config->length == 0 || config->length > 1000) {
        ESP_LOGE(TAG, "Invalid LED count: %lu (must be 1-1000)", config->length);
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate handle structure
    led_strip_handle_t *strip = calloc(1, sizeof(led_strip_handle_t));
    if (!strip) {
        ESP_LOGE(TAG, "Failed to allocate LED strip handle");
        return ESP_ERR_NO_MEM;
    }

    // Copy configuration
    strip->type = config->type;
    strip->length = config->length;
    strip->gpio_pin = config->gpio_pin;

    // Allocate LED buffers
    strip->working_buffer = calloc(config->length, sizeof(led_color_t));
    strip->display_buffer = calloc(config->length, sizeof(led_color_t));

    if (!strip->working_buffer || !strip->display_buffer) {
        ESP_LOGE(TAG, "Failed to allocate LED buffers");
        led_strip_deinit(strip);
        return ESP_ERR_NO_MEM;
    }

    // Pre-allocate RMT symbol buffer to avoid malloc/free on each refresh
    strip->symbol_buffer_size = config->length * SYMBOLS_PER_LED + 1; // +1 for reset
    strip->symbol_buffer = malloc(strip->symbol_buffer_size * sizeof(rmt_symbol_word_t));

    if (!strip->symbol_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RMT symbol buffer (%zu symbols)", strip->symbol_buffer_size);
        led_strip_deinit(strip);
        return ESP_ERR_NO_MEM;
    }

    // Create mutex for thread safety
    strip->access_mutex = xSemaphoreCreateMutex();
    if (!strip->access_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        led_strip_deinit(strip);
        return ESP_ERR_NO_MEM;
    }

    // Setup RMT for LED strip control
    esp_err_t ret = led_strip_setup_rmt(strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup RMT: %s", esp_err_to_name(ret));
        led_strip_deinit(strip);
        return ret;
    }

    strip->initialized = true;
    *handle = strip;

    ESP_LOGI(TAG, "LED strip initialized: type=%d, length=%lu, gpio=%d",
             config->type, config->length, config->gpio_pin);

    return ESP_OK;
}

esp_err_t led_strip_set_pixel_rgb(led_strip_handle_t *handle, uint32_t pixel_num,
                                  uint8_t red, uint8_t green, uint8_t blue) {
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Invalid handle or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (pixel_num >= handle->length) {
        ESP_LOGE(TAG, "Pixel %lu out of range (max %lu)", pixel_num, handle->length - 1);
        return ESP_ERR_INVALID_ARG;
    }

    // Note: ESP_LOG removed from ISR execution path to prevent lock violations

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    handle->working_buffer[pixel_num].red = red;
    handle->working_buffer[pixel_num].green = green;
    handle->working_buffer[pixel_num].blue = blue;

    xSemaphoreGive(handle->access_mutex);

    return ESP_OK;
}

esp_err_t led_strip_set_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                    const led_color_t *color) {
    if (!color) {
        return ESP_ERR_INVALID_ARG;
    }

    return led_strip_set_pixel_rgb(handle, pixel_num, color->red, color->green, color->blue);
}

esp_err_t led_strip_get_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                    led_color_t *color) {
    if (!handle || !handle->initialized || !color) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pixel_num >= handle->length) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    *color = handle->display_buffer[pixel_num];

    xSemaphoreGive(handle->access_mutex);

    return ESP_OK;
}

esp_err_t led_strip_refresh(led_strip_handle_t *handle) {
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    // Wait for the previous RMT TX to drain before we overwrite symbol_buffer.
    // rmt_transmit() is asynchronous — without this barrier, the next encode
    // would write fresh symbols on top of bytes the DMA is still reading,
    // producing GRB-byte-axis splicing (visible as green-tinted random pixels).
    rmt_tx_wait_all_done(handle->rmt_tx_channel, portMAX_DELAY);

    // Copy working buffer to display buffer
    memcpy(handle->display_buffer, handle->working_buffer,
           handle->length * sizeof(led_color_t));

    // Use pre-allocated RMT symbols buffer
    size_t symbol_count = handle->length * SYMBOLS_PER_LED + 1; // +1 for reset
    rmt_symbol_word_t *symbols = handle->symbol_buffer;

    if (!symbols || symbol_count > handle->symbol_buffer_size) {
        xSemaphoreGive(handle->access_mutex);
        // Note: ESP_LOG removed from potential ISR execution path to prevent lock violations
        return ESP_ERR_INVALID_STATE;
    }

    // Encode LED data to RMT symbols
    esp_err_t ret = led_strip_encode_data(handle, symbols);
    if (ret != ESP_OK) {
        xSemaphoreGive(handle->access_mutex);
        return ret;
    }

    // Transmit the encoded data using RMT TX
    // Note: ESP_LOG removed from ISR execution path to prevent lock violations

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,  // No loop
    };

    esp_err_t tx_ret = rmt_transmit(handle->rmt_tx_channel, handle->rmt_encoder, symbols, symbol_count * sizeof(rmt_symbol_word_t), &tx_config);
    if (tx_ret != ESP_OK) {
        // Note: ESP_LOG removed from ISR execution path to prevent lock violations
        ret = tx_ret;
    }

    // Symbol buffer is pre-allocated, no need to free
    xSemaphoreGive(handle->access_mutex);

    return ESP_OK;
}

esp_err_t led_strip_clear(led_strip_handle_t *handle) {
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    // Clear working buffer
    memset(handle->working_buffer, 0, handle->length * sizeof(led_color_t));

    xSemaphoreGive(handle->access_mutex);

    // Refresh to apply changes
    return led_strip_refresh(handle);
}

esp_err_t led_strip_set_all(led_strip_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue) {
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    for (uint32_t i = 0; i < handle->length; i++) {
        handle->working_buffer[i].red = red;
        handle->working_buffer[i].green = green;
        handle->working_buffer[i].blue = blue;
    }

    xSemaphoreGive(handle->access_mutex);

    return led_strip_refresh(handle);
}

esp_err_t led_strip_vu_meter(led_strip_handle_t *handle, uint8_t level_left,
                             uint8_t level_right, uint8_t brightness, uint8_t color_mode) {
    if (!handle || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (level_left > 100 || level_right > 100 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    // Clear all LEDs first
    memset(handle->working_buffer, 0, handle->length * sizeof(led_color_t));

    uint32_t half_length = handle->length / 2;

    // Left channel (first half of strip)
    uint32_t left_pixels = (level_left * half_length) / 100;
    for (uint32_t i = 0; i < left_pixels; i++) {
        uint8_t intensity = (brightness * 255) / 100;
        handle->working_buffer[i].red = intensity;
        handle->working_buffer[i].green = 0;
        handle->working_buffer[i].blue = 0;
    }

    // Right channel (second half of strip)
    uint32_t right_pixels = (level_right * half_length) / 100;
    for (uint32_t i = 0; i < right_pixels; i++) {
        uint32_t pixel_idx = half_length + i;
        if (pixel_idx < handle->length) {
            uint8_t intensity = (brightness * 255) / 100;
            handle->working_buffer[pixel_idx].red = 0;
            handle->working_buffer[pixel_idx].green = intensity;
            handle->working_buffer[pixel_idx].blue = 0;
        }
    }

    xSemaphoreGive(handle->access_mutex);

    return led_strip_refresh(handle);
}

esp_err_t led_strip_spectrum(led_strip_handle_t *handle, uint8_t *spectrum_data,
                            size_t data_length, uint8_t brightness, uint8_t style) {
    if (!handle || !handle->initialized || !spectrum_data) {
        return ESP_ERR_INVALID_ARG;
    }

    if (brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(handle->access_mutex, portMAX_DELAY);

    // Clear all LEDs first
    memset(handle->working_buffer, 0, handle->length * sizeof(led_color_t));

    // Map spectrum data to LED strip
    for (uint32_t i = 0; i < handle->length; i++) {
        uint32_t data_idx = (i * data_length) / handle->length;
        if (data_idx < data_length) {
            uint8_t level = spectrum_data[data_idx];
            uint8_t intensity = (level * brightness) / 100;

            // Create rainbow effect based on position
            if (i < handle->length / 3) {
                // Red to Yellow
                handle->working_buffer[i].red = intensity;
                handle->working_buffer[i].green = (intensity * i * 3) / handle->length;
                handle->working_buffer[i].blue = 0;
            } else if (i < 2 * handle->length / 3) {
                // Yellow to Green
                handle->working_buffer[i].red = intensity - ((intensity * (i - handle->length/3) * 3) / handle->length);
                handle->working_buffer[i].green = intensity;
                handle->working_buffer[i].blue = 0;
            } else {
                // Green to Blue
                handle->working_buffer[i].red = 0;
                handle->working_buffer[i].green = intensity - ((intensity * (i - 2*handle->length/3) * 3) / handle->length);
                handle->working_buffer[i].blue = intensity;
            }
        }
    }

    xSemaphoreGive(handle->access_mutex);

    return led_strip_refresh(handle);
}

esp_err_t led_strip_deinit(led_strip_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear LEDs before deinitialization
    if (handle->initialized) {
        led_strip_clear(handle);
    }

    // Clean up RMT encoder
    if (handle->rmt_encoder) {
        rmt_del_encoder(handle->rmt_encoder);
    }

    // Clean up RMT channel
    if (handle->rmt_tx_channel) {
        rmt_disable(handle->rmt_tx_channel);
        rmt_del_channel(handle->rmt_tx_channel);
    }

    // Free buffers
    if (handle->working_buffer) {
        free(handle->working_buffer);
    }
    if (handle->display_buffer) {
        free(handle->display_buffer);
    }
    if (handle->symbol_buffer) {
        free(handle->symbol_buffer);
    }

    // Delete mutex
    if (handle->access_mutex) {
        vSemaphoreDelete(handle->access_mutex);
    }

    // Free handle
    free(handle);

    ESP_LOGI(TAG, "LED strip deinitialized");

    return ESP_OK;
}

// Internal helper functions

static esp_err_t led_strip_setup_rmt(led_strip_handle_t *handle) {
    ESP_LOGI(TAG, "Setting up RMT TX channel for GPIO %d", handle->gpio_pin);

    // RMT TX channel configuration
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = handle->gpio_pin,
        // Claim all 8 ESP32 RMT memory blocks for this channel (8 × 64 = 512).
        // For a 48-LED strip we need 1152+1 symbols per frame; bigger FIFO drops
        // the CPU-driven refill count from ~18 to ~2 per frame, with each refill
        // window now ~320 µs (was ~40 µs) — comfortably above WiFi interrupt
        // bursts. No other channel uses RMT in this project, so claiming the
        // whole pool is free. ESP32 classic doesn't support `with_dma = true`
        // (lacks GDMA hardware); this is the strongest software-only mitigation.
        .mem_block_symbols = 512,
        .resolution_hz = 10000000, // 10MHz resolution (100ns per tick) - FIXED!
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

    // Create RMT copy encoder for transmitting pre-encoded data
    rmt_copy_encoder_config_t encoder_config = {};
    ret = rmt_new_copy_encoder(&encoder_config, &handle->rmt_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT copy encoder: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "RMT TX channel and encoder created successfully");
    return ESP_OK;
}

static esp_err_t led_strip_encode_data(led_strip_handle_t *handle, rmt_symbol_word_t *symbols) {
    const led_timing_t *timing = &led_timings[handle->type];
    size_t symbol_idx = 0;

    // Note: ESP_LOG removed from ISR execution path to prevent lock violations

    // Encode each LED
    for (uint32_t led = 0; led < handle->length; led++) {
        led_color_t *pixel = &handle->display_buffer[led];

        // LEDs use GRB order (Green, Red, Blue) - WS2812 standard
        uint8_t color_data[3] = {pixel->green, pixel->red, pixel->blue};

        // Note: ESP_LOG removed from ISR execution path to prevent lock violations

        // Encode each color byte
        for (int byte_idx = 0; byte_idx < 3; byte_idx++) {
            uint8_t byte = color_data[byte_idx];

            // Encode each bit (MSB first)
            for (int bit = 7; bit >= 0; bit--) {
                if (byte & (1 << bit)) {
                    // Encode '1' bit
                    symbols[symbol_idx].level0 = 1;
                    symbols[symbol_idx].duration0 = timing->t1h_ticks;
                    symbols[symbol_idx].level1 = 0;
                    symbols[symbol_idx].duration1 = timing->t1l_ticks;
                } else {
                    // Encode '0' bit
                    symbols[symbol_idx].level0 = 1;
                    symbols[symbol_idx].duration0 = timing->t0h_ticks;
                    symbols[symbol_idx].level1 = 0;
                    symbols[symbol_idx].duration1 = timing->t0l_ticks;
                }
                symbol_idx++;
            }
        }
    }

    // Add reset pulse
    symbols[symbol_idx].level0 = 0;
    symbols[symbol_idx].duration0 = timing->reset_ticks;
    symbols[symbol_idx].level1 = 0;
    symbols[symbol_idx].duration1 = 0;

    return ESP_OK;
}
