#ifndef AUDIO_GENERATOR_H
#define AUDIO_GENERATOR_H

#include "esp_err.h"
#include "esp_log.h"
#include "dsps_tone_gen.h"
#include <stdatomic.h>

/**
 * @brief Audio Generator Component
 *
 * Provides advanced audio generation capabilities including:
 * - Binaural beat generation
 * - Frequency sweeps (linear and quadratic)
 * - Amplitude modulation
 * - Stereo panning
 * - Multiple channel support
 */

#define AUDIO_GEN_SAMPLE_RATE       44100
#define AUDIO_GEN_CHANNELS          2
#define AUDIO_GEN_BUFFER_SIZE       1024

typedef enum {
    AUDIO_GEN_SWEEP_NONE = 0,
    AUDIO_GEN_SWEEP_LINEAR,
    AUDIO_GEN_SWEEP_QUADRATIC
} audio_gen_sweep_type_t;

typedef enum {
    AUDIO_PARAM_FREQUENCY = 0,
    AUDIO_PARAM_AMPLITUDE,
    AUDIO_PARAM_PAN,
    AUDIO_PARAM_MOD_FREQ,
    AUDIO_PARAM_COUNT
} audio_param_t;

typedef struct {
    float start;
    float target;
    uint64_t start_sample;      // generator sample index when sweep started
    uint64_t duration_samples;  // 0 = inactive (no sweep on this param)
    audio_gen_sweep_type_t curve;
} audio_param_sweep_t;

typedef struct {
    float frequency;        // Base frequency in Hz
    float frequency_r;      // Right channel frequency (for binaural)
    float amplitude;        // Amplitude 0.0-1.0
    float pan;             // Pan -1.0 (left) to +1.0 (right)
    float mod_frequency;   // Modulation frequency in Hz
    float mod_depth;       // Modulation depth 0.0-1.0
    audio_gen_sweep_type_t sweep_type;
    float sweep_target;    // Target frequency for sweep
    uint32_t duration_ms;  // Duration in milliseconds
} audio_gen_params_t;

typedef struct {
    audio_gen_params_t params;
    float current_freq;
    float current_freq_r;
    float current_amp;       // live interpolated amplitude
    float current_pan;       // live interpolated pan
    float current_mod_freq;  // live interpolated mod frequency
    // Q32 phase accumulators: one full sine cycle = 2^32.  Phase increment per
    // sample = (uint32_t)(freq * (2^32 / sample_rate)); modular uint32_t add
    // gives free wrap-around — no branch.
    uint32_t phase_l_q32;
    uint32_t phase_r_q32;
    uint32_t mod_phase_q32;
    uint64_t samples_generated;
    uint64_t total_samples;
    bool active;
    audio_param_sweep_t sweeps[AUDIO_PARAM_COUNT];  // per-param sweep state

    // Lock-free pending-params slot (Step 5.2).
    // Writer: copy new_params → pending_params under mutex (serialise concurrent
    //         timeline writers), then atomic_fetch_add(&pending_version, 1).
    // Reader (fill_buffer, per-buffer boundary): atomic_load pending_version;
    //         if != applied_version, latch pending_params without touching phase
    //         accumulators (phase coherence) or sample counters.
    // Memory ordering: the atomic increment acts as a release fence on the write
    //   side; the atomic load with acquire semantics on the read side ensures the
    //   pending_params copy is visible before applied_version is updated.
    audio_gen_params_t  pending_params;
    _Atomic uint32_t    pending_version;   // bumped by writer after each update
    uint32_t            applied_version;   // last version seen by fill_buffer
} audio_gen_channel_t;

/**
 * @brief Initialize audio generator
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_generator_init(void);

/**
 * @brief Start audio generation on a channel
 *
 * @param channel Channel number (0-based)
 * @param params Audio generation parameters
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_generator_start_channel(int channel, const audio_gen_params_t* params);

/**
 * @brief Stop audio generation on a channel
 *
 * @param channel Channel number
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_generator_stop_channel(int channel);

/**
 * @brief Generate audio samples for mixing with I2S output
 *
 * @param output_buffer Output buffer for stereo samples
 * @param samples Number of stereo samples to generate
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_generator_fill_buffer(float* output_buffer, size_t samples);

/**
 * @brief Update sweep parameters for real-time changes
 *
 * @param channel Channel number
 * @param new_target New target frequency for sweep
 * @param sweep_type Type of sweep interpolation
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_generator_update_sweep(int channel, float new_target, audio_gen_sweep_type_t sweep_type);

/**
 * @brief Get channel status
 *
 * @param channel Channel number
 * @return true if channel is active
 */
bool audio_generator_is_active(int channel);

/**
 * @brief Read the current interpolated frequency for a channel.
 *
 * Safe to call from any task; acquires the generator mutex.
 * Intended for sweep-verification tests — not for use in real-time paths.
 *
 * @param channel Channel number (0-based)
 * @param out     Pointer to receive the current frequency in Hz
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if channel or pointer invalid
 */
esp_err_t audio_generator_get_current_freq(int channel, float *out);

/**
 * @brief Read the current interpolated amplitude for a channel.
 *
 * Safe to call from any task; acquires the generator mutex.
 * Intended for sweep-verification tests — not for use in real-time paths.
 *
 * @param channel Channel number (0-based)
 * @param out     Pointer to receive the current amplitude (0.0–1.0)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if channel or pointer invalid
 */
esp_err_t audio_generator_get_current_amp(int channel, float *out);

/**
 * @brief Start a time-windowed parameter sweep on a channel.
 *
 * Unlike the legacy params.sweep_type path (which uses total_samples as the
 * denominator), this API accepts an explicit duration_samples so that the
 * timeline executor can schedule sweeps that don't span the entire channel
 * lifetime (Phase 3 requirement).
 *
 * @param channel          Channel number (0-based)
 * @param param            Which parameter to sweep (AUDIO_PARAM_*)
 * @param start            Start value
 * @param target           End value
 * @param duration_samples Number of samples over which to interpolate
 * @param curve            Interpolation curve (LINEAR or QUADRATIC)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad args
 */
esp_err_t audio_generator_start_sweep(int channel, audio_param_t param,
                                      float start, float target,
                                      uint64_t duration_samples,
                                      audio_gen_sweep_type_t curve);

/**
 * @brief Update parameters of an already-running channel without restarting it.
 *
 * Performs a lock-free parameter update: copies new_params into the channel's
 * pending slot and bumps an atomic version counter.  fill_buffer picks up the
 * new params at the next buffer boundary without touching phase accumulators
 * (phase_l_q32, phase_r_q32, mod_phase_q32) or sample counters, avoiding any audible
 * discontinuity.
 *
 * Returns ESP_ERR_INVALID_STATE if the channel is not currently active — caller
 * should use audio_generator_start_channel() instead.
 *
 * @param channel    Channel number (0-based)
 * @param new_params New parameter values to apply
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if channel inactive,
 *         ESP_ERR_INVALID_ARG on bad args
 */
esp_err_t audio_generator_update_params(int channel, const audio_gen_params_t *new_params);

/**
 * @brief Read the current interpolated right-channel frequency for a channel.
 *
 * Safe to call from any task; acquires the generator mutex.
 * Intended for binaural-beat precision verification — not for use in real-time paths.
 *
 * @param channel Channel number (0-based)
 * @param out     Pointer to receive the current right-channel frequency in Hz
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if channel or pointer invalid
 */
esp_err_t audio_generator_get_current_freq_r(int channel, float *out);

#endif // AUDIO_GENERATOR_H
