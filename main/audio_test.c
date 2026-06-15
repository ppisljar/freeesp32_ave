#include "audio_test.h"
#include "audio_manager.h"
#include "audio_generator.h"
#include "audio_config.h"
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
