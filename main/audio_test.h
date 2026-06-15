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

#endif // AUDIO_TEST_H
