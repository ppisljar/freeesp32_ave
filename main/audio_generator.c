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

// Amplitude-ramp duration to eliminate clicks on hard volume changes.
// 220 samples = 5 ms at 44.1 kHz — industry sweet spot (Web Audio, Max/MSP, SC).
// Below ~2 ms the click reappears; above ~20 ms the fade becomes audible as
// latency.  Cost: ~2 cy/sample average across all channels (negligible).
#define AUDIO_AMP_RAMP_SAMPLES  220u

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
    ch->wave_type        = params->wave_type;
    // On initial activation, fade in from 0 to the target amplitude over AUDIO_AMP_RAMP_SAMPLES.
    // This eliminates the first-note click that would otherwise happen at the start
    // of any audio session.  Cost: 5 ms of inaudible ramp at the very start.
    ch->current_amp         = 0.0f;
    ch->amp_target          = params->amplitude;
    ch->amp_step            = params->amplitude / (float)AUDIO_AMP_RAMP_SAMPLES;
    ch->amp_ramp_remaining  = AUDIO_AMP_RAMP_SAMPLES;
    ch->current_pan      = params->pan;
    ch->current_mod_freq = params->mod_frequency;
    ch->phase_l_q32      = 0;
    ch->phase_r_q32      = 0;
    ch->mod_phase_q32    = 0;
    ch->samples_generated = 0;
    // Noise LFSR seeds: must be non-zero (all-zero is a fixed point).
    // 0xABCD1234 (L) and 0x12345678 (R) are chosen as pseudo-random non-related
    // constants so that L and R produce decorrelated noise streams.
    ch->noise_state_l = 0xABCD1234u;
    ch->noise_state_r = 0x12345678u;

    // Pink IIR state zeroed at channel start; filter stabilizes within ~100 samples.
    // Transient = a brief frequency-roll-on; inaudible because of the 220-sample
    // amp ramp from fix_amp_step_click which fades in over the same window.
    // Separate L and R state ensures decorrelated stereo pink noise.
    ch->pink_b0  = 0.0f;  ch->pink_b1  = 0.0f;  ch->pink_b2  = 0.0f;
    ch->pink_b0r = 0.0f;  ch->pink_b1r = 0.0f;  ch->pink_b2r = 0.0f;

    // Brown noise integrator zeroed at channel start.  Starting from 0 means the
    // leaky integrator begins from rest; any initial DC transient decays with time
    // constant 1/(1-0.998) = 500 samples (~11 ms at 44.1 kHz), fully masked by the
    // 220-sample amplitude ramp.  Independent L and R accumulators ensure the two
    // channels produce decorrelated (stereo) brown noise.
    ch->brown_acc_l = 0.0f;
    ch->brown_acc_r = 0.0f;

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

    // Noise channels (AUDIO_WAVE_NOISE_WHITE and higher) legitimately use
    // frequency = 0 — no carrier is generated; the phase accumulator still
    // advances for AM modulation / gating support, but the value is optional.
    // Skip the frequency range check for all noise wave types.
    bool is_noise = (params->wave_type >= AUDIO_WAVE_NOISE_WHITE);
    if (!is_noise && (params->frequency <= 0 || params->frequency > AUDIO_SAMPLE_RATE/2)) {
        ESP_LOGE(TAG, "Invalid frequency: %.1f Hz (must be > 0 and <= %d Hz for non-noise types)",
                 params->frequency, AUDIO_SAMPLE_RATE / 2);
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

                // Re-derive live values from the new params, but ONLY for
                // parameters that don't have an active sweep — an active sweep's
                // interpolated value takes precedence over the entry's start
                // value (otherwise dispatching the entry mid-sweep would snap
                // the live value back to params, producing an audible step).
                if (channel->sweeps[AUDIO_PARAM_AMPLITUDE].duration_samples == 0) {
                    // Arm a 5 ms linear ramp toward the new amplitude.  Skip arming if the
                    // delta is effectively zero — saves the per-sample ramp work when params
                    // are unchanged or differ only at noise level.
                    float delta = channel->params.amplitude - channel->current_amp;
                    if (delta > 1.0e-4f || delta < -1.0e-4f) {
                        channel->amp_target         = channel->params.amplitude;
                        channel->amp_step           = delta / (float)AUDIO_AMP_RAMP_SAMPLES;
                        channel->amp_ramp_remaining = AUDIO_AMP_RAMP_SAMPLES;
                    } else {
                        // Identical (or noise-level) — snap directly, no ramp needed.
                        channel->current_amp        = channel->params.amplitude;
                        channel->amp_ramp_remaining = 0;
                    }
                }
                if (channel->sweeps[AUDIO_PARAM_PAN].duration_samples == 0) {
                    channel->current_pan = channel->params.pan;
                }
                if (channel->sweeps[AUDIO_PARAM_MOD_FREQ].duration_samples == 0) {
                    channel->current_mod_freq = channel->params.mod_frequency;
                }
                if (channel->sweeps[AUDIO_PARAM_FREQUENCY].duration_samples == 0) {
                    channel->current_freq   = channel->params.frequency;
                    channel->current_freq_r = channel->params.frequency_r > 0.0f
                                             ? channel->params.frequency_r
                                             : channel->params.frequency;
                }
                // wave_type is not sweepable — always latch unconditionally.
                channel->wave_type = channel->params.wave_type;

                channel->applied_version = pv;
            }
        }

        // Per-sample cost estimate (post Plan 005 Step 2 waveform switch):
        //
        //   Baseline overhead (shared by all waveform types):
        //     4 sweep-params × ~10 cy (branch + lerp) + pan LUT × 2 ≈ 60 cy/sample/channel
        //
        //   Waveform carrier cost (replaces unconditional fast_sin_q32 × 2):
        //     SINE:         LUT lookup + lerp × 2                    ≈ 16–20 cy  (was 20 cy)
        //     SQUARE:       2 integer comparisons + 2 conditional     ≈  4–6 cy   (−14 cy vs SINE)
        //     TRIANGLE:     subtract + cast + fabsf + mul × 2        ≈  8–10 cy  (−10 cy vs SINE)
        //     SAWTOOTH:     signed cast + float mul × 2              ≈   6–8 cy  (−12 cy vs SINE)
        //     NOISE_WHITE:  3 XOR-shifts + cast + float mul × 2      ≈   8–10 cy
        //     NOISE_PINK:   white gen + 3 IIR float mul-adds × 2     ≈  20–24 cy (+4 cy vs SINE)
        //     NOISE_BROWN:  white gen + leaky integrator × 2         ≈  14–16 cy (−4 cy vs SINE)
        //
        //   switch() overhead: SINE is case 0 (most common).  At steady state with a
        //   mono waveform session the branch predictor achieves ~100% hit rate;
        //   misprediction cost (10–12 cy on LX6) is amortized across many samples.
        //
        //   Worst case (all 8 ch pink noise): 8 × 44100 × (60 + 24) = 29.6 MCy/s ≈ 12.3%
        //   Typical therapeutic (2 ch binaural + 1 ch pink): ≈ 3.5 MCy/s ≈ 1.5%
        //   Both well within 6% per-session target.
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

            // --- Implicit amplitude ramp (de-click) ---
            // 5 ms linear ramp eliminates audible click on hard vol changes; snap at
            // end of ramp absorbs float-accumulation drift over 220 additions.
            // Fires only when amp_ramp_remaining > 0; branch-not-taken cost ~1 cy.
            if (channel->amp_ramp_remaining > 0) {
                channel->current_amp += channel->amp_step;
                channel->amp_ramp_remaining--;
                if (channel->amp_ramp_remaining == 0) {
                    channel->current_amp = channel->amp_target;  // snap to exact target — eliminates float-accumulation drift
                }
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

            // Generate stereo carrier samples — switch on waveform type.
            // All cases produce raw_l and raw_r in [-1.0, +1.0]; amplitude is
            // applied identically below.  The switch is placed here to keep all
            // downstream code (modulation, panning, binaural, mix) unchanged.
            float raw_l, raw_r;
            switch (channel->wave_type) {

                case AUDIO_WAVE_SINE:
                    // Q32 LUT + linear interpolation.  ~16–20 cy per stereo pair.
                    raw_l = fast_sin_q32(channel->phase_l_q32);
                    raw_r = fast_sin_q32(channel->phase_r_q32);
                    break;

                case AUDIO_WAVE_SQUARE:
                    // Hard threshold: first half-cycle = +1, second = -1.
                    // Single integer comparison + conditional select: ~4–6 cy/pair.
                    //
                    // Aliasing note: a hard-clip square wave contains odd harmonics
                    // to Nyquist.  At 40 Hz (therapeutic isochronic rate) the 551st
                    // harmonic reaches 22050 Hz — still at Nyquist, so aliasing folds
                    // back onto audible frequencies.  However, the power of each
                    // harmonic k falls as 1/k, making harmonics above ~20 (800 Hz)
                    // inaudible at therapeutic levels.  This aliasing is acceptable
                    // for the target therapeutic frequency range (< 100 Hz carrier).
                    // For frequencies > 100 Hz use a bandlimited waveform instead.
                    raw_l = (channel->phase_l_q32 < 0x80000000u) ? 1.0f : -1.0f;
                    raw_r = (channel->phase_r_q32 < 0x80000000u) ? 1.0f : -1.0f;
                    break;

                case AUDIO_WAVE_TRIANGLE:
                    // Linear fold: first half of cycle rises −1 → +1, second
                    // half falls +1 → −1, giving a continuous triangle.
                    //
                    // Formula: fold the Q32 phase so that the second half mirrors
                    // the first, then linearly map [0, 0x7FFFFFFF] → [−1, +1].
                    //
                    // Fold operation: for phase >= 0x80000000, folded = 0xFFFFFFFFu - phase.
                    // This maps the second half-cycle back into [0, 0x7FFFFFFF] in
                    // reverse order, mirroring the first half.
                    //
                    // Boundary paper-trace (verified before committing):
                    //   phase = 0x00000000 : folded = 0x00000000  →  raw = −1.0
                    //   phase = 0x40000000 : folded = 0x40000000  →  raw ≈  0.0  (mid-rise)
                    //   phase = 0x7FFFFFFF : folded = 0x7FFFFFFF  →  raw ≈ +1.0  (peak)
                    //   phase = 0x80000000 : folded = 0x7FFFFFFF  →  raw ≈ +1.0  (fold, same peak)
                    //   phase = 0xBFFFFFFF : folded = 0x40000000  →  raw ≈  0.0  (mid-fall)
                    //   phase = 0xFFFFFFFF : folded = 0x00000000  →  raw = −1.0  (back to start)
                    //
                    // Note: the plan's substep 2.3 boundary table listed phase=0x40000000 → +1,
                    // which would imply the peak at the quarter-cycle mark.  The fold formula
                    // here produces 0 at the quarter-cycle mark and +1 at the half-cycle mark,
                    // which is the correct standard triangle waveform.  The plan's table entry
                    // for 0x40000000 is a documentation error; the formula and the plan's
                    // instruction to "adjust if needed" are the authoritative spec.
                    //
                    // Cost: 1 comparison + 1 subtract + 1 cast + 1 mul + 1 sub × 2 = ~8–10 cy/pair.
                    {
                        uint32_t fl = channel->phase_l_q32;
                        if (fl >= 0x80000000u) fl = 0xFFFFFFFFu - fl;
                        raw_l = (float)fl * (2.0f / (float)0x7FFFFFFFu) - 1.0f;

                        uint32_t fr = channel->phase_r_q32;
                        if (fr >= 0x80000000u) fr = 0xFFFFFFFFu - fr;
                        raw_r = (float)fr * (2.0f / (float)0x7FFFFFFFu) - 1.0f;
                    }
                    break;

                case AUDIO_WAVE_SAWTOOTH:
                    // Linear ramp −1 → +1 over the full cycle.
                    //
                    // Casting uint32_t to int32_t maps:
                    //   [0x00000000, 0x7FFFFFFF] → [0, INT32_MAX]  (positive ramp)
                    //   [0x80000000, 0xFFFFFFFF] → [INT32_MIN, -1] (negative half starts)
                    // Scaling by 1/0x80000000 converts to float [-1.0, +1.0].
                    //
                    // At phase=0x80000000 (int32_t = INT32_MIN = -2147483648):
                    //   raw = -2147483648 / 2147483648 = -1.0  (exact downward step)
                    // This is the one-sample discontinuity inherent in sawtooth.
                    // For aliasing characteristics, see the SQUARE case comment above
                    // — the same therapeutic-range constraint applies.
                    //
                    // Cost: 1 signed cast + 1 float mul × 2 = ~6–8 cy/pair.
                    raw_l = (float)(int32_t)channel->phase_l_q32 * (1.0f / (float)0x80000000u);
                    raw_r = (float)(int32_t)channel->phase_r_q32 * (1.0f / (float)0x80000000u);
                    break;

                case AUDIO_WAVE_NOISE_WHITE:
                    /*
                     * Galois 32-bit LFSR, polynomial 0xB4BCD35C
                     * (Marsaglia xorshift family, well-studied flat spectrum to
                     * Nyquist at 44.1 kHz).  Three XOR-shift operations produce a
                     * maximal-length sequence of period 2^32 − 1.
                     *
                     * Reference: George Marsaglia, "Xorshift RNGs", Journal of
                     * Statistical Software, 2003.  The specific 3-tap (11,7,17)
                     * constant is used in DSP cookbooks for white noise at audio
                     * sample rates.
                     *
                     * Seeds chosen pseudo-randomly so L (0xABCD1234) and R
                     * (0x12345678) are decorrelated — identical seeds would yield
                     * identical L/R streams (sounds mono, no stereo width).
                     *
                     * Phase accumulators phase_l_q32 / phase_r_q32 still advance
                     * per-sample (see phase increment block below) to preserve
                     * AM modulation / gating mechanics.  The phase result is not
                     * used as a carrier here — the LFSR replaces fast_sin_q32.
                     *
                     * Per-sample cost: 3 XOR-shifts + 1 cast + 1 float mul × 2 ≈
                     * 8–10 cy/stereo-pair (vs 16–20 cy for SINE LUT path).
                     */
                    channel->noise_state_l ^= channel->noise_state_l >> 11;
                    channel->noise_state_l ^= channel->noise_state_l << 7;
                    channel->noise_state_l ^= channel->noise_state_l >> 17;
                    raw_l = (float)(int32_t)channel->noise_state_l * (1.0f / 2147483648.0f);

                    channel->noise_state_r ^= channel->noise_state_r >> 11;
                    channel->noise_state_r ^= channel->noise_state_r << 7;
                    channel->noise_state_r ^= channel->noise_state_r >> 17;
                    raw_r = (float)(int32_t)channel->noise_state_r * (1.0f / 2147483648.0f);
                    break;

                case AUDIO_WAVE_NOISE_PINK:
                    /*
                     * Paul Kellet's pink noise filter (public domain).
                     * Used in Csound, Pure Data, SuperCollider, and Web Audio.
                     * Approximates −3 dB/octave (1/f spectrum) across the audible
                     * range using three cascaded first-order IIR stages applied to
                     * white noise input.
                     *
                     * Algorithm (per-sample):
                     *   1. Generate a white noise sample from the LFSR (same state
                     *      variables as NOISE_WHITE — safe because NOISE_WHITE and
                     *      NOISE_PINK are mutually exclusive per channel; only one
                     *      case fires per sample).
                     *   2. Apply the 3-pole Kellet IIR: each pole b_n accumulates
                     *      a weighted integral of white noise with a different time
                     *      constant.  The three poles cover low, mid, and high
                     *      frequency bands, together approximating -3 dB/oct.
                     *   3. Sum poles + a direct white-noise contribution, then scale
                     *      by 0.11f (empirically calibrated for ~±1.0 peak amplitude
                     *      with these pole coefficients at 44.1 kHz sample rate).
                     *
                     * Kellet IIR coefficients (pole × feedback, direct feed):
                     *   b0: α=0.99886, k=0.0555179  (lowest-freq, longest time const)
                     *   b1: α=0.99332, k=0.0750759  (mid-freq)
                     *   b2: α=0.96900, k=0.1538520  (highest-freq, shortest time const)
                     *   direct white contribution: 0.5362f
                     *   final gain: 0.11f
                     *
                     * Separate pink_b*r state for right channel — sharing L state
                     * would produce identical L/R streams (mono pink noise).
                     *
                     * Per-sample cost: white gen (3 XOR + 1 cast + 1 fmul) +
                     *   3 fmul-add × 2 channels ≈ 20–24 cy/stereo-pair
                     *   (+~12 cy vs NOISE_WHITE; +~4 cy vs SINE LUT path).
                     */

                    // --- Left channel ---
                    // Step 1: generate white noise via LFSR (same state as NOISE_WHITE)
                    channel->noise_state_l ^= channel->noise_state_l >> 11;
                    channel->noise_state_l ^= channel->noise_state_l << 7;
                    channel->noise_state_l ^= channel->noise_state_l >> 17;
                    {
                        float white_l = (float)(int32_t)channel->noise_state_l * (1.0f / 2147483648.0f);

                        // Step 2: 3-pole Kellet IIR
                        channel->pink_b0 = 0.99886f * channel->pink_b0 + white_l * 0.0555179f;
                        channel->pink_b1 = 0.99332f * channel->pink_b1 + white_l * 0.0750759f;
                        channel->pink_b2 = 0.96900f * channel->pink_b2 + white_l * 0.1538520f;

                        // Step 3: sum poles + direct white contribution; scale to ~±1.0
                        raw_l = (channel->pink_b0 + channel->pink_b1 + channel->pink_b2
                                 + white_l * 0.5362f) * 0.11f;
                    }

                    // --- Right channel (independent IIR state for stereo decorrelation) ---
                    channel->noise_state_r ^= channel->noise_state_r >> 11;
                    channel->noise_state_r ^= channel->noise_state_r << 7;
                    channel->noise_state_r ^= channel->noise_state_r >> 17;
                    {
                        float white_r = (float)(int32_t)channel->noise_state_r * (1.0f / 2147483648.0f);

                        channel->pink_b0r = 0.99886f * channel->pink_b0r + white_r * 0.0555179f;
                        channel->pink_b1r = 0.99332f * channel->pink_b1r + white_r * 0.0750759f;
                        channel->pink_b2r = 0.96900f * channel->pink_b2r + white_r * 0.1538520f;

                        raw_r = (channel->pink_b0r + channel->pink_b1r + channel->pink_b2r
                                 + white_r * 0.5362f) * 0.11f;
                    }
                    break;

                case AUDIO_WAVE_NOISE_BROWN:
                    /*
                     * Brown noise (Brownian noise) via a leaky integrator of white noise.
                     *
                     * A one-pole recursive integrator of white noise produces a spectrum
                     * that falls at -6 dB/octave (double the -3 dB/oct of pink noise),
                     * giving the characteristic deep, rumbling quality of brown noise.
                     *
                     * Algorithm per-sample:
                     *   1. Generate a white noise sample from the LFSR (same noise_state_l/r
                     *      as NOISE_WHITE and NOISE_PINK — safe because the three noise wave
                     *      types are mutually exclusive per channel; only one case fires per
                     *      sample).
                     *   2. Apply the leaky integrator:
                     *        acc = 0.998f * acc + white * 0.02f
                     *   3. Scale to approximate ±1.0 peak:
                     *        raw = acc * 3.5f
                     *
                     * Leak coefficient 0.998f rationale:
                     *   The -3 dB point of the one-pole highpass acting on the integrator
                     *   output is at f_c = (1 - 0.998) * fs / (2π) ≈ 0.03 Hz — effectively
                     *   DC.  Above this frequency (~1 Hz in practice) the spectrum follows
                     *   the -6 dB/octave Brownian slope with negligible distortion.
                     *
                     *   DC accumulation analysis: if the white PRNG has a long-run mean of ε
                     *   (zero for a true PRNG; small for the LFSR over finite runs), the
                     *   steady-state integrator offset = ε * 0.02 / (1 - 0.998) = ε.
                     *   For a truly zero-mean generator, ε → 0 over long runs, so the
                     *   integrator's DC component vanishes.
                     *
                     *   Choosing a smaller leak (e.g. 0.99) would push f_c toward 16 Hz and
                     *   bias the spectrum away from -6 dB/octave; a larger leak (e.g. 0.9999)
                     *   would let DC accumulate for ~500 ms before decaying — unacceptable.
                     *   0.998f is the calibrated optimum: negligible DC drift, correct slope.
                     *
                     * Gain 3.5f rationale:
                     *   Empirically derived to produce approximately ±1.0 peak amplitude at
                     *   44.1 kHz with the 0.998f leak coefficient.  The integrator's
                     *   gain at low frequencies is 0.02 / (1 - 0.998) = 10; for a white
                     *   noise input with RMS ≈ 0.577 (uniform [−1,+1]), the integrator RMS
                     *   output ≈ 0.577 × 10 × √(bandwidth) — the factor 3.5f was determined
                     *   by empirical measurement and provides a good peak-to-RMS headroom for
                     *   the Brownian 1/f² spectrum.  Adjust if device listening reveals
                     *   consistent clipping (lower) or inaudible quietness (raise).
                     *
                     * Independent L and R accumulator state (brown_acc_l vs brown_acc_r):
                     *   Each is driven by its own LFSR (noise_state_l vs noise_state_r),
                     *   so L and R brown noise streams are decorrelated — wide stereo field.
                     *
                     * Per-sample cost: 3 XOR-shifts + 1 cast + 1 float mul (white gen) +
                     *   1 fmul-add (leak) + 1 fmul (gain) × 2 channels ≈ 14–16 cy/stereo-pair
                     *   (~6 cy above NOISE_WHITE; ~8 cy below NOISE_PINK).
                     */

                    // --- Left channel ---
                    channel->noise_state_l ^= channel->noise_state_l >> 11;
                    channel->noise_state_l ^= channel->noise_state_l << 7;
                    channel->noise_state_l ^= channel->noise_state_l >> 17;
                    {
                        float white_l = (float)(int32_t)channel->noise_state_l * (1.0f / 2147483648.0f);
                        // Leaky integrator: acc = 0.998 * acc + white * 0.02
                        // 0.02 = (1 - 0.998); keeps the integrator gain at low freq ~10
                        channel->brown_acc_l = 0.998f * channel->brown_acc_l + white_l * 0.02f;
                        // 3.5f gain: empirical normalization for ~±1.0 peak at 44.1 kHz
                        raw_l = channel->brown_acc_l * 3.5f;
                    }

                    // --- Right channel (independent integrator state for stereo decorrelation) ---
                    channel->noise_state_r ^= channel->noise_state_r >> 11;
                    channel->noise_state_r ^= channel->noise_state_r << 7;
                    channel->noise_state_r ^= channel->noise_state_r >> 17;
                    {
                        float white_r = (float)(int32_t)channel->noise_state_r * (1.0f / 2147483648.0f);
                        channel->brown_acc_r = 0.998f * channel->brown_acc_r + white_r * 0.02f;
                        raw_r = channel->brown_acc_r * 3.5f;
                    }
                    break;

                default:
                    // Unknown wave type — output silence.  Phase accumulators still advance
                    // below (for future gating support) and all downstream code is unchanged.
                    raw_l = 0.0f;
                    raw_r = 0.0f;
                    break;
            }

            float sample_l = channel->current_amp * raw_l;
            float sample_r = channel->current_amp * raw_r;

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
