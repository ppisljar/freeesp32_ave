#include "audio_generator.h"
#include "audio_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

static const char* TAG = "audio_generator";

#define MAX_AUDIO_CHANNELS      8
#define PI                      3.14159265359f

// Global audio generator state
static audio_gen_channel_t audio_channels[MAX_AUDIO_CHANNELS];
static bool generator_initialized = false;
static SemaphoreHandle_t audio_gen_mutex = NULL;

// Internal functions
static float interpolate_sweep(float start, float target, float progress, audio_gen_sweep_type_t type);
static void apply_modulation(float* sample, float mod_phase, float mod_depth);
static void apply_panning(float input, float pan, float* left, float* right);

esp_err_t audio_generator_init(void) {
    if (generator_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing audio generator");

    // Create mutex for thread safety
    audio_gen_mutex = xSemaphoreCreateMutex();
    if (!audio_gen_mutex) {
        ESP_LOGE(TAG, "Failed to create audio generator mutex");
        return ESP_FAIL;
    }

    // Initialize all channels
    memset(audio_channels, 0, sizeof(audio_channels));

    for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        audio_channels[i].active = false;
        audio_channels[i].phase_l = 0.0f;
        audio_channels[i].phase_r = 0.0f;
        audio_channels[i].mod_phase = 0.0f;
        audio_channels[i].samples_generated = 0;
    }

    generator_initialized = true;

    ESP_LOGI(TAG, "Audio generator initialized with %d channels", MAX_AUDIO_CHANNELS);
    return ESP_OK;
}

esp_err_t audio_generator_start_channel(int channel, const audio_gen_params_t* params) {
    if (!generator_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel < 0 || channel >= MAX_AUDIO_CHANNELS || !params) {
        return ESP_ERR_INVALID_ARG;
    }

    // Validate parameters
    if (params->frequency <= 0 || params->frequency > AUDIO_SAMPLE_RATE/2) {
        ESP_LOGE(TAG, "Invalid frequency: %.1f Hz (must be 0-22050 Hz)", params->frequency);
        return ESP_ERR_INVALID_ARG;
    }

    if (params->amplitude < 0.0f || params->amplitude > 1.0f) {
        ESP_LOGE(TAG, "Invalid amplitude: %.2f (must be 0.0-1.0)", params->amplitude);
        return ESP_ERR_INVALID_ARG;
    }

    if (params->pan < -1.0f || params->pan > 1.0f) {
        ESP_LOGE(TAG, "Invalid pan: %.2f (must be -1.0 to 1.0)", params->pan);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);

    audio_gen_channel_t* ch = &audio_channels[channel];

    // Copy parameters
    memcpy(&ch->params, params, sizeof(audio_gen_params_t));

    // Initialize channel state
    ch->current_freq = params->frequency;
    ch->current_freq_r = params->frequency_r > 0 ? params->frequency_r : params->frequency;
    ch->phase_l = 0.0f;
    ch->phase_r = 0.0f;
    ch->mod_phase = 0.0f;
    ch->samples_generated = 0;

    // Use 64-bit arithmetic to prevent overflow
    ch->total_samples = ((uint64_t)params->duration_ms * AUDIO_SAMPLE_RATE) / 1000ULL;
    ch->active = true;

    // Add validation to catch potential issues
    if (params->duration_ms > 0 && ch->total_samples == 0) {
        ESP_LOGW(TAG, "Duration calculation resulted in zero samples - possible overflow");
        xSemaphoreGive(audio_gen_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Started audio channel %d: freq=%.1f Hz, freq_r=%.1f Hz, amp=%.2f, pan=%.2f, duration=%u ms = %llu samples (%.1f hours)",
             channel, params->frequency, ch->current_freq_r, params->amplitude, params->pan, params->duration_ms,
             ch->total_samples, (float)ch->total_samples / (AUDIO_SAMPLE_RATE * 3600.0f));

    xSemaphoreGive(audio_gen_mutex);
    return ESP_OK;
}

esp_err_t audio_generator_stop_channel(int channel) {
    if (!generator_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel < 0 || channel >= MAX_AUDIO_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);

    audio_channels[channel].active = false;
    ESP_LOGI(TAG, "Stopped audio channel %d", channel);

    xSemaphoreGive(audio_gen_mutex);
    return ESP_OK;
}

esp_err_t audio_generator_fill_buffer(float* output_buffer, size_t samples) {
    if (!generator_initialized || !output_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);

    // Clear output buffer
    memset(output_buffer, 0, samples * 2 * sizeof(float)); // Stereo

    // Mix all active channels
    for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
        audio_gen_channel_t* channel = &audio_channels[ch];

        if (!channel->active) {
            continue;
        }

        // Check if channel has finished its duration
        if (channel->total_samples > 0 && channel->samples_generated >= channel->total_samples) {
            channel->active = false;
            ESP_LOGI(TAG, "Channel %d completed (%llu samples, %.1f%% progress)", ch, channel->samples_generated,
                     (float)channel->samples_generated * 100.0f / channel->total_samples);
            continue;
        }

        for (size_t i = 0; i < samples; i++) {
            // Update sweep frequency if needed
            if (channel->params.sweep_type != AUDIO_GEN_SWEEP_NONE && channel->total_samples > 0) {
                float progress = (float)channel->samples_generated / (float)channel->total_samples;
                channel->current_freq = interpolate_sweep(channel->params.frequency,
                                                        channel->params.sweep_target,
                                                        progress,
                                                        channel->params.sweep_type);

                // Update right channel frequency for binaural beats
                if (channel->params.frequency_r > 0) {
                    float freq_diff = channel->params.frequency_r - channel->params.frequency;
                    channel->current_freq_r = channel->current_freq + freq_diff;
                }
            }

            // Generate stereo samples
            float sample_l = channel->params.amplitude * sinf(channel->phase_l);
            float sample_r = channel->params.amplitude * sinf(channel->phase_r);

            // Apply modulation if enabled
            if (channel->params.mod_frequency > 0 && channel->params.mod_depth > 0) {
                apply_modulation(&sample_l, channel->mod_phase, channel->params.mod_depth);
                apply_modulation(&sample_r, channel->mod_phase, channel->params.mod_depth);

                // Update modulation phase
                channel->mod_phase += 2.0f * PI * channel->params.mod_frequency / AUDIO_SAMPLE_RATE;
                if (channel->mod_phase > 2.0f * PI) {
                    channel->mod_phase -= 2.0f * PI;
                }
            }

            // Apply panning
            float final_left, final_right;
            apply_panning(sample_l, channel->params.pan, &final_left, &final_right);

            // For binaural beats, right channel uses its own frequency
            if (channel->current_freq_r != channel->current_freq) {
                float binaural_right;
                apply_panning(sample_r, channel->params.pan, NULL, &binaural_right);
                final_right = binaural_right;
            }

            // Mix into output buffer (stereo interleaved)
            output_buffer[i * 2] += final_left;       // Left channel
            output_buffer[i * 2 + 1] += final_right;  // Right channel

            // Update phases
            channel->phase_l += 2.0f * PI * channel->current_freq / AUDIO_SAMPLE_RATE;
            if (channel->phase_l > 2.0f * PI) {
                channel->phase_l -= 2.0f * PI;
            }

            channel->phase_r += 2.0f * PI * channel->current_freq_r / AUDIO_SAMPLE_RATE;
            if (channel->phase_r > 2.0f * PI) {
                channel->phase_r -= 2.0f * PI;
            }

            channel->samples_generated++;
        }
    }

    xSemaphoreGive(audio_gen_mutex);
    return ESP_OK;
}

esp_err_t audio_generator_update_sweep(int channel, float new_target, audio_gen_sweep_type_t sweep_type) {
    if (!generator_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel < 0 || channel >= MAX_AUDIO_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);

    audio_gen_channel_t* ch = &audio_channels[channel];
    if (ch->active) {
        ch->params.sweep_target = new_target;
        ch->params.sweep_type = sweep_type;
        ESP_LOGD(TAG, "Updated sweep for channel %d: target=%.1f Hz, type=%d",
                 channel, new_target, sweep_type);
    }

    xSemaphoreGive(audio_gen_mutex);
    return ESP_OK;
}

bool audio_generator_is_active(int channel) {
    if (!generator_initialized || channel < 0 || channel >= MAX_AUDIO_CHANNELS) {
        return false;
    }

    bool active;
    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);
    active = audio_channels[channel].active;
    xSemaphoreGive(audio_gen_mutex);

    return active;
}

// Internal helper functions

static float interpolate_sweep(float start, float target, float progress, audio_gen_sweep_type_t type) {
    if (progress <= 0.0f) return start;
    if (progress >= 1.0f) return target;

    switch (type) {
        case AUDIO_GEN_SWEEP_LINEAR:
            return start + (target - start) * progress;

        case AUDIO_GEN_SWEEP_QUADRATIC: {
            // Quadratic ease-in-out curve
            float t = progress < 0.5f ? 2.0f * progress * progress
                                      : 1.0f - 2.0f * (1.0f - progress) * (1.0f - progress);
            return start + (target - start) * t;
        }

        default:
            return start;
    }
}

static void apply_modulation(float* sample, float mod_phase, float mod_depth) {
    float mod_factor = 1.0f + mod_depth * sinf(mod_phase);
    *sample *= mod_factor;
}

static void apply_panning(float input, float pan, float* left, float* right) {
    // Pan ranges from -1.0 (full left) to +1.0 (full right)
    // Use equal-power panning law for smooth transitions
    float pan_angle = (pan + 1.0f) * PI / 4.0f; // Convert to 0 to π/2 range

    if (left) {
        *left = input * cosf(pan_angle);
    }

    if (right) {
        *right = input * sinf(pan_angle);
    }
}
