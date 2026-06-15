#include "led_matrix_example.h"
#include "led_strip.h"
#include "audio_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char* TAG = "led_matrix";
static led_strip_handle_t *matrix_handle = NULL;

// LED flicker state management
typedef struct {
    bool active;                    // Flicker currently running
    float frequency;               // Current flicker frequency (Hz)
    uint8_t duty_cycle;           // On-time percentage (0-100)
    uint8_t brightness;           // Maximum brightness (0-100)
    uint8_t red, green, blue;     // LED colors when ON
    esp_timer_handle_t timer;     // High-precision timer for flicker
    bool led_state;               // Current ON/OFF state
    uint64_t cycle_start_time_us; // Timestamp of current cycle start
} led_flicker_state_t;

static led_flicker_state_t flicker_state = {
    .active = false,
    .frequency = 0.0f,
    .duty_cycle = 50,
    .brightness = 100,
    .red = 255,
    .green = 255,
    .blue = 255,
    .timer = NULL,
    .led_state = false,
    .cycle_start_time_us = 0
};

/**
 * @brief Convert 2D matrix coordinates to linear LED index (zig-zag pattern)
 *
 * @param x Column (0-11)
 * @param y Row (0-3)
 * @return Linear LED index (0-47)
 */
static uint32_t matrix_xy_to_index(uint8_t x, uint8_t y)
{
    if (x >= LED_MATRIX_WIDTH || y >= LED_MATRIX_HEIGHT) {
        return LED_STRIP_COUNT; // Invalid index
    }

    uint32_t index;

    // Zig-zag pattern: even columns go down, odd columns go up
    if (x % 2 == 0) {
        // Even column: top to bottom (normal)
        index = x * LED_MATRIX_HEIGHT + y;
    } else {
        // Odd column: bottom to top (reversed)
        index = x * LED_MATRIX_HEIGHT + (LED_MATRIX_HEIGHT - 1 - y);
    }

    return index;
}

/**
 * @brief Initialize 12x4 LED matrix on GPIO 12
 */
esp_err_t led_matrix_init(void)
{
    ESP_LOGI(TAG, "Initializing 12x4 LED matrix on GPIO %d", LED_STRIP_GPIO);

    led_strip_config_t config = {
        .type = LED_STRIP_WS2812,       // WS2812B compatible
        .length = LED_STRIP_COUNT,      // 48 LEDs (12x4)
        .gpio_pin = LED_STRIP_GPIO      // GPIO 12
    };

    esp_err_t ret = led_strip_init(&config, &matrix_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED matrix: %s", esp_err_to_name(ret));
        return ret;
    }

    // Clear all LEDs
    led_strip_clear(matrix_handle);
    led_strip_refresh(matrix_handle);

    ESP_LOGI(TAG, "LED matrix initialized successfully");
    return ESP_OK;
}

/**
 * @brief Set LED at matrix coordinates (x,y) with color order correction
 *
 * Many WS2812 strips use GRB order instead of RGB
 */
esp_err_t led_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!matrix_handle) {
        ESP_LOGE(TAG, "Matrix handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t index = matrix_xy_to_index(x, y);
    if (index >= LED_STRIP_COUNT) {
        ESP_LOGE(TAG, "Invalid LED index %lu for coordinates (%d,%d)", index, x, y);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Setting LED[%d,%d] (index %lu) to RGB(%d,%d,%d)", x, y, index, red, green, blue);

    // Use RGB color order first, then try GRB if colors are wrong
    return led_strip_set_pixel_rgb(matrix_handle, index, red, green, blue);
}

/**
 * @brief Update matrix display
 */
esp_err_t led_matrix_refresh(void)
{
    if (!matrix_handle) {
        ESP_LOGE(TAG, "Matrix handle not initialized for refresh");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Refreshing LED matrix display");
    return led_strip_refresh(matrix_handle);
}

/**
 * @brief Clear entire matrix
 */
esp_err_t led_matrix_clear(void)
{
    if (!matrix_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = led_strip_clear(matrix_handle);
    if (ret == ESP_OK) {
        ret = led_strip_refresh(matrix_handle);
    }
    return ret;
}

/**
 * @brief Display VU meter on matrix (12 columns for levels)
 */
esp_err_t led_matrix_vu_meter(float left_level, float right_level)
{
    if (!matrix_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clear matrix first
    led_strip_clear(matrix_handle);

    // Convert levels (0.0-1.0) to column count (0-12)
    uint8_t left_cols = (uint8_t)(left_level * LED_MATRIX_WIDTH);
    uint8_t right_cols = (uint8_t)(right_level * LED_MATRIX_WIDTH);

    // Draw left channel (top 2 rows)
    for (uint8_t x = 0; x < left_cols && x < LED_MATRIX_WIDTH; x++) {
        uint8_t intensity = (x * 255) / LED_MATRIX_WIDTH;
        led_matrix_set_pixel(x, 0, intensity, 255 - intensity, 0);  // Green to red
        led_matrix_set_pixel(x, 1, intensity, 255 - intensity, 0);
    }

    // Draw right channel (bottom 2 rows)
    for (uint8_t x = 0; x < right_cols && x < LED_MATRIX_WIDTH; x++) {
        uint8_t intensity = (x * 255) / LED_MATRIX_WIDTH;
        led_matrix_set_pixel(x, 2, intensity, 255 - intensity, 0);  // Green to red
        led_matrix_set_pixel(x, 3, intensity, 255 - intensity, 0);
    }

    return led_matrix_refresh();
}

/**
 * @brief Demo pattern - test all LEDs in sequence
 */
esp_err_t led_matrix_test_pattern(void)
{
    if (!matrix_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Running LED matrix test pattern");

    // Test each LED in the zig-zag pattern
    for (uint8_t x = 0; x < LED_MATRIX_WIDTH; x++) {
        for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; y++) {
            // Set current pixel to white
            led_matrix_set_pixel(x, y, 255, 255, 255);
            led_matrix_refresh();

            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay

            // Turn off current pixel
            led_matrix_set_pixel(x, y, 0, 0, 0);
        }
    }

    // Final clear
    led_matrix_clear();

    ESP_LOGI(TAG, "Test pattern complete");
    return ESP_OK;
}

/**
 * @brief Timer callback for LED flicker control
 */
static void IRAM_ATTR led_flicker_timer_callback(void* arg) {
    if (!flicker_state.active || !matrix_handle) {
        return;
    }

    uint64_t now_us = esp_timer_get_time();
    uint64_t cycle_duration_us = (uint64_t)(1000000.0f / flicker_state.frequency);
    uint64_t elapsed_us = now_us - flicker_state.cycle_start_time_us;

    // Calculate duty cycle timing
    uint64_t on_time_us = (cycle_duration_us * flicker_state.duty_cycle) / 100;

    bool should_be_on = (elapsed_us < on_time_us);

    // Update LED state if it needs to change
    if (should_be_on != flicker_state.led_state) {
        flicker_state.led_state = should_be_on;

        if (should_be_on) {
            // Turn all LEDs ON with specified color and brightness
            uint8_t scaled_red = (flicker_state.red * flicker_state.brightness) / 100;
            uint8_t scaled_green = (flicker_state.green * flicker_state.brightness) / 100;
            uint8_t scaled_blue = (flicker_state.blue * flicker_state.brightness) / 100;
            led_strip_set_all(matrix_handle, scaled_red, scaled_green, scaled_blue);
        } else {
            // Turn all LEDs OFF
            led_strip_set_all(matrix_handle, 0, 0, 0);
        }
        led_strip_refresh(matrix_handle);
    }

    // Reset cycle when complete
    if (elapsed_us >= cycle_duration_us) {
        flicker_state.cycle_start_time_us = now_us;
    }
}

/**
 * @brief Start LED flicker for meditation/brainwave entrainment
 */
esp_err_t led_matrix_start_flicker(float frequency, uint8_t duty_cycle, uint8_t brightness) {
    if (!matrix_handle) {
        ESP_LOGE(TAG, "Matrix handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (frequency <= 0.0f || frequency > 100.0f) {
        ESP_LOGE(TAG, "Invalid flicker frequency: %.1f Hz (must be 0.1-100.0)", frequency);
        return ESP_ERR_INVALID_ARG;
    }

    if (duty_cycle > 100) {
        ESP_LOGE(TAG, "Invalid duty cycle: %d%% (must be 0-100)", duty_cycle);
        return ESP_ERR_INVALID_ARG;
    }

    if (brightness > 100) {
        ESP_LOGE(TAG, "Invalid brightness: %d%% (must be 0-100)", brightness);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop existing flicker if active
    led_matrix_stop_flicker();

    // Configure flicker parameters
    flicker_state.frequency = frequency;
    flicker_state.duty_cycle = duty_cycle;
    flicker_state.brightness = brightness;
    flicker_state.cycle_start_time_us = esp_timer_get_time();
    flicker_state.led_state = false;

    // Create high-precision timer for flicker
    esp_timer_create_args_t timer_args = {
        .callback = led_flicker_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_flicker",
        .skip_unhandled_events = true
    };

    esp_err_t ret = esp_timer_create(&timer_args, &flicker_state.timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create flicker timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start timer with high frequency for smooth flicker
    // Use 1000μs (1kHz) for frequencies up to 50Hz, 500μs (2kHz) for higher frequencies
    uint64_t timer_period_us = (frequency > 50.0f) ? 500 : 1000;
    ret = esp_timer_start_periodic(flicker_state.timer, timer_period_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start flicker timer: %s", esp_err_to_name(ret));
        esp_timer_delete(flicker_state.timer);
        flicker_state.timer = NULL;
        return ret;
    }

    flicker_state.active = true;
    ESP_LOGI(TAG, "Started LED flicker: %.1f Hz, %d%% duty, %d%% brightness",
             frequency, duty_cycle, brightness);

    return ESP_OK;
}

/**
 * @brief Stop LED flicker
 */
esp_err_t led_matrix_stop_flicker(void) {
    if (!flicker_state.active) {
        return ESP_OK; // Already stopped
    }

    flicker_state.active = false;

    if (flicker_state.timer) {
        esp_timer_stop(flicker_state.timer);
        esp_timer_delete(flicker_state.timer);
        flicker_state.timer = NULL;
    }

    // Turn off all LEDs
    if (matrix_handle) {
        led_strip_set_all(matrix_handle, 0, 0, 0);
        led_strip_refresh(matrix_handle);
    }

    ESP_LOGI(TAG, "LED flicker stopped");
    return ESP_OK;
}

/**
 * @brief Update flicker parameters while running
 */
esp_err_t led_matrix_update_flicker_params(float frequency, uint8_t duty_cycle, uint8_t brightness) {
    if (!flicker_state.active) {
        ESP_LOGW(TAG, "Cannot update flicker params - flicker not active");
        return ESP_ERR_INVALID_STATE;
    }

    if (frequency <= 0.0f || frequency > 100.0f) {
        ESP_LOGE(TAG, "Invalid flicker frequency: %.1f Hz", frequency);
        return ESP_ERR_INVALID_ARG;
    }

    if (duty_cycle > 100) {
        ESP_LOGE(TAG, "Invalid duty cycle: %d%%", duty_cycle);
        return ESP_ERR_INVALID_ARG;
    }

    if (brightness > 100) {
        ESP_LOGE(TAG, "Invalid brightness: %d%%", brightness);
        return ESP_ERR_INVALID_ARG;
    }

    // Update parameters atomically
    flicker_state.frequency = frequency;
    flicker_state.duty_cycle = duty_cycle;
    flicker_state.brightness = brightness;
    flicker_state.cycle_start_time_us = esp_timer_get_time(); // Reset cycle timing

    ESP_LOGI(TAG, "Updated flicker params: %.1f Hz, %d%% duty, %d%% brightness",
             frequency, duty_cycle, brightness);

    return ESP_OK;
}

/**
 * @brief Set flicker color (default: white)
 */
esp_err_t led_matrix_set_flicker_color(uint8_t red, uint8_t green, uint8_t blue) {
    flicker_state.red = red;
    flicker_state.green = green;
    flicker_state.blue = blue;

    ESP_LOGI(TAG, "Set flicker color to RGB(%d,%d,%d)", red, green, blue);
    return ESP_OK;
}

/**
 * @brief Check if LED flicker is currently active
 */
bool led_matrix_is_flickering(void) {
    return flicker_state.active;
}

/**
 * @brief Get current flicker frequency
 */
float led_matrix_get_current_frequency(void) {
    return flicker_state.active ? flicker_state.frequency : 0.0f;
}
