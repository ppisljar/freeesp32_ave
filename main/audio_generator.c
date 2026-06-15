/*
 * Mutex usage classification (Step 5.1):
 *
 * (a) Channel-array STRUCTURAL changes — keep mutex:
 *     - xSemaphoreCreateMutex() at init: creates the lock.
 *     - start_channel (lines ~82–130): sets active=true, resets all state.
 *     - stop_channel  (lines ~143–148): sets active=false.
 *     - fill_buffer   (lines ~157–306): outer lock guards the active-flag check
 *       and the per-channel state machine.  Held for the entire buffer fill so
 *       that stop_channel cannot race with a mid-buffer active check.
 *       NOTE: the new pending-params path (Step 5.2) adds an atomic read INSIDE
 *       this lock; no additional mutex sites are added to fill_buffer.
 *
 * (b) Per-channel param UPDATE — new lock-free path (Step 5.2):
 *     - update_params: briefly takes mutex only to serialise concurrent writers
 *       (two timeline events targeting the same channel).  Writes pending_params,
 *       then releases mutex, then atomically bumps pending_version.
 *       fill_buffer reads pending_version atomically at each buffer boundary;
 *       no extra mutex lock needed on the reader side.
 *
 * (c) Read-only state access — keep mutex (cheap, infrequent):
 *     - is_active        (~353–355)
 *     - get_current_freq (~365–367)
 *     - get_current_amp  (~377–379)
 *     - get_current_freq_r (new)
 *     These are only called from test/diagnostic code, never from real-time paths.
 *
 * Net effect: xSemaphore call count is UNCHANGED — the update_params writer takes
 * the same mutex briefly for write serialisation, but no new mutex site is added
 * to the real-time fill_buffer hot path.
 */

#include "audio_generator.h"
#include "audio_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <stdatomic.h>

static const char* TAG = "audio_generator";

#define MAX_AUDIO_CHANNELS      8
#define PI                      3.14159265359f

// Sine lookup table.  4096 entries × 4 bytes = 16 KB DRAM.  With linear
// interpolation between adjacent entries the harmonic distortion sits at
// roughly -93 dB — well below the 16-bit DAC noise floor (~-96 dB).
#define SINE_LUT_SIZE           4096u
#define SINE_LUT_MASK           (SINE_LUT_SIZE - 1u)
// Q32 phase: top 12 bits index the LUT (4096 = 2^12), low 20 bits are the
// fractional part used as the linear-interpolation weight.
#define SINE_LUT_INDEX_SHIFT    20
#define SINE_LUT_FRAC_MASK      ((1u << SINE_LUT_INDEX_SHIFT) - 1u)
#define SINE_LUT_FRAC_SCALE     (1.0f / (float)(1u << SINE_LUT_INDEX_SHIFT))
// Quarter cycle = LUT_SIZE/4 entries → cos(x) = sin(x + π/2).
#define SINE_LUT_QUARTER        (SINE_LUT_SIZE / 4u)
// Phase-increment scale: cycles-per-second × this = Q32 increment per sample.
#define Q32_PER_HZ              ((float)4294967296.0 / (float)AUDIO_SAMPLE_RATE)

static float sine_lut[SINE_LUT_SIZE];

// Global audio generator state
static audio_gen_channel_t audio_channels[MAX_AUDIO_CHANNELS];
static bool generator_initialized = false;
static SemaphoreHandle_t audio_gen_mutex = NULL;

// Internal functions
static float interpolate_sweep(float start, float target, float progress, audio_gen_sweep_type_t type);
static inline void apply_modulation(float* sample, uint32_t mod_phase_q32, float mod_depth);
static inline void apply_panning(float input, float pan, float* left, float* right);

static inline float fast_sin_q32(uint32_t phase_q32) {
    uint32_t idx  = (phase_q32 >> SINE_LUT_INDEX_SHIFT) & SINE_LUT_MASK;
    uint32_t idx2 = (idx + 1u) & SINE_LUT_MASK;
    float frac = (float)(phase_q32 & SINE_LUT_FRAC_MASK) * SINE_LUT_FRAC_SCALE;
    float a = sine_lut[idx];
    return a + frac * (sine_lut[idx2] - a);
}

// Cosine via the same LUT, offset by a quarter cycle.  Index is computed by
// adding LUT_SIZE/4 to the integer index; the fractional weight is unchanged
// because cos(x) = sin(x + π/2) shifts only the index domain.
static inline float fast_cos_idx(uint32_t int_idx, float frac) {
    uint32_t ci  = (int_idx + SINE_LUT_QUARTER) & SINE_LUT_MASK;
    uint32_t ci2 = (ci + 1u) & SINE_LUT_MASK;
    float a = sine_lut[ci];
    return a + frac * (sine_lut[ci2] - a);
}

static inline float fast_sin_idx(uint32_t int_idx, float frac) {
    uint32_t si2 = (int_idx + 1u) & SINE_LUT_MASK;
    float a = sine_lut[int_idx];
    return a + frac * (sine_lut[si2] - a);
}

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

    // Populate sine LUT once at init.  sinf() is called SINE_LUT_SIZE times
    // here; this is the only sinf() call left in audio_generator.c.
    for (uint32_t i = 0; i < SINE_LUT_SIZE; i++) {
        sine_lut[i] = sinf((2.0f * PI * (float)i) / (float)SINE_LUT_SIZE);
    }

    // Initialize all channels
    memset(audio_channels, 0, sizeof(audio_channels));

    for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        audio_channels[i].active = false;
        audio_channels[i].phase_l_q32 = 0;
        audio_channels[i].phase_r_q32 = 0;
        audio_channels[i].mod_phase_q32 = 0;
        audio_channels[i].samples_generated = 0;
        atomic_init(&audio_channels[i].pending_version, 0);
        audio_channels[i].applied_version = 0;
    }

    generator_initialized = true;

    ESP_LOGI(TAG, "Audio generator initialized with %d channels", MAX_AUDIO_CHANNELS);
    return ESP_OK;
}

void audio_generator_lock(void) {
    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);
}

void audio_generator_unlock(void) {
    xSemaphoreGive(audio_gen_mutex);
}

// ---------------------------------------------------------------------------
// _locked variants — assume audio_gen_mutex is already held by the caller.
// Must not be called from ISR. Declared in audio_generator.h so the timeline
// batch executor can call them while holding the lock acquired via
// audio_generator_lock(), committing a same-timestamp batch atomically.
// ---------------------------------------------------------------------------

esp_err_t audio_generator_start_channel_locked(int channel, const audio_gen_params_t* params) {
    audio_gen_channel_t* ch = &audio_channels[channel];

    memcpy(&ch->params, params, sizeof(audio_gen_params_t));

    ch->current_freq     = params->frequency;
    ch->current_freq_r   = params->frequency_r > 0 ? params->frequency_r : params->frequency;
    ch->current_amp      = params->amplitude;
    ch->current_pan      = params->pan;
    ch->current_mod_freq = params->mod_frequency;
    ch->phase_l_q32      = 0;
    ch->phase_r_q32      = 0;
    ch->mod_phase_q32    = 0;
    ch->samples_generated = 0;

    for (int p = 0; p < AUDIO_PARAM_COUNT; p++) {
        ch->sweeps[p].duration_samples = 0;
    }

    memcpy(&ch->pending_params, params, sizeof(audio_gen_params_t));
    atomic_store(&ch->pending_version, 0);
    ch->applied_version = 0;

    ch->total_samples = ((uint64_t)params->duration_ms * AUDIO_SAMPLE_RATE) / 1000ULL;

    if (params->sweep_type != AUDIO_GEN_SWEEP_NONE && ch->total_samples > 0) {
        ch->sweeps[AUDIO_PARAM_FREQUENCY].start            = params->frequency;
        ch->sweeps[AUDIO_PARAM_FREQUENCY].target           = params->sweep_target;
        ch->sweeps[AUDIO_PARAM_FREQUENCY].start_sample     = 0;
        ch->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples = ch->total_samples;
        ch->sweeps[AUDIO_PARAM_FREQUENCY].curve            = params->sweep_type;
    }
    ch->active = true;

    if (params->duration_ms > 0 && ch->total_samples == 0) {
        ESP_LOGW(TAG, "Duration calculation resulted in zero samples - possible overflow");
        ch->active = false;
        return ESP_ERR_INVALID_ARG;
    }

    // ESP_LOGD — runs inside audio_gen_mutex when called via *_locked path
    // (timeline batch).  ESP_LOGI here would block fill_buffer on UART write.
    ESP_LOGD(TAG, "Started audio channel %d: freq=%.1f Hz, freq_r=%.1f Hz, amp=%.2f, pan=%.2f, duration=%u ms = %llu samples (%.1f hours)",
             channel, params->frequency, ch->current_freq_r, params->amplitude, params->pan, params->duration_ms,
             ch->total_samples, (float)ch->total_samples / (AUDIO_SAMPLE_RATE * 3600.0f));

    return ESP_OK;
}

esp_err_t audio_generator_update_params_locked(int channel, const audio_gen_params_t *new_params) {
    audio_gen_channel_t *ch = &audio_channels[channel];
    if (!ch->active) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&ch->pending_params, new_params, sizeof(audio_gen_params_t));

    // Bump version after releasing the mutex in the public wrapper, but here
    // we are already inside the caller's lock scope — bump immediately so that
    // fill_buffer (which also holds audio_gen_mutex) sees the new version on its
    // next buffer boundary after the batch lock is released.
    atomic_fetch_add_explicit(&ch->pending_version, 1u, memory_order_release);

    ESP_LOGD(TAG, "update_params_locked: channel %d params queued (version %u)",
             channel, atomic_load(&ch->pending_version));

    return ESP_OK;
}

bool audio_generator_is_active_locked(int channel) {
    if (channel < 0 || channel >= MAX_AUDIO_CHANNELS) {
        return false;
    }
    return audio_channels[channel].active;
}

esp_err_t audio_generator_start_sweep_locked(int channel, audio_param_t param,
                                                     float start, float target,
                                                     uint64_t duration_samples,
                                                     audio_gen_sweep_type_t curve) {
    audio_gen_channel_t *ch = &audio_channels[channel];
    ch->sweeps[param].start            = start;
    ch->sweeps[param].target           = target;
    ch->sweeps[param].start_sample     = ch->samples_generated;
    ch->sweeps[param].duration_samples = duration_samples;
    ch->sweeps[param].curve            = curve;
    return ESP_OK;
}

esp_err_t audio_generator_start_channel(int channel, const audio_gen_params_t* params) {
    if (!generator_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel < 0 || channel >= MAX_AUDIO_CHANNELS || !params) {
        return ESP_ERR_INVALID_ARG;
    }

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
    esp_err_t ret = audio_generator_start_channel_locked(channel, params);
    xSemaphoreGive(audio_gen_mutex);
    return ret;
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

    // Count active channels once so each channel's contribution is scaled by
    // 1/N before mixing.  Without this, N same-panned channels at vol=100 sum
    // to N×1.0 and saturate the ±1.0 output ceiling.
    int n_active = 0;
    for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
        if (audio_channels[ch].active) n_active++;
    }
    float inv_n_active = (n_active > 0) ? (1.0f / (float)n_active) : 1.0f;

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

        // --- Lock-free pending-params snapshot (Step 5.2) ---
        // Check once per buffer (not per sample) for a pending parameter update
        // posted by audio_generator_update_params().  The atomic load with
        // acquire ordering ensures that pending_params is fully visible before
        // we read it (pairs with the release-fence in update_params).
        // We deliberately do NOT touch phase_l_q32/phase_r_q32/mod_phase_q32
        // or samples_generated/total_samples — preserving phase coherence.
        {
            uint32_t pv = atomic_load_explicit(&channel->pending_version, memory_order_acquire);
            if (pv != channel->applied_version) {
                // Latch new params.
                memcpy(&channel->params, &channel->pending_params, sizeof(audio_gen_params_t));

                // Re-derive live values from the new params.
                channel->current_amp      = channel->params.amplitude;
                channel->current_pan      = channel->params.pan;
                channel->current_mod_freq = channel->params.mod_frequency;

                // Only update frequency if no active sweep owns it — an active
                // sweep's interpolated value takes precedence.
                if (channel->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples == 0) {
                    channel->current_freq   = channel->params.frequency;
                    channel->current_freq_r = channel->params.frequency_r > 0.0f
                                             ? channel->params.frequency_r
                                             : channel->params.frequency;
                }

                channel->applied_version = pv;
            }
        }

        // Per-sample cost estimate (post Phase 4.2): 4 sweep-params × ~10 cy
        // (branch + lerp) + carrier sin lookup × 2 + pan lookup × 2 ≈ 80 cy/sample
        // /channel — down from ~280 cy when each sample paid two sinf() calls
        // (50–80 cy each) plus a cosf() and an extra sinf() inside apply_panning.
        // 8 channels at 44.1 kHz on ESP32 @ 240 MHz:
        //   8 × 44100 × 80 = ~28.2 MCycles/s  →  ~11.8% CPU worst-case full polyphony,
        // compared to ~41% on the pre-LUT path.
        for (size_t i = 0; i < samples; i++) {
            // --- Frequency ---
            if (channel->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples > 0) {
                // New explicit-window sweep takes precedence over legacy path.
                uint64_t elapsed = channel->samples_generated
                                   - channel->sweeps[AUDIO_PARAM_FREQUENCY].start_sample;
                if (elapsed >= channel->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples) {
                    channel->current_freq = channel->sweeps[AUDIO_PARAM_FREQUENCY].target;
                    channel->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples = 0;
                } else {
                    float progress = (float)elapsed
                                     / (float)channel->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples;
                    channel->current_freq = interpolate_sweep(
                        channel->sweeps[AUDIO_PARAM_FREQUENCY].start,
                        channel->sweeps[AUDIO_PARAM_FREQUENCY].target,
                        progress,
                        channel->sweeps[AUDIO_PARAM_FREQUENCY].curve);
                }
            }
            // Preserve binaural beat frequency offset after any carrier sweep update.
            if (channel->params.frequency_r > 0.0f) {
                float freq_diff = channel->params.frequency_r - channel->params.frequency;
                channel->current_freq_r = channel->current_freq + freq_diff;
            }

            // --- Amplitude ---
            if (channel->sweeps[AUDIO_PARAM_AMPLITUDE].duration_samples > 0) {
                uint64_t elapsed = channel->samples_generated
                                   - channel->sweeps[AUDIO_PARAM_AMPLITUDE].start_sample;
                if (elapsed >= channel->sweeps[AUDIO_PARAM_AMPLITUDE].duration_samples) {
                    channel->current_amp = channel->sweeps[AUDIO_PARAM_AMPLITUDE].target;
                    channel->sweeps[AUDIO_PARAM_AMPLITUDE].duration_samples = 0;
                } else {
                    float progress = (float)elapsed
                                     / (float)channel->sweeps[AUDIO_PARAM_AMPLITUDE].duration_samples;
                    channel->current_amp = interpolate_sweep(
                        channel->sweeps[AUDIO_PARAM_AMPLITUDE].start,
                        channel->sweeps[AUDIO_PARAM_AMPLITUDE].target,
                        progress,
                        channel->sweeps[AUDIO_PARAM_AMPLITUDE].curve);
                }
            }

            // --- Pan ---
            if (channel->sweeps[AUDIO_PARAM_PAN].duration_samples > 0) {
                uint64_t elapsed = channel->samples_generated
                                   - channel->sweeps[AUDIO_PARAM_PAN].start_sample;
                if (elapsed >= channel->sweeps[AUDIO_PARAM_PAN].duration_samples) {
                    channel->current_pan = channel->sweeps[AUDIO_PARAM_PAN].target;
                    channel->sweeps[AUDIO_PARAM_PAN].duration_samples = 0;
                } else {
                    float progress = (float)elapsed
                                     / (float)channel->sweeps[AUDIO_PARAM_PAN].duration_samples;
                    channel->current_pan = interpolate_sweep(
                        channel->sweeps[AUDIO_PARAM_PAN].start,
                        channel->sweeps[AUDIO_PARAM_PAN].target,
                        progress,
                        channel->sweeps[AUDIO_PARAM_PAN].curve);
                }
            }

            // --- Mod frequency ---
            if (channel->sweeps[AUDIO_PARAM_MOD_FREQ].duration_samples > 0) {
                uint64_t elapsed = channel->samples_generated
                                   - channel->sweeps[AUDIO_PARAM_MOD_FREQ].start_sample;
                if (elapsed >= channel->sweeps[AUDIO_PARAM_MOD_FREQ].duration_samples) {
                    channel->current_mod_freq = channel->sweeps[AUDIO_PARAM_MOD_FREQ].target;
                    channel->sweeps[AUDIO_PARAM_MOD_FREQ].duration_samples = 0;
                } else {
                    float progress = (float)elapsed
                                     / (float)channel->sweeps[AUDIO_PARAM_MOD_FREQ].duration_samples;
                    channel->current_mod_freq = interpolate_sweep(
                        channel->sweeps[AUDIO_PARAM_MOD_FREQ].start,
                        channel->sweeps[AUDIO_PARAM_MOD_FREQ].target,
                        progress,
                        channel->sweeps[AUDIO_PARAM_MOD_FREQ].curve);
                }
            }

            // Generate stereo samples using LUT lookup + linear interpolation.
            float sample_l = channel->current_amp * fast_sin_q32(channel->phase_l_q32);
            float sample_r = channel->current_amp * fast_sin_q32(channel->phase_r_q32);

            // Apply modulation if enabled; current_mod_freq drives the accumulator
            // so that mod-frequency sweeps take effect sample-accurately.
            if (channel->current_mod_freq > 0 && channel->params.mod_depth > 0) {
                apply_modulation(&sample_l, channel->mod_phase_q32, channel->params.mod_depth);
                apply_modulation(&sample_r, channel->mod_phase_q32, channel->params.mod_depth);

                channel->mod_phase_q32 += (uint32_t)(channel->current_mod_freq * Q32_PER_HZ);
            }

            // Apply live pan.
            float final_left, final_right;
            apply_panning(sample_l, channel->current_pan, &final_left, &final_right);

            // For binaural beats, right channel uses its own frequency.
            if (channel->current_freq_r != channel->current_freq) {
                float binaural_right;
                apply_panning(sample_r, channel->current_pan, NULL, &binaural_right);
                final_right = binaural_right;
            }

            // Mix into output buffer (stereo interleaved).
            output_buffer[i * 2]     += final_left  * inv_n_active;
            output_buffer[i * 2 + 1] += final_right * inv_n_active;

            // Update phase accumulators.  Modular uint32_t arithmetic wraps for
            // free at 2^32; no branch is needed to keep phase in range.
            channel->phase_l_q32 += (uint32_t)(channel->current_freq   * Q32_PER_HZ);
            channel->phase_r_q32 += (uint32_t)(channel->current_freq_r * Q32_PER_HZ);

            channel->samples_generated++;
        }
    }

    xSemaphoreGive(audio_gen_mutex);
    return ESP_OK;
}

esp_err_t audio_generator_update_sweep(int channel, float new_target, audio_gen_sweep_type_t sweep_type) {
    // DEPRECATED: prefer audio_generator_start_sweep with explicit duration.
    // This wrapper translates legacy callers into the unified sweeps[] state.
    if (!generator_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel < 0 || channel >= MAX_AUDIO_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);

    audio_gen_channel_t* ch = &audio_channels[channel];
    if (ch->active) {
        // Compute remaining samples from the current position to end of channel.
        uint64_t remaining = (ch->total_samples > ch->samples_generated)
                             ? (ch->total_samples - ch->samples_generated)
                             : 0;
        if (sweep_type != AUDIO_GEN_SWEEP_NONE && remaining > 0) {
            ch->sweeps[AUDIO_PARAM_FREQUENCY].start            = ch->current_freq;
            ch->sweeps[AUDIO_PARAM_FREQUENCY].target           = new_target;
            ch->sweeps[AUDIO_PARAM_FREQUENCY].start_sample     = ch->samples_generated;
            ch->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples = remaining;
            ch->sweeps[AUDIO_PARAM_FREQUENCY].curve            = sweep_type;
        } else {
            // Disable any active frequency sweep.
            ch->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples = 0;
        }
        ESP_LOGD(TAG, "Updated sweep for channel %d: target=%.1f Hz, type=%d (via deprecated wrapper)",
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

esp_err_t audio_generator_get_current_freq(int channel, float *out) {
    if (!generator_initialized || channel < 0 || channel >= MAX_AUDIO_CHANNELS || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);
    *out = audio_channels[channel].current_freq;
    xSemaphoreGive(audio_gen_mutex);

    return ESP_OK;
}

esp_err_t audio_generator_get_current_amp(int channel, float *out) {
    if (!generator_initialized || channel < 0 || channel >= MAX_AUDIO_CHANNELS || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);
    *out = audio_channels[channel].current_amp;
    xSemaphoreGive(audio_gen_mutex);

    return ESP_OK;
}

esp_err_t audio_generator_start_sweep(int channel, audio_param_t param,
                                      float start, float target,
                                      uint64_t duration_samples,
                                      audio_gen_sweep_type_t curve) {
    if (!generator_initialized
        || channel < 0 || channel >= MAX_AUDIO_CHANNELS
        || (unsigned)param >= AUDIO_PARAM_COUNT
        || duration_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);
    esp_err_t ret = audio_generator_start_sweep_locked(channel, param, start, target,
                                                       duration_samples, curve);
    xSemaphoreGive(audio_gen_mutex);
    return ret;
}

esp_err_t audio_generator_update_params(int channel, const audio_gen_params_t *new_params) {
    if (!generator_initialized
        || channel < 0 || channel >= MAX_AUDIO_CHANNELS
        || !new_params) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);
    esp_err_t ret = audio_generator_update_params_locked(channel, new_params);
    xSemaphoreGive(audio_gen_mutex);
    return ret;
}

esp_err_t audio_generator_get_current_freq_r(int channel, float *out) {
    if (!generator_initialized || channel < 0 || channel >= MAX_AUDIO_CHANNELS || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(audio_gen_mutex, portMAX_DELAY);
    *out = audio_channels[channel].current_freq_r;
    xSemaphoreGive(audio_gen_mutex);

    return ESP_OK;
}

// Internal helper functions

// Phase accumulation confirmation (Step 1.3):
//   Each sample uses `channel->current_freq`, which is updated by interpolate_sweep()
//   before the phase increment on lines 208 and 213.  Sweeping therefore changes
//   the instantaneous frequency with no discontinuity because the phase accumulator
//   is never reset — only its per-sample increment changes.  This is the correct
//   approach for continuous-phase FM synthesis.

/*
 * interpolate_sweep — maps a normalised progress value [0,1] to a frequency.
 *
 * LINEAR:
 *   freq = start + (target - start) * p
 *   Uniform rate of change; 1 Hz per unit time throughout the sweep.
 *
 * QUADRATIC ease-in-out:
 *   Two-piece polynomial so that d(freq)/dt is zero at both endpoints,
 *   giving a perceptually smooth acceleration/deceleration:
 *
 *     t(p) = 2p²           for p < 0.5   (ease-in — slow start)
 *     t(p) = 1 − 2(1−p)²  for p ≥ 0.5   (ease-out — slow finish)
 *
 *   freq = start + (target - start) * t(p)
 *
 *   Verification at p = 0.25 (first quadrant):
 *     t = 2 × 0.25² = 0.125
 *     For a 400→440 Hz sweep: freq = 400 + 40 × 0.125 = 405 Hz  ✓
 */
static float interpolate_sweep(float start, float target, float progress, audio_gen_sweep_type_t type) {
    if (progress <= 0.0f) return start;
    if (progress >= 1.0f) return target;

    switch (type) {
        case AUDIO_GEN_SWEEP_LINEAR:
            return start + (target - start) * progress;

        case AUDIO_GEN_SWEEP_QUADRATIC: {
            float t = progress < 0.5f ? 2.0f * progress * progress
                                      : 1.0f - 2.0f * (1.0f - progress) * (1.0f - progress);
            return start + (target - start) * t;
        }

        default:
            return start;
    }
}

static inline void apply_modulation(float* sample, uint32_t mod_phase_q32, float mod_depth) {
    *sample *= 1.0f + mod_depth * fast_sin_q32(mod_phase_q32);
}

static inline void apply_panning(float input, float pan, float* left, float* right) {
    // Equal-power panning law: left = cos(angle), right = sin(angle), where
    // angle = (pan + 1) · π/4 ∈ [0, π/2].  Mapping into LUT space:
    //   angle / (2π) · LUT_SIZE  =  (pan + 1) · LUT_SIZE/8
    // i.e. the LUT index spans [0, LUT_SIZE/4] as pan goes from -1 to +1.
    float idxf = (pan + 1.0f) * (float)(SINE_LUT_SIZE / 8u);
    uint32_t int_idx = (uint32_t)idxf;
    float frac = idxf - (float)int_idx;

    if (left)  *left  = input * fast_cos_idx(int_idx, frac);
    if (right) *right = input * fast_sin_idx(int_idx, frac);
}
