#ifndef AUDIO_TEST_H
#define AUDIO_TEST_H

#include "esp_err.h"

/**
 * @brief Audio Test Functions
 *
 * Provides test functions for audio generation and LED synchronization
 */

/**
 * @brief Test basic audio generation
 *
 * Generates test tones and outputs them to I2S
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_test_basic_generation(void);

/**
 * @brief Test binaural beats
 *
 * Generates binaural beats for testing
 * @param base_freq Base frequency in Hz
 * @param beat_freq Beat frequency difference in Hz
 * @param duration_ms Duration in milliseconds
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_test_binaural_beats(float base_freq, float beat_freq, uint32_t duration_ms);

/**
 * @brief Test frequency sweep
 *
 * Generates a frequency sweep from start to end frequency
 * @param start_freq Starting frequency in Hz
 * @param end_freq Ending frequency in Hz
 * @param duration_ms Duration in milliseconds
 * @param sweep_type Type of sweep (linear or quadratic)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_test_frequency_sweep(float start_freq, float end_freq, uint32_t duration_ms, int sweep_type);

/**
 * @brief Test audio output task
 *
 * Continuously generates and outputs audio samples
 * This function runs as a FreeRTOS task
 * @param pvParameters Task parameters (unused)
 */
void audio_test_output_task(void* pvParameters);

/**
 * @brief Start audio output test task
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_test_start_output_task(void);

/**
 * @brief Stop audio output test task
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_test_stop_output_task(void);

/**
 * @brief ISR baseline soak test (Step 1 instrumentation validation)
 *
 * Runs for ~5 minutes exercising LED flicker at 8 Hz and 20 Hz plus VU-meter
 * sync mode while printing isr_profiling_report() every 30 seconds.
 * The web-upload portion (flash write path) must be triggered manually by the
 * operator via the web interface during the run — see the soak procedure in
 * reports/non_planned_reports/isr_baseline_2026-06-15.md.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_test_isr_baseline_soak(void);

/**
 * @brief Verify linear sweep interpolation at p=0.25.
 *
 * Sweeps channel 0 from 400 Hz to 440 Hz over 1 second with AUDIO_GEN_SWEEP_LINEAR,
 * samples current_freq after ~250 ms (25% progress), and checks that it matches
 * 410 Hz within ±0.5 Hz.
 *
 * @return ESP_OK if frequency is within tolerance, ESP_FAIL otherwise.
 */
esp_err_t audio_test_sweep_verify_linear(void);

/**
 * @brief Verify quadratic sweep interpolation at p=0.25.
 *
 * Sweeps channel 0 from 400 Hz to 440 Hz over 1 second with AUDIO_GEN_SWEEP_QUADRATIC,
 * samples current_freq after ~250 ms (25% progress, first ease-in branch), and checks
 * that it matches 405 Hz within ±0.5 Hz.
 *   t(0.25) = 2 * 0.25^2 = 0.125  →  freq = 400 + 40 * 0.125 = 405 Hz
 *
 * @return ESP_OK if frequency is within tolerance, ESP_FAIL otherwise.
 */
esp_err_t audio_test_sweep_verify_quadratic(void);

/**
 * @brief Verify amplitude fade-in via audio_generator_start_sweep at p=0.25.
 *
 * Starts channel 0 at 440 Hz with amplitude 0.0, then calls
 * audio_generator_start_sweep(0, AUDIO_PARAM_AMPLITUDE, 0.0, 1.0, 44100, LINEAR).
 * After ~260 ms (~25% of 44100 samples) checks current_amp ≈ 0.25 within ±0.02.
 *
 * @return ESP_OK if amplitude is within tolerance, ESP_FAIL otherwise.
 */
esp_err_t audio_test_sweep_verify_amplitude(void);

/**
 * @brief Eight-channel concurrent soak test (Step 5.4).
 *
 * Starts channels 0-7 at 200, 220, 240, 260, 280, 300, 320, 340 Hz
 * (amplitude 0.05 each — 8-channel sum peaks at 0.40).  After 30 seconds,
 * sweeps all 8 channels to half their start frequency over another 30 seconds.
 * Stops all channels and logs ISR stats.  Returns ESP_OK if no queue drops
 * were detected during the run.
 *
 * @return ESP_OK if soak completes without queue drops, ESP_FAIL otherwise.
 */
esp_err_t audio_test_multichannel_soak(void);

/**
 * @brief Binaural-beat right-channel frequency precision check (Step 5.5).
 *
 * Configures channel 0 with frequency=200.0 Hz, frequency_r=200.01 Hz
 * (target 0.01 Hz beat difference).  Runs for 60 seconds, then reads back
 * current_freq and current_freq_r.  Verifies both are within ±0.001 Hz of
 * their configured values (10x tighter than the ±0.01 Hz spec requirement).
 *
 * Note: true acoustic precision requires hardware-in-loop ADC capture (out of
 * scope for Phase 3).  This test verifies the generator's internal state only.
 *
 * @return ESP_OK if both frequencies are within ±0.001 Hz, ESP_FAIL otherwise.
 */
esp_err_t audio_test_binaural_precision_verify(void);

/**
 * @brief Waveform type demonstration — square, triangle, sawtooth, sine (Plan 005 Step 2).
 *
 * Cycles through SQUARE → TRIANGLE → SAWTOOTH → SINE on channel 0 at 40 Hz,
 * 3 seconds each.  Operator should hear four distinctly different timbres:
 *
 *   SQUARE:   harsh/buzzy, only odd harmonics
 *   TRIANGLE: softer than square but brighter than sine
 *   SAWTOOTH: richest/harshest, odd and even harmonics
 *   SINE:     pure tone, no harmonics (baseline reference)
 *
 * 40 Hz is within the therapeutic isochronic target range and is perceptible
 * as a tone on standard speakers.  This test is intended for device-level
 * verification by the orchestrator after flashing.
 *
 * @return ESP_OK when all four waveforms complete, ESP_FAIL if any fails to start.
 */
esp_err_t audio_test_waveform_types(void);

/**
 * @brief White noise generator demonstration (Plan 005 Step 3).
 *
 * Starts channel 1 with wave_type=AUDIO_WAVE_NOISE_WHITE, frequency=0 Hz
 * (noise channels bypass the frequency guard), volume=50% (amplitude=0.5),
 * for a 30 second duration.
 *
 * Device-level verification: operator should hear broadband "sh" hiss with
 * wide stereo field (L/R streams are decorrelated via different LFSR seeds).
 * No click on start — the 5 ms amplitude ramp fires as for all waveform types.
 *
 * @return ESP_OK when noise starts successfully, ESP_FAIL if channel start fails.
 */
esp_err_t audio_test_noise_white(void);

/**
 * @brief Pink noise generator demonstration (Plan 005 Step 4).
 *
 * Starts channel 2 with wave_type=AUDIO_WAVE_NOISE_PINK, frequency=0 Hz
 * (noise channels bypass the frequency guard), volume=50% (amplitude=0.5),
 * for a 30 second duration.
 *
 * Pink noise uses the Paul Kellet 3-pole IIR filter applied to white noise,
 * producing a -3 dB/octave spectrum across the audible range.  The result
 * sounds warmer than white noise (more bass energy, less high-frequency hiss).
 *
 * The IIR state is zeroed at channel start and stabilizes within ~100 samples
 * — inaudible because the 220-sample amplitude ramp covers the same window.
 * L and R channels use independent IIR state (pink_b0/b1/b2 vs pink_b0r/b1r/b2r)
 * to produce decorrelated stereo pink noise.
 *
 * Device-level verification: operator should hear a "warmer" sound than white
 * noise at the same volume setting.  Wide stereo field.  No onset click.
 *
 * @return ESP_OK when noise starts successfully, ESP_FAIL if channel start fails.
 */
esp_err_t audio_test_noise_pink(void);

/**
 * @brief Brown noise generator demonstration (Plan 005 Step 5).
 *
 * Starts channel 3 with wave_type=AUDIO_WAVE_NOISE_BROWN, frequency=0 Hz
 * (noise channels bypass the frequency guard), volume=50% (amplitude=0.5),
 * for a 30 second duration.
 *
 * Brown noise (Brownian / red noise) is produced by a leaky one-pole integrator
 * of white noise (leak coefficient 0.998f, output gain 3.5f), producing a
 * -6 dB/octave spectrum.  It should sound distinctly deeper and more bass-heavy
 * than both white and pink noise — dominated by low-frequency rumble.
 *
 * Leak coefficient 0.998f: -3 dB point at ~0.03 Hz, preventing DC accumulation
 * while maintaining the -6 dB/octave spectral slope across the audible range.
 * Gain 3.5f: empirical normalization for approximately ±1.0 peak at 44.1 kHz.
 *
 * L and R channels use independent integrator state (brown_acc_l vs brown_acc_r)
 * to produce decorrelated stereo brown noise.
 *
 * Device-level verification: operator should hear the deepest of the three noise
 * types — dominated by low-frequency rumble, very little high-frequency content.
 * Wide stereo field.  No onset click (5 ms amp ramp masks integrator transient).
 *
 * @return ESP_OK when noise starts successfully, ESP_FAIL if channel start fails.
 */
esp_err_t audio_test_noise_brown(void);

#endif // AUDIO_TEST_H
