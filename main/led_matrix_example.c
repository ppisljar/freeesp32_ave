#include "led_matrix_example.h"
#include "led_strip.h"
#include "audio_config.h"
#include "isr_profiling.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gptimer.h"

static const char* TAG = "led_matrix";
static led_strip_handle_t *matrix_handle = NULL;

// LED flicker state management with dedicated hardware timer
typedef struct {
    volatile bool active;              // Flicker currently running (read by ISR)
    uint32_t frequency_milliHz;        // Current flicker frequency (Hz * 1000 for precision)
    volatile uint8_t duty_cycle;       // On-time percentage (0-100); written by task, read by ISR
    volatile uint8_t brightness;       // Maximum brightness (0-100); written by task, read by ISR
    volatile uint8_t red, green, blue; // LED colors when ON; written by task, read by flicker task
    gptimer_handle_t flicker_timer;    // Dedicated hardware timer for LED flicker
    volatile bool led_state;           // Target ON/OFF state; written by ISR, read by flicker task
    volatile bool led_dirty;           // ISR sets true when led_state changes; task clears it
    volatile uint64_t cycle_start_time_us; // Timestamp of current cycle start (ISR-owned, ISR only writes)
} led_flicker_state_t;

static led_flicker_state_t flicker_state = {
    .active = false,
    .frequency_milliHz = 0,
    .duty_cycle = 50,
    .brightness = 100,
    .red = 255,
    .green = 255,
    .blue = 255,
    .flicker_timer = NULL,
    .led_state = false,
    .led_dirty = false,
    .cycle_start_time_us = 0
};

// Task handle for the LED flicker output task (Option B: ISR notifies task)
static TaskHandle_t led_flicker_task_handle = NULL;

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
    // Note: ESP_LOG removed from ISR execution path to prevent lock violations
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

    // Convert levels (0.0-1.0) to column count (0-12) using integer arithmetic
    // Multiply by 1000 for precision, then scale to LED_MATRIX_WIDTH
    uint32_t left_level_int = (uint32_t)(left_level * 1000.0f);
    uint32_t right_level_int = (uint32_t)(right_level * 1000.0f);
    uint8_t left_cols = (left_level_int * LED_MATRIX_WIDTH) / 1000;
    uint8_t right_cols = (right_level_int * LED_MATRIX_WIDTH) / 1000;

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
 * @brief Hardware timer alarm callback for LED flicker control (Option B: minimal ISR)
 *
 * Only determines the target ON/OFF state via integer arithmetic and notifies
 * led_flicker_task to do the actual LED I/O. rmt_transmit() MUST NOT be called
 * from this ISR — it is task-context-only per the ESP-IDF RMT driver contract.
 */
static bool IRAM_ATTR led_flicker_timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data) {
    ISR_PROFILE_BEGIN(2);

    if (!flicker_state.active || !matrix_handle) {
        ISR_PROFILE_END(2);
        return false;
    }

    uint64_t now_us = esp_timer_get_time();

    // cycle_duration_us = 1,000,000,000 / frequency_milliHz  (all integer, no FPU)
    uint64_t cycle_duration_us = (1000000ULL * 1000ULL) / flicker_state.frequency_milliHz;
    uint64_t elapsed_us = now_us - flicker_state.cycle_start_time_us;

    uint64_t on_time_us = (cycle_duration_us * flicker_state.duty_cycle) / 100;
    bool should_be_on = (elapsed_us < on_time_us);

    if (should_be_on != flicker_state.led_state) {
        flicker_state.led_state = should_be_on;
        flicker_state.led_dirty = true;

        // Coalescing: pdTRUE increments the task notification value; the task
        // drains with ulTaskNotifyTake(pdTRUE,...) which clears it in one shot,
        // so rapid ISR fires collapse to a single task wake.
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(led_flicker_task_handle, &higher_priority_task_woken);

        ISR_PROFILE_END(2);
        return higher_priority_task_woken == pdTRUE;
    }

    if (elapsed_us >= cycle_duration_us) {
        flicker_state.cycle_start_time_us = now_us;
    }

    ISR_PROFILE_END(2);
    return false;
}

/**
 * @brief Task that performs actual LED I/O for the flicker effect (Option B)
 *
 * Priority 22, pinned to core 1 (symmetric with timing_dispatch_task on core 1).
 * Woken by led_flicker_timer_callback via vTaskNotifyGiveFromISR. Multiple ISR
 * fires between task wakes collapse to one call to led_strip_set_all because
 * we read led_state once after waking — this is safe for ON/OFF toggling where
 * only the most-recent target matters.
 *
 * volatile reads: led_state, red, green, blue, brightness are declared volatile
 * in led_flicker_state_t. On Xtensa (ESP32), 32-bit-or-smaller aligned reads are
 * atomic at the instruction level, so a single volatile read of each uint8_t field
 * is safe against mid-write tearing from the ISR or led_matrix_update_flicker_params.
 */
static void led_flicker_task(void *arg) {
    while (1) {
        // Block until the ISR fires at least once; clears notification count on exit
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Exit before any LED I/O so we never hold access_mutex at self-deletion.
        if (!flicker_state.active) break;

        if (!matrix_handle) {
            continue;
        }

        bool target_on = flicker_state.led_state;
        uint8_t r, g, b;

        if (target_on) {
            // Scale brightness in task context — avoids any FPU/division concerns in ISR
            uint8_t bri = flicker_state.brightness;
            r = (flicker_state.red   * bri) / 100;
            g = (flicker_state.green * bri) / 100;
            b = (flicker_state.blue  * bri) / 100;
        } else {
            r = g = b = 0;
        }

        // led_strip_set_all acquires access_mutex, writes the working buffer, then
        // calls led_strip_refresh -> rmt_transmit. All task-context-safe.
        led_strip_set_all(matrix_handle, r, g, b);
    }

    // Signal to led_matrix_stop_flicker that the task has exited cleanly
    // and will no longer touch access_mutex or any LED state.
    led_flicker_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Start LED flicker for meditation/brainwave entrainment using dedicated hardware timer
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

    // Configure flicker parameters (convert float frequency to milliHz for ISR-safe integer arithmetic)
    flicker_state.frequency_milliHz = (uint32_t)(frequency * 1000.0f);
    flicker_state.duty_cycle = duty_cycle;
    flicker_state.brightness = brightness;
    flicker_state.cycle_start_time_us = esp_timer_get_time();
    flicker_state.led_state = false;

    // Create dedicated hardware timer for LED flicker (separate from timing engine)
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,  // 1MHz = 1μs resolution
    };

    esp_err_t ret = gptimer_new_timer(&timer_config, &flicker_state.flicker_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED flicker timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure alarm event callback
    gptimer_event_callbacks_t cbs = {
        .on_alarm = led_flicker_timer_callback,
    };
    ret = gptimer_register_event_callbacks(flicker_state.flicker_timer, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register LED flicker callback: %s", esp_err_to_name(ret));
        gptimer_del_timer(flicker_state.flicker_timer);
        flicker_state.flicker_timer = NULL;
        return ret;
    }

    // Enable the timer
    ret = gptimer_enable(flicker_state.flicker_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable LED flicker timer: %s", esp_err_to_name(ret));
        gptimer_del_timer(flicker_state.flicker_timer);
        flicker_state.flicker_timer = NULL;
        return ret;
    }

    // Configure alarm for periodic execution based on frequency (ISR-safe integer arithmetic)
    // Use higher frequency for smooth flicker (100x the target frequency, minimum 1kHz)
    uint32_t timer_freq_hz = (flicker_state.frequency_milliHz * 100) / 1000;  // Convert milliHz to Hz and multiply by 100
    if (timer_freq_hz < 1000) timer_freq_hz = 1000; // Minimum 1kHz
    uint64_t alarm_period_us = 1000000 / timer_freq_hz;

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = alarm_period_us,
        .flags.auto_reload_on_alarm = true,
    };

    ret = gptimer_set_alarm_action(flicker_state.flicker_timer, &alarm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LED flicker alarm: %s", esp_err_to_name(ret));
        gptimer_disable(flicker_state.flicker_timer);
        gptimer_del_timer(flicker_state.flicker_timer);
        flicker_state.flicker_timer = NULL;
        return ret;
    }

    // Create the LED output task before starting the timer so the task handle
    // is valid when the first ISR fires.
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        led_flicker_task,
        "led_flicker",
        2048,
        NULL,
        22,                // Priority 22 — above default tasks, below IDLE watchdog
        &led_flicker_task_handle,
        1                  // Core 1 — symmetric with timing_dispatch_task
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create led_flicker_task");
        gptimer_disable(flicker_state.flicker_timer);
        gptimer_del_timer(flicker_state.flicker_timer);
        flicker_state.flicker_timer = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Start the hardware timer
    ret = gptimer_start(flicker_state.flicker_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LED flicker timer: %s", esp_err_to_name(ret));
        vTaskDelete(led_flicker_task_handle);
        led_flicker_task_handle = NULL;
        gptimer_disable(flicker_state.flicker_timer);
        gptimer_del_timer(flicker_state.flicker_timer);
        flicker_state.flicker_timer = NULL;
        return ret;
    }

    flicker_state.active = true;
    // Note: ESP_LOG removed from ISR execution path to prevent lock violations

    return ESP_OK;
}

/**
 * @brief Stop LED flicker
 */
esp_err_t led_matrix_stop_flicker(void) {
    if (!flicker_state.active) {
        return ESP_OK; // Already stopped
    }

    // Clear active flag before stopping timer so any in-flight ISR invocation
    // exits via the early-return path without notifying the task.
    flicker_state.active = false;

    // Timer stop MUST precede the task wake: after this point the ISR cannot
    // fire, so no further vTaskNotifyGiveFromISR calls will arrive.
    if (flicker_state.flicker_timer) {
        gptimer_stop(flicker_state.flicker_timer);
        gptimer_disable(flicker_state.flicker_timer);
        gptimer_del_timer(flicker_state.flicker_timer);
        flicker_state.flicker_timer = NULL;
    }

    // Wake the task so it observes !active and self-deletes without touching
    // access_mutex.  led_flicker_task_handle is cleared to NULL by the task
    // itself, which is the exit signal we poll for below.
    if (led_flicker_task_handle) {
        xTaskNotifyGive(led_flicker_task_handle);
        for (int i = 0; i < 50 && led_flicker_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Safe to drive LEDs off now: the flicker task has exited (or the 50 ms
    // budget expired, in which case we proceed anyway rather than blocking).
    if (matrix_handle) {
        led_strip_set_all(matrix_handle, 0, 0, 0);
    }

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

    // Convert new frequency to milliHz for comparison (ISR-safe integer arithmetic)
    uint32_t new_frequency_milliHz = (uint32_t)(frequency * 1000.0f);

    // Update hardware timer frequency if changed
    if (flicker_state.frequency_milliHz != new_frequency_milliHz && flicker_state.flicker_timer) {
        // Calculate new timer frequency (100x the target frequency, minimum 1kHz)
        uint32_t timer_freq_hz = (new_frequency_milliHz * 100) / 1000;  // Convert milliHz to Hz and multiply by 100
        if (timer_freq_hz < 1000) timer_freq_hz = 1000;
        uint64_t alarm_period_us = 1000000 / timer_freq_hz;

        // Update alarm configuration
        gptimer_alarm_config_t alarm_config = {
            .reload_count = 0,
            .alarm_count = alarm_period_us,
            .flags.auto_reload_on_alarm = true,
        };

        esp_err_t ret = gptimer_set_alarm_action(flicker_state.flicker_timer, &alarm_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to update timer frequency: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Update parameters atomically (ISR-safe integer storage)
    flicker_state.frequency_milliHz = new_frequency_milliHz;
    flicker_state.duty_cycle = duty_cycle;
    flicker_state.brightness = brightness;
    flicker_state.cycle_start_time_us = esp_timer_get_time(); // Reset cycle timing

    // Note: ESP_LOG removed from ISR execution path to prevent lock violations

    return ESP_OK;
}

/**
 * @brief Set flicker color (default: white)
 */
esp_err_t led_matrix_set_flicker_color(uint8_t red, uint8_t green, uint8_t blue) {
    flicker_state.red = red;
    flicker_state.green = green;
    flicker_state.blue = blue;

    // Note: ESP_LOG removed from ISR execution path to prevent lock violations
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
    return flicker_state.active ? (float)(flicker_state.frequency_milliHz) / 1000.0f : 0.0f;
}
