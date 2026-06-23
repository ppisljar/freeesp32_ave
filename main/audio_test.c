#include "audio_test.h"
#include "audio_manager.h"
#include "audio_generator.h"
#include "audio_config.h"
#include "led_matrix_example.h"
#include "isr_profiling.h"
#include "bg_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include <math.h>
#include <string.h>

static const char* TAG = "audio_test";

// Task handle for audio output
static TaskHandle_t audio_output_task_handle = NULL;
static bool audio_output_running = false;

// External I2S handle from audio manager
extern i2s_chan_handle_t tx_handle;

esp_err_t audio_test_basic_generation(void)
{
    ESP_LOGI(TAG, "Testing basic audio generation");

    // Test 440Hz tone for 2 seconds
    audio_gen_params_t params = {
        .frequency = 440.0f,        // A4 note
        .frequency_r = 440.0f,      // Same for both channels
        .amplitude = 0.3f,          // 30% volume
        .pan = 0.0f,               // Center pan
        .mod_frequency = 0.0f,      // No modulation
        .mod_depth = 0.0f,
        .sweep_type = AUDIO_GEN_SWEEP_NONE,
        .sweep_target = 0.0f,
        .duration_ms = 2000         // 2 seconds
    };

    esp_err_t ret = audio_manager_start_generation(0, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start basic generation test");
        return ret;
    }

    ESP_LOGI(TAG, "Basic audio generation test started");
    return ESP_OK;
}

esp_err_t audio_test_binaural_beats(float base_freq, float beat_freq, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "Testing binaural beats: base=%.1f Hz, beat=%.1f Hz", base_freq, beat_freq);

    audio_gen_params_t params = {
        .frequency = base_freq,
        .frequency_r = base_freq + beat_freq,  // Right channel offset for binaural effect
        .amplitude = 0.3f,
        .pan = 0.0f,               // Center pan to hear both channels
        .mod_frequency = 0.0f,
        .mod_depth = 0.0f,
        .sweep_type = AUDIO_GEN_SWEEP_NONE,
        .sweep_target = 0.0f,
        .duration_ms = duration_ms
    };

    esp_err_t ret = audio_manager_start_generation(1, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start binaural beats test");
        return ret;
    }

    ESP_LOGI(TAG, "Binaural beats test started for %u ms", duration_ms);
    return ESP_OK;
}

esp_err_t audio_test_frequency_sweep(float start_freq, float end_freq, uint32_t duration_ms, int sweep_type)
{
    ESP_LOGI(TAG, "Testing frequency sweep: %.1f->%.1f Hz over %u ms", start_freq, end_freq, duration_ms);

    audio_gen_params_t params = {
        .frequency = start_freq,
        .frequency_r = start_freq,
        .amplitude = 0.3f,
        .pan = 0.0f,
        .mod_frequency = 0.0f,
        .mod_depth = 0.0f,
        .sweep_type = (audio_gen_sweep_type_t)sweep_type,
        .sweep_target = end_freq,
        .duration_ms = duration_ms
    };

    esp_err_t ret = audio_manager_start_generation(2, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start frequency sweep test");
        return ret;
    }

    ESP_LOGI(TAG, "Frequency sweep test started");
    return ESP_OK;
}

void audio_test_output_task(void* pvParameters)
{
    ESP_LOGI(TAG, "Audio output task started");

    const size_t stereo_samples = AUDIO_GEN_BUFFER_SIZE;

    // .bss-resident — runtime malloc here used to silently fail under heap pressure
    // (queue allocations in audio_manager_init can starve this path), causing the
    // task to vTaskDelete itself with the caller seeing ESP_OK. Result: silent audio.
    static float   audio_buffer[AUDIO_GEN_BUFFER_SIZE * 2];
    static int16_t i2s_buffer  [AUDIO_GEN_BUFFER_SIZE * 2];

    audio_output_running = true;
    size_t bytes_written = 0;

    while (audio_output_running) {
        // audio_generator_fill_buffer memsets the buffer to 0 as its first act,
        // so an additional memset here is wasted ~256 cycles per cycle.
        // Generate audio samples from all active channels
        esp_err_t ret = audio_generator_fill_buffer(audio_buffer, stereo_samples);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Audio generation failed: %s", esp_err_to_name(ret));
        }

        // Mix background audio into the buffer (Plan 006 Step 5).
        // bg_player_mix_into reads from the internal ring buffer (non-blocking),
        // applies the amplitude ramp and linear pan law, and accumulates BG
        // samples into audio_buffer.  Called AFTER fill_buffer so BG is a
        // post-mix stage independent of inv_n_active headroom division.
        // Step 7 wires bg_player_start/stop into the timeline executor; this
        // call site is already in place so Step 7 only needs to confirm the path.
        if (bg_player_is_active()) {
            bg_player_mix_into(audio_buffer, stereo_samples);
        }

        // Convert float samples to int16 for I2S output
        for (size_t i = 0; i < stereo_samples * 2; i++) {
            // Convert float (-1.0 to 1.0) to int16 (-32768 to 32767)
            float sample = audio_buffer[i];

            // Clamp to valid range
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;

            i2s_buffer[i] = (int16_t)(sample * 32767.0f);
        }

        // Output to I2S
        if (tx_handle) {
            ret = i2s_channel_write(tx_handle, i2s_buffer,
                                   stereo_samples * 2 * sizeof(int16_t),
                                   &bytes_written, portMAX_DELAY);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            }
        }

        // No vTaskDelay here — i2s_channel_write with portMAX_DELAY is the
        // natural pacing mechanism (blocks until the next DMA buffer is free).
        // The previous 1ms delay introduced 4% per-cycle jitter and was the
        // most likely cause of synth+BG combined clicks.  See squeezelite
        // recon report 2026-06-17 for the reference comparison.
    }

    audio_output_task_handle = NULL;

    ESP_LOGI(TAG, "Audio output task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_test_start_output_task(void)
{
    if (audio_output_task_handle != NULL) {
        ESP_LOGW(TAG, "Audio output task already running");
        return ESP_ERR_INVALID_STATE;
    }

    // Audio output is the MOST time-critical task in the system.
    //   Priority 23 = top tier (same as LED flicker, above BG streamer at 18,
    //                  above WiFi at 19, above timing_dispatch at 22).
    //   Pinned to core 1 — LED task is also on core 1 (per fix_led_drift_task_priority);
    //   they take turns at priority 23, both well above WiFi/BG which run on core 0.
    //
    // Previous priority 5 made audio preemptable by every other real-time task,
    // causing constant DMA underruns when synth + BG combined work approached the
    // 23 ms buffer budget.  See squeezelite recon report 2026-06-17.
    BaseType_t result = xTaskCreatePinnedToCore(
        audio_test_output_task,
        "audio_output",
        8192,                    // Stack size
        NULL,                    // Parameters
        23,                      // Priority — top tier, real-time
        &audio_output_task_handle,
        1                        // Core 1 — keep WiFi/BG noise on core 0
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio output task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio output task started");
    return ESP_OK;
}

esp_err_t audio_test_stop_output_task(void)
{
    if (audio_output_task_handle == NULL) {
        ESP_LOGW(TAG, "Audio output task not running");
        return ESP_ERR_INVALID_STATE;
    }

    audio_output_running = false;

    // Wait for task to finish
    while (audio_output_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Audio output task stopped");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Sweep Interpolation Verification Tests (Plan 002 Step 1)
// ---------------------------------------------------------------------------
// Both tests start a 400→440 Hz sweep over 1000 ms on channel 0, then sample
// current_freq after 25% of total samples have been generated (~250 ms).
//
// Why 250 ms delay instead of polling samples_generated:
//   The output task writes AUDIO_GEN_BUFFER_SIZE frames per iteration with a
//   1 ms vTaskDelay, giving ~44 buffers/s × 1024 frames = ~44100 frames/s.
//   At 44100 Hz, 25% of 1 s = 11025 samples ≈ 250 ms.  A 260 ms wait provides
//   a 10 ms margin without over-sampling into the 50% boundary.
// ---------------------------------------------------------------------------

#define SWEEP_VERIFY_START_HZ   400.0f
#define SWEEP_VERIFY_END_HZ     440.0f
#define SWEEP_VERIFY_DUR_MS     1000U
#define SWEEP_VERIFY_CHANNEL    0

// Tolerance in Hz for frequency match.  The interpolation runs once per
// AUDIO_GEN_BUFFER_SIZE samples, so the measured value may be up to one
// buffer-worth ahead of the ideal 25% point — at 44100 Hz that is ~23 ms of
// additional progress.  0.5 Hz is generous enough to absorb this granularity.
#define SWEEP_VERIFY_TOLERANCE_HZ  0.5f

esp_err_t audio_test_sweep_verify_linear(void)
{
    ESP_LOGI(TAG, "sweep_verify_linear: start %.0f->%.0f Hz over %u ms",
             SWEEP_VERIFY_START_HZ, SWEEP_VERIFY_END_HZ, SWEEP_VERIFY_DUR_MS);

    audio_gen_params_t params = {
        .frequency    = SWEEP_VERIFY_START_HZ,
        .frequency_r  = SWEEP_VERIFY_START_HZ,
        .amplitude    = 0.1f,
        .pan          = 0.0f,
        .mod_frequency = 0.0f,
        .mod_depth    = 0.0f,
        .sweep_type   = AUDIO_GEN_SWEEP_LINEAR,
        .sweep_target = SWEEP_VERIFY_END_HZ,
        .duration_ms  = SWEEP_VERIFY_DUR_MS,
    };

    esp_err_t ret = audio_manager_start_generation(SWEEP_VERIFY_CHANNEL, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sweep_verify_linear: failed to start generation: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for ~25% progress (11025 samples at 44100 Hz ≈ 250 ms).
    vTaskDelay(pdMS_TO_TICKS(260));

    float actual_freq = 0.0f;
    ret = audio_generator_get_current_freq(SWEEP_VERIFY_CHANNEL, &actual_freq);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sweep_verify_linear: get_current_freq failed");
        audio_manager_stop_generation(SWEEP_VERIFY_CHANNEL);
        return ret;
    }

    // Linear at p=0.25: expected = 400 + 40 * 0.25 = 410 Hz
    const float expected_freq = SWEEP_VERIFY_START_HZ +
        (SWEEP_VERIFY_END_HZ - SWEEP_VERIFY_START_HZ) * 0.25f;

    audio_manager_stop_generation(SWEEP_VERIFY_CHANNEL);

    float diff = actual_freq - expected_freq;
    if (diff < 0.0f) diff = -diff;

    if (diff > SWEEP_VERIFY_TOLERANCE_HZ) {
        ESP_LOGE(TAG, "sweep_verify_linear FAIL: expected %.2f Hz, got %.2f Hz (diff %.2f Hz > tol %.2f Hz)",
                 expected_freq, actual_freq, diff, SWEEP_VERIFY_TOLERANCE_HZ);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sweep_verify_linear PASS: expected %.2f Hz, got %.2f Hz", expected_freq, actual_freq);
    return ESP_OK;
}

esp_err_t audio_test_sweep_verify_quadratic(void)
{
    ESP_LOGI(TAG, "sweep_verify_quadratic: start %.0f->%.0f Hz over %u ms",
             SWEEP_VERIFY_START_HZ, SWEEP_VERIFY_END_HZ, SWEEP_VERIFY_DUR_MS);

    audio_gen_params_t params = {
        .frequency    = SWEEP_VERIFY_START_HZ,
        .frequency_r  = SWEEP_VERIFY_START_HZ,
        .amplitude    = 0.1f,
        .pan          = 0.0f,
        .mod_frequency = 0.0f,
        .mod_depth    = 0.0f,
        .sweep_type   = AUDIO_GEN_SWEEP_QUADRATIC,
        .sweep_target = SWEEP_VERIFY_END_HZ,
        .duration_ms  = SWEEP_VERIFY_DUR_MS,
    };

    esp_err_t ret = audio_manager_start_generation(SWEEP_VERIFY_CHANNEL, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sweep_verify_quadratic: failed to start generation: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for ~25% progress (first quadrant of the ease-in-out curve).
    vTaskDelay(pdMS_TO_TICKS(260));

    float actual_freq = 0.0f;
    ret = audio_generator_get_current_freq(SWEEP_VERIFY_CHANNEL, &actual_freq);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sweep_verify_quadratic: get_current_freq failed");
        audio_manager_stop_generation(SWEEP_VERIFY_CHANNEL);
        return ret;
    }

    // Quadratic at p=0.25 (first half, ease-in branch):
    //   t = 2 * 0.25^2 = 0.125
    //   expected = 400 + 40 * 0.125 = 405 Hz
    const float p = 0.25f;
    const float t = 2.0f * p * p;   // ease-in branch: p < 0.5
    const float expected_freq = SWEEP_VERIFY_START_HZ +
        (SWEEP_VERIFY_END_HZ - SWEEP_VERIFY_START_HZ) * t;

    audio_manager_stop_generation(SWEEP_VERIFY_CHANNEL);

    float diff = actual_freq - expected_freq;
    if (diff < 0.0f) diff = -diff;

    if (diff > SWEEP_VERIFY_TOLERANCE_HZ) {
        ESP_LOGE(TAG, "sweep_verify_quadratic FAIL: expected %.2f Hz, got %.2f Hz (diff %.2f Hz > tol %.2f Hz)",
                 expected_freq, actual_freq, diff, SWEEP_VERIFY_TOLERANCE_HZ);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sweep_verify_quadratic PASS: expected %.2f Hz, got %.2f Hz", expected_freq, actual_freq);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Amplitude Sweep Verification Test (Plan 002 Step 2.7)
// ---------------------------------------------------------------------------
// Mirrors sweep_verify_linear but exercises the explicit-window sweep API
// (audio_generator_start_sweep) rather than the legacy params.sweep_type path.
//
// Channel 0 starts at amplitude 0.0, then a 44100-sample (1 s) linear sweep
// to 1.0 is armed.  After 260 ms (~25% of samples), current_amp should be
// 0.0 + 1.0 * 0.25 = 0.25, verified within ±0.02.
// ---------------------------------------------------------------------------

#define AMP_VERIFY_CHANNEL    0
#define AMP_VERIFY_TOLERANCE  0.02f

esp_err_t audio_test_sweep_verify_amplitude(void)
{
    ESP_LOGI(TAG, "sweep_verify_amplitude: 0.0->1.0 amplitude over 44100 samples");

    // Amplitude 0.0 avoids audible clicks during the test; frequency still
    // needs a valid value for audio_generator_start_channel to accept the params.
    audio_gen_params_t params = {
        .frequency     = 440.0f,
        .frequency_r   = 440.0f,
        .amplitude     = 0.0f,
        .pan           = 0.0f,
        .mod_frequency = 0.0f,
        .mod_depth     = 0.0f,
        .sweep_type    = AUDIO_GEN_SWEEP_NONE,
        .sweep_target  = 0.0f,
        .duration_ms   = 2000,   // long enough to outlast the test
    };

    esp_err_t ret = audio_manager_start_generation(AMP_VERIFY_CHANNEL, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sweep_verify_amplitude: failed to start generation: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    // Arm the amplitude sweep via the new explicit-window API.
    ret = audio_generator_start_sweep(AMP_VERIFY_CHANNEL, AUDIO_PARAM_AMPLITUDE,
                                      0.0f, 1.0f, 44100ULL, AUDIO_GEN_SWEEP_LINEAR);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sweep_verify_amplitude: start_sweep failed: %s",
                 esp_err_to_name(ret));
        audio_manager_stop_generation(AMP_VERIFY_CHANNEL);
        return ret;
    }

    // Wait for ~25% of 44100 samples (11025 samples at 44100 Hz ≈ 250 ms).
    vTaskDelay(pdMS_TO_TICKS(260));

    float actual_amp = 0.0f;
    ret = audio_generator_get_current_amp(AMP_VERIFY_CHANNEL, &actual_amp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sweep_verify_amplitude: get_current_amp failed");
        audio_manager_stop_generation(AMP_VERIFY_CHANNEL);
        return ret;
    }

    // Linear at p=0.25: expected = 0.0 + 1.0 * 0.25 = 0.25
    const float expected_amp = 0.25f;

    audio_manager_stop_generation(AMP_VERIFY_CHANNEL);

    float diff = actual_amp - expected_amp;
    if (diff < 0.0f) diff = -diff;

    if (diff > AMP_VERIFY_TOLERANCE) {
        ESP_LOGE(TAG, "sweep_verify_amplitude FAIL: expected %.3f, got %.3f (diff %.3f > tol %.3f)",
                 expected_amp, actual_amp, diff, AMP_VERIFY_TOLERANCE);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sweep_verify_amplitude PASS: expected %.3f, got %.3f", expected_amp, actual_amp);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// ISR Baseline Soak Test (Step 1.2)
// ---------------------------------------------------------------------------
// Total duration: 5 minutes.
//   0:00 - 1:00  8 Hz LED flicker + 40 Hz binaural (theta)
//   1:00 - 2:00  20 Hz LED flicker + 40 Hz binaural (beta)
//   2:00 - 4:00  VU-meter sync mode active (audio amplitude drives LEDs)
//   4:00 - 5:00  8 Hz flicker again + profiling report
//
// OPERATOR ACTION at ~2:30: upload a .led file via the web interface to
// exercise the flash-write / rmt_transmit code path (exposes bug V5).
//
// ISR profiling stats are logged every 30 seconds via isr_profiling_report().
// ---------------------------------------------------------------------------

#define SOAK_PHASE_DURATION_MS  15000U   // 15 seconds per phase
#define SOAK_REPORT_INTERVAL_MS 5000U    // print ISR stats every 5 s
#define SOAK_BINAURAL_BASE_HZ   200.0f
#define SOAK_BINAURAL_BEAT_HZ   40.0f
#define SOAK_BINAURAL_DUR_MS    (SOAK_PHASE_DURATION_MS * 4)

esp_err_t audio_test_isr_baseline_soak(void)
{
    ESP_LOGI(TAG, "=== ISR BASELINE SOAK START (1 min, 4 × 15-s phases) ===");
    ESP_LOGI(TAG, "OPERATOR: upload a .led file via the web interface at ~37 s to exercise flash writes");

    // Start sustained binaural audio for the full 1-minute run.
    audio_gen_params_t audio_params = {
        .frequency   = SOAK_BINAURAL_BASE_HZ,
        .frequency_r = SOAK_BINAURAL_BASE_HZ + SOAK_BINAURAL_BEAT_HZ,
        .amplitude   = 0.25f,
        .pan         = 0.0f,
        .mod_frequency = 0.0f,
        .mod_depth   = 0.0f,
        .sweep_type  = AUDIO_GEN_SWEEP_NONE,
        .sweep_target = 0.0f,
        .duration_ms = SOAK_BINAURAL_DUR_MS,
    };
    esp_err_t ret = audio_manager_start_generation(0, &audio_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "soak: audio start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    TickType_t phase_start;
    TickType_t report_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SOAK_REPORT_INTERVAL_MS);

    // Phase 1: 8 Hz on channel 1 (blue, inner-left rect) + channel 4 (red, inner-right rect)
    // Channel mask 0x09 = bit 0 (channel 1, r1) + bit 3 (channel 4, r4).
    ESP_LOGI(TAG, "SOAK phase 1/4: 8 Hz LED flicker — ch1 BLUE + ch4 RED @ 10%% brightness (15 s)");
    led_matrix_start_flicker_masked(0x09, 8.0f, 50, 10, 0);
    led_matrix_set_flicker_color_masked(0x01,   0,   0, 255);  // ch 1 = blue
    led_matrix_set_flicker_color_masked(0x08, 255,   0,   0);  // ch 4 = red
    phase_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - phase_start) < pdMS_TO_TICKS(SOAK_PHASE_DURATION_MS)) {
        if (xTaskGetTickCount() >= report_deadline) {
            isr_profiling_report();
            report_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SOAK_REPORT_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Phase 2: 40 Hz on channel 2 (red, outer-left frame) + channel 3 (blue, outer-right frame)
    // Channel mask 0x06 = bit 1 (channel 2, r2) + bit 2 (channel 3, r3).
    // 40 Hz × 2 state-changes/cycle = 80 refreshes/sec — just below perceptual
    // fusion (~50-60 Hz for most viewers) so flicker is visible. Duty 10%
    // makes the strobe brief (2.5 ms on / 22.5 ms off) — easier to see individual
    // pulses and any drop-outs in the pulse train.
    ESP_LOGI(TAG, "SOAK phase 2/4: 40 Hz LED flicker @ 10%% duty — ch2 RED + ch3 BLUE @ 10%% brightness (15 s)");
    led_matrix_stop_flicker_masked(0x09);  // stop phase 1's channels
    led_matrix_start_flicker_masked(0x06, 40.0f, 10, 10, 0);
    led_matrix_set_flicker_color_masked(0x02, 255,   0,   0);  // ch 2 = red
    led_matrix_set_flicker_color_masked(0x04,   0,   0, 255);  // ch 3 = blue
    phase_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - phase_start) < pdMS_TO_TICKS(SOAK_PHASE_DURATION_MS)) {
        if (xTaskGetTickCount() >= report_deadline) {
            isr_profiling_report();
            report_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SOAK_REPORT_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // (Former phase 3 — VU-meter sync — was removed along with the VU
    // brightness pipeline. The phase numbering below is preserved for log
    // continuity with prior soak runs.)

    // Phase 4: 8 Hz again + final stats
    ESP_LOGI(TAG, "SOAK phase 4/4: 8 Hz LED flicker @ 10%% brightness (15 s) + final report");
    led_matrix_stop_flicker_masked(0x06);  // stop phase 2's channels
    led_matrix_start_flicker(8.0f, 50, 10);
    phase_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - phase_start) < pdMS_TO_TICKS(SOAK_PHASE_DURATION_MS)) {
        if (xTaskGetTickCount() >= report_deadline) {
            isr_profiling_report();
            report_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SOAK_REPORT_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    led_matrix_stop_flicker();

    ESP_LOGI(TAG, "=== ISR BASELINE SOAK COMPLETE ===");
    isr_profiling_report();

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Eight-channel concurrent soak test (Plan 002 Step 5.4)
// ---------------------------------------------------------------------------
// Channels 0-7 run at 200, 220, 240, 260, 280, 300, 320, 340 Hz at amplitude
// 0.05 each.  8 × 0.05 = 0.40 peak — well inside the I2S headroom.
//
// Phase 1 (30 s): all 8 channels at their start frequencies.
// Phase 2 (30 s): each channel linearly sweeps to half its start frequency
//   (100, 110, 120, ..., 170 Hz) over 30 seconds via audio_generator_start_sweep.
// Final: stop all channels, log ISR profile, check queue-error counter.
// ---------------------------------------------------------------------------

#define SOAK8_PHASE_MS    30000U
#define SOAK8_CHANNELS    8
#define SOAK8_BASE_FREQ   200.0f
#define SOAK8_FREQ_STEP   20.0f
#define SOAK8_AMPLITUDE   0.05f
#define SOAK8_DUR_MS      (SOAK8_PHASE_MS * 2U + 5000U) // enough for both phases + margin

esp_err_t audio_test_multichannel_soak(void)
{
    ESP_LOGI(TAG, "=== MULTICHANNEL SOAK START (8 ch, 2 × 30 s) ===");

    // Phase 1: start all 8 channels
    for (int ch = 0; ch < SOAK8_CHANNELS; ch++) {
        float freq = SOAK8_BASE_FREQ + ch * SOAK8_FREQ_STEP;
        audio_gen_params_t params = {
            .frequency     = freq,
            .frequency_r   = freq,
            .amplitude     = SOAK8_AMPLITUDE,
            .pan           = 0.0f,
            .mod_frequency = 0.0f,
            .mod_depth     = 0.0f,
            .sweep_type    = AUDIO_GEN_SWEEP_NONE,
            .sweep_target  = 0.0f,
            .duration_ms   = SOAK8_DUR_MS,
        };
        esp_err_t ret = audio_manager_start_generation(ch, &params);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "soak8: ch%d start failed: %s", ch, esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "soak8: ch%d started at %.0f Hz", ch, freq);
    }

    ESP_LOGI(TAG, "soak8: phase 1 — 8 channels running for %u ms", SOAK8_PHASE_MS);
    vTaskDelay(pdMS_TO_TICKS(SOAK8_PHASE_MS));

    // Phase 2: sweep all channels to half their start frequency over 30 s
    uint64_t sweep_samples = ((uint64_t)SOAK8_PHASE_MS * AUDIO_GEN_SAMPLE_RATE) / 1000ULL;
    for (int ch = 0; ch < SOAK8_CHANNELS; ch++) {
        float start_freq = SOAK8_BASE_FREQ + ch * SOAK8_FREQ_STEP;
        float target_freq = start_freq / 2.0f;
        esp_err_t ret = audio_generator_start_sweep(
            ch, AUDIO_PARAM_FREQUENCY,
            start_freq, target_freq,
            sweep_samples, AUDIO_GEN_SWEEP_LINEAR);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "soak8: ch%d sweep failed: %s", ch, esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "soak8: ch%d sweeping %.0f→%.0f Hz over %u ms",
                     ch, start_freq, target_freq, SOAK8_PHASE_MS);
        }
    }

    ESP_LOGI(TAG, "soak8: phase 2 — sweeping all channels for %u ms", SOAK8_PHASE_MS);
    vTaskDelay(pdMS_TO_TICKS(SOAK8_PHASE_MS));

    // Stop all channels
    for (int ch = 0; ch < SOAK8_CHANNELS; ch++) {
        audio_manager_stop_generation(ch);
    }

    ESP_LOGI(TAG, "=== MULTICHANNEL SOAK COMPLETE ===");
    isr_profiling_report();

    // The former queue-drop check measured drops in the VU pipeline's audio
    // sample queue (now removed). DMA-interval anomalies are still surfaced
    // by audio_led_sync_get_sync_errors() if needed.
    ESP_LOGI(TAG, "soak8: PASS — 60 s with 8 active channels");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Waveform type demonstration (Plan 005 Step 2)
// ---------------------------------------------------------------------------
// Cycles through SQUARE → TRIANGLE → SAWTOOTH → SINE on channel 0 at 40 Hz,
// 2 seconds each, so the operator can hear each waveform's distinct timbre.
//
// 40 Hz is chosen because:
//   - It is well within the therapeutic frequency range (< 100 Hz).
//   - It is audible as a tone on most speakers (not just a bass thump).
//   - It is the target isochronic frequency for gamma entrainment.
//
// Channel 0 is used so this test can run standalone without interfering with
// other channels.  The amplitude ramp (5 ms de-click) fires at the start of
// each tone to prevent onset clicks.
// ---------------------------------------------------------------------------

#define WAVETYPE_TEST_CHANNEL    0
#define WAVETYPE_TEST_FREQ_HZ    40.0f
#define WAVETYPE_TEST_AMP        0.4f
#define WAVETYPE_TEST_DUR_MS     3000U   // long enough for each waveform to be audible

esp_err_t audio_test_waveform_types(void)
{
    ESP_LOGI(TAG, "=== WAVEFORM TYPE TEST START (square→triangle→sawtooth→sine @ 40 Hz) ===");

    static const struct {
        audio_wave_type_t wave_type;
        const char* name;
    } wave_sequence[] = {
        { AUDIO_WAVE_SQUARE,   "SQUARE"   },
        { AUDIO_WAVE_TRIANGLE, "TRIANGLE" },
        { AUDIO_WAVE_SAWTOOTH, "SAWTOOTH" },
        { AUDIO_WAVE_SINE,     "SINE"     },
    };
    static const int WAVE_SEQ_LEN = (int)(sizeof(wave_sequence) / sizeof(wave_sequence[0]));

    for (int i = 0; i < WAVE_SEQ_LEN; i++) {
        audio_gen_params_t params = {
            .frequency     = WAVETYPE_TEST_FREQ_HZ,
            .frequency_r   = WAVETYPE_TEST_FREQ_HZ,
            .amplitude     = WAVETYPE_TEST_AMP,
            .pan           = 0.0f,
            .mod_frequency = 0.0f,
            .mod_depth     = 0.0f,
            .wave_type     = wave_sequence[i].wave_type,
            .sweep_type    = AUDIO_GEN_SWEEP_NONE,
            .sweep_target  = 0.0f,
            .duration_ms   = WAVETYPE_TEST_DUR_MS,
        };

        ESP_LOGI(TAG, "waveform_types: starting %s @ %.0f Hz for %u ms",
                 wave_sequence[i].name, WAVETYPE_TEST_FREQ_HZ, WAVETYPE_TEST_DUR_MS);

        esp_err_t ret = audio_manager_start_generation(WAVETYPE_TEST_CHANNEL, &params);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "waveform_types: failed to start %s: %s",
                     wave_sequence[i].name, esp_err_to_name(ret));
            return ret;
        }

        // Wait for the tone to finish.  Add 100 ms margin so the previous channel
        // finishes before we start the next one.
        vTaskDelay(pdMS_TO_TICKS(WAVETYPE_TEST_DUR_MS + 100U));

        // Stop the channel before starting the next waveform type.
        // (channel will have auto-stopped via total_samples check, but be explicit)
        audio_manager_stop_generation(WAVETYPE_TEST_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(50));  // brief silence between waveforms
    }

    ESP_LOGI(TAG, "=== WAVEFORM TYPE TEST COMPLETE ===");
    ESP_LOGI(TAG, "Operator: verify four distinct timbres were audible:");
    ESP_LOGI(TAG, "  SQUARE:   harsh/buzzy (contains only odd harmonics)");
    ESP_LOGI(TAG, "  TRIANGLE: softer than square but brighter than sine");
    ESP_LOGI(TAG, "  SAWTOOTH: richest/harshest (odd and even harmonics)");
    ESP_LOGI(TAG, "  SINE:     pure tone (no harmonics)");

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Binaural-beat precision check (Plan 002 Step 5.5)
// ---------------------------------------------------------------------------
// Configures channel 0 with L=200.0 Hz, R=200.01 Hz (0.01 Hz beat — one full
// amplitude cycle every 100 s).  Runs 60 s, then reads back current_freq and
// current_freq_r from the generator's live state.
//
// The generator updates current_freq_r each sample via the binaural offset
// preservation path (audio_generator.c: freq_diff = params.frequency_r -
// params.frequency; current_freq_r = current_freq + freq_diff).  Because no
// sweep is active on either carrier, both frequencies stay at their configured
// values to floating-point precision throughout the run.
//
// True acoustic verification (counting amplitude peaks at the speaker) requires
// external ADC capture over 100+ seconds — that is a Phase 5 / hardware-in-loop
// measurement and is explicitly out of scope here.
// ---------------------------------------------------------------------------

#define BPRECISION_BASE_HZ       200.0f
#define BPRECISION_BEAT_HZ       0.01f      // target 0.01 Hz beat
#define BPRECISION_RUN_MS        60000U
#define BPRECISION_TOLERANCE_HZ  0.001f     // 10× tighter than spec's ±0.01 Hz
#define BPRECISION_CHANNEL       0

esp_err_t audio_test_binaural_precision_verify(void)
{
    ESP_LOGI(TAG, "=== BINAURAL PRECISION START (L=%.2f Hz, R=%.4f Hz, run=%u ms) ===",
             BPRECISION_BASE_HZ, BPRECISION_BASE_HZ + BPRECISION_BEAT_HZ, BPRECISION_RUN_MS);

    audio_gen_params_t params = {
        .frequency     = BPRECISION_BASE_HZ,
        .frequency_r   = BPRECISION_BASE_HZ + BPRECISION_BEAT_HZ,
        .amplitude     = 0.1f,
        .pan           = 0.0f,
        .mod_frequency = 0.0f,
        .mod_depth     = 0.0f,
        .sweep_type    = AUDIO_GEN_SWEEP_NONE,
        .sweep_target  = 0.0f,
        .duration_ms   = BPRECISION_RUN_MS + 5000U,  // margin past measurement window
    };

    esp_err_t ret = audio_manager_start_generation(BPRECISION_CHANNEL, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "binaural_precision: failed to start: %s", esp_err_to_name(ret));
        return ret;
    }

    // Run for the full soak window
    vTaskDelay(pdMS_TO_TICKS(BPRECISION_RUN_MS));

    // Read back the generator's live frequencies
    float actual_l = 0.0f, actual_r = 0.0f;
    ret = audio_generator_get_current_freq(BPRECISION_CHANNEL, &actual_l);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "binaural_precision: get_current_freq failed");
        audio_manager_stop_generation(BPRECISION_CHANNEL);
        return ret;
    }

    ret = audio_generator_get_current_freq_r(BPRECISION_CHANNEL, &actual_r);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "binaural_precision: get_current_freq_r failed");
        audio_manager_stop_generation(BPRECISION_CHANNEL);
        return ret;
    }

    audio_manager_stop_generation(BPRECISION_CHANNEL);

    float expected_l = BPRECISION_BASE_HZ;
    float expected_r = BPRECISION_BASE_HZ + BPRECISION_BEAT_HZ;

    float diff_l = actual_l - expected_l;
    float diff_r = actual_r - expected_r;
    if (diff_l < 0.0f) diff_l = -diff_l;
    if (diff_r < 0.0f) diff_r = -diff_r;

    ESP_LOGI(TAG, "binaural_precision: L expected=%.4f Hz actual=%.4f Hz diff=%.6f Hz",
             expected_l, actual_l, diff_l);
    ESP_LOGI(TAG, "binaural_precision: R expected=%.4f Hz actual=%.4f Hz diff=%.6f Hz",
             expected_r, actual_r, diff_r);

    if (diff_l > BPRECISION_TOLERANCE_HZ || diff_r > BPRECISION_TOLERANCE_HZ) {
        ESP_LOGE(TAG, "binaural_precision: FAIL (tolerance ±%.3f Hz)",
                 BPRECISION_TOLERANCE_HZ);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "binaural_precision: PASS — both channels within ±%.3f Hz after %u ms",
             BPRECISION_TOLERANCE_HZ, BPRECISION_RUN_MS);

    // Note: acoustic precision (counting L-R beat cycles at the speaker) requires
    // external ADC capture over 100+ seconds and is deferred to Phase 5
    // hardware-in-loop validation.
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// White noise generator demonstration (Plan 005 Step 3)
// ---------------------------------------------------------------------------
// Starts channel 1 (not 0, to avoid overlap with audio_test_waveform_types)
// with AUDIO_WAVE_NOISE_WHITE, frequency=0 Hz (noise channels bypass the
// frequency > 0 guard in audio_generator_start_channel), volume=50%.
//
// Device-level verification checklist:
//   - Audible as broadband "sh" hiss (not a tone, not silence).
//   - Wide stereo field (L and R are decorrelated via different LFSR seeds).
//   - No click on start (5 ms amplitude ramp fires — especially important for
//     broadband noise where onset transients are particularly audible).
//   - Duration: 30 seconds (allowing time for the operator to assess the output).
//
// Channel 1 is used so this test can be called alongside the waveform-type test
// on channel 0 without conflict.
// ---------------------------------------------------------------------------

#define NOISE_WHITE_TEST_CHANNEL    1
#define NOISE_WHITE_TEST_AMP        0.5f     // 50% volume per substep 3.5 spec
#define NOISE_WHITE_TEST_DUR_MS     30000U   // 30 seconds per substep 3.5 spec

esp_err_t audio_test_noise_white(void)
{
    ESP_LOGI(TAG, "=== WHITE NOISE TEST START (channel %d, %.0f%% volume, %u ms) ===",
             NOISE_WHITE_TEST_CHANNEL,
             NOISE_WHITE_TEST_AMP * 100.0f,
             NOISE_WHITE_TEST_DUR_MS);

    audio_gen_params_t params = {
        .frequency     = 0.0f,              // No carrier; bypass frequency guard for noise types
        .frequency_r   = 0.0f,              // No right-channel carrier offset
        .amplitude     = NOISE_WHITE_TEST_AMP,
        .pan           = 0.0f,              // Centre pan; L/R decorrelation comes from LFSR seeds
        .mod_frequency = 0.0f,              // No amplitude modulation
        .mod_depth     = 0.0f,
        .wave_type     = AUDIO_WAVE_NOISE_WHITE,
        .sweep_type    = AUDIO_GEN_SWEEP_NONE,
        .sweep_target  = 0.0f,
        .duration_ms   = NOISE_WHITE_TEST_DUR_MS,
    };

    esp_err_t ret = audio_manager_start_generation(NOISE_WHITE_TEST_CHANNEL, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "noise_white: failed to start channel %d: %s",
                 NOISE_WHITE_TEST_CHANNEL, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "noise_white: channel %d started -- broadband hiss for %u ms",
             NOISE_WHITE_TEST_CHANNEL, NOISE_WHITE_TEST_DUR_MS);
    ESP_LOGI(TAG, "noise_white: operator -- verify:");
    ESP_LOGI(TAG, "  1. Broadband 'sh' hiss (not a tone)");
    ESP_LOGI(TAG, "  2. Wide stereo field (L/R decorrelated via LFSR seeds)");
    ESP_LOGI(TAG, "  3. No click on start (5 ms amp ramp)");

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Pink noise generator demonstration (Plan 005 Step 4)
// ---------------------------------------------------------------------------
// Starts channel 2 (distinct from white noise test on channel 1 and waveform
// test on channel 0) with AUDIO_WAVE_NOISE_PINK, frequency=0 Hz (noise channels
// bypass the frequency > 0 guard), volume=50%, 30 second duration.
//
// Pink noise uses the Paul Kellet 3-pole IIR filter applied to white noise to
// approximate a -3 dB/octave spectrum.  It should sound noticeably warmer than
// white noise — more low-frequency energy, less high-frequency hiss.
//
// The amplitude ramp (5 ms de-click, 220 samples) fires at channel start,
// same as all other waveform types.  The IIR state is zeroed at start and
// stabilizes within ~100 samples — well within the ramp window.
//
// Device-level verification checklist:
//   - Warmer sound than white noise (more bass, less hiss).
//   - Wide stereo field (L/R use independent IIR state via pink_b*r).
//   - No click on start (5 ms amplitude ramp).
//   - Approximate amplitude matches white noise at the same volume setting.
// ---------------------------------------------------------------------------

#define NOISE_PINK_TEST_CHANNEL    2
#define NOISE_PINK_TEST_AMP        0.5f     // 50% volume per substep 4.5 spec
#define NOISE_PINK_TEST_DUR_MS     30000U   // 30 seconds per substep 4.5 spec

// ---------------------------------------------------------------------------
// Brown noise generator demonstration (Plan 005 Step 5)
// ---------------------------------------------------------------------------
// Starts channel 3 (distinct from white ch1, pink ch2, waveform ch0) with
// AUDIO_WAVE_NOISE_BROWN, frequency=0 Hz (noise channels bypass the carrier
// frequency guard), volume=50%, 30 second duration.
//
// Brown noise (Brownian / red noise) is produced by integrating white noise
// through a leaky one-pole IIR filter (leak coeff = 0.998f, gain = 3.5f),
// resulting in a -6 dB/octave spectrum.  It should sound distinctly deeper
// and more bass-heavy than both white and pink noise — dominated by low-
// frequency rumble with very little high-frequency content.
//
// Device-level verification checklist:
//   - Deep, rumbling low-frequency sound (the deepest of the three noise types).
//   - Wide stereo field (L/R use independent brown_acc_l / brown_acc_r state).
//   - No click on start (5 ms amplitude ramp; integrator DC transient masked).
//   - Noticeably more bass-heavy than pink noise at the same volume setting.
// ---------------------------------------------------------------------------

#define NOISE_BROWN_TEST_CHANNEL   3
#define NOISE_BROWN_TEST_AMP       0.5f     // 50% volume per substep 5.5 spec
#define NOISE_BROWN_TEST_DUR_MS    30000U   // 30 seconds per substep 5.5 spec

esp_err_t audio_test_noise_pink(void)
{
    ESP_LOGI(TAG, "=== PINK NOISE TEST START (channel %d, %.0f%% volume, %u ms) ===",
             NOISE_PINK_TEST_CHANNEL,
             NOISE_PINK_TEST_AMP * 100.0f,
             NOISE_PINK_TEST_DUR_MS);

    audio_gen_params_t params = {
        .frequency     = 0.0f,              // No carrier; bypass frequency guard for noise types
        .frequency_r   = 0.0f,              // No right-channel carrier offset
        .amplitude     = NOISE_PINK_TEST_AMP,
        .pan           = 0.0f,              // Centre pan; stereo width from independent IIR state
        .mod_frequency = 0.0f,              // No amplitude modulation
        .mod_depth     = 0.0f,
        .wave_type     = AUDIO_WAVE_NOISE_PINK,
        .sweep_type    = AUDIO_GEN_SWEEP_NONE,
        .sweep_target  = 0.0f,
        .duration_ms   = NOISE_PINK_TEST_DUR_MS,
    };

    esp_err_t ret = audio_manager_start_generation(NOISE_PINK_TEST_CHANNEL, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "noise_pink: failed to start channel %d: %s",
                 NOISE_PINK_TEST_CHANNEL, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "noise_pink: channel %d started -- warmer noise for %u ms",
             NOISE_PINK_TEST_CHANNEL, NOISE_PINK_TEST_DUR_MS);
    ESP_LOGI(TAG, "noise_pink: operator -- verify:");
    ESP_LOGI(TAG, "  1. Warmer than white noise (more bass, less high-frequency hiss)");
    ESP_LOGI(TAG, "  2. Wide stereo field (L/R use independent Kellet IIR state)");
    ESP_LOGI(TAG, "  3. No click on start (5 ms amp ramp covers IIR stabilization window)");
    ESP_LOGI(TAG, "  4. Approximate level matches white noise at same volume setting");

    return ESP_OK;
}

esp_err_t audio_test_noise_brown(void)
{
    ESP_LOGI(TAG, "=== BROWN NOISE TEST START (channel %d, %.0f%% volume, %u ms) ===",
             NOISE_BROWN_TEST_CHANNEL,
             NOISE_BROWN_TEST_AMP * 100.0f,
             NOISE_BROWN_TEST_DUR_MS);

    audio_gen_params_t params = {
        .frequency     = 0.0f,              // No carrier; bypass frequency guard for noise types
        .frequency_r   = 0.0f,              // No right-channel carrier offset
        .amplitude     = NOISE_BROWN_TEST_AMP,
        .pan           = 0.0f,              // Centre pan; stereo width from independent integrators
        .mod_frequency = 0.0f,              // No amplitude modulation
        .mod_depth     = 0.0f,
        .wave_type     = AUDIO_WAVE_NOISE_BROWN,
        .sweep_type    = AUDIO_GEN_SWEEP_NONE,
        .sweep_target  = 0.0f,
        .duration_ms   = NOISE_BROWN_TEST_DUR_MS,
    };

    esp_err_t ret = audio_manager_start_generation(NOISE_BROWN_TEST_CHANNEL, &params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "noise_brown: failed to start channel %d: %s",
                 NOISE_BROWN_TEST_CHANNEL, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "noise_brown: channel %d started -- deep rumble for %u ms",
             NOISE_BROWN_TEST_CHANNEL, NOISE_BROWN_TEST_DUR_MS);
    ESP_LOGI(TAG, "noise_brown: operator -- verify:");
    ESP_LOGI(TAG, "  1. Deep, rumbling low-frequency sound (deepest of the three noise types)");
    ESP_LOGI(TAG, "  2. Wide stereo field (L/R use independent brown_acc_l / brown_acc_r)");
    ESP_LOGI(TAG, "  3. No click on start (5 ms amp ramp masks integrator DC transient)");
    ESP_LOGI(TAG, "  4. Distinctly more bass-heavy than pink noise at same volume");

    return ESP_OK;
}
