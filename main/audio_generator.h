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
 * - Up to NUM_AUDIO_CHANNELS (default 16, see below) simultaneous synthesis channels
 */

/**
 * @brief Number of simultaneous audio synthesis channels (default 16).
 *
 * Each channel maintains independent frequency, amplitude, pan, modulation,
 * waveform type, sweep state, and noise LFSR/IIR state. Channels are identified
 * by index 0-(NUM_AUDIO_CHANNELS-1); the .led timeline format uses 1-based indices
 * (1-16), with index 0 rejected.
 *
 * Changing this value requires a full rebuild and reflash. Before increasing the
 * count, profile CPU load at the current setting — each additional channel adds
 * approximately 3-4% CPU load at 44.1 kHz (see Plan 009 Step 4). At 16 channels
 * the system has been validated to remain within the <6% total CPU budget.
 *
 * All channel-indexed arrays and loop bounds derive from this macro; no literals
 * need updating when the value changes.
 */
#define NUM_AUDIO_CHANNELS 16


#define AUDIO_GEN_SAMPLE_RATE       44100
#define AUDIO_GEN_CHANNELS          2
#define AUDIO_GEN_BUFFER_SIZE       1024

typedef enum {
    AUDIO_WAVE_SINE        = 0,   // Sine wave (default) — uses Q32 LUT
    AUDIO_WAVE_SQUARE      = 1,   // Square wave — hard threshold on phase
    AUDIO_WAVE_TRIANGLE    = 2,   // Triangle wave — linear fold on phase
    AUDIO_WAVE_SAWTOOTH    = 3,   // Sawtooth wave — linear ramp -1→+1
    AUDIO_WAVE_NOISE_WHITE = 4,   // White noise — Galois LFSR
    AUDIO_WAVE_NOISE_PINK  = 5,   // Pink noise — Kellet 3-pole IIR
    AUDIO_WAVE_NOISE_BROWN = 6,   // Brown noise — leaky integrator
    AUDIO_WAVE_COUNT       = 7
} audio_wave_type_t;

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
    audio_wave_type_t wave_type;  // Waveform type (0 = SINE default)
    audio_gen_sweep_type_t sweep_type;
    float sweep_target;    // Target frequency for sweep
    uint32_t duration_ms;  // Duration in milliseconds
} audio_gen_params_t;

typedef struct {
    audio_gen_params_t params;
    float current_freq;
    float current_freq_r;
    float current_amp;       // live interpolated amplitude — driven by ramp or snap
    float amp_target;        // destination amplitude the ramp is heading toward
    float amp_step;          // signed per-sample delta = (target - current) / RAMP_SAMPLES
    uint32_t amp_ramp_remaining; // countdown in samples; 0 = no ramp active
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
    // Stop-fade flag: when true, the channel is amp-ramping toward 0 and will
    // self-deactivate (active=false) at the end of the ramp.  Set by
    // audio_generator_stop_channel() and by the duration-end path in fill_buffer.
    // Cleared by fill_buffer when the ramp completes.  Eliminates the click that
    // would otherwise occur when active=false snaps a non-zero current_amp to 0.
    volatile bool stopping;
    audio_wave_type_t wave_type;   // Active waveform, latched from params at start/apply

    // White noise LFSR state (Galois 32-bit xorshift, polynomial 0xB4BCD35C).
    // Seeds chosen so L and R are decorrelated — identical seeds produce identical
    // L/R output (sounds mono); these two seeds differ in every bit group.
    // MUST NEVER be zero: an all-zero LFSR is a fixed point that outputs silence forever.
    uint32_t noise_state_l;   // LFSR state for left channel white noise
    uint32_t noise_state_r;   // LFSR state for right channel white noise

    // Pink noise 3-pole IIR state (Paul Kellet algorithm).
    // Separate state for L and R so the two channels produce decorrelated pink
    // noise — sharing state would yield identical L/R streams (mono pink noise).
    // All six fields are zeroed at channel start; the filter stabilizes within
    // ~100 samples (see zero-init rationale comment in audio_generator.c).
    float pink_b0,  pink_b1,  pink_b2;   // IIR integrator state — left channel
    float pink_b0r, pink_b1r, pink_b2r;  // IIR integrator state — right channel

    // Brown noise leaky integrator state (one-pole IIR of white noise).
    // Leak coefficient 0.998f: -3 dB point at ~0.03 Hz, effectively flat from
    // 1 Hz upward.  Prevents infinite DC accumulation while preserving the
    // -6 dB/octave (Brownian) spectral slope across the audible range.
    // Independent L and R accumulators produce decorrelated stereo brown noise.
    // Both initialized to 0.0f at channel start (integrator starts from rest).
    float brown_acc_l;   // leaky integrator state — left channel
    float brown_acc_r;   // leaky integrator state — right channel

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
 * @brief Check whether any channel is currently fading out (stopping).
 *
 * Returns true while at least one channel has its stop-fade ramp active.
 * Used by web_server's stop_handler to wait for all channels to complete
 * their 5 ms fade-to-zero before turning off LEDs, so the user perceives
 * a clean synchronized stop.
 *
 * @return true if any channel is still fading out, false otherwise.
 */
bool audio_generator_any_stopping(void);

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

/** Log one line per active sweep across all channels (current value,
 *  start->target window, % done, seconds remaining). Snapshots state under
 *  audio_gen_mutex briefly, then releases the lock before any ESP_LOGI call
 *  — UART writes are slow enough that logging under the mutex would block
 *  fill_buffer and cause I2S underrun.
 *  @return Number of active sweeps logged. */
int audio_generator_log_sweep_progress(void);

/** Log full state of every active audio channel — all current parameter
 *  values plus any sweep details. Designed for one-shot snapshots ("what is
 *  the system actually doing right now?"), e.g. on a debug button press.
 *  Same snapshot-then-release pattern as audio_generator_log_sweep_progress. */
void audio_generator_log_full_state(void);

/** Task-context only — DO NOT call from ISR. Pair every lock() with unlock().
 *  Used by the timeline executor to commit same-timestamp batches atomically:
 *  hold the lock for the entire batch so fill_buffer sees all channels activate
 *  in the same DMA buffer, preventing inter-ear onset delay. */
void audio_generator_lock(void);
void audio_generator_unlock(void);

/**
 * @brief _locked variants — caller MUST hold the lock acquired via
 * audio_generator_lock() before calling any of these. Must not be called
 * from ISR. Only the timeline batch executor should use these directly.
 */
esp_err_t audio_generator_start_channel_locked(int channel, const audio_gen_params_t *params);
esp_err_t audio_generator_update_params_locked(int channel, const audio_gen_params_t *new_params);
bool      audio_generator_is_active_locked(int channel);
esp_err_t audio_generator_start_sweep_locked(int channel, audio_param_t param,
                                             float start, float target,
                                             uint64_t duration_samples,
                                             audio_gen_sweep_type_t curve);

#endif // AUDIO_GENERATOR_H
