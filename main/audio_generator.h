#ifndef AUDIO_GENERATOR_H
#define AUDIO_GENERATOR_H

#include "esp_err.h"
#include "esp_log.h"
#include "dsps_tone_gen.h"

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
    float phase_l;
    float phase_r;
    float mod_phase;
    uint64_t samples_generated;
    uint64_t total_samples;
    bool active;
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

#endif // AUDIO_GENERATOR_H
