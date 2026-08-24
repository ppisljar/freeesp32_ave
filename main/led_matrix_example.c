// Step 7: led_matrix_example.c updated to use per-channel API (led_strip_set_channel).
// region_grid and led_to_channel_bit LUT removed — channel mapping lives in led_strip.c.

#include "led_matrix_example.h"
#include "led_strip.h"
#include "audio_config.h"
#include "settings.h"
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

/*
 * Periodic modulation slot — used by the mod_engine prefixes (^~/\_).
 * One slot per modulatable field per channel; evaluated inside the ISR's
 * cycle-boundary block to produce a fresh value, overriding any sweep
 * result for the same param. Values are stored in the same units as the
 * matching sw_X (milliHz for freq, Q8.8 for duty/bright/RGB) so the ISR
 * can write the evaluator result directly to the live field.
 *
 * Wave shapes (0..4):
 *   0 = triangle, 1 = sine (parabolic approximation), 2 = saw-up,
 *   3 = saw-down, 4 = square.
 *
 * active=false means inactive (sweep result is used).
 */
typedef struct {
    volatile bool     active;
    uint8_t           wave;          // mod_wave_t value
    int32_t           start_q;       // start value in field units
    int32_t           end_q;         // end value
    uint32_t          period_us;     // full cycle in microseconds
    volatile uint64_t start_time_us; // when the modulation began
} led_mod_slot_t;

// Per-channel flicker and sweep state. One instance per logical LED zone.
typedef struct {
    volatile bool active;              // Flicker currently running (read by ISR)
    volatile uint32_t frequency_milliHz; // Current flicker frequency (Hz * 1000 for precision)
    volatile uint8_t duty_cycle;       // On-time percentage (0-100); written at cycle boundary by ISR
    volatile uint8_t brightness;       // Maximum brightness (0-100); written at cycle boundary by ISR
    // Q8.8 companions of duty_cycle/brightness (value = percent<<8, 0..25600). The
    // ISR keeps these at full sweep resolution so slow fades over a small range
    // render smoothly instead of stepping across the ~18 integer-percent levels.
    // The uint8_t fields above are kept for reporting / sweep-restart carryover.
    volatile uint16_t duty_q8;         // Q8.8 on-time percent; drives latched_on_time_us
    volatile uint16_t brightness_q8;   // Q8.8 max brightness; handed to the strip layer
    volatile uint8_t red, green, blue; // LED colors when ON; written at cycle boundary by ISR
    volatile bool led_state;           // Target ON/OFF state; written by ISR, read by flicker task
    volatile bool led_dirty;           // ISR sets true when led_state changes; task clears it
    volatile uint64_t cycle_start_time_us; // Timestamp of current cycle start (ISR-owned)
    volatile uint64_t latched_on_time_us;  // ON-time latched at cycle boundary; prevents mid-cycle duty glitch

    // CPU-efficiency caches (ISR-owned; purely derived — do NOT affect output).
    // cached_cycle_duration_us mirrors 1e9/frequency_milliHz; recomputed lazily
    // ONLY when cd_cache_freq != frequency_milliHz, so the 64-bit divide runs on
    // frequency change instead of every tick. Same value as the per-tick divide.
    volatile uint64_t cached_cycle_duration_us;
    volatile uint32_t cd_cache_freq;       // frequency_milliHz the cache was computed for
    // Smooth-carrier per-cycle-stable derived values (A3). Recomputed lazily only
    // when duty_q8 or frequency changes (keyed by a3_cache_duty_q8/a3_cache_freq),
    // read per-tick otherwise. Bit-identical to the per-tick divides.
    volatile uint32_t cached_duty_q16;     // (duty_q8 * 65536) / LED_BRIGHTNESS_Q8_MAX
    volatile uint32_t cached_attack_q16;   // (clamp(attack_ms*1000) << 16) / cycle_us
    volatile uint16_t a3_cache_duty_q8;    // duty_q8 the A3 cache was computed for
    volatile uint32_t a3_cache_freq;       // frequency_milliHz the A3 cache was computed for
    volatile uint16_t a3_cache_attack_ms;  // attack_ms the A3 cache was computed for
    // DC (freq==0) constant-output gate: when set, the last DC recompute produced a
    // value that cannot change until a sweep/mod becomes active, so per-tick recompute
    // is skipped. Cleared whenever a sweep/mod is (re)installed or channel (re)starts.
    volatile bool     dc_constant_valid;

    // B2 (entrainment_firmware_plan): carrier waveform of the flicker itself.
    // SQUARE (default) keeps the exact legacy on/off behaviour. SINE/TRIANGLE make
    // the per-tick output a smooth brightness curve (output_level) — clean single-
    // frequency drive, far less fatigue, and (with V-E2 antiphase) luminance-flat
    // "invisible" flicker because a raised-sine antiphase pair sums to a constant.
    // The shape is LUMINANCE-LINEAR on purpose (no perceptual gamma) so antiphase
    // pairs cancel in photons; PWM/brightness is ~linear in luminance.
    uint8_t           carrier_waveform;    // LED_CARRIER_*; latched at channel start
    uint16_t          attack_ms;           // trapezoid edge duration (latched at start)
    uint32_t          jitter_millihz;      // flicker-rate jitter amplitude (0 = off)
    uint32_t          jitter_period_us;    // jitter wander period
    volatile uint8_t  output_level;        // non-square: ISR-computed 0..brightness output (coarse, kept for compat)
    volatile uint16_t output_level_q8;     // non-square: Q8.8 (0..brightness_q8) output handed to the strip

    // V-E2 (entrainment_firmware_plan): flicker phase offset in DEGREES (0..359).
    // Applied at USE-TIME in the ISR (delay = deg * live_period / 360), so it stays
    // a fixed *phase* through frequency ramps, not a fixed delay. Two channels that
    // share a transport origin (same .ledc time / peer-piggyback) re-anchor in
    // lockstep, so a 180° offset gives rock-solid antiphase → luminance-flat
    // "invisible" flicker for a complementary-colour (e.g. cool/warm) pair.
    volatile int16_t  phase_offset_deg;    // 0..359

    // Sweep state — written by led_matrix_start_sweep_masked() (task context),
    // read at cycle boundaries inside the ISR.  6 parameters × led_sweep_param_t.
    led_sweep_param_t sw_freq;        // milliHz units
    led_sweep_param_t sw_duty;        // Q8.8 units
    led_sweep_param_t sw_brightness;  // Q8.8 units
    led_sweep_param_t sw_phase;       // degrees (0..359)
    led_sweep_param_t sw_attack;      // milliseconds
    led_sweep_param_t sw_r;           // Q8.8 units
    led_sweep_param_t sw_g;           // Q8.8 units
    led_sweep_param_t sw_b;           // Q8.8 units
    volatile uint64_t sweep_start_us;     // esp_timer_get_time() when sweep began
    volatile uint64_t sweep_duration_us;  // Total sweep duration in microseconds

    // Modulation slots — written by led_matrix_set_mod_masked() (task ctx),
    // read at cycle boundaries inside the ISR.  Take priority over sweep
    // when active.
    led_mod_slot_t mod_freq;
    led_mod_slot_t mod_duty;
    led_mod_slot_t mod_brightness;
    led_mod_slot_t mod_r;
    led_mod_slot_t mod_g;
    led_mod_slot_t mod_b;
    led_mod_slot_t mod_phase;    // degrees (natural units, not Q8.8)
    led_mod_slot_t mod_attack;   // milliseconds
} led_flicker_state_t;

// NUM_LED_CHANNELS independent channel states (bits 0..N-1 of channel_mask map to channels 1..N).
static led_flicker_state_t flicker_state[NUM_LED_CHANNELS] = {
    [0 ... NUM_LED_CHANNELS - 1] = {
        .active               = false,
        .frequency_milliHz    = 0,
        .duty_cycle           = 50,
        .brightness           = 100,
        .duty_q8              = 50 * 256,
        .brightness_q8        = 100 * 256,
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
        .sw_phase       = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_attack      = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_r           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_g           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sw_b           = { .start_q = 0, .target_q = 0, .curve = 0 },
        .sweep_start_us    = 0,
        .sweep_duration_us = 0,
    },
};

// Flicker carrier waveforms — match ledc_format.md env values: 0 square (legacy
// on/off), 1 sine, 2 triangle, 3 trapezoid (uses duty + attack). (env=4 tremolo
// has no LED meaning → treated as square.)
#define LED_CARRIER_SQUARE    0u
#define LED_CARRIER_SINE      1u
#define LED_CARRIER_TRIANGLE  2u
#define LED_CARRIER_TRAPEZOID 3u

// Module-wide carrier + attack + jitter defaults, latched into each channel at
// flicker start. Default SQUARE / jitter off → existing sessions behave identically.
static volatile uint8_t  s_flicker_carrier   = LED_CARRIER_SQUARE;
static volatile uint16_t s_flicker_attack_ms = 3u;   // trapezoid edge duration
static volatile uint32_t s_flicker_jitter_millihz   = 0u;      // flicker-rate jitter amp (0 = off)
static volatile uint32_t s_flicker_jitter_period_us = 45000000u; // wander period (default 45 s)

// V-E1 (entrainment_firmware_plan) — flicker ISR tick sizing. The tick scales UP
// with the highest active flicker frequency so edges/phase stay accurate (clean
// antiphase for invisible flicker needs fine edges at 40 Hz). At MULT=250 a 40 Hz
// flicker gets 250 ticks/cycle (0.4% edge error) vs the old fixed 1 kHz (8%). The
// tick only ever increases within a session; MAX caps ISR CPU (~8%/8ch at 10 kHz).
#define LED_FLICKER_TICK_MULT  250u    // ISR ticks per flicker cycle target
#define LED_FLICKER_TICK_MIN   1000u   // floor (Hz) — low-frequency flicker
#define LED_FLICKER_TICK_MAX   10000u  // ceiling (Hz) — CPU guard
static uint32_t s_flicker_tick_hz = 0; // current ISR tick rate (Hz); 0 = not yet set

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

    uint8_t grid_w = (uint8_t)settings_get()->led_grid_width;
    uint8_t grid_h = (uint8_t)settings_get()->led_grid_height;

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

    // Clear all LEDs / channels on startup. led_strip_clear() pushes the
    // cleared buffer to hardware internally (see s_direct_clear → s_direct_refresh
    // in led_strip.c, and the equivalent path in the addressable backends),
    // so a follow-up led_strip_refresh() is redundant. Worse — for the
    // DIRECT backend, two back-to-back ledc_update_duty bursts at boot can
    // race the LEDC timer's first PWM cycle: the second burst's poll on
    // duty_start can spin forever if the timer hasn't yet completed a
    // 40 µs cycle to self-clear the first burst's pending duty update,
    // tripping the Interrupt Watchdog (IWDT, 300 ms) during boot.
    led_strip_clear(matrix_handle);

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

/*
 * Evaluate a modulation slot to its current value at time `now_us`. Pure
 * integer math + IRAM-safe — runs inside the LED ISR alongside the sweep
 * interpolator. Wave shapes encoded as small integers (matches mod_wave_t
 * in mod_engine.h):
 *   0 = TRIANGLE, 1 = SINE (parabolic approx), 2 = SAW_UP,
 *   3 = SAW_DOWN, 4 = SQUARE
 *
 * Sine uses the parabolic approximation 4·phase·(1−phase) which closely
 * matches (1−cos(2π·phase))/2 — accurate to ~5% mid-quarter, visually
 * indistinguishable from true sine, and avoids the IRAM-incompatible cosf.
 *
 * Returns the modulated value in the slot's native units. Caller is
 * responsible for unit conversion if needed (e.g., the duty/brightness
 * fields strip the Q8.8 fractional bits with >> 8 after this call).
 */
static inline int32_t IRAM_ATTR led_eval_mod_iram(const led_mod_slot_t *m, uint64_t now_us) {
    if (m->period_us == 0) return m->start_q;
    /* CRITICAL: this function runs inside the 1 kHz LED ISR (IRAM context).
     * It MUST NOT call any libgcc helper that lives in flash (__udivdi3,
     * __umoddi3, __muldi3, etc.) — when the ISR fires during a flash-cache-
     * disabled window (WiFi radio ops, SPI flash writes, NVS commits), the
     * CPU stalls inside the cache-fault handler waiting for flash code that
     * is currently inaccessible. The IWDT can't fire (it's in flash too),
     * so the result is a total system freeze with no panic dump.
     *
     * Therefore: NO 64-bit arithmetic anywhere in this function. Every op
     * must lower to a native Xtensa instruction. Specifically:
     *   - 64-bit divide/modulo → __udivdi3 / __umoddi3 (flash)  — BANNED
     *   - 64-bit multiply       → __muldi3              (flash)  — BANNED
     *   - Right-shift of int64  → __ashrdi3             (flash)  — BANNED
     * 32-bit divide/modulo lowers to native UDIVMOD on Xtensa; safe. */

    /* Step 1 — elapsed time within current cycle, all 32-bit.
     * The full uint64_t elapsed_us is only needed if start_time_us and
     * now_us can be more than 2^32 µs (~71 minutes) apart. Below 71 min,
     * truncating both to uint32_t and subtracting (with wraparound) gives
     * the correct elapsed value. Modulation periods are << 71 min, so the
     * truncation is safe for the modulo operation that follows. */
    uint32_t t_in_cycle;
    {
        uint32_t now_lo   = (uint32_t)now_us;
        uint32_t start_lo = (uint32_t)m->start_time_us;
        uint32_t elapsed  = now_lo - start_lo;   // unsigned wrap is well-defined
        t_in_cycle = elapsed % m->period_us;     // 32/32 → native UDIVMOD
    }

    /* Step 2 — phase_q16 = t_in_cycle * 65536 / period_us, all 32-bit.
     * Naively (t_in_cycle << 16) might overflow uint32_t when period_us is
     * large (> 65535). To stay 32-bit, rescale: shift period_us right until
     * it fits in 16 bits, shift t_in_cycle by the same amount, then do the
     * 32/32 divide. The precision loss is at most `shift` low bits of the
     * phase, which is invisible for visual modulation. */
    uint32_t period = m->period_us;
    uint32_t t      = t_in_cycle;
    while (period > 0xFFFFu) { period >>= 1; t >>= 1; }
    uint32_t phase_q16 = (t << 16) / period;  // 32/32 → native UDIVMOD
    if (phase_q16 > 65536u) phase_q16 = 65536u;
    uint32_t shape_q16; // 0..65536 result, where 0 = start, 65536 = end
    switch (m->wave) {
        case 0: // TRIANGLE
            shape_q16 = (phase_q16 < 32768u)
                        ? (phase_q16 << 1)
                        : (131072u - (phase_q16 << 1));
            break;
        case 1: { // SINE (parabolic approx: 4·x·(1−x), scaled to 0..65536)
            /* 32-bit safe: scale phase_q16 down to Q8 (0..256) first so the
             * multiply stays in 32 bits. Loses 8 bits of precision but that's
             * still 1/256 = 0.4% — invisible for visual modulation. */
            uint32_t p_q8       = phase_q16 >> 8;            // 0..256
            uint32_t one_minus  = 256u - p_q8;               // 0..256
            uint32_t prod_q16   = p_q8 * one_minus;          // max 128*128 = 16384, fits 32-bit
            shape_q16           = prod_q16 << 2;             // *4 → max 65536, fits uint32_t
            if (shape_q16 > 65536u) shape_q16 = 65536u;
            break;
        }
        case 2: // SAW_UP — linear ramp 0→1, then jump back
            shape_q16 = phase_q16;
            break;
        case 3: // SAW_DOWN — linear ramp 1→0, then jump back to 1
            shape_q16 = 65536u - phase_q16;
            break;
        case 4: // SQUARE
            shape_q16 = (phase_q16 < 32768u) ? 0u : 65536u;
            break;
        default:
            shape_q16 = 0u;
            break;
    }
    /* Final interpolation: result = start_q + delta * shape_q16 / 65536, all 32-bit.
     * delta * shape_q16 can exceed int32 range (e.g. freq mod with delta in
     * milliHz can be ~10^6, and shape_q16 up to 65536 → product ~7×10^10).
     * Split shape_q16 into two 8-bit halves so each partial product fits
     * easily in int32:
     *   shape_q16 = hi8 * 256 + mid8 * 1 + (lo8 / 256)   where hi8 is 0..256
     *   Actually simpler: shape_q16 / 65536 = (hi << 8 + lo) / 65536
     *                                       = hi/256 + lo/65536  (256 == 1<<8)
     *   result = start + delta * hi / 256 + delta * lo / 65536
     * delta * hi: max 10^6 * 256 = 2.56×10^8, fits int32 (max 2.14×10^9). */
    int32_t delta   = m->end_q - m->start_q;
    uint32_t hi     = shape_q16 >> 8;             // 0..256
    uint32_t lo     = shape_q16 & 0xFFu;          // 0..255
    int32_t scaled  = (int32_t)((delta * (int32_t)hi) >> 8)
                    + (int32_t)((delta * (int32_t)lo) >> 16);
    return m->start_q + scaled;
}

/*
 * B2 — per-tick flicker CARRIER output level (0..brightness) for non-square
 * carriers. IRAM-safe / 32-bit-only (same rules as led_eval_mod_iram: no 64-bit
 * ops, no flash libgcc helpers). SINE reuses the parabolic (1−cos(2πx))/2 ≈
 * 4x(1−x) approximation; TRIANGLE is the linear fold. The shape is
 * LUMINANCE-LINEAR (no perceptual gamma) so a 180°-antiphase pair of raised
 * sines sums to a constant → luminance-flat "invisible" flicker.
 */
static inline uint16_t IRAM_ATTR led_carrier_level_iram(uint8_t wave, uint32_t elapsed_us,
                                                       uint32_t cycle_us, uint16_t brightness_q8,
                                                       uint32_t duty_q16, uint32_t attack_q16) {
    if (cycle_us == 0u) return brightness_q8;
    // phase_q16 = elapsed_us * 65536 / cycle_us — rescale to stay 32-bit.
    uint32_t period = cycle_us;
    uint32_t t      = elapsed_us;
    while (period > 0xFFFFu) { period >>= 1; t >>= 1; }
    uint32_t phase_q16 = (t << 16) / period;      // native UDIVMOD
    if (phase_q16 > 65536u) phase_q16 = 65536u;
    uint32_t shape_q16;
    if (wave == LED_CARRIER_TRIANGLE) {
        shape_q16 = (phase_q16 < 32768u) ? (phase_q16 << 1) : (131072u - (phase_q16 << 1));
    } else if (wave == LED_CARRIER_TRAPEZOID) {
        // Duty-gated with raised (linear) edges. duty_q16/attack_q16 are Q16 fractions
        // of the cycle. phase<a and (duty-phase)<a stay < 32768, so <<16 fits u32.
        if (phase_q16 >= duty_q16) {
            shape_q16 = 0u;
        } else {
            uint32_t a = attack_q16;
            uint32_t half = duty_q16 >> 1;
            if (a > half) a = half;
            if (a == 0u)                       shape_q16 = 65536u;
            else if (phase_q16 < a)            shape_q16 = (phase_q16 << 16) / a;
            else if (phase_q16 > duty_q16 - a) shape_q16 = ((duty_q16 - phase_q16) << 16) / a;
            else                               shape_q16 = 65536u;
            if (shape_q16 > 65536u) shape_q16 = 65536u;
        }
    } else { // SINE (parabola 4·x·(1−x))
        uint32_t p_q8      = phase_q16 >> 8;       // 0..256
        uint32_t one_minus = 256u - p_q8;
        shape_q16          = (p_q8 * one_minus) << 2;
        if (shape_q16 > 65536u) shape_q16 = 65536u;
    }
    // output = brightness_q8 * shape / 65536; brightness_q8 ≤25600, product ≤1.68e9 fits u32.
    return (uint16_t)(((uint32_t)brightness_q8 * shape_q16) >> 16);
}

// LED flicker-rate jitter (anti-habituation): a slow bipolar triangle LFO returning
// a signed millihertz offset. IRAM-safe / 32-bit only. amp_millihz kept small
// (<~5 Hz), so amp*tri fits int32; no 64-bit.
static inline int32_t IRAM_ATTR led_jitter_millihz_iram(uint32_t now_lo, uint32_t amp_millihz,
                                                        uint32_t period_us) {
    if (amp_millihz == 0u || period_us == 0u) return 0;
    uint32_t period = period_us;
    uint32_t t      = now_lo % period_us;   // 0..period
    while (period > 0xFFFFu) { period >>= 1; t >>= 1; }
    uint32_t ph = (t << 16) / period;       // Q16 phase 0..65536
    // Bipolar triangle in Q16 (−65536..+65536): up, down, up.
    int32_t tri;
    if (ph < 16384u)      tri =  (int32_t)(ph << 2);              // 0 → +1
    else if (ph < 49152u) tri =  131072 - (int32_t)(ph << 2);    // +1 → −1
    else                  tri =  (int32_t)(ph << 2) - 262144;    // −1 → 0
    return ((int32_t)amp_millihz * tri) >> 16;                    // amp·tri, arithmetic shift
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

        // --- DC channel (frequency 0): constant-on, no flicker gate ----------
        // Evaluate the brightness/colour SWEEPS and any periodic MODULATION
        // EVERY tick (not just at flicker-cycle boundaries, which the freq=0
        // channel has none of). This is what lets a single channel run a fast
        // chromatic swap — e.g. a 40 Hz blue<->green antiphase colour mod that
        // stays luminance-flat — instead of a spatial two-ring antiphase pair.
        // It also avoids the divide-by-zero the cycle path below would hit.
        if (s->frequency_milliHz == 0u) {
            // A4: whether any brightness/colour input can still vary this tick.
            // Modulation is continuous → never constant while active. A sweep is
            // time-varying only while its progress has not yet reached the target
            // (sweep_duration_us != 0 AND now_us < sweep_start_us + duration). Once
            // it has snapped to target (or there is no duration), the interpolated
            // value stops changing, so the DC output is constant. When constant and
            // already published (dc_constant_valid), skip the whole recompute — the
            // published fields are byte-for-byte what the per-tick recompute would
            // reproduce, so output is identical.
            bool mod_active = s->mod_brightness.active || s->mod_r.active ||
                              s->mod_g.active || s->mod_b.active;
            uint64_t sweep_dur = s->sweep_duration_us;
            bool sweep_varying = (sweep_dur != 0) &&
                                 (now_us < s->sweep_start_us + sweep_dur);
            if (!mod_active && !sweep_varying && s->dc_constant_valid) {
                continue;   // constant output already published — nothing to do
            }

            // Compute all DC outputs into LOCALS first (A1: math outside spinlock).
            uint32_t dc_prog;
            if (sweep_dur == 0) {
                dc_prog = 65536u;
            } else {
                uint64_t es = (now_us >= s->sweep_start_us) ? (now_us - s->sweep_start_us) : 0;
                uint64_t p  = (es * 65536ULL) / sweep_dur;
                dc_prog = (p > 65536ULL) ? 65536u : (uint32_t)p;
            }
            uint32_t bri_q8 = led_interp_param(&s->sw_brightness, dc_prog);
            if (s->mod_brightness.active) { int32_t mv = led_eval_mod_iram(&s->mod_brightness, now_us); bri_q8 = (mv < 0) ? 0 : (uint32_t)mv; }
            uint32_t r_q8 = led_interp_param(&s->sw_r, dc_prog);
            uint32_t g_q8 = led_interp_param(&s->sw_g, dc_prog);
            uint32_t b_q8 = led_interp_param(&s->sw_b, dc_prog);
            if (s->mod_r.active) { int32_t mv = led_eval_mod_iram(&s->mod_r, now_us); r_q8 = (mv < 0) ? 0 : (uint32_t)mv; }
            if (s->mod_g.active) { int32_t mv = led_eval_mod_iram(&s->mod_g, now_us); g_q8 = (mv < 0) ? 0 : (uint32_t)mv; }
            if (s->mod_b.active) { int32_t mv = led_eval_mod_iram(&s->mod_b, now_us); b_q8 = (mv < 0) ? 0 : (uint32_t)mv; }
            uint16_t new_bri_q8 = (uint16_t)bri_q8;
            uint8_t nr = (uint8_t)(r_q8 >> 8), ng = (uint8_t)(g_q8 >> 8), nb = (uint8_t)(b_q8 >> 8);
            uint8_t new_bri = (uint8_t)(new_bri_q8 >> 8);
            // Constant iff nothing time-varying remains after this tick.
            bool now_constant = !mod_active && !sweep_varying;

            // A1: publish the handful of output fields under the spinlock only.
            portENTER_CRITICAL_ISR(&s_flicker_mux);
            bool changed = !s->led_state || new_bri_q8 != s->brightness_q8 ||
                           nr != s->red || ng != s->green || nb != s->blue;
            s->brightness_q8 = new_bri_q8;
            s->brightness    = new_bri;
            s->red = nr; s->green = ng; s->blue = nb;
            s->led_state = true;              // DC = always on (no duty gate)
            s->output_level_q8 = new_bri_q8;  // smooth-carrier readers get full level
            s->output_level    = new_bri;
            s->dc_constant_valid = now_constant;
            if (changed) { s->led_dirty = true; any_dirty = true; }
            portEXIT_CRITICAL_ISR(&s_flicker_mux);
            continue;
        }

        // A2: cycle_duration_us = 1,000,000,000 / frequency_milliHz. The 64-bit
        // divide is cached and only recomputed when frequency_milliHz changes
        // (from any writer — task API or the cycle-boundary recompute below), so
        // per-tick we read the cached value. Bit-identical to the per-tick divide.
        uint32_t cur_freq_mhz = s->frequency_milliHz;
        if (s->cd_cache_freq != cur_freq_mhz) {
            s->cached_cycle_duration_us = (1000000ULL * 1000ULL) / cur_freq_mhz;
            s->cd_cache_freq = cur_freq_mhz;
        }
        uint64_t cycle_duration_us = s->cached_cycle_duration_us;
        // now_us >= cycle_start_time_us is guaranteed by the pre-anchor check above.
        uint64_t elapsed_us = now_us - s->cycle_start_time_us;

        // --- Cycle boundary: advance cycle and recompute all swept params ---
        // A1: to shrink interrupts-off time, the per-channel sweep/mod/interp math
        // is computed into LOCAL variables. The task-context writers of sw_*/sweep_*/
        // red/green/blue are guarded by s_flicker_mux, so we take the spinlock ONCE
        // to snapshot those shared inputs, release it while doing the arithmetic,
        // then take it again only to PUBLISH the handful of output fields. The
        // snapshot is a consistent (untorn) view exactly as before, and the
        // published values are bit-identical to the previous in-lock computation.
        if (elapsed_us >= cycle_duration_us) {
            // -- snapshot shared inputs (consistent view vs task writers) --
            portENTER_CRITICAL_ISR(&s_flicker_mux);
            led_sweep_param_t sw_freq = s->sw_freq, sw_duty = s->sw_duty,
                              sw_bri = s->sw_brightness, sw_r = s->sw_r,
                              sw_g = s->sw_g, sw_b = s->sw_b;
            led_mod_slot_t md_freq = s->mod_freq, md_duty = s->mod_duty,
                           md_bri = s->mod_brightness, md_r = s->mod_r,
                           md_g = s->mod_g, md_b = s->mod_b;
            uint64_t snap_sweep_start = s->sweep_start_us;
            uint64_t snap_sweep_dur   = s->sweep_duration_us;
            uint32_t snap_jitter_mhz  = s->jitter_millihz;
            uint32_t snap_jitter_per  = s->jitter_period_us;
            portEXIT_CRITICAL_ISR(&s_flicker_mux);

            // -- compute everything into locals, spinlock released --
            uint32_t progress_q16;
            if (snap_sweep_dur == 0) {
                progress_q16 = 65536u; // Snap to target immediately if no duration.
            } else {
                // Guard against unsigned underflow: sweep_start_us may be in the future
                // (logical_anchor_us is offset forward by AUDIO_DMA_PIPELINE_LAG_US so the
                // LED cycle-origin aligns with DAC sample emission). Treat "anchor in
                // future" as progress=0, not as wrap-around → 65536 (snap-to-target).
                uint64_t elapsed_sweep = (now_us >= snap_sweep_start)
                                         ? (now_us - snap_sweep_start)
                                         : 0;
                uint64_t prog64 = (elapsed_sweep * 65536ULL) / snap_sweep_dur;
                progress_q16 = (prog64 > 65536ULL) ? 65536u : (uint32_t)prog64;
            }

            // For each param: first compute the sweep result; then if a
            // modulation is active on that field, evaluate it and override.
            // Modulation wins because it's continuous (sweep is one-shot).
            // Frequency (milliHz units) — guard against zero.
            uint32_t new_freq = led_interp_param(&sw_freq, progress_q16);
            if (md_freq.active) {
                int32_t mv = led_eval_mod_iram(&md_freq, now_us);
                if (mv > 0) new_freq = (uint32_t)mv;
            }
            bool freq_changed = false;
            if (new_freq > 0) {
                // Complement 3: anti-habituation flicker-rate jitter (default off).
                if (snap_jitter_mhz > 0u) {
                    int32_t off = led_jitter_millihz_iram((uint32_t)now_us,
                                                          snap_jitter_mhz, snap_jitter_per);
                    int32_t jf = (int32_t)new_freq + off;
                    if (jf < 100) jf = 100;            // clamp ≥ 0.1 Hz
                    new_freq = (uint32_t)jf;
                }
                cycle_duration_us = (1000000ULL * 1000ULL) / new_freq;
                freq_changed = true;
            }

            // Duty (Q8.8 → truncate to uint8_t 0-100).
            uint32_t duty_q8 = led_interp_param(&sw_duty, progress_q16);
            if (md_duty.active) {
                int32_t mv = led_eval_mod_iram(&md_duty, now_us);
                duty_q8 = (mv < 0) ? 0 : (uint32_t)mv;
            }
            uint8_t  new_duty_cycle = (uint8_t)(duty_q8 >> 8);
            uint16_t new_duty_q8    = (uint16_t)duty_q8;   // full Q8.8 (0..25600)
            // Latch on_time_us at cycle boundary so mid-cycle duty writes from task
            // context take effect only at the next cycle, never mid-cycle. Use the
            // Q8.8 duty so a duty sweep moves the on-time smoothly, not in 1% steps.
            uint64_t new_latched_on = (cycle_duration_us * new_duty_q8) / LED_BRIGHTNESS_Q8_MAX;

            // Brightness (Q8.8 → truncate to uint8_t 0-100).
            uint32_t bri_q8 = led_interp_param(&sw_bri, progress_q16);
            if (md_bri.active) {
                int32_t mv = led_eval_mod_iram(&md_bri, now_us);
                bri_q8 = (mv < 0) ? 0 : (uint32_t)mv;
            }
            uint8_t  new_brightness    = (uint8_t)(bri_q8 >> 8);
            uint16_t new_brightness_q8 = (uint16_t)bri_q8;   // full Q8.8 → smooth fades

            // Colour channels (Q8.8 → truncate to uint8_t 0-255).
            uint32_t r_q8 = led_interp_param(&sw_r, progress_q16);
            uint32_t g_q8 = led_interp_param(&sw_g, progress_q16);
            uint32_t b_q8 = led_interp_param(&sw_b, progress_q16);
            if (md_r.active) { int32_t mv = led_eval_mod_iram(&md_r, now_us); r_q8 = (mv < 0) ? 0 : (uint32_t)mv; }
            if (md_g.active) { int32_t mv = led_eval_mod_iram(&md_g, now_us); g_q8 = (mv < 0) ? 0 : (uint32_t)mv; }
            if (md_b.active) { int32_t mv = led_eval_mod_iram(&md_b, now_us); b_q8 = (mv < 0) ? 0 : (uint32_t)mv; }
            uint8_t new_r = (uint8_t)(r_q8 >> 8);
            uint8_t new_g = (uint8_t)(g_q8 >> 8);
            uint8_t new_b = (uint8_t)(b_q8 >> 8);

            // -- publish outputs under the spinlock (bit-identical values) --
            portENTER_CRITICAL_ISR(&s_flicker_mux);
            s->cycle_start_time_us = now_us;
            if (freq_changed) {
                s->frequency_milliHz        = new_freq;
                s->cached_cycle_duration_us = cycle_duration_us;  // A2 cache stays coherent
                s->cd_cache_freq            = new_freq;
            }
            s->duty_cycle         = new_duty_cycle;
            s->duty_q8            = new_duty_q8;
            s->latched_on_time_us = new_latched_on;
            s->brightness         = new_brightness;
            s->brightness_q8      = new_brightness_q8;
            s->red   = new_r;
            s->green = new_g;
            s->blue  = new_b;
            portEXIT_CRITICAL_ISR(&s_flicker_mux);
            elapsed_us = 0;
        }

        // V-E2: apply the phase offset at use-time from the LIVE period, so it is a
        // fixed phase (not a fixed delay) and survives frequency ramps. 32-bit safe:
        // deg≤359, cycle_us≤1e7 (flicker ≥0.1 Hz) → product <4.3e9 fits uint32.
        uint32_t eff_elapsed = (uint32_t)elapsed_us;
        if (s->phase_offset_deg != 0) {
            uint32_t cyc = (uint32_t)cycle_duration_us;
            uint32_t off = ((uint32_t)s->phase_offset_deg * cyc) / 360u;
            eff_elapsed += off;
            if (eff_elapsed >= cyc) eff_elapsed -= cyc;   // wrap (eff < 2*cyc)
        }

        if (s->carrier_waveform == LED_CARRIER_SQUARE) {
            // Legacy path (phase-offset aware). Duty-gated on/off; dirty at edges.
            bool should_be_on = (eff_elapsed < (uint32_t)s->latched_on_time_us);
            if (should_be_on != s->led_state) {
                s->led_state = should_be_on;
                s->led_dirty = true;
                any_dirty = true;
            }
        } else {
            // B2 smooth carrier — output_level follows a per-tick brightness curve.
            // Dirty whenever the level changes (≈ every tick), so the task refreshes
            // the strip continuously. Cheap for DIRECT/DOTSTAR; heavier for NEOPIXEL.
            //
            // A3: duty_q16/attack_q16 depend only on cycle-boundary-stable inputs
            // (duty_q8, attack_ms, cycle_duration_us). Cache keyed on all three:
            // recompute the two divides ONLY when one changes, otherwise read the
            // cached values — bit-identical to the per-tick form.
            //
            // attack_ms used to be latched at channel start and never change, so
            // the key omitted it. It is a compound cell now and can ramp, so a
            // key without it would serve a stale edge width until duty or
            // frequency happened to move.
            uint32_t cyc_us = (uint32_t)cycle_duration_us;
            // Key on cd_cache_freq (the frequency cyc_us was derived from, updated at
            // the cycle boundary) — NOT cur_freq_mhz (read at top-of-tick, still the
            // OLD rate on a boundary tick that changed frequency). This guarantees
            // attack_q16 (which depends on cyc_us) recomputes on the exact tick the
            // rate changes, so a trapezoid carrier stays bit-identical across sweeps.
            if (s->a3_cache_duty_q8 != s->duty_q8 || s->a3_cache_freq != s->cd_cache_freq ||
                s->a3_cache_attack_ms != s->attack_ms) {
                uint32_t d16 = ((uint32_t)s->duty_q8 * 65536u) / LED_BRIGHTNESS_Q8_MAX;
                uint32_t attack_us = (uint32_t)s->attack_ms * 1000u;
                if (attack_us > 60000u) attack_us = 60000u;   // keep <<16 in 32-bit
                uint32_t a16 = cyc_us ? ((attack_us << 16) / cyc_us) : 0u;
                s->cached_duty_q16   = d16;
                s->cached_attack_q16 = a16;
                s->a3_cache_duty_q8  = s->duty_q8;
                s->a3_cache_freq     = s->cd_cache_freq;
                s->a3_cache_attack_ms = s->attack_ms;
            }
            uint32_t duty_q16   = s->cached_duty_q16;
            uint32_t attack_q16 = s->cached_attack_q16;
            uint16_t lvl = led_carrier_level_iram(s->carrier_waveform,
                                                  eff_elapsed, cyc_us,
                                                  s->brightness_q8, duty_q16, attack_q16);
            if (lvl != s->output_level_q8) {
                s->output_level_q8 = lvl;
                s->output_level    = (uint8_t)(lvl >> 8);  // coarse mirror (compat)
                s->led_state       = (lvl > 0u);   // keep active/exit bookkeeping sane
                s->led_dirty       = true;
                any_dirty          = true;
            }
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
 * On each wake: snapshot all channels under s_flicker_mux (ONE critical
 * section for the whole snapshot), then call led_strip_set_channel() per
 * channel (skipping channels that are inactive AND were already 0 last frame)
 * and a single led_strip_refresh().  The strip layer handles per-backend
 * compositing (brightness scaling for NEOPIXEL/DOTSTAR; PWM duty for DIRECT).
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

        /* Pulse shape (phase / attack) — task context, deliberately.
         *
         * Both are consumed at flicker-cycle granularity: phase becomes a delay
         * derived from the live period, attack the trapezoid edge width. Neither
         * needs sample or tick resolution, and evaluating them in the ISR cost
         * ~2.9 KB of IRAM (two more inline expansions of led_eval_mod_iram and
         * led_interp_param) on a build that was already 96% full. The ISR just
         * reads the fields; this loop, which the ISR notifies on every state
         * change, keeps them current. */
        {
            uint64_t now_us = (uint64_t)esp_timer_get_time();
            for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
                led_flicker_state_t *s = &flicker_state[ch];
                if (!s->active) continue;
                portENTER_CRITICAL(&s_flicker_mux);
                led_sweep_param_t swp = s->sw_phase, swa = s->sw_attack;
                led_mod_slot_t    mdp = s->mod_phase, mda = s->mod_attack;
                uint64_t sw_start = s->sweep_start_us, sw_dur = s->sweep_duration_us;
                portEXIT_CRITICAL(&s_flicker_mux);

                if (!mdp.active && swp.curve == LED_INTERP_NONE &&
                    !mda.active && swa.curve == LED_INTERP_NONE) {
                    continue;   /* nothing time-varying on this channel */
                }
                uint32_t prog_q16;
                if (sw_dur == 0) {
                    prog_q16 = 65536u;
                } else {
                    uint64_t es = (now_us >= sw_start) ? (now_us - sw_start) : 0;
                    uint64_t p  = (es * 65536ULL) / sw_dur;
                    prog_q16 = (p > 65536ULL) ? 65536u : (uint32_t)p;
                }
                /* Both ends are reduced to 0..359 / clamped at install time, so
                 * interpolating between them stays in range — no modulo needed. */
                int16_t  new_phase  = s->phase_offset_deg;
                uint16_t new_attack = s->attack_ms;
                if (mdp.active) {
                    int32_t mv = led_eval_mod_iram(&mdp, now_us);
                    new_phase = (int16_t)(((mv < 0) ? 0 : (uint32_t)mv) >> 8);
                } else if (swp.curve != LED_INTERP_NONE) {
                    new_phase = (int16_t)led_interp_param(&swp, prog_q16);
                }
                if (mda.active) {
                    int32_t mv = led_eval_mod_iram(&mda, now_us);
                    new_attack = (uint16_t)(((mv < 0) ? 0 : (uint32_t)mv) >> 8);
                } else if (swa.curve != LED_INTERP_NONE) {
                    new_attack = (uint16_t)led_interp_param(&swa, prog_q16);
                }
                portENTER_CRITICAL(&s_flicker_mux);
                s->phase_offset_deg = new_phase;
                s->attack_ms        = new_attack;
                portEXIT_CRITICAL(&s_flicker_mux);
            }
        }

        // Snapshot per-channel state under s_flicker_mux.
        // The ISR writes red/green/blue as three separate stores under
        // portENTER_CRITICAL_ISR; reading them unprotected produces torn
        // (mixed-epoch) color triples.  One critical section per channel
        // is adequate — the ISR's cycle-boundary block takes the same mux.
        bool     ch_active[NUM_LED_CHANNELS], ch_led_state[NUM_LED_CHANNELS];
        uint16_t ch_brightness[NUM_LED_CHANNELS];   // Q8.8 (0..25600)
        uint8_t  ch_red[NUM_LED_CHANNELS], ch_green[NUM_LED_CHANNELS], ch_blue[NUM_LED_CHANNELS];
        uint8_t  ch_carrier[NUM_LED_CHANNELS];
        uint16_t ch_output[NUM_LED_CHANNELS];       // Q8.8 (0..25600)
        // A5(b): remember which channels were driven to 0-while-inactive last frame,
        // so we can skip the redundant led_strip_set_channel(...,0,...) for them.
        // Starts all-false so the FIRST frame always writes every channel.
        static bool ch_was_inactive_zero[NUM_LED_CHANNELS] = { false };
        // A5(a): snapshot ALL channels under ONE critical section. The ISR writes
        // red/green/blue as separate stores under portENTER_CRITICAL_ISR; a single
        // task-side region still yields an untorn per-channel view (the ISR can only
        // interleave between channels, never mid-triple within this region on the
        // same core, and cross-core the spinlock serialises).
        portENTER_CRITICAL(&s_flicker_mux);
        for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
            led_flicker_state_t *s = &flicker_state[ch];
            ch_active[ch]     = s->active;
            ch_led_state[ch]  = s->led_state;
            ch_brightness[ch] = s->brightness_q8;
            ch_red[ch]        = s->red;
            ch_green[ch]      = s->green;
            ch_blue[ch]       = s->blue;
            ch_carrier[ch]    = s->carrier_waveform;
            ch_output[ch]     = s->output_level_q8;
        }
        portEXIT_CRITICAL(&s_flicker_mux);

        // One set_channel call per logical channel.  Brightness compositing
        // (r' = r × brightness / 100) now lives inside led_strip.c so the
        // caller passes raw values from the timeline.
        //
        // When a channel is inactive or in the OFF half of its flicker cycle,
        // we pass brightness=0 so the strip layer drives the output to black /
        // zero duty — semantically equivalent to the old "write RGB=0" path
        // but portable across all three backends.
        for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
            if (!ch_active[ch]) {
                // A5(b): an inactive channel is driven to black. If it was already
                // driven to black last frame, the strip buffer for it is unchanged,
                // so skip the redundant write. The value written is identical (0).
                if (ch_was_inactive_zero[ch]) {
                    continue;
                }
                led_strip_set_channel(matrix_handle, ch, 0, 0, 0, 0);
                ch_was_inactive_zero[ch] = true;
            } else if (ch_carrier[ch] == LED_CARRIER_SQUARE) {
                // Legacy square: brightness during ON half, 0 during OFF half.
                led_strip_set_channel(matrix_handle, ch,
                                      ch_led_state[ch] ? ch_brightness[ch] : 0,
                                      ch_red[ch], ch_green[ch], ch_blue[ch]);
                ch_was_inactive_zero[ch] = false;   // A5(b): active → re-arm zero-skip
            } else {
                // B2 smooth carrier: drive the per-tick output level directly.
                led_strip_set_channel(matrix_handle, ch, ch_output[ch],
                                      ch_red[ch], ch_green[ch], ch_blue[ch]);
                ch_was_inactive_zero[ch] = false;   // A5(b): active → re-arm zero-skip
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

        // Start at the floor tick; the scale-up step below raises it to match the
        // requested frequency (V-E1). Initial value must be non-zero for start().
        s_flicker_tick_hz = LED_FLICKER_TICK_MIN;
        uint64_t alarm_period_us = 1000000ULL / s_flicker_tick_hz;

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

    // V-E1: raise the ISR tick to match the requested frequency (never lower it
    // within a session — the highest active frequency wins). gptimer alarm can be
    // reconfigured on a running timer, so this applies immediately.
    {
        uint32_t desired = (min_freq_milliHz / 1000u) * LED_FLICKER_TICK_MULT;
        if (desired < LED_FLICKER_TICK_MIN) desired = LED_FLICKER_TICK_MIN;
        if (desired > LED_FLICKER_TICK_MAX) desired = LED_FLICKER_TICK_MAX;
        if (desired > s_flicker_tick_hz && s_flicker_timer != NULL) {
            gptimer_alarm_config_t ac = {
                .reload_count = 0,
                .alarm_count  = 1000000ULL / desired,
                .flags.auto_reload_on_alarm = true,
            };
            if (gptimer_set_alarm_action(s_flicker_timer, &ac) == ESP_OK) {
                s_flicker_tick_hz = desired;
                ESP_LOGI(TAG, "flicker tick raised to %u Hz (for %.1f Hz flicker)",
                         (unsigned)desired, (double)min_freq_milliHz / 1000.0);
            }
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
        s->duty_q8             = (uint16_t)duty_cycle * 256u;   // Q8.8 mirrors
        s->brightness_q8       = (uint16_t)brightness * 256u;
        s->carrier_waveform    = s_flicker_carrier;   // B2: latch the current carrier
        s->attack_ms           = s_flicker_attack_ms; // trapezoid edge duration
        s->jitter_millihz      = s_flicker_jitter_millihz;    // C3: flicker-rate jitter
        s->jitter_period_us    = s_flicker_jitter_period_us;
        s->output_level        = 0;
        s->output_level_q8     = 0;
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
        s->dc_constant_valid = false;   // A4: republish on (re)start
        s->active = true;
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/**
 * @brief Set the flicker CARRIER waveform (B2), latched by each channel at its
 *        next flicker start. 0=SQUARE (legacy on/off), 1=SINE, 2=TRIANGLE.
 *        SINE/TRIANGLE give smooth, low-fatigue single-frequency drive and, with
 *        an antiphase pair (V-E2), luminance-flat "invisible" flicker.
 */
void led_matrix_set_carrier(uint8_t wave)
{
    // env=4 (tremolo) has no LED meaning → square; 0..3 pass through.
    s_flicker_carrier = (wave > LED_CARRIER_TRAPEZOID) ? LED_CARRIER_SQUARE : wave;
}

// Set the trapezoid edge duration (ms) used by env=3 (TRAPEZOID), latched at start.
void led_matrix_set_attack(uint16_t ms)
{
    s_flicker_attack_ms = (ms > 60u) ? 60u : ms;
}

// Set the flicker-rate jitter (anti-habituation): amplitude in Hz and wander period
// in ms. Latched per channel at flicker start. amp 0 = off. amp capped at 5 Hz.
void led_matrix_set_jitter(float amp_hz, float period_ms)
{
    if (amp_hz < 0.0f) amp_hz = 0.0f;
    if (amp_hz > 5.0f) amp_hz = 5.0f;
    s_flicker_jitter_millihz = (uint32_t)(amp_hz * 1000.0f);
    if (period_ms >= 1000.0f) s_flicker_jitter_period_us = (uint32_t)(period_ms * 1000.0f);
}

/**
 * @brief Current flicker carrier waveform (0=square,1=sine,2=triangle).
 */
uint8_t led_matrix_get_carrier(void)
{
    return s_flicker_carrier;
}

/**
 * @brief Set the flicker PHASE offset (degrees, normalized 0..359) on the channels
 *        in `channel_mask` (V-E2). Applied at use-time from the live period, so it
 *        stays a fixed phase through frequency ramps. 180° on one of a
 *        complementary-colour pair (sharing the same start time) gives antiphase /
 *        luminance-flat "invisible" flicker.
 */
void led_matrix_set_phase_masked(uint8_t channel_mask, int16_t deg)
{
    deg %= 360; if (deg < 0) deg += 360;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        flicker_state[ch].phase_offset_deg = deg;
        portEXIT_CRITICAL(&s_flicker_mux);
    }
}

/**
 * @brief Set the flicker carrier waveform (env / carrier field of the .ledc LED
 *        line) on the masked channels. Unlike led_matrix_set_carrier() (which
 *        only affects the module-global latched at the NEXT flicker start), this
 *        writes per-channel state and so also takes effect on a channel that is
 *        already flickering. 0=square,1=sine,2=triangle,3=trapezoid.
 */
void led_matrix_set_carrier_masked(uint8_t channel_mask, uint8_t wave)
{
    if (wave > LED_CARRIER_TRAPEZOID) wave = LED_CARRIER_SQUARE;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        flicker_state[ch].carrier_waveform = wave;
        portEXIT_CRITICAL(&s_flicker_mux);
    }
}

/**
 * @brief Set the trapezoid edge duration (ms, capped at 60) on the masked
 *        channels. Per-channel counterpart of led_matrix_set_attack().
 */
void led_matrix_set_attack_masked(uint8_t channel_mask, uint16_t ms)
{
    if (ms > 60u) ms = 60u;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        flicker_state[ch].attack_ms = ms;
        portEXIT_CRITICAL(&s_flicker_mux);
    }
}

/**
 * @brief Set the flicker-rate jitter (anti-habituation) on the masked channels.
 *        amp in Hz (0=off, capped 5 Hz), period in ms (only applied if >=1000).
 *        Per-channel counterpart of led_matrix_set_jitter().
 */
void led_matrix_set_jitter_masked(uint8_t channel_mask, float amp_hz, float period_ms)
{
    if (amp_hz < 0.0f) amp_hz = 0.0f;
    if (amp_hz > 5.0f) amp_hz = 5.0f;
    uint32_t millihz   = (uint32_t)(amp_hz * 1000.0f);
    uint32_t period_us = (period_ms >= 1000.0f) ? (uint32_t)(period_ms * 1000.0f) : 0u;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        flicker_state[ch].jitter_millihz = millihz;
        if (period_us) flicker_state[ch].jitter_period_us = period_us;
        portEXIT_CRITICAL(&s_flicker_mux);
    }
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
        s->duty_q8             = (uint16_t)duty_cycle * 256u;
        s->brightness_q8       = (uint16_t)brightness * 256u;
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
        s->dc_constant_valid = false;   // A4
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
        s->brightness_q8 = (uint16_t)brightness * 256u;
        // Mirror into sw_brightness so the ISR's cycle-boundary recompute
        // (which reads sw_brightness via led_interp_param) preserves this
        // value instead of reverting to the prior sweep target.
        s->sw_brightness = (led_sweep_param_t){
            (uint32_t)brightness * 256u,
            (uint32_t)brightness * 256u,
            LED_INTERP_NONE
        };
        s->dc_constant_valid = false;   // A4: force DC path to republish new value
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
        s->dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Per-field setters for the modulation engine.
 *
 * Each updates ONE parameter on the masked channels and leaves all others
 * untouched. Mirrors the brightness pattern in led_matrix_update_brightness_masked
 * above. Used by mod_engine to push triangle/sine/saw/square modulated
 * values without disturbing the channel's other state.
 * ------------------------------------------------------------------------ */

esp_err_t led_matrix_update_frequency_masked(uint8_t channel_mask, float frequency)
{
    if (frequency <= 0.0f || frequency > 500.0f) return ESP_ERR_INVALID_ARG;
    uint32_t f_mhz = (uint32_t)(frequency * 1000.0f);
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        if (!flicker_state[ch].active) continue;
        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->frequency_milliHz = f_mhz;
        s->sw_freq = (led_sweep_param_t){ f_mhz, f_mhz, LED_INTERP_NONE };
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

esp_err_t led_matrix_update_duty_masked(uint8_t channel_mask, uint8_t duty_cycle)
{
    if (duty_cycle > 100) return ESP_ERR_INVALID_ARG;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        if (!flicker_state[ch].active) continue;
        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->duty_cycle = duty_cycle;
        s->duty_q8    = (uint16_t)duty_cycle * 256u;
        s->sw_duty = (led_sweep_param_t){ (uint32_t)duty_cycle * 256u,
                                          (uint32_t)duty_cycle * 256u,
                                          LED_INTERP_NONE };
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

esp_err_t led_matrix_update_color_r_masked(uint8_t channel_mask, uint8_t red)
{
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->red = red;
        s->sw_r = (led_sweep_param_t){ (uint32_t)red * 256u, (uint32_t)red * 256u, LED_INTERP_NONE };
        s->dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

esp_err_t led_matrix_update_color_g_masked(uint8_t channel_mask, uint8_t green)
{
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->green = green;
        s->sw_g = (led_sweep_param_t){ (uint32_t)green * 256u, (uint32_t)green * 256u, LED_INTERP_NONE };
        s->dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

esp_err_t led_matrix_update_color_b_masked(uint8_t channel_mask, uint8_t blue)
{
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        led_flicker_state_t *s = &flicker_state[ch];
        portENTER_CRITICAL(&s_flicker_mux);
        s->blue = blue;
        s->sw_b = (led_sweep_param_t){ (uint32_t)blue * 256u, (uint32_t)blue * 256u, LED_INTERP_NONE };
        s->dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/* -----------------------------------------------------------------------
 * Modulation slot setters. mod_engine calls these when an entry uses one
 * of the modulation prefixes (^~/\_). The actual wave evaluation runs
 * inside the LED ISR at cycle boundaries (same path as sweeps) — no
 * polling task, no mutex contention, zero ongoing CPU cost.
 *
 * Each setter stores the wave params in the named slot for every channel
 * matching the mask. Values are scaled to the field's native units (Q8.8
 * for duty/brightness/RGB; milliHz for frequency).
 * --------------------------------------------------------------------- */

static inline void s_apply_mod(led_mod_slot_t *m, uint8_t wave,
                               int32_t start_q, int32_t end_q,
                               uint32_t period_ms)
{
    m->wave         = wave;
    m->start_q      = start_q;
    m->end_q        = end_q;
    m->period_us    = period_ms * 1000u;
    m->start_time_us = (uint64_t)esp_timer_get_time();
    m->active       = true;  /* set LAST so a partially-written slot is never seen */
}

esp_err_t led_matrix_set_mod_freq_masked(uint8_t channel_mask, uint8_t wave,
                                          float start_hz, float end_hz, uint32_t period_ms)
{
    int32_t s_q = (int32_t)(start_hz * 1000.0f);
    int32_t e_q = (int32_t)(end_hz   * 1000.0f);
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        s_apply_mod(&flicker_state[ch].mod_freq, wave, s_q, e_q, period_ms);
        flicker_state[ch].dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

esp_err_t led_matrix_set_mod_duty_masked(uint8_t channel_mask, uint8_t wave,
                                          uint8_t start_pct, uint8_t end_pct, uint32_t period_ms)
{
    int32_t s_q = (int32_t)start_pct * 256;
    int32_t e_q = (int32_t)end_pct   * 256;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        s_apply_mod(&flicker_state[ch].mod_duty, wave, s_q, e_q, period_ms);
        flicker_state[ch].dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

esp_err_t led_matrix_set_mod_brightness_masked(uint8_t channel_mask, uint8_t wave,
                                                uint8_t start_pct, uint8_t end_pct, uint32_t period_ms)
{
    int32_t s_q = (int32_t)start_pct * 256;
    int32_t e_q = (int32_t)end_pct   * 256;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        s_apply_mod(&flicker_state[ch].mod_brightness, wave, s_q, e_q, period_ms);
        flicker_state[ch].dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/* Color channel variants — start/end in 0..255 range; stored as Q8.8. */
esp_err_t led_matrix_set_mod_color_masked(uint8_t channel_mask, uint8_t wave,
                                           char component, uint8_t start_v, uint8_t end_v,
                                           uint32_t period_ms)
{
    int32_t s_q = (int32_t)start_v * 256;
    int32_t e_q = (int32_t)end_v   * 256;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        led_mod_slot_t *m;
        if      (component == 'r' || component == 'R') m = &flicker_state[ch].mod_r;
        else if (component == 'g' || component == 'G') m = &flicker_state[ch].mod_g;
        else                                            m = &flicker_state[ch].mod_b;
        s_apply_mod(m, wave, s_q, e_q, period_ms);
        flicker_state[ch].dc_constant_valid = false;   // A4
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/* Pulse-shape variants — degrees / milliseconds, stored Q8.8 like the rest so
 * s_apply_mod and led_eval_mod_iram need no special case; the cycle-boundary
 * consumer shifts back down. */
esp_err_t led_matrix_set_mod_phase_masked(uint8_t channel_mask, uint8_t wave,
                                           uint16_t start_deg, uint16_t end_deg, uint32_t period_ms)
{
    int32_t s_q = (int32_t)(start_deg % 360u) * 256;
    int32_t e_q = (int32_t)(end_deg   % 360u) * 256;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        s_apply_mod(&flicker_state[ch].mod_phase, wave, s_q, e_q, period_ms);
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

esp_err_t led_matrix_set_mod_attack_masked(uint8_t channel_mask, uint8_t wave,
                                            uint16_t start_ms, uint16_t end_ms, uint32_t period_ms)
{
    int32_t s_q = (int32_t)start_ms * 256;
    int32_t e_q = (int32_t)end_ms   * 256;
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        s_apply_mod(&flicker_state[ch].mod_attack, wave, s_q, e_q, period_ms);
        portEXIT_CRITICAL(&s_flicker_mux);
    }
    return ESP_OK;
}

/* Clear any active modulation on the named field for matching channels.
 * Sweep / step value (whichever is currently in sw_X) takes over again. */
esp_err_t led_matrix_clear_mod_masked(uint8_t channel_mask, uint8_t field)
{
    for (uint8_t ch = 0; ch < NUM_LED_CHANNELS; ch++) {
        if (!(channel_mask & (1u << ch))) continue;
        portENTER_CRITICAL(&s_flicker_mux);
        switch (field) {
            case 0: flicker_state[ch].mod_freq.active       = false; break;
            case 1: flicker_state[ch].mod_duty.active       = false; break;
            case 2: flicker_state[ch].mod_brightness.active = false; break;
            case 3: flicker_state[ch].mod_r.active          = false; break;
            case 4: flicker_state[ch].mod_g.active          = false; break;
            case 5: flicker_state[ch].mod_b.active          = false; break;
            case 6: flicker_state[ch].mod_phase.active      = false; break;
            case 7: flicker_state[ch].mod_attack.active     = false; break;
        }
        flicker_state[ch].dc_constant_valid = false;   // A4: sweep/step value takes over
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

    // Task context (timeline task / web handlers) — NOT the ISR, so ESP_LOG is
    // safe here. The ISR flicker callback stays log-free.
    ESP_LOGD(TAG, "LEDDBG sweep mask=0x%02x freq=%umHz->%umHz dur=%ums duty=%u bri=%u",
             channel_mask, (unsigned)spec->freq_milliHz_start,
             (unsigned)spec->freq_milliHz_target, (unsigned)spec->duration_ms,
             (unsigned)spec->duty_start, (unsigned)spec->bright_start);

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
        s->duty_q8             = (uint16_t)s->duty_cycle * 256u;   // Q8.8 mirrors of the start values
        s->brightness_q8       = (uint16_t)s->brightness * 256u;
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
        /* Pulse shape. Natural units, not Q8.8 — degrees and milliseconds are
         * already integers and the ISR consumes them directly. Like the params
         * above, a running channel continues from its LIVE value so an
         * interrupted ramp does not snap. */
        {
            uint16_t eff_phase_start  = (spec->phase_curve  != LED_INTERP_NONE && already_active)
                                        ? (uint16_t)s->phase_offset_deg : spec->phase_start;
            uint16_t eff_attack_start = (spec->attack_curve != LED_INTERP_NONE && already_active)
                                        ? s->attack_ms : spec->attack_start;
            s->sw_phase = (led_sweep_param_t){
                .start_q  = eff_phase_start,
                .target_q = spec->phase_target,
                .curve    = (uint8_t)spec->phase_curve,
            };
            s->sw_attack = (led_sweep_param_t){
                .start_q  = eff_attack_start,
                .target_q = spec->attack_target,
                .curve    = (uint8_t)spec->attack_curve,
            };
            /* Non-swept fields hold their literal — but only when the entry
             * actually carried the field. An absent field means "leave
             * unchanged", and 0 is a legitimate phase, so this keys off the
             * explicit _set flag rather than a sentinel value. */
            if (spec->phase_curve == LED_INTERP_NONE && spec->phase_set) {
                s->phase_offset_deg = (int16_t)(spec->phase_target % 360u);
            }
            if (spec->attack_curve == LED_INTERP_NONE && spec->attack_set) {
                s->attack_ms = spec->attack_target;
            }
        }
        s->sweep_start_us    = sweep_start_us;
        s->sweep_duration_us = sweep_duration_us;
        s->dc_constant_valid = false;   // A4: republish on (re)start / new sweep
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

int led_matrix_get_snapshot(led_matrix_channel_snapshot_t *out, int count)
{
    if (!out || count <= 0) return 0;
    int n = (count < NUM_LED_CHANNELS) ? count : NUM_LED_CHANNELS;

    /* Snapshot under spinlock — short critical section, just memcpy the
     * fields out. Same pattern as the existing log_full_state below. */
    portENTER_CRITICAL(&s_flicker_mux);
    for (int ch = 0; ch < n; ch++) {
        led_flicker_state_t *s = &flicker_state[ch];
        out[ch].active            = s->active;
        out[ch].freq              = s->frequency_milliHz / 1000.0f;
        out[ch].duty              = s->duty_cycle;
        out[ch].brightness        = s->brightness;
        out[ch].r                 = s->red;
        out[ch].g                 = s->green;
        out[ch].b                 = s->blue;
        out[ch].mod_freq_active   = s->mod_freq.active;
        out[ch].mod_duty_active   = s->mod_duty.active;
        out[ch].mod_bright_active = s->mod_brightness.active;
        out[ch].mod_r_active      = s->mod_r.active;
        out[ch].mod_g_active      = s->mod_g.active;
        out[ch].mod_b_active      = s->mod_b.active;
    }
    portEXIT_CRITICAL(&s_flicker_mux);
    return n;
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
