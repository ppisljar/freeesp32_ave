#ifndef LED_MATRIX_EXAMPLE_H
#define LED_MATRIX_EXAMPLE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Interpolation curve types for LED parameter sweeps
 */
typedef enum {
    LED_INTERP_NONE      = 0,  /**< No sweep — hold at target value immediately */
    LED_INTERP_LINEAR    = 1,  /**< Linear interpolation from start to target */
    LED_INTERP_QUADRATIC = 2,  /**< Piecewise quadratic ease-in-out */
} led_interp_t;

/**
 * @brief Full sweep specification for led_matrix_start_sweep_masked().
 *
 * For each parameter, if its curve is LED_INTERP_NONE, the parameter is
 * set to <param>_target immediately (start value is ignored).  Otherwise
 * the parameter is swept from <param>_start to <param>_target over
 * duration_ms milliseconds using the selected curve.
 *
 * W ("white") is composited at write time: it is added to R, G, and B
 * (each clamped to 255) so the strip remains RGB-only at the wire level.
 */
typedef struct {
    uint32_t freq_milliHz_start;   /**< Frequency start value in milliHz */
    uint32_t freq_milliHz_target;  /**< Frequency target value in milliHz */
    uint8_t  duty_start;           /**< Duty cycle start (0-100) */
    uint8_t  duty_target;          /**< Duty cycle target (0-100) */
    uint8_t  bright_start;         /**< Brightness start (0-100) */
    uint8_t  bright_target;        /**< Brightness target (0-100) */
    uint8_t  r_start;              /**< Red start (0-255) */
    uint8_t  g_start;              /**< Green start (0-255) */
    uint8_t  b_start;              /**< Blue start (0-255) */
    uint8_t  w_start;              /**< White start (0-255) */
    uint8_t  r_target;             /**< Red target (0-255) */
    uint8_t  g_target;             /**< Green target (0-255) */
    uint8_t  b_target;             /**< Blue target (0-255) */
    uint8_t  w_target;             /**< White target (0-255) */
    led_interp_t freq_curve;       /**< Interpolation curve for frequency */
    led_interp_t duty_curve;       /**< Interpolation curve for duty cycle */
    led_interp_t bright_curve;     /**< Interpolation curve for brightness */
    led_interp_t r_curve;          /**< Interpolation curve for red */
    led_interp_t g_curve;          /**< Interpolation curve for green */
    led_interp_t b_curve;          /**< Interpolation curve for blue */
    led_interp_t w_curve;          /**< Interpolation curve for white */
    uint32_t duration_ms;          /**< Sweep duration in milliseconds */
} led_sweep_spec_t;

/**
 * @brief Initialize 12x4 LED matrix on GPIO 12.
 *
 * Also populates the led_to_channel_bit LUT from the spec-defined region map.
 *
 * @return ESP_OK on success
 */
esp_err_t led_matrix_init(void);

/**
 * @brief Set LED at matrix coordinates (x,y)
 *
 * @param x Column (0-11)
 * @param y Row (0-3)
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Update matrix display
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_refresh(void);

/**
 * @brief Clear entire matrix
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_clear(void);

/**
 * @brief Display VU meter on matrix
 *
 * @param left_level Left channel level (0.0-1.0)
 * @param right_level Right channel level (0.0-1.0)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_vu_meter(float left_level, float right_level);

/**
 * @brief Demo pattern - test all LEDs in sequence
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_test_pattern(void);

// ===========================================================================
// MASKED (multi-channel) API  — primary interface for multi-zone LED control.
//
// channel_mask bits 0-3 correspond to channels 1-4 (spec region r1-r4).
// Bit 0 = channel 1 = inner-left rectangle (r1).
// Bit 1 = channel 2 = outer-left frame (r2).
// Bit 2 = channel 3 = outer-right frame (r3).
// Bit 3 = channel 4 = inner-right rectangle (r4).
//
// Operations are applied to ALL channels whose bit is set in the mask.
// ===========================================================================

/**
 * @brief Start LED flicker on channels indicated by channel_mask.
 *
 * @param channel_mask  Bitmask 0x01-0x0F; bit 0 = channel 1, bit 3 = channel 4.
 * @param frequency     Flicker frequency in Hz (0.1-100.0).
 * @param duty_cycle    Duty cycle percentage (0-100).
 * @param brightness    Maximum brightness (0-100).
 * @param cycle_hint_us Transport-clock anchor for cycle_start_time_us (µs, from
 *                      esp_timer_get_time() domain).  When non-zero, all new
 *                      channels in the mask use this value as their cycle origin
 *                      so that entries at the same logical timestamp are always
 *                      phase-locked regardless of dispatch lag.  When zero,
 *                      falls back to the existing peer-piggyback / now_us path.
 * @return ESP_OK on success.
 */
esp_err_t led_matrix_start_flicker_masked(uint8_t channel_mask, float frequency,
                                           uint8_t duty_cycle, uint8_t brightness,
                                           uint64_t cycle_hint_us);

/**
 * @brief Stop LED flicker on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 * @return ESP_OK on success.
 */
esp_err_t led_matrix_stop_flicker_masked(uint8_t channel_mask);

/**
 * @brief Update flicker parameters on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 * @param frequency    New flicker frequency in Hz.
 * @param duty_cycle   New duty cycle percentage (0-100).
 * @param brightness   New maximum brightness (0-100).
 * @return ESP_OK on success.
 */
esp_err_t led_matrix_update_flicker_params_masked(uint8_t channel_mask, float frequency,
                                                    uint8_t duty_cycle, uint8_t brightness);

/**
 * @brief Set flicker color on channels indicated by channel_mask.
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 * @param red          Red component (0-255).
 * @param green        Green component (0-255).
 * @param blue         Blue component (0-255).
 * @return ESP_OK on success.
 */
esp_err_t led_matrix_set_flicker_color_masked(uint8_t channel_mask,
                                               uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Start a parametric LED flicker sweep on channels indicated by channel_mask.
 *
 * Starts flicker (if not already running) and configures all seven parameters
 * (frequency, duty, brightness, R, G, B, W) to sweep from their start values to
 * their target values over duration_ms.  Parameters whose curve is
 * LED_INTERP_NONE are snapped to target immediately.
 *
 * Thread-safe to call from task context.  Not callable from ISR.
 *
 * @param channel_mask  Bitmask 0x01-0x0F.
 * @param spec          Pointer to sweep specification; contents are copied per channel.
 * @param cycle_hint_us Transport-clock anchor for sweep_start_us (µs, from
 *                      esp_timer_get_time() domain).  When non-zero, new channels
 *                      use this value as their sweep (and cycle) origin so that
 *                      same-timestamp entries are phase-locked.  When zero, falls
 *                      back to esp_timer_get_time() at call time.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE on error.
 */
esp_err_t led_matrix_start_sweep_masked(uint8_t channel_mask, const led_sweep_spec_t *spec,
                                         uint64_t cycle_hint_us);

/**
 * @brief Get current flicker frequency for the lowest-bit-set channel in the mask.
 *
 * Returns the frequency of the first (lowest-bit) active channel in the mask,
 * or 0.0 if the mask is 0 or no matching channel is active.
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 * @return Current frequency in Hz, or 0.0.
 */
float led_matrix_get_current_frequency_masked(uint8_t channel_mask);

// ===========================================================================
// BACKWARDS-COMPAT SINGLE-CHANNEL API  — operate on channel 1 (bit 0) only.
// All existing callers (audio_test.c, audio_led_sync.c, config_parser.c) use
// these.  They are thin trampolines to the _masked variants with mask=0x01.
// ===========================================================================

/**
 * @brief Start LED flicker for meditation/brainwave entrainment (channel 1 only).
 *
 * Backwards-compat trampoline; calls led_matrix_start_flicker_masked(0x01, ..., 0).
 *
 * @param frequency  Flicker frequency in Hz (0.1-100.0)
 * @param duty_cycle Duty cycle percentage (0-100)
 * @param brightness Maximum brightness (0-100)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_start_flicker(float frequency, uint8_t duty_cycle, uint8_t brightness);

/**
 * @brief Stop LED flicker (channel 1 only).
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_stop_flicker(void);

/**
 * @brief Update flicker parameters while running (channel 1 only).
 *
 * @param frequency New flicker frequency in Hz
 * @param duty_cycle New duty cycle percentage (0-100)
 * @param brightness New maximum brightness (0-100)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_update_flicker_params(float frequency, uint8_t duty_cycle, uint8_t brightness);

/**
 * @brief Set flicker color (channel 1 only; default: white).
 *
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_set_flicker_color(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Start a parametric LED flicker sweep (channel 1 only).
 *
 * Backwards-compat trampoline; calls led_matrix_start_sweep_masked(0x01, spec, 0).
 *
 * @param spec  Pointer to sweep specification; contents are copied.
 * @return ESP_OK on success.
 */
esp_err_t led_matrix_start_sweep(const led_sweep_spec_t *spec);

/**
 * @brief Check if LED flicker is currently active on ANY of the 4 channels.
 *
 * @return bool true if any channel is flickering, false otherwise
 */
bool led_matrix_is_flickering(void);

/**
 * @brief Check if LED flicker is active on ALL channels set in the mask.
 *
 * Returns true only when every bit set in channel_mask has an active channel.
 * Used by config_parser to decide whether to call start_flicker_masked (first
 * activation) or update_flicker_params_masked (rhythm-preserving update).
 *
 * @param channel_mask Bitmask 0x01-0x0F.
 * @return bool true if all masked channels are flickering, false otherwise.
 */
bool led_matrix_is_flickering_masked(uint8_t channel_mask);

/**
 * @brief Get current flicker frequency of the lowest-numbered active channel.
 *
 * @return float Current frequency in Hz (0.0 if no channel is flickering)
 */
float led_matrix_get_current_frequency(void);

/**
 * @brief Return bitmask of channels currently flickering.
 *
 * Bit N is set iff flicker_state[N].active.  Used by the VU meter sync path to
 * broadcast brightness updates to every active channel, not just channel 0.
 *
 * @return uint8_t Bitmask 0x00-0x0F.
 */
uint8_t led_matrix_get_active_mask(void);

#ifdef __cplusplus
}
#endif

#endif // LED_MATRIX_EXAMPLE_H
