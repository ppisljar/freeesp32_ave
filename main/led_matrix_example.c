// TODO Step 3: config_parser passes raw RGBW (default 255,255,255,0 if missing) via channel_mask interface

#include "led_matrix_example.h"
#include "led_strip.h"
#include "audio_config.h"
#include "isr_profiling.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/gptimer.h"
#include <string.h>

static const char* TAG = "led_matrix";
static led_strip_handle_t *matrix_handle = NULL;

/**
 * @brief Per-parameter sweep state, held inside led_flicker_state_t.
 *
 * Units:
 *   freq        — milliHz  (same scale as frequency_milliHz)
 *   duty        — Q8.8     (value * 256; truncate >> 8 when applying)
 *   brightness  — Q8.8
 *   r, g, b, w  — Q8.8
 *
 * curve == LED_INTERP_NONE means "hold at target, no sweep".
 */
typedef struct {
    uint32_t start_q;   // Start value in param units (see above)
    uint32_t target_q;  // Target value in param units
    uint8_t  curve;     // 0=NONE, 1=LINEAR, 2=QUADRATIC
} led_sweep_param_t;

// Per-channel flicker and sweep state. One instance per logical LED zone.
typedef struct {
    volatile bool active;              // Flicker currently running (read by ISR)
    volatile uint32_t frequency_milliHz; // Current flicker frequency (Hz * 1000 for precision)
    volatile uint8_t duty_cycle;       // On-time percentage (0-100); written at cycle boundary by ISR
    volatile uint8_t brightness;       // Maximum brightness (0-100); written at cycle boundary by ISR
    volatile uint8_t red, green, blue; // LED colors when ON; written at cycle boundary by ISR
    volatile uint8_t _w_value;         // White component (0-255); composited into R/G/B at write time
    volatile bool led_state;           // Target ON/OFF state; written by ISR, read by flicker task
    volatile bool led_dirty;           // ISR sets true when led_state changes; task clears it
    volatile uint64_t cycle_start_time_us; // Timestamp of current cycle start (ISR-owned)

    // Sweep state — written by led_matrix_start_sweep_masked() (task context),
    // read at cycle boundaries inside the ISR.  7 parameters × led_sweep_param_t.
    led_sweep_param_t sw_freq;        // milliHz units
    led_sweep_param_t sw_duty;        // Q8.8 units
    led_sweep_param_t sw_brightness;  // Q8.8 units
    led_sweep_param_t sw_r;           // Q8.8 units
    led_sweep_param_t sw_g;           // Q8.8 units
    led_sweep_param_t sw_b;           // Q8.8 units
    led_sweep_param_t sw_w;           // Q8.8 units
    volatile uint64_t sweep_start_us;     // esp_timer_get_time() when sweep began
    volatile uint64_t sweep_duration_us;  // Total sweep duration in microseconds
} led_flicker_state_t;

// 4 independent channel states (bits 0-3 of channel_mask map to channels 1-4).
static led_flicker_state_t flicker_state[4] = {
    [0 ... 3] = {
        .active               = false,
        .frequency_milliHz    = 0,
        .duty_cycle           = 50,
        .brightness           = 100,
        .red                  = 255,
        .green                = 255,
        .blue                 = 255,
        ._w_value             = 0,
        .led_state            = false,
        .led_dirty            = false,
        .cycle_start_time_us  = 0,
        .sw_freq        = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_duty        = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_brightness  = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_r           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_g           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_b           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_w           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sweep_start_us    = 0,
        .sweep_duration_us = 0,
    },
};

// One shared hardware timer drives all 4 channels; the ISR iterates over them.
static gptimer_handle_t s_flicker_timer = NULL;

// Task handle for the LED flicker output task (one task handles all channels).
static TaskHandle_t led_flicker_task_handle = NULL;

// Cross-core spinlock guarding multi-field writes to flicker_state[].
// Task-context APIs (start/stop/update/set_color) use portENTER_CRITICAL;
// the ISR's cycle-boundary block uses portENTER_CRITICAL_ISR. This prevents
// the ISR's read-then-overwrite of red/green/blue from snapshotting a
// half-updated sweep slot, and prevents the TOCTOU at channel-stop where the
// task could read active=true between writes of `led_state` and `active`.
static portMUX_TYPE s_flicker_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Spec-defined region map indexed by (row, col).
//
// Row-3 col-4 belongs to r1 (per ledc_spec.md asymmetry); row-3 col-7 does
// NOT belong to r4 (intentional per spec — the inner-right rectangle has no
// bottom corner extension).  These asymmetries are part of the spec, not bugs.
//
// Bit positions in led_to_channel_bit[]: r1=0, r2=1, r3=2, r4=3.
// So led_to_channel_bit[i] == (1 << channel_bit) for the channel owning LED i.
// ---------------------------------------------------------------------------
static const uint8_t region_grid[LED_MATRIX_HEIGHT][LED_MATRIX_WIDTH] = {
    { 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2 },  // row 0: r2 r2 r2 r2 r2 r2 r3 r3 r3 r3 r3 r3
    { 1, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 2 },  // row 1: r2 r1 r1 r1 r1 r2 r3 r4 r4 r4 r4 r3
    { 1, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 2 },  // row 2: r2 r1 r1 r1 r1 r2 r3 r4 r4 r4 r4 r3
    { 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2 },  // row 3: symmetric layout (r1 = 8 LEDs, r2 = 16, r3 = 16, r4 = 8)
};

// led_to_channel_bit[led_index] = (1 << channel_bit), indexed by LINEAR LED
// index 0-47 (not by (x,y)).  Filled in led_matrix_init() by walking the
// region_grid above and translating (x,y) through matrix_xy_to_index() so
// that the zig-zag physical wiring is correctly accounted for.
static uint8_t led_to_channel_bit[LED_STRIP_COUNT];

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

    // Build the led_to_channel_bit LUT by walking (x,y) through the region_grid,
    // translating each (x,y) via matrix_xy_to_index() to get the physical linear
    // index, then storing 1<<bit at that slot.  This correctly handles the zig-zag
    // wiring so that logical (row,col) positions map to the right physical LEDs.
    for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; y++) {
        for (uint8_t x = 0; x < LED_MATRIX_WIDTH; x++) {
            uint32_t idx = matrix_xy_to_index(x, y);
            if (idx < LED_STRIP_COUNT) {
                led_to_channel_bit[idx] = (uint8_t)(1u << region_grid[y][x]);
            }
        }
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
 * @brief Piecewise quadratic ease-in-out in Q16 fixed-point.
 *
 * Input:  progress_q16 in [0, 65536]   (0 = start, 65536 = end)
 * Output: smoothed_q16 in [0, 65536]
 *
 * Curve: t = 2p²  for p < 0.5
 *        t = 1 - 2(1-p)²  for p >= 0.5
 * (Identical to the audio generator's piecewise quadratic ease-in-out.)
 *
 * IRAM_ATTR: must be callable from the ISR (led_flicker_timer_callback).
 */
static inline uint32_t IRAM_ATTR led_quad_q16(uint32_t progress_q16) {
    if (progress_q16 < 32768u) {
        // t = 2 * p^2, in Q16: t_q16 = (2 * p_q16^2) >> 16
        return (uint32_t)((2ULL * (uint64_t)progress_q16 * (uint64_t)progress_q16) >> 16);
    } else {
        uint64_t inv = 65536ULL - (uint64_t)progress_q16;
        return 65536u - (uint32_t)((2ULL * inv * inv) >> 16);
    }
}

/**
 * @brief Interpolate a single sweep parameter at the given Q16 progress.
 *
 * Returns the interpolated value in the same units as start_q / target_q.
 * Called only at cycle boundaries from within the ISR.
 */
static inline uint32_t IRAM_ATTR led_interp_param(const led_sweep_param_t *sw, uint32_t progress_q16) {
    if (sw->curve == LED_INTERP_NONE) {
        return sw->target_q;
    }
    uint32_t smooth_q16 = (sw->curve == LED_INTERP_QUADRATIC)
                          ? led_quad_q16(progress_q16)
                          : progress_q16; // LINEAR
    // Signed-safe: start_q and target_q are uint32_t, so use 64-bit signed delta.
    int64_t delta = (int64_t)sw->target_q - (int64_t)sw->start_q;
    int64_t result = (int64_t)sw->start_q + (delta * (int64_t)smooth_q16) / 65536LL;
    if (result < 0) result = 0;
    return (uint32_t)result;
}

/**
 * @brief Hardware timer alarm callback for LED flicker control (minimal ISR)
 *
 * Only determines per-channel ON/OFF state via integer arithmetic and notifies
 * led_flicker_task to do the actual LED I/O. rmt_transmit() MUST NOT be called
 * from this ISR — it is task-context-only per the ESP-IDF RMT driver contract.
 *
 * Iterates all 4 channels: for each active channel it recomputes all 7 swept
 * parameters at cycle boundaries and updates led_state. A single
 * vTaskNotifyGiveFromISR suffices even if multiple channels become dirty —
 * the task drains all dirty channels in one pass.
 *
 * ISR budget cap: plan specifies max 5000 cycles for all 4 channels combined.
 * Four channels × ~250 cycles/channel ≈ 1000 cycles baseline — well within cap.
 */
static bool IRAM_ATTR led_flicker_timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data) {
    ISR_PROFILE_BEGIN(2);

    if (!matrix_handle) {
        ISR_PROFILE_END(2);
        return false;
    }

    uint64_t now_us = esp_timer_get_time();
    bool any_dirty = false;

    for (uint8_t ch = 0; ch < 4; ch++) {
        led_flicker_state_t *s = &flicker_state[ch];

        if (!s->active) {
            continue;
        }

        // cycle_duration_us = 1,000,000,000 / frequency_milliHz  (all integer, no FPU)
        uint64_t cycle_duration_us = (1000000ULL * 1000ULL) / s->frequency_milliHz;
        uint64_t elapsed_us = now_us - s->cycle_start_time_us;

        // --- Cycle boundary: advance cycle and recompute all swept params ---
        // Critical section: snapshot sw_* and write red/green/blue/_w_value
        // atomically vs task-context writers (set_color/start/update).
        portENTER_CRITICAL_ISR(&s_flicker_mux);
        if (elapsed_us >= cycle_duration_us) {
            s->cycle_start_time_us = now_us;
            elapsed_us = 0;

            // Compute Q16 progress through the sweep (0..65536, clamped).
            uint64_t sweep_dur = s->sweep_duration_us;
            uint32_t progress_q16;
            if (sweep_dur == 0) {
                progress_q16 = 65536u; // Snap to target immediately if no duration.
            } else {
                uint64_t elapsed_sweep = now_us - s->sweep_start_us;
                uint64_t prog64 = (elapsed_sweep * 65536ULL) / sweep_dur;
                progress_q16 = (prog64 > 65536ULL) ? 65536u : (uint32_t)prog64;
            }

            // Interpolate frequency (milliHz units) — guard against zero.
            uint32_t new_freq = led_interp_param(&s->sw_freq, progress_q16);
            if (new_freq > 0) {
                s->frequency_milliHz = new_freq;
                // Recompute cycle_duration_us for the new frequency immediately.
                cycle_duration_us = (1000000ULL * 1000ULL) / new_freq;
            }

            // Interpolate duty (Q8.8 → truncate to uint8_t 0-100).
            uint32_t duty_q8 = led_interp_param(&s->sw_duty, progress_q16);
            s->duty_cycle = (uint8_t)(duty_q8 >> 8);

            // Interpolate brightness (Q8.8 → truncate to uint8_t 0-100).
            uint32_t bri_q8 = led_interp_param(&s->sw_brightness, progress_q16);
            s->brightness = (uint8_t)(bri_q8 >> 8);

            // Interpolate colour channels (Q8.8 → truncate to uint8_t 0-255).
            uint32_t r_q8 = led_interp_param(&s->sw_r, progress_q16);
            uint32_t g_q8 = led_interp_param(&s->sw_g, progress_q16);
            uint32_t b_q8 = led_interp_param(&s->sw_b, progress_q16);
            uint32_t w_q8 = led_interp_param(&s->sw_w, progress_q16);
            s->red      = (uint8_t)(r_q8 >> 8);
            s->green    = (uint8_t)(g_q8 >> 8);
            s->blue     = (uint8_t)(b_q8 >> 8);
            s->_w_value = (uint8_t)(w_q8 >> 8);
        }
        portEXIT_CRITICAL_ISR(&s_flicker_mux);

        uint64_t on_time_us = (cycle_duration_us * s->duty_cycle) / 100;
        bool should_be_on = (elapsed_us < on_time_us);

        if (should_be_on != s->led_state) {
            s->led_state = should_be_on;
            s->led_dirty = true;
            any_dirty = true;
        }
    }

    if (any_dirty) {
        // One notify covers all dirty channels; task drains all of them.
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(led_flicker_task_handle, &higher_priority_task_woken);

        ISR_PROFILE_END(2);
        return higher_priority_task_woken == pdTRUE;
    }

    ISR_PROFILE_END(2);
    return false;
}

/**
 * @brief Task that performs actual LED I/O for the flicker effect.
 *
 * Priority 22, pinned to core 1.  Woken by led_flicker_timer_callback via
 * vTaskNotifyGiveFromISR.  On wake, it walks all 48 LEDs, looks up each
 * LED's owning channel via led_to_channel_bit[], applies that channel's
 * current on/off state and color, and calls led_strip_refresh once.
 *
 * Pre-computes per-channel (r,g,b) with brightness scaling and W compositing
 * once per wake before the per-LED loop (48 iterations × 4 channel checks = 192
 * ops per wake — negligible vs. the actual RMT DMA transfer).
 */
static void led_flicker_task(void *arg) {
    while (1) {
        // Block until the ISR fires at least once; clears notification count on exit.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Check if ALL channels have gone inactive — if so, exit.
        bool any_active = false;
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (flicker_state[ch].active) { any_active = true; break; }
        }
        if (!any_active) break;

        if (!matrix_handle) {
            continue;
        }

        // Pre-compute per-channel output colors so the per-LED loop only indexes.
        // Colors are only meaningful when led_state == true; the false case writes (0,0,0).
        // The snapshot under s_flicker_mux is essential: the ISR writes red/green/blue
        // as three separate stores under portENTER_CRITICAL_ISR, so reading them
        // unprotected here produces torn (mixed-epoch) color triples.
        uint8_t ch_r[4], ch_g[4], ch_b[4];
        for (uint8_t ch = 0; ch < 4; ch++) {
            led_flicker_state_t *s = &flicker_state[ch];
            bool active, led_state;
            uint8_t bri, w_val, red, green, blue;
            portENTER_CRITICAL(&s_flicker_mux);
            active    = s->active;
            led_state = s->led_state;
            bri       = s->brightness;
            w_val     = s->_w_value;
            red       = s->red;
            green     = s->green;
            blue      = s->blue;
            portEXIT_CRITICAL(&s_flicker_mux);

            if (!active || !led_state) {
                ch_r[ch] = ch_g[ch] = ch_b[ch] = 0;
                continue;
            }
            uint8_t w_scaled = (w_val * bri) / 100;
            uint8_t r = (red   * bri) / 100;
            uint8_t g = (green * bri) / 100;
            uint8_t b = (blue  * bri) / 100;
            // W ("white component") is added to R, G, B and clamped to 255.
            // Strategy (b): lift toward white — visually intuitive for RGB-only strip.
            if ((uint16_t)r + w_scaled > 255u) r = 255; else r = (uint8_t)(r + w_scaled);
            if ((uint16_t)g + w_scaled > 255u) g = 255; else g = (uint8_t)(g + w_scaled);
            if ((uint16_t)b + w_scaled > 255u) b = 255; else b = (uint8_t)(b + w_scaled);
            ch_r[ch] = r;
            ch_g[ch] = g;
            ch_b[ch] = b;
        }

        // Walk every physical LED, look up its owning channel, apply that
        // channel's precomputed color.  led_to_channel_bit[i] == (1<<ch_bit),
        // so we find ch_bit by scanning 0-3 (only 4 iterations per LED).
        for (uint32_t led = 0; led < LED_STRIP_COUNT; led++) {
            uint8_t mask = led_to_channel_bit[led];
            uint8_t r = 0, g = 0, b = 0;
            for (uint8_t ch = 0; ch < 4; ch++) {
                if (mask & (1u << ch)) {
                    // Each LED belongs to exactly one channel per the spec's
                    // region map, so first match wins and we break immediately.
                    r = ch_r[ch];
                    g = ch_g[ch];
                    b = ch_b[ch];
                    break;
                }
            }
            led_strip_set_pixel_rgb(matrix_handle, led, r, g, b);
        }

        // Single refresh at the end — all pixel data was written above.
        led_strip_refresh(matrix_handle);
    }

    // Signal to stop_flicker that this task has exited cleanly.
    led_flicker_task_handle = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Internal helper: ensure the shared hardware timer and flicker task exist.
// Called by led_matrix_start_flicker_masked() before activating any channel.
// ---------------------------------------------------------------------------
static esp_err_t s_ensure_timer_and_task(uint32_t min_freq_milliHz)
{
    // Create the flicker task once; it persists until all channels are stopped.
    if (led_flicker_task_handle == NULL) {
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
            return ESP_ERR_NO_MEM;
        }
    }

    // Create the shared timer if it doesn't exist yet.
    if (s_flicker_timer == NULL) {
        gptimer_config_t timer_config = {
            .clk_src      = GPTIMER_CLK_SRC_DEFAULT,
            .direction    = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000,  // 1 MHz = 1 µs resolution
        };
        esp_err_t ret = gptimer_new_timer(&timer_config, &s_flicker_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create flicker timer: %s", esp_err_to_name(ret));
            return ret;
        }

        gptimer_event_callbacks_t cbs = { .on_alarm = led_flicker_timer_callback };
        ret = gptimer_register_event_callbacks(s_flicker_timer, &cbs, NULL);
        if (ret != ESP_OK) {
            gptimer_del_timer(s_flicker_timer);
            s_flicker_timer = NULL;
            return ret;
        }

        ret = gptimer_enable(s_flicker_timer);
        if (ret != ESP_OK) {
            gptimer_del_timer(s_flicker_timer);
            s_flicker_timer = NULL;
            return ret;
        }

        // Timer resolution: 100× the highest frequency channel, minimum 1 kHz.
        // This gives at least 100 ISR ticks per flicker cycle for duty accuracy.
        uint32_t timer_freq_hz = (min_freq_milliHz * 100) / 1000;
        if (timer_freq_hz < 1000) timer_freq_hz = 1000;
        uint64_t alarm_period_us = 1000000ULL / timer_freq_hz;

        gptimer_alarm_config_t alarm_config = {
            .reload_count = 0,
            .alarm_count  = alarm_period_us,
            .flags.auto_reload_on_alarm = true,
        };
        ret = gptimer_set_alarm_action(s_flicker_timer, &alarm_config);
        if (ret != ESP_OK) {
            gptimer_disable(s_flicker_timer);
            gptimer_del_timer(s_flicker_timer);
            s_flicker_timer = NULL;
            return ret;
        }

        ret = gptimer_start(s_flicker_timer);
        if (ret != ESP_OK) {
            gptimer_disable(s_flicker_timer);
            gptimer_del_timer(s_flicker_timer);
            s_flicker_timer = NULL;
            return ret;
        }
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Internal helper: stop and destroy the shared timer + task when no channel
// is active any more.
// ---------------------------------------------------------------------------
static void s_maybe_teardown_timer_and_task(void)
{
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (flicker_state[ch].active) return; // at least one channel still running
    }

    // All channels idle — tear down the shared timer.
    if (s_flicker_timer) {
        gptimer_stop(s_flicker_timer);
        gptimer_disable(s_flicker_timer);
        gptimer_del_timer(s_flicker_timer);
        s_flicker_timer = NULL;
    }

    // Wake the task so it observes !any_active and self-deletes.
    if (led_flicker_task_handle) {
        xTaskNotifyGive(led_flicker_task_handle);
        for (int i = 0; i < 50 && led_flicker_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ===========================================================================
// MASKED (multi-channel) PUBLIC API
// ===========================================================================

/**
 * @brief Start LED flicker on all channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F; bit 0 = channel 1, bit 3 = channel 4.
 * @param frequency    Flicker frequency in Hz (0.1-100.0).
 * @param duty_cycle   Duty cycle percentage (0-100).
 * @param brightness   Maximum brightness (0-100).
 */
esp_err_t led_matrix_start_flicker_masked(uint8_t channel_mask, float frequency,
                                           uint8_t duty_cycle, uint8_t brightness)
{
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

    uint32_t freq_milliHz = (uint32_t)(frequency * 1000.0f);
    uint64_t now_us = esp_timer_get_time();

    esp_err_t ret = s_ensure_timer_and_task(freq_milliHz);
    if (ret != ESP_OK) return ret;

    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(channel_mask & (1u << ch))) continue;

        led_flicker_state_t *s = &flicker_state[ch];
        // Critical section: all per-channel writes happen atomically vs the ISR's
        // cycle-boundary read of sw_r/g/b/w and overwrite of red/green/blue.
        portENTER_CRITICAL(&s_flicker_mux);
        s->frequency_milliHz   = freq_milliHz;
        s->duty_cycle          = duty_cycle;
        s->brightness          = brightness;
        s->cycle_start_time_us = now_us;
        s->led_state           = false;
        s->led_dirty           = false;
        // Clear any pending sweep so the new params are held constant.
        // ALL seven swept params (freq/duty/bright/R/G/B/W) must be set so the
        // cycle-boundary recompute in the ISR doesn't overwrite the fields with 0.
        s->sw_freq       = (led_sweep_param_t){ freq_milliHz, freq_milliHz, LED_INTERP_NONE };
        s->sw_duty       = (led_sweep_param_t){ (uint32_t)duty_cycle * 256u, (uint32_t)duty_cycle * 256u, LED_INTERP_NONE };
        s->sw_brightness = (led_sweep_param_t){ (uint32_t)brightness * 256u, (uint32_t)brightness * 256u, LED_INTERP_NONE };
        s->sw_r          = (led_sweep_param_t){ (uint32_t)s->red      * 256u, (uint32_t)s->red      * 256u, LED_INTERP_NONE };
        s->sw_g          = (led_sweep_param_t){ (uint32_t)s->green    * 256u, (uint32_t)s->green    * 256u, LED_INTERP_NONE };
        s->sw_b          = (led_sweep_param_t){ (uint32_t)s->blue     * 256u, (uint32_t)s->blue     * 256u, LED_INTERP_NONE };
        s->sw_w          = (led_sweep_param_t){ (uint32_t)s->_w_value * 256u, (uint32_t)s->_w_value * 256u, LED_INTERP_NONE };
        s->sweep_start_us    = now_us;
        s->sweep_duration_us = 0;
        s->active = true;
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/**
 * @brief Stop LED flicker on all channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F; bit 0 = channel 1, bit 3 = channel 4.
 */
esp_err_t led_matrix_stop_flicker_masked(uint8_t channel_mask)
{
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        // Critical section: clear led_state BEFORE active so the task can never
        // observe (active=true && led_state=true) on a channel we're stopping —
        // that race produced a single-frame stale-color blip at phase boundary.
        portENTER_CRITICAL(&s_flicker_mux);
        flicker_state[ch].led_state = false;
        flicker_state[ch].led_dirty = false;
        flicker_state[ch].active    = false;
        portEXIT_CRITICAL(&s_flicker_mux);
    }

    s_maybe_teardown_timer_and_task();

    if (matrix_handle) {
        led_strip_set_all(matrix_handle, 0, 0, 0);
    }
    return ESP_OK;
}

/**
 * @brief Update flicker parameters on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 * @param frequency    New flicker frequency in Hz.
 * @param duty_cycle   New duty cycle percentage (0-100).
 * @param brightness   New maximum brightness (0-100).
 */
esp_err_t led_matrix_update_flicker_params_masked(uint8_t channel_mask, float frequency,
                                                    uint8_t duty_cycle, uint8_t brightness)
{
    if (frequency <= 0.0f || frequency > 100.0f) {
        ESP_LOGE(TAG, "Invalid flicker frequency: %.1f Hz", frequency);
        return ESP_ERR_INVALID_ARG;
    }
    if (duty_cycle > 100 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t new_freq_milliHz = (uint32_t)(frequency * 1000.0f);
    uint64_t now_us = esp_timer_get_time();

    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        if (!flicker_state[ch].active) continue;

        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->frequency_milliHz   = new_freq_milliHz;
        s->duty_cycle          = duty_cycle;
        s->brightness          = brightness;
        s->cycle_start_time_us = now_us;
        // Sweep slots must mirror the new values so the next cycle-boundary
        // recompute in the ISR doesn't snap them back to a stale sweep target.
        s->sw_freq       = (led_sweep_param_t){ new_freq_milliHz, new_freq_milliHz, LED_INTERP_NONE };
        s->sw_duty       = (led_sweep_param_t){ (uint32_t)duty_cycle * 256u, (uint32_t)duty_cycle * 256u, LED_INTERP_NONE };
        s->sw_brightness = (led_sweep_param_t){ (uint32_t)brightness * 256u, (uint32_t)brightness * 256u, LED_INTERP_NONE };
        s->sweep_duration_us = 0;
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/**
 * @brief Set flicker color on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 */
esp_err_t led_matrix_set_flicker_color_masked(uint8_t channel_mask,
                                               uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->red   = red;
        s->green = green;
        s->blue  = blue;
        // Mirror in the sweep slots — the ISR's cycle-boundary recompute reads
        // from sw_r/g/b, not from the .red/.green/.blue fields directly.
        s->sw_r = (led_sweep_param_t){ (uint32_t)red   * 256u, (uint32_t)red   * 256u, LED_INTERP_NONE };
        s->sw_g = (led_sweep_param_t){ (uint32_t)green * 256u, (uint32_t)green * 256u, LED_INTERP_NONE };
        s->sw_b = (led_sweep_param_t){ (uint32_t)blue  * 256u, (uint32_t)blue  * 256u, LED_INTERP_NONE };
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/**
 * @brief Start a parametric LED flicker sweep on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 * @param spec         Pointer to sweep specification; contents are copied per channel.
 */
esp_err_t led_matrix_start_sweep_masked(uint8_t channel_mask, const led_sweep_spec_t *spec)
{
    if (!spec) return ESP_ERR_INVALID_ARG;
    if (!matrix_handle) return ESP_ERR_INVALID_STATE;

    if (spec->duration_ms == 0 && (
            spec->freq_curve   != LED_INTERP_NONE ||
            spec->duty_curve   != LED_INTERP_NONE ||
            spec->bright_curve != LED_INTERP_NONE ||
            spec->r_curve      != LED_INTERP_NONE ||
            spec->g_curve      != LED_INTERP_NONE ||
            spec->b_curve      != LED_INTERP_NONE ||
            spec->w_curve      != LED_INTERP_NONE)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t init_freq_milliHz = (spec->freq_curve != LED_INTERP_NONE)
                                  ? spec->freq_milliHz_start
                                  : spec->freq_milliHz_target;
    if (init_freq_milliHz == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = s_ensure_timer_and_task(init_freq_milliHz);
    if (ret != ESP_OK) return ret;

    uint64_t sweep_duration_us = (uint64_t)spec->duration_ms * 1000ULL;
    uint64_t sweep_start_us    = esp_timer_get_time();

    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(channel_mask & (1u << ch))) continue;

        led_flicker_state_t *s = &flicker_state[ch];

        // Set initial live values for params that are not being swept.
        s->frequency_milliHz   = init_freq_milliHz;
        s->duty_cycle          = (spec->duty_curve   != LED_INTERP_NONE) ? spec->duty_start   : spec->duty_target;
        s->brightness          = (spec->bright_curve != LED_INTERP_NONE) ? spec->bright_start : spec->bright_target;
        s->red                 = (spec->r_curve      != LED_INTERP_NONE) ? spec->r_start      : spec->r_target;
        s->green               = (spec->g_curve      != LED_INTERP_NONE) ? spec->g_start      : spec->g_target;
        s->blue                = (spec->b_curve      != LED_INTERP_NONE) ? spec->b_start      : spec->b_target;
        s->_w_value            = (spec->w_curve      != LED_INTERP_NONE) ? spec->w_start      : spec->w_target;
        s->cycle_start_time_us = sweep_start_us;
        s->led_state           = false;

        // Populate per-param sweep state.
        s->sw_freq = (led_sweep_param_t){
            .start_q  = spec->freq_milliHz_start,
            .target_q = spec->freq_milliHz_target,
            .curve    = (uint8_t)spec->freq_curve,
        };
        s->sw_duty = (led_sweep_param_t){
            .start_q  = (uint32_t)spec->duty_start  * 256u,
            .target_q = (uint32_t)spec->duty_target * 256u,
            .curve    = (uint8_t)spec->duty_curve,
        };
        s->sw_brightness = (led_sweep_param_t){
            .start_q  = (uint32_t)spec->bright_start  * 256u,
            .target_q = (uint32_t)spec->bright_target * 256u,
            .curve    = (uint8_t)spec->bright_curve,
        };
        s->sw_r = (led_sweep_param_t){
            .start_q  = (uint32_t)spec->r_start * 256u,
            .target_q = (uint32_t)spec->r_target * 256u,
            .curve    = (uint8_t)spec->r_curve,
        };
        s->sw_g = (led_sweep_param_t){
            .start_q  = (uint32_t)spec->g_start * 256u,
            .target_q = (uint32_t)spec->g_target * 256u,
            .curve    = (uint8_t)spec->g_curve,
        };
        s->sw_b = (led_sweep_param_t){
            .start_q  = (uint32_t)spec->b_start * 256u,
            .target_q = (uint32_t)spec->b_target * 256u,
            .curve    = (uint8_t)spec->b_curve,
        };
        s->sw_w = (led_sweep_param_t){
            .start_q  = (uint32_t)spec->w_start * 256u,
            .target_q = (uint32_t)spec->w_target * 256u,
            .curve    = (uint8_t)spec->w_curve,
        };
        s->sweep_start_us    = sweep_start_us;
        s->sweep_duration_us = sweep_duration_us;
        s->active            = true;
    }
    return ESP_OK;
}

/**
 * @brief Return frequency of the lowest-bit-set channel in the mask.
 *
 * Returns 0.0 if the mask is 0 or no matching channel is active.
 * The lowest-bit-set semantic is documented in the header.
 */
float led_matrix_get_current_frequency_masked(uint8_t channel_mask)
{
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (channel_mask & (1u << ch)) {
            return flicker_state[ch].active
                   ? (float)flicker_state[ch].frequency_milliHz / 1000.0f
                   : 0.0f;
        }
    }
    return 0.0f;
}

// ===========================================================================
// BACKWARDS-COMPAT TRAMPOLINES (channel 1 = bit 0 = flicker_state[0])
// Existing callers in audio_led_sync.c and config_parser.c use these.
// audio_test.c also uses them — DO NOT change audio_test.c (Step 2 agent owns it).
// ===========================================================================

esp_err_t led_matrix_start_flicker(float frequency, uint8_t duty_cycle, uint8_t brightness) {
    return led_matrix_start_flicker_masked(0x01u, frequency, duty_cycle, brightness);
}

esp_err_t led_matrix_stop_flicker(void) {
    return led_matrix_stop_flicker_masked(0x01u);
}

esp_err_t led_matrix_update_flicker_params(float frequency, uint8_t duty_cycle, uint8_t brightness) {
    return led_matrix_update_flicker_params_masked(0x01u, frequency, duty_cycle, brightness);
}

esp_err_t led_matrix_set_flicker_color(uint8_t red, uint8_t green, uint8_t blue) {
    return led_matrix_set_flicker_color_masked(0x01u, red, green, blue);
}

// led_matrix_start_sweep() (the single-channel variant from Step 4) now
// delegates to channel 1 only (mask=0x01).  Step 4b adds the masked variant
// as the primary API.
esp_err_t led_matrix_start_sweep(const led_sweep_spec_t *spec) {
    return led_matrix_start_sweep_masked(0x01u, spec);
}

bool led_matrix_is_flickering(void) {
    // True if ANY of the 4 channels is active.
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (flicker_state[ch].active) return true;
    }
    return false;
}

float led_matrix_get_current_frequency(void) {
    // Return frequency of the lowest-numbered active channel, or 0.
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (flicker_state[ch].active) {
            return (float)flicker_state[ch].frequency_milliHz / 1000.0f;
        }
    }
    return 0.0f;
}
