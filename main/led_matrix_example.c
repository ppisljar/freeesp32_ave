// Step 7: led_matrix_example.c updated to use per-channel API (led_strip_set_channel).
// region_grid and led_to_channel_bit LUT removed — channel mapping lives in led_strip.c.

#include "led_matrix_example.h"
#include "led_strip.h"
#include "audio_config.h"
#include "sdkconfig.h"
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
    volatile bool led_state;           // Target ON/OFF state; written by ISR, read by flicker task
    volatile bool led_dirty;           // ISR sets true when led_state changes; task clears it
    volatile uint64_t cycle_start_time_us; // Timestamp of current cycle start (ISR-owned)
    volatile uint64_t latched_on_time_us;  // ON-time latched at cycle boundary; prevents mid-cycle duty glitch

    // Sweep state — written by led_matrix_start_sweep_masked() (task context),
    // read at cycle boundaries inside the ISR.  6 parameters × led_sweep_param_t.
    led_sweep_param_t sw_freq;        // milliHz units
    led_sweep_param_t sw_duty;        // Q8.8 units
    led_sweep_param_t sw_brightness;  // Q8.8 units
    led_sweep_param_t sw_r;           // Q8.8 units
    led_sweep_param_t sw_g;           // Q8.8 units
    led_sweep_param_t sw_b;           // Q8.8 units
    volatile uint64_t sweep_start_us;     // esp_timer_get_time() when sweep began
    volatile uint64_t sweep_duration_us;  // Total sweep duration in microseconds
} led_flicker_state_t;

// NUM_LED_CHANNELS independent channel states (bits 0..N-1 of channel_mask map to channels 1..N).
static led_flicker_state_t flicker_state[NUM_LED_CHANNELS] = {
    [0 ... NUM_LED_CHANNELS - 1] = {
        .active               = false,
        .frequency_milliHz    = 0,
        .duty_cycle           = 50,
        .brightness           = 100,
        .red                  = 255,
        .green                = 255,
        .blue                 = 255,
        .led_state            = false,
        .led_dirty            = false,
        .cycle_start_time_us  = 0,
        .latched_on_time_us   = 0,
        .sw_freq        = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_duty        = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_brightness  = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_r           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_g           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_b           = { .start_q = 0, .target_q = 0, .curve = 0 },
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

// Forward declaration: defined after led_matrix_init but called from it for
// pre-warming the timer and flicker task at boot (Layer 5, Step 5.2).
static esp_err_t s_ensure_timer_and_task(uint32_t min_freq_milliHz);

/**
 * @brief Convert 2D grid coordinates to linear LED index (zig-zag column pattern).
 *
 * Only meaningful when pixel addressing is supported (NEOPIXEL / DOTSTAR) and
 * CONFIG_LED_GRID_WIDTH × CONFIG_LED_GRID_HEIGHT == led_strip_get_pixel_count().
 *
 * Returns led_strip_get_pixel_count(matrix_handle) (an invalid index) when:
 *   - pixel addressing is not supported (DIRECT mode), or
 *   - (x, y) is out of the configured grid bounds.
 *
 * Callers must check the returned value before passing it to led_strip_set_pixel_rgb.
 */
static uint32_t matrix_xy_to_index(uint8_t x, uint8_t y)
{
    // Gate: direct mode has no individually addressable pixels.
    if (!matrix_handle || !led_strip_supports_pixel_addressing(matrix_handle)) {
        return led_strip_get_pixel_count(matrix_handle); // invalid sentinel
    }

#ifdef CONFIG_LED_GRID_WIDTH
    uint8_t grid_w = (uint8_t)CONFIG_LED_GRID_WIDTH;
#else
    uint8_t grid_w = LED_MATRIX_WIDTH;
#endif
#ifdef CONFIG_LED_GRID_HEIGHT
    uint8_t grid_h = (uint8_t)CONFIG_LED_GRID_HEIGHT;
#else
    uint8_t grid_h = LED_MATRIX_HEIGHT;
#endif

    if (x >= grid_w || y >= grid_h) {
        return led_strip_get_pixel_count(matrix_handle); // out of bounds
    }

    // Zig-zag pattern: even columns top-to-bottom, odd columns bottom-to-top.
    uint32_t index;
    if (x % 2 == 0) {
        index = (uint32_t)x * grid_h + y;
    } else {
        index = (uint32_t)x * grid_h + (grid_h - 1u - y);
    }
    return index;
}

/**
 * @brief Initialize LED matrix / strip backend from menuconfig.
 *
 * All hardware parameters (backend type, GPIO pin(s), pixel count, channel
 * map) are read from menuconfig via LED_STRIP_CONFIG_FROM_MENUCONFIG.  No
 * compile-time constants from audio_config.h are used here; the strip layer
 * owns all hardware details.
 */
esp_err_t led_matrix_init(void)
{
    ESP_LOGI(TAG, "Initializing LED strip backend from menuconfig");

    // Pull all hardware parameters from CONFIG_LED_* menuconfig symbols.
    // The channel map string (CONFIG_LED_CHANNEL_MAP) is parsed by led_strip_init.
    led_strip_config_t config = LED_STRIP_CONFIG_FROM_MENUCONFIG;

    esp_err_t ret = led_strip_init(&config, &matrix_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "LED strip initialized: backend=%d, pixels=%lu",
             (int)led_strip_get_backend(matrix_handle),
             (unsigned long)led_strip_get_pixel_count(matrix_handle));

    // Clear all LEDs / channels on startup.
    led_strip_clear(matrix_handle);
    led_strip_refresh(matrix_handle);

    // Pre-create the flicker gptimer and led_flicker_task at boot so that the
    // first call to led_matrix_start_flicker_masked does not pay the ~5 ms
    // one-time setup cost during live timeline dispatch.
    // min_freq_milliHz=1000 → timer_freq_hz clamps to 1000 Hz (1 kHz tick).
    // When a real channel later calls s_ensure_timer_and_task the timer already
    // exists, so the alarm period is NOT changed — 1 kHz is adequate for all
    // supported flicker rates.  See Layer 5 Step 5.2 in plan 007.
    esp_err_t timer_ret = s_ensure_timer_and_task(1000);
    if (timer_ret != ESP_OK) {
        // Non-fatal: flicker will still work, just with first-call setup cost.
        ESP_LOGW(TAG, "Pre-init of flicker timer/task failed (%s); will retry on first use",
                 esp_err_to_name(timer_ret));
    }

    ESP_LOGI(TAG, "LED matrix initialized successfully");
    return ESP_OK;
}

/**
 * @brief Set LED at matrix coordinates (x,y) with color order correction.
 *
 * Only valid for addressable backends (NEOPIXEL / DOTSTAR).  Returns
 * ESP_ERR_NOT_SUPPORTED for DIRECT mode (no per-pixel addressing).
 */
esp_err_t led_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t red, uint8_t green, uint8_t blue)
{
    if (!matrix_handle) {
        ESP_LOGE(TAG, "Matrix handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Gate: direct mode has no individually addressable pixels.
    if (!led_strip_supports_pixel_addressing(matrix_handle)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t pixel_count = led_strip_get_pixel_count(matrix_handle);
    uint32_t index = matrix_xy_to_index(x, y);
    if (index >= pixel_count) {
        ESP_LOGE(TAG, "Invalid LED index %lu for coordinates (%d,%d)", index, x, y);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Setting LED[%d,%d] (index %lu) to RGB(%d,%d,%d)", x, y, index, red, green, blue);

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
 * @brief Display VU meter on matrix (12 columns for levels).
 *
 * Only valid for addressable backends (NEOPIXEL / DOTSTAR).  Returns
 * ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_matrix_vu_meter(float left_level, float right_level)
{
    if (!matrix_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    // Gate: direct mode has no individually addressable pixels.
    if (!led_strip_supports_pixel_addressing(matrix_handle)) {
        return ESP_ERR_NOT_SUPPORTED;
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
 * @brief Demo pattern - test all LEDs in sequence.
 *
 * Only valid for addressable backends (NEOPIXEL / DOTSTAR).  Returns
 * ESP_ERR_NOT_SUPPORTED for DIRECT mode.
 */
esp_err_t led_matrix_test_pattern(void)
{
    if (!matrix_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    // Gate: direct mode has no individually addressable pixels.
    if (!led_strip_supports_pixel_addressing(matrix_handle)) {
        return ESP_ERR_NOT_SUPPORTED;
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
    ISR_PROFILE_BEGIN(1);

    if (!matrix_handle) {
        ISR_PROFILE_END(1);
        return false;
    }

    uint64_t now_us = esp_timer_get_time();
    bool any_dirty = false;

    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        led_flicker_state_t *s = &flicker_state[ch];

        if (!s->active) {
            continue;
        }

        // Pre-anchor wait: cycle_start_time_us is the transport-clock anchor,
        // which on FRESH activation equals logical_anchor_us = T0 + entry_time
        // + AUDIO_DMA_PIPELINE_LAG_US. That places it up to ~46 ms in the
        // FUTURE relative to dispatch time. Skip ALL processing for this tick
        // until the anchor arrives — the channel was initialised with
        // led_state=false in start_sweep_masked/start_flicker_masked, so the
        // LED stays OFF during the wait. As soon as now_us catches up, normal
        // cycle processing resumes with the first on-pulse at its correct
        // duration (latched_on_time_us), giving true audio-LED sync at the
        // first cycle origin (Plan 007 design intent).
        //
        // Without this check, the earlier underflow guard on elapsed_us would
        // clamp to 0 → should_be_on=(0 < latched_on_time_us)=true → LED stays
        // ON for the entire ~46 ms anchor wait, producing one very long first
        // pulse that visually reads as "first blink at full power".
        if (now_us < s->cycle_start_time_us) {
            continue;
        }

        // cycle_duration_us = 1,000,000,000 / frequency_milliHz  (all integer, no FPU)
        uint64_t cycle_duration_us = (1000000ULL * 1000ULL) / s->frequency_milliHz;
        // now_us >= cycle_start_time_us is guaranteed by the pre-anchor check above.
        uint64_t elapsed_us = now_us - s->cycle_start_time_us;

        // --- Cycle boundary: advance cycle and recompute all swept params ---
        // Critical section: snapshot sw_* and write red/green/blue
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
                // Guard against unsigned underflow: sweep_start_us may be in the future
                // (logical_anchor_us is offset forward by AUDIO_DMA_PIPELINE_LAG_US so the
                // LED cycle-origin aligns with DAC sample emission). Treat "anchor in
                // future" as progress=0, not as wrap-around → 65536 (snap-to-target).
                uint64_t elapsed_sweep = (now_us >= s->sweep_start_us)
                                         ? (now_us - s->sweep_start_us)
                                         : 0;
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
            // Latch on_time_us at cycle boundary so mid-cycle duty writes from task
            // context take effect only at the next cycle, never mid-cycle.
            s->latched_on_time_us = (cycle_duration_us * s->duty_cycle) / 100;

            // Interpolate brightness (Q8.8 → truncate to uint8_t 0-100).
            uint32_t bri_q8 = led_interp_param(&s->sw_brightness, progress_q16);
            s->brightness = (uint8_t)(bri_q8 >> 8);

            // Interpolate colour channels (Q8.8 → truncate to uint8_t 0-255).
            uint32_t r_q8 = led_interp_param(&s->sw_r, progress_q16);
            uint32_t g_q8 = led_interp_param(&s->sw_g, progress_q16);
            uint32_t b_q8 = led_interp_param(&s->sw_b, progress_q16);
            s->red   = (uint8_t)(r_q8 >> 8);
            s->green = (uint8_t)(g_q8 >> 8);
            s->blue  = (uint8_t)(b_q8 >> 8);
        }
        portEXIT_CRITICAL_ISR(&s_flicker_mux);

        // Use the latch computed at the last cycle boundary — duty changes take
        // effect at cycle boundaries, not mid-cycle.
        bool should_be_on = (elapsed_us < s->latched_on_time_us);

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

        ISR_PROFILE_END(1);
        return higher_priority_task_woken == pdTRUE;
    }

    ISR_PROFILE_END(1);
    return false;
}

/**
 * @brief Task that performs actual LED I/O for the flicker effect.
 *
 * Priority 23, pinned to core 1.  Woken by led_flicker_timer_callback via
 * vTaskNotifyGiveFromISR.
 *
 * On each wake: snapshot all 4 channels under s_flicker_mux (one critical
 * section per channel), then call led_strip_set_channel() × 4 and a single
 * led_strip_refresh().  The strip layer handles per-backend compositing
 * (brightness scaling for NEOPIXEL/DOTSTAR; PWM duty for DIRECT).
 *
 * The per-LED walk and led_to_channel_bit[] LUT that previously lived here
 * have been removed — the channel-map is now owned by led_strip.c and applied
 * inside led_strip_set_channel().  The flicker ON/OFF semantics shift from
 * "set RGB = 0 when OFF" to "set brightness = 0 when OFF", which is correct
 * for all three backends:
 *   - NEOPIXEL/DOTSTAR: brightness=0 → R'=G'=B'=0 in the working buffer.
 *   - DIRECT:           brightness=0 → LEDC duty = 0 (LED off).
 */
static void led_flicker_task(void *arg) {
    while (1) {
        // Block until the ISR fires at least once; clears notification count on exit.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Check if ALL channels have gone inactive — if so, exit.
        bool any_active = false;
        for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
            if (flicker_state[ch].active) { any_active = true; break; }
        }
        if (!any_active) break;

        if (!matrix_handle) {
            continue;
        }

        // Snapshot per-channel state under s_flicker_mux.
        // The ISR writes red/green/blue as three separate stores under
        // portENTER_CRITICAL_ISR; reading them unprotected produces torn
        // (mixed-epoch) color triples.  One critical section per channel
        // is adequate — the ISR's cycle-boundary block takes the same mux.
        bool    ch_active[NUM_LED_CHANNELS], ch_led_state[NUM_LED_CHANNELS];
        uint8_t ch_brightness[NUM_LED_CHANNELS], ch_red[NUM_LED_CHANNELS], ch_green[NUM_LED_CHANNELS], ch_blue[NUM_LED_CHANNELS];
        for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
            led_flicker_state_t *s = &flicker_state[ch];
            portENTER_CRITICAL(&s_flicker_mux);
            ch_active[ch]     = s->active;
            ch_led_state[ch]  = s->led_state;
            ch_brightness[ch] = s->brightness;
            ch_red[ch]        = s->red;
            ch_green[ch]      = s->green;
            ch_blue[ch]       = s->blue;
            portEXIT_CRITICAL(&s_flicker_mux);
        }

        // One set_channel call per logical channel.  Brightness compositing
        // (r' = r × brightness / 100) now lives inside led_strip.c so the
        // caller passes raw values from the timeline.
        //
        // When a channel is inactive or in the OFF half of its flicker cycle,
        // we pass brightness=0 so the strip layer drives the output to black /
        // zero duty — semantically equivalent to the old "write RGB=0" path
        // but portable across all three backends.
        for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
            if (!ch_active[ch] || !ch_led_state[ch]) {
                led_strip_set_channel(matrix_handle, ch, 0, 0, 0, 0);
            } else {
                led_strip_set_channel(matrix_handle, ch,
                                      ch_brightness[ch],
                                      ch_red[ch], ch_green[ch], ch_blue[ch]);
            }
        }

        // Single refresh at the end — all channel data was written above.
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
            24,                // Priority 24 — one ABOVE audio_output_task (23) so the
                               // LED render task ALWAYS preempts mid-fill_buffer when
                               // notified by the 1 kHz LED ISR. Without this, both
                               // tasks at priority 23 on core 1 round-robin every
                               // tick, delaying LED writes by 3–5 ms during a
                               // fill_buffer slice. With many active audio channels
                               // that delay exceeds one LED cycle, producing visibly
                               // "doubled" or "halved" pulses — observable on a
                               // photodiode recording. The render task does ~100 µs
                               // of work per transition (snapshot under spinlock +
                               // one led_strip_refresh), so preempting audio costs
                               // ~0.24% CPU at 12 Hz × 24 transitions/sec — well
                               // absorbed by the I2S DMA's 23 ms back-pressure
                               // buffer. See empirical photodiode measurement notes.
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
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
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
 * @param channel_mask Bitmask 0x01-0xFF; bit 0 = channel 1, bit 7 = channel 8.
 * @param frequency    Flicker frequency in Hz (0.1-100.0).
 * @param duty_cycle   Duty cycle percentage (0-100).
 * @param brightness   Maximum brightness (0-100).
 */
esp_err_t led_matrix_start_flicker_masked(uint8_t channel_mask, float frequency,
                                           uint8_t duty_cycle, uint8_t brightness,
                                           uint64_t cycle_hint_us)
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

    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;

        led_flicker_state_t *s = &flicker_state[ch];
        // Critical section: all per-channel writes happen atomically vs the ISR's
        // cycle-boundary read of sw_r/g/b/w and overwrite of red/green/blue.
        portENTER_CRITICAL(&s_flicker_mux);
        s->frequency_milliHz   = freq_milliHz;
        s->duty_cycle          = duty_cycle;
        s->brightness          = brightness;
        // Only reset the cycle origin and force the LED off on FIRST activation.
        // When the channel is already running, preserve cycle_start_time_us so the
        // ISR continues the existing rhythm — resetting it here would produce a
        // cycle of arbitrary length at the dispatch instant (the stutter the user sees).
        if (!s->active) {
            // Anchor selection priority (Layer 2 transport clock):
            //   1. cycle_hint_us — transport_origin_us + entry->time_ms * 1000, passed
            //      by config_parser when a canonical T0 is known.  All channels at the
            //      same logical timestamp get the identical ref_start regardless of when
            //      they are dispatched, eliminating inter-channel phase drift entirely.
            //   2. Already-running peer at the same frequency (peer-piggyback) — used
            //      when cycle_hint_us is 0 (legacy callers) and a channel is already
            //      active at the matching frequency.
            //   3. now_us — true first activation with no hint and no peer match.
            //
            // Late-anchor note (Step 2.4): ref_start may be in the past when the
            // transport clock anchor precedes esp_timer_get_time() by more than a few
            // milliseconds (e.g. dispatch lag in a busy batch).  The ISR's rollover
            // logic in the timer callback handles this naturally: elapsed_us will be
            // large on the first tick, causing immediate modulo wrap into the correct
            // cycle position.  This is correct at all frequencies — a 0.1 Hz channel
            // with a 30 ms late anchor has elapsed_us = 30 000 µs << 10 000 000 µs
            // cycle, so it starts at the right phase offset without any special case.
            uint64_t ref_start;
            if (cycle_hint_us != 0) {
                ref_start = cycle_hint_us;
            } else {
                ref_start = now_us;
                for (uint8_t peer = 0; peer < NUM_LED_CHANNELS; peer++) {
                    if (peer == ch) continue;
                    if (flicker_state[peer].active &&
                        flicker_state[peer].frequency_milliHz == freq_milliHz) {
                        ref_start = flicker_state[peer].cycle_start_time_us;
                        break;
                    }
                }
            }
            s->cycle_start_time_us = ref_start;
            // Compute the correct on-time immediately so the ISR's
            // (elapsed_us < latched_on_time_us) test is correct from the very
            // first tick.  Initialising to 0 caused one full dark cycle
            // (≈ 166 ms at 6 Hz) before the ISR's cycle-boundary recompute
            // set the real value.  See plans/007_timeline_sync_architecture.md
            // Step 1.3 and bug_led_audio_phase_sync_2026-06-17.md (Inv 10).
            {
                uint64_t cycle_duration_us = (1000000ULL * 1000ULL) / (uint64_t)freq_milliHz;
                s->latched_on_time_us = (cycle_duration_us * (uint64_t)duty_cycle) / 100ULL;
            }
            s->led_state           = false;
            s->led_dirty           = false;
        }
        // Clear any pending sweep so the new params are held constant.
        // ALL six swept params (freq/duty/bright/R/G/B) must be set so the
        // cycle-boundary recompute in the ISR doesn't overwrite the fields with 0.
        s->sw_freq       = (led_sweep_param_t){ freq_milliHz, freq_milliHz, LED_INTERP_NONE };
        s->sw_duty       = (led_sweep_param_t){ (uint32_t)duty_cycle * 256u, (uint32_t)duty_cycle * 256u, LED_INTERP_NONE };
        s->sw_brightness = (led_sweep_param_t){ (uint32_t)brightness * 256u, (uint32_t)brightness * 256u, LED_INTERP_NONE };
        s->sw_r          = (led_sweep_param_t){ (uint32_t)s->red   * 256u, (uint32_t)s->red   * 256u, LED_INTERP_NONE };
        s->sw_g          = (led_sweep_param_t){ (uint32_t)s->green * 256u, (uint32_t)s->green * 256u, LED_INTERP_NONE };
        s->sw_b          = (led_sweep_param_t){ (uint32_t)s->blue  * 256u, (uint32_t)s->blue  * 256u, LED_INTERP_NONE };
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
 * @param channel_mask Bitmask 0x01-0xFF; bit 0 = channel 1, bit 7 = channel 8.
 */
esp_err_t led_matrix_stop_flicker_masked(uint8_t channel_mask)
{
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
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

    // Teardown the timer+task only when all channels are now idle.  When this
    // returns, either (a) no channels remain active and the render task has
    // self-deleted (handle is NULL), or (b) channels remain and the render task
    // is still alive.
    s_maybe_teardown_timer_and_task();

    if (matrix_handle) {
        if (led_flicker_task_handle == NULL) {
            // Render task is gone — safe to paint a final black frame ourselves.
            // No concurrent refresh can race with this clear (Bug F).
            // Use led_strip_clear (not led_strip_set_all) because led_strip_set_all
            // returns ESP_ERR_NOT_SUPPORTED for DIRECT mode, leaving stale
            // brightness values in place and the LEDs stuck at whatever state
            // they were in at the moment of stop. led_strip_clear works for
            // all backends — for DIRECT it zeros the brightness array, and
            // the refresh below pushes that to the hardware.
            led_strip_clear(matrix_handle);
            led_strip_refresh(matrix_handle);
        } else {
            // Render task is still running.  Don't touch the strip directly —
            // that would (i) blank the whole strip including channels we didn't
            // stop (Bug E) and (ii) race with led_strip_refresh in the task
            // (Bug F).  Instead wake the task; its next snapshot will paint the
            // stopped channels black because their active/led_state are false,
            // leaving the still-running channels' LEDs untouched.
            xTaskNotifyGive(led_flicker_task_handle);
        }
    }
    return ESP_OK;
}

/**
 * @brief Update flicker parameters on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0xFF.
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

    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        if (!flicker_state[ch].active) continue;

        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->frequency_milliHz   = new_freq_milliHz;
        s->duty_cycle          = duty_cycle;
        s->brightness          = brightness;
        // Do NOT reset cycle_start_time_us here — the channel is already running
        // and its cycle origin must be preserved.  The new frequency takes effect at
        // the next cycle boundary when the ISR recomputes cycle_duration_us from
        // sw_freq.  Resetting the origin would insert a cycle of arbitrary length at
        // the dispatch instant, which is the stutter this fix eliminates.
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
 * @brief Update ONLY brightness on the masked channels.
 *
 * Leaves frequency_milliHz, duty_cycle, sweep state, and cycle timing untouched.
 * Used by the VU sync path so audio amplitude modulates brightness without
 * clobbering each channel's independent flicker frequency.
 */
esp_err_t led_matrix_update_brightness_masked(uint8_t channel_mask, uint8_t brightness)
{
    if (brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        if (!flicker_state[ch].active) continue;

        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->brightness = brightness;
        // Mirror into sw_brightness so the ISR's cycle-boundary recompute
        // (which reads sw_brightness via led_interp_param) preserves this
        // value instead of reverting to the prior sweep target.
        s->sw_brightness = (led_sweep_param_t){
            (uint32_t)brightness * 256u,
            (uint32_t)brightness * 256u,
            LED_INTERP_NONE
        };
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/**
 * @brief Set flicker color on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0xFF.
 */
esp_err_t led_matrix_set_flicker_color_masked(uint8_t channel_mask,
                                               uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
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
 * @param channel_mask Bitmask 0x01-0xFF.
 * @param spec         Pointer to sweep specification; contents are copied per channel.
 */
esp_err_t led_matrix_start_sweep_masked(uint8_t channel_mask, const led_sweep_spec_t *spec,
                                         uint64_t cycle_hint_us)
{
    if (!spec) return ESP_ERR_INVALID_ARG;
    if (!matrix_handle) return ESP_ERR_INVALID_STATE;

    if (spec->duration_ms == 0 && (
            spec->freq_curve   != LED_INTERP_NONE ||
            spec->duty_curve   != LED_INTERP_NONE ||
            spec->bright_curve != LED_INTERP_NONE ||
            spec->r_curve      != LED_INTERP_NONE ||
            spec->g_curve      != LED_INTERP_NONE ||
            spec->b_curve      != LED_INTERP_NONE)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t init_freq_milliHz = (spec->freq_curve != LED_INTERP_NONE)
                                  ? spec->freq_milliHz_start
                                  : spec->freq_milliHz_target;
    if (init_freq_milliHz == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = s_ensure_timer_and_task(init_freq_milliHz);
    if (ret != ESP_OK) return ret;

    uint64_t sweep_duration_us = (uint64_t)spec->duration_ms * 1000ULL;
    // Transport-clock anchor (Layer 2): when the caller supplies a non-zero
    // cycle_hint_us (= transport_origin_us + entry->time_ms * 1000), use it as
    // the sweep origin so all channels at the same logical timestamp share the
    // identical sweep_start_us regardless of dispatch lag.  When zero (legacy
    // callers / trampolines), fall back to wall-clock at call time.
    // The anchor may be in the past; see late-anchor note in start_flicker_masked.
    uint64_t sweep_start_us = (cycle_hint_us != 0) ? cycle_hint_us : esp_timer_get_time();

    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;

        led_flicker_state_t *s = &flicker_state[ch];
        // Critical section: all per-channel writes happen atomically vs the ISR's
        // cycle-boundary read of sw_*/sweep_start_us/sweep_duration_us. Mirrors
        // the pattern in start_flicker_masked. Without this, the ISR could
        // snapshot a half-installed sweep (e.g. new sw_r.start_q but stale
        // sweep_duration_us=0) and snap pixels to a target color for one cycle.
        portENTER_CRITICAL(&s_flicker_mux);

        // For each swept parameter on an already-running channel, the sweep
        // must continue smoothly from the LIVE current value (s->X), not from
        // the parsed entry's literal (spec->X_start). The literal is correct
        // only if the previous sweep completed exactly at the boundary; if it
        // was interrupted (scheduling jitter, manual override), starting from
        // the literal produces a visible "snap". For non-swept params or fresh
        // channels, the entry's literal IS the correct start.
        bool already_active = s->active;
        uint32_t eff_freq_start   = (spec->freq_curve   != LED_INTERP_NONE && already_active) ? s->frequency_milliHz : spec->freq_milliHz_start;
        uint8_t  eff_duty_start   = (spec->duty_curve   != LED_INTERP_NONE && already_active) ? s->duty_cycle        : spec->duty_start;
        uint8_t  eff_bright_start = (spec->bright_curve != LED_INTERP_NONE && already_active) ? s->brightness        : spec->bright_start;
        uint8_t  eff_r_start      = (spec->r_curve      != LED_INTERP_NONE && already_active) ? s->red               : spec->r_start;
        uint8_t  eff_g_start      = (spec->g_curve      != LED_INTERP_NONE && already_active) ? s->green             : spec->g_start;
        uint8_t  eff_b_start      = (spec->b_curve      != LED_INTERP_NONE && already_active) ? s->blue              : spec->b_start;

        // Set initial live values. For swept params the effective start is the
        // live value when active (no-op assignment) or the spec start when fresh.
        // For non-swept params, hold at the spec's literal (start==target).
        s->frequency_milliHz   = (spec->freq_curve   != LED_INTERP_NONE) ? eff_freq_start   : init_freq_milliHz;
        s->duty_cycle          = (spec->duty_curve   != LED_INTERP_NONE) ? eff_duty_start   : spec->duty_target;
        s->brightness          = (spec->bright_curve != LED_INTERP_NONE) ? eff_bright_start : spec->bright_target;
        s->red                 = (spec->r_curve      != LED_INTERP_NONE) ? eff_r_start      : spec->r_target;
        s->green               = (spec->g_curve      != LED_INTERP_NONE) ? eff_g_start      : spec->g_target;
        s->blue                = (spec->b_curve      != LED_INTERP_NONE) ? eff_b_start      : spec->b_target;
        // Only reset the cycle origin on FIRST activation of this channel.
        // When a sweep is dispatched on an already-running channel, preserve the
        // existing cycle_start_time_us so the rhythm continues uninterrupted.
        // The sweep interpolation uses sweep_start_us (below) as its own clock
        // reference — it does not need cycle_start_time_us to be reset.
        // sweep_start_us already honors cycle_hint_us (computed above), so
        // new channels inherit the transport-clock anchor automatically.
        if (!s->active) {
            s->cycle_start_time_us = sweep_start_us;
            // Compute latched_on_time_us from the initial frequency so the first
            // ISR tick knows the on-time without waiting for a cycle boundary.
            uint64_t init_cycle_us = (init_freq_milliHz > 0)
                                     ? (1000000ULL * 1000ULL) / (uint64_t)init_freq_milliHz
                                     : 0ULL;
            uint8_t init_duty = (spec->duty_curve != LED_INTERP_NONE) ? spec->duty_start : spec->duty_target;
            s->latched_on_time_us  = (init_cycle_us * (uint64_t)init_duty) / 100ULL;
            s->led_state           = false;
        }

        // Populate per-param sweep state. Use eff_*_start (live value when the
        // channel is mid-sweep on this param) so the interpolator ramps from
        // where the LED actually is — not from the parsed literal.
        s->sw_freq = (led_sweep_param_t){
            .start_q  = eff_freq_start,
            .target_q = spec->freq_milliHz_target,
            .curve    = (uint8_t)spec->freq_curve,
        };
        s->sw_duty = (led_sweep_param_t){
            .start_q  = (uint32_t)eff_duty_start    * 256u,
            .target_q = (uint32_t)spec->duty_target * 256u,
            .curve    = (uint8_t)spec->duty_curve,
        };
        s->sw_brightness = (led_sweep_param_t){
            .start_q  = (uint32_t)eff_bright_start    * 256u,
            .target_q = (uint32_t)spec->bright_target * 256u,
            .curve    = (uint8_t)spec->bright_curve,
        };
        s->sw_r = (led_sweep_param_t){
            .start_q  = (uint32_t)eff_r_start    * 256u,
            .target_q = (uint32_t)spec->r_target * 256u,
            .curve    = (uint8_t)spec->r_curve,
        };
        s->sw_g = (led_sweep_param_t){
            .start_q  = (uint32_t)eff_g_start    * 256u,
            .target_q = (uint32_t)spec->g_target * 256u,
            .curve    = (uint8_t)spec->g_curve,
        };
        s->sw_b = (led_sweep_param_t){
            .start_q  = (uint32_t)eff_b_start    * 256u,
            .target_q = (uint32_t)spec->b_target * 256u,
            .curve    = (uint8_t)spec->b_curve,
        };
        s->sweep_start_us    = sweep_start_us;
        s->sweep_duration_us = sweep_duration_us;
        s->active            = true;
        portEXIT_CRITICAL(&s_flicker_mux);
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
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
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
    return led_matrix_start_flicker_masked(0x01u, frequency, duty_cycle, brightness, 0);
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
    return led_matrix_start_sweep_masked(0x01u, spec, 0);
}

bool led_matrix_is_flickering(void) {
    // True if ANY of the 4 channels is active.
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (flicker_state[ch].active) return true;
    }
    return false;
}

bool led_matrix_is_flickering_masked(uint8_t channel_mask) {
    // True if ALL channels set in the mask are currently active.
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if ((channel_mask & (1u << ch)) && !flicker_state[ch].active) return false;
    }
    return (channel_mask != 0);
}

uint8_t led_matrix_get_active_mask(void) {
    uint8_t mask = 0;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (flicker_state[ch].active) mask |= (uint8_t)(1u << ch);
    }
    return mask;
}

float led_matrix_get_current_frequency(void) {
    // Return frequency of the lowest-numbered active channel, or 0.
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (flicker_state[ch].active) {
            return (float)flicker_state[ch].frequency_milliHz / 1000.0f;
        }
    }
    return 0.0f;
}

bool led_matrix_supports_pixel_addressing(void) {
    return matrix_handle && led_strip_supports_pixel_addressing(matrix_handle);
}

// Log full state of every active LED channel — current params plus any
// sweep details. Snapshot-then-release pattern; safe to call from any task
// (not from ISR).
void led_matrix_log_full_state(void)
{
    struct led_full_snap {
        bool     active;
        uint32_t frequency_milliHz;
        uint8_t  duty_cycle, brightness, red, green, blue;
        bool     has_sweep[6];
        uint32_t start_q[6], target_q[6];
        float    progress_pct;
        uint32_t remaining_ms;
    } snaps[NUM_LED_CHANNELS] = {0};

    uint64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_flicker_mux);
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        led_flicker_state_t *s = &flicker_state[ch];
        snaps[ch].active = s->active;
        if (!s->active) continue;
        snaps[ch].frequency_milliHz = s->frequency_milliHz;
        snaps[ch].duty_cycle = s->duty_cycle;
        snaps[ch].brightness = s->brightness;
        snaps[ch].red = s->red; snaps[ch].green = s->green; snaps[ch].blue = s->blue;
        snaps[ch].has_sweep[0] = (s->sw_freq.curve       != LED_INTERP_NONE);
        snaps[ch].has_sweep[1] = (s->sw_duty.curve       != LED_INTERP_NONE);
        snaps[ch].has_sweep[2] = (s->sw_brightness.curve != LED_INTERP_NONE);
        snaps[ch].has_sweep[3] = (s->sw_r.curve          != LED_INTERP_NONE);
        snaps[ch].has_sweep[4] = (s->sw_g.curve          != LED_INTERP_NONE);
        snaps[ch].has_sweep[5] = (s->sw_b.curve          != LED_INTERP_NONE);
        snaps[ch].start_q[0]  = s->sw_freq.start_q;       snaps[ch].target_q[0] = s->sw_freq.target_q;
        snaps[ch].start_q[1]  = s->sw_duty.start_q;       snaps[ch].target_q[1] = s->sw_duty.target_q;
        snaps[ch].start_q[2]  = s->sw_brightness.start_q; snaps[ch].target_q[2] = s->sw_brightness.target_q;
        snaps[ch].start_q[3]  = s->sw_r.start_q;          snaps[ch].target_q[3] = s->sw_r.target_q;
        snaps[ch].start_q[4]  = s->sw_g.start_q;          snaps[ch].target_q[4] = s->sw_g.target_q;
        snaps[ch].start_q[5]  = s->sw_b.start_q;          snaps[ch].target_q[5] = s->sw_b.target_q;
        if (s->sweep_duration_us > 0) {
            uint64_t elapsed = (now_us > s->sweep_start_us) ? (now_us - s->sweep_start_us) : 0;
            if (elapsed > s->sweep_duration_us) elapsed = s->sweep_duration_us;
            snaps[ch].progress_pct = 100.0f * (float)elapsed / (float)s->sweep_duration_us;
            snaps[ch].remaining_ms = (uint32_t)((s->sweep_duration_us - elapsed) / 1000ULL);
        }
    }
    portEXIT_CRITICAL(&s_flicker_mux);

    static const char *names[6] = { "freq", "duty", "bri", "R", "G", "B" };
    int n_active = 0;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!snaps[ch].active) continue;
        n_active++;
        ESP_LOGI(TAG, "  LED[ch=%u] freq=%.2fHz duty=%u%% bri=%u%% RGB=(%u,%u,%u)",
                 (unsigned)ch, snaps[ch].frequency_milliHz / 1000.0f,
                 (unsigned)snaps[ch].duty_cycle, (unsigned)snaps[ch].brightness,
                 (unsigned)snaps[ch].red, (unsigned)snaps[ch].green, (unsigned)snaps[ch].blue);
        for (int p = 0; p < 6; p++) {
            if (!snaps[ch].has_sweep[p]) continue;
            if (p == 0) {
                ESP_LOGI(TAG, "    sweep %s: %.2f->%.2fHz  %.0f%% done  %ums left",
                         names[p], snaps[ch].start_q[p] / 1000.0f, snaps[ch].target_q[p] / 1000.0f,
                         snaps[ch].progress_pct, (unsigned)snaps[ch].remaining_ms);
            } else {
                ESP_LOGI(TAG, "    sweep %s: %u->%u  %.0f%% done  %ums left",
                         names[p], (unsigned)(snaps[ch].start_q[p] >> 8),
                         (unsigned)(snaps[ch].target_q[p] >> 8),
                         snaps[ch].progress_pct, (unsigned)snaps[ch].remaining_ms);
            }
        }
    }
    if (n_active == 0) {
        ESP_LOGI(TAG, "  LED: no active channels");
    }
}

// Log one line per active LED sweep across all channels (current interpolated
// value, start->target window, % done, seconds remaining). Snapshots state
// under s_flicker_mux briefly, then releases the spinlock before any
// ESP_LOGI — UART writes under a spinlock would prevent the flicker ISR from
// firing and cause visible LED stutter.
// Returns the number of active sweeps logged.
int led_matrix_log_sweep_progress(void)
{
    struct led_sweep_snap {
        bool     active;
        bool     has_sweep[6];   // 0=freq 1=duty 2=bri 3=R 4=G 5=B
        uint32_t start_q[6];
        uint32_t target_q[6];
        uint32_t current_q[6];   // current interpolated, in same q-units as start/target
        float    progress_pct;
        uint32_t remaining_ms;
    } snaps[NUM_LED_CHANNELS] = {0};

    uint64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_flicker_mux);
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        led_flicker_state_t *s = &flicker_state[ch];
        snaps[ch].active = s->active;
        if (!s->active) continue;

        // Per-param: sweep is active iff curve != LED_INTERP_NONE
        snaps[ch].has_sweep[0] = (s->sw_freq.curve       != LED_INTERP_NONE);
        snaps[ch].has_sweep[1] = (s->sw_duty.curve       != LED_INTERP_NONE);
        snaps[ch].has_sweep[2] = (s->sw_brightness.curve != LED_INTERP_NONE);
        snaps[ch].has_sweep[3] = (s->sw_r.curve          != LED_INTERP_NONE);
        snaps[ch].has_sweep[4] = (s->sw_g.curve          != LED_INTERP_NONE);
        snaps[ch].has_sweep[5] = (s->sw_b.curve          != LED_INTERP_NONE);

        snaps[ch].start_q[0]   = s->sw_freq.start_q;       snaps[ch].target_q[0] = s->sw_freq.target_q;
        snaps[ch].start_q[1]   = s->sw_duty.start_q;       snaps[ch].target_q[1] = s->sw_duty.target_q;
        snaps[ch].start_q[2]   = s->sw_brightness.start_q; snaps[ch].target_q[2] = s->sw_brightness.target_q;
        snaps[ch].start_q[3]   = s->sw_r.start_q;          snaps[ch].target_q[3] = s->sw_r.target_q;
        snaps[ch].start_q[4]   = s->sw_g.start_q;          snaps[ch].target_q[4] = s->sw_g.target_q;
        snaps[ch].start_q[5]   = s->sw_b.start_q;          snaps[ch].target_q[5] = s->sw_b.target_q;

        // Live values that the ISR most recently computed (in display units).
        snaps[ch].current_q[0] = s->frequency_milliHz;
        snaps[ch].current_q[1] = (uint32_t)s->duty_cycle * 256u;   // mirror Q8.8 scale
        snaps[ch].current_q[2] = (uint32_t)s->brightness  * 256u;
        snaps[ch].current_q[3] = (uint32_t)s->red         * 256u;
        snaps[ch].current_q[4] = (uint32_t)s->green       * 256u;
        snaps[ch].current_q[5] = (uint32_t)s->blue        * 256u;

        // Shared progress across all per-param sweeps (single sweep_start/duration per channel).
        if (s->sweep_duration_us > 0) {
            uint64_t elapsed = (now_us > s->sweep_start_us) ? (now_us - s->sweep_start_us) : 0;
            if (elapsed > s->sweep_duration_us) elapsed = s->sweep_duration_us;
            snaps[ch].progress_pct = 100.0f * (float)elapsed / (float)s->sweep_duration_us;
            snaps[ch].remaining_ms = (uint32_t)((s->sweep_duration_us - elapsed) / 1000ULL);
        }
    }
    portEXIT_CRITICAL(&s_flicker_mux);

    static const char *names[6] = { "freq", "duty", "bri ", "R   ", "G   ", "B   " };
    int n_logged = 0;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!snaps[ch].active) continue;
        for (int p = 0; p < 6; p++) {
            if (!snaps[ch].has_sweep[p]) continue;
            // freq logs in Hz (milliHz / 1000); others log raw byte value (Q8.8 >> 8).
            if (p == 0) {
                ESP_LOGI(TAG, "  LED[ch=%u] %s: %.2fHz  [%.2f->%.2f  %.0f%%  %ums left]",
                         (unsigned)ch, names[p],
                         snaps[ch].current_q[p] / 1000.0f,
                         snaps[ch].start_q[p]   / 1000.0f,
                         snaps[ch].target_q[p]  / 1000.0f,
                         snaps[ch].progress_pct,
                         (unsigned)snaps[ch].remaining_ms);
            } else {
                ESP_LOGI(TAG, "  LED[ch=%u] %s: %u  [%u->%u  %.0f%%  %ums left]",
                         (unsigned)ch, names[p],
                         (unsigned)(snaps[ch].current_q[p] >> 8),
                         (unsigned)(snaps[ch].start_q[p]   >> 8),
                         (unsigned)(snaps[ch].target_q[p]  >> 8),
                         snaps[ch].progress_pct,
                         (unsigned)snaps[ch].remaining_ms);
            }
            n_logged++;
        }
    }
    return n_logged;
}
