#include "audio_test.h"
#include "audio_manager.h"
#include "audio_generator.h"
#include "audio_config.h"
#include "led_matrix_example.h"
#include "audio_led_sync.h"
#include "isr_profiling.h"
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

    const size_t buffer_size = AUDIO_GEN_BUFFER_SIZE;
    const size_t stereo_samples = buffer_size;

    // Allocate audio buffer for stereo samples
    float* audio_buffer = malloc(stereo_samples * 2 * sizeof(float));
    int16_t* i2s_buffer = malloc(stereo_samples * 2 * sizeof(int16_t));

    if (!audio_buffer || !i2s_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        audio_output_running = false;
        vTaskDelete(NULL);
        return;
    }

    audio_output_running = true;
    size_t bytes_written = 0;

    while (audio_output_running) {
        // Clear audio buffer
        memset(audio_buffer, 0, stereo_samples * 2 * sizeof(float));

        // Generate audio samples from all active channels
        esp_err_t ret = audio_generator_fill_buffer(audio_buffer, stereo_samples);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Audio generation failed: %s", esp_err_to_name(ret));
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

        // Small delay to prevent task from consuming too much CPU
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    free(audio_buffer);
    free(i2s_buffer);
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

    BaseType_t result = xTaskCreate(
        audio_test_output_task,
        "audio_output",
        8192,                    // Stack size
        NULL,                    // Parameters
        5,                       // Priority
        &audio_output_task_handle
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

#define SOAK_PHASE_DURATION_MS  60000U   // 1 minute per phase
#define SOAK_REPORT_INTERVAL_MS 30000U   // print ISR stats every 30 s
#define SOAK_BINAURAL_BASE_HZ   200.0f
#define SOAK_BINAURAL_BEAT_HZ   40.0f
#define SOAK_BINAURAL_DUR_MS    (SOAK_PHASE_DURATION_MS * 4)

esp_err_t audio_test_isr_baseline_soak(void)
{
    ESP_LOGI(TAG, "=== ISR BASELINE SOAK START (4 min, 4 × 1-min phases) ===");
    ESP_LOGI(TAG, "OPERATOR: upload a .led file via the web interface at ~2:30 to exercise flash writes");

    // Start sustained binaural audio for the full 5-minute run.
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

    // Phase 1: 8 Hz flicker
    ESP_LOGI(TAG, "SOAK phase 1/4: 8 Hz LED flicker @ 10%% brightness (1 min)");
    led_matrix_start_flicker(8.0f, 50, 10);
    phase_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - phase_start) < pdMS_TO_TICKS(SOAK_PHASE_DURATION_MS)) {
        if (xTaskGetTickCount() >= report_deadline) {
            isr_profiling_report();
            report_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SOAK_REPORT_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Phase 2: 20 Hz flicker
    ESP_LOGI(TAG, "SOAK phase 2/4: 20 Hz LED flicker @ 10%% brightness (1 min)");
    led_matrix_update_flicker_params(20.0f, 50, 10);
    phase_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - phase_start) < pdMS_TO_TICKS(SOAK_PHASE_DURATION_MS)) {
        if (xTaskGetTickCount() >= report_deadline) {
            isr_profiling_report();
            report_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SOAK_REPORT_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Phase 3: VU-meter sync (1 min)
    ESP_LOGI(TAG, "SOAK phase 3/4: VU-meter sync mode (1 min)");
    led_matrix_stop_flicker();
    audio_led_sync_start(SYNC_MODE_VU_METER);
    phase_start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - phase_start) < pdMS_TO_TICKS(SOAK_PHASE_DURATION_MS)) {
        if (xTaskGetTickCount() >= report_deadline) {
            isr_profiling_report();
            report_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SOAK_REPORT_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    audio_led_sync_stop();

    // Phase 4: 8 Hz again + final stats
    ESP_LOGI(TAG, "SOAK phase 4/4: 8 Hz LED flicker @ 10%% brightness (1 min) + final report");
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
