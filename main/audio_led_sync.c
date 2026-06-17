#include "audio_led_sync.h"
#include "led_matrix_example.h"
#include "timing_engine.h"
#include "audio_config.h"
#include "lock_free_comm.h"
#include "isr_profiling.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_dsp.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"   /* xQueueCreateWithCaps */
#include "esp_heap_caps.h"            /* MALLOC_CAP_SPIRAM */
#include <math.h>
#include <string.h>

static const char* TAG = "audio_led_sync";

// Global synchronization state
static audio_led_sync_state_t *g_sync_state = NULL;

// Analysis task buffers and state
// (ANALYSIS_BUFFER_SIZE removed with detect_beat_frequency in Layer 5)
#define AUDIO_ANALYSIS_QUEUE_SIZE 32

// Audio analysis task handle and queue
static TaskHandle_t audio_analysis_task_handle = NULL;
static volatile QueueHandle_t audio_sample_queue = NULL;
static volatile bool queue_ready = false;

// sample_count is in stereo PAIRS (max 256); samples[] holds 2×sample_count int16s.
typedef struct {
    int16_t samples[512];
    size_t sample_count;
    uint64_t timestamp_us;
    uint64_t total_samples_after_batch; // Snapshot of total_audio_samples_processed post-increment.
} audio_sample_msg_t;

// VU meter smoothing (task context)
static float vu_meter_value = 0.0f;
static float vu_attack_coeff = 0.0f;
static float vu_decay_coeff = 0.0f;
// NOTE: beat_frequency_history, beat_history_index, amplitude_buffer, fft_buffer,
// and buffer_index were removed in Layer 5 together with detect_beat_frequency()
// and SYNC_MODE_BEAT_FREQUENCY.

// Sample-accurate timing variables
static uint64_t last_i2s_callback_time = 0;
static uint64_t total_audio_samples_processed = 0;

// Task notification handle: audio_led_sync_start stores its own TaskHandle
// here before spawning the analysis task.  The analysis task signals it once
// the main loop is reached, replacing the former vTaskDelay(10ms) polling wait.
static TaskHandle_t s_sync_start_caller = NULL;

// Forward declarations
static void audio_analysis_task(void *pvParameters);
static void calculate_vu_meter(const float *audio_samples, size_t sample_count);
// detect_beat_frequency removed — SYNC_MODE_BEAT_FREQUENCY deleted in Layer 5.
static void update_led_from_sync_data(void);
static esp_err_t configure_vu_meter_timing(const vu_meter_config_t *config);
static esp_err_t start_audio_analysis_task(void);
static esp_err_t stop_audio_analysis_task(void);

esp_err_t audio_led_sync_init(void) {
    if (g_sync_state != NULL) {
        ESP_LOGW(TAG, "Audio-LED sync already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Allocate synchronization state
    g_sync_state = calloc(1, sizeof(audio_led_sync_state_t));
    if (!g_sync_state) {
        ESP_LOGE(TAG, "Failed to allocate sync state");
        return ESP_ERR_NO_MEM;
    }

    // Initialize lock-free communication system for real-time synchronization
    esp_err_t ret = lock_free_comm_init();
    if (ret != ESP_OK) {
        free(g_sync_state);
        g_sync_state = NULL;
        ESP_LOGE(TAG, "Failed to initialize lock-free communication: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize default VU meter configuration
    g_sync_state->vu_config = (vu_meter_config_t){
        .attack_time_ms = 5.0f,     // Quick response for meditation
        .decay_time_ms = 50.0f,     // Smooth decay for visual comfort
        .sensitivity = 0.5f,        // Medium sensitivity
        .brightness_min = 10,       // 10% minimum brightness
        .brightness_max = 100       // 100% maximum brightness
    };

    // Initialize default beat synchronization
    g_sync_state->beat_config = (beat_sync_config_t){
        .beat_frequency_hz = 20.0f, // Default 20Hz binaural beat
        .phase_offset_deg = 0.0f,   // No phase offset
        .tracking_bandwidth = 2.0f, // ±2Hz tracking bandwidth
        .auto_detect = true         // Auto-detect binaural beats
    };

    g_sync_state->mode = SYNC_MODE_DISABLED;
    g_sync_state->active = false;
    g_sync_state->i2s_handle = NULL;

    // Configure VU meter timing coefficients
    configure_vu_meter_timing(&g_sync_state->vu_config);

    // Create audio sample queue in PSRAM — this queue holds ~34 KB of
    // per-buffer audio samples (AUDIO_ANALYSIS_QUEUE_SIZE × sizeof(audio_sample_msg_t)).
    // Putting it in PSRAM frees that DRAM for FreeRTOS task stacks, the BG ring,
    // WiFi/lwIP buffers, and I2S DMA descriptors.  ISR access (xQueueSendFromISR)
    // is safe per ESP-IDF docs since this codebase does no flash writes from ISR.
    audio_sample_queue = xQueueCreateWithCaps(AUDIO_ANALYSIS_QUEUE_SIZE,
                                              sizeof(audio_sample_msg_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_sample_queue) {
        free(g_sync_state);
        g_sync_state = NULL;
        lock_free_comm_deinit();
        ESP_LOGE(TAG, "Failed to create audio sample queue");
        return ESP_ERR_NO_MEM;
    }

    // Mark queue as ready for ISR use (memory barrier ensures visibility)
    queue_ready = true;

    ESP_LOGI(TAG, "Audio-LED synchronization initialized");
    return ESP_OK;
}

esp_err_t audio_led_sync_start(audio_led_sync_mode_t mode) {
    if (!g_sync_state) {
        ESP_LOGE(TAG, "Sync not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Lock-free check and update of sync state
    if (g_sync_state->active) {
        ESP_LOGW(TAG, "Sync already active");
        return ESP_ERR_INVALID_STATE;
    }

    g_sync_state->mode = mode;
    g_sync_state->sample_count = 0;
    g_sync_state->led_update_counter = 0;
    g_sync_state->sync_errors = 0;
    g_sync_state->queue_errors = 0;

    // Reset VU meter state
    vu_meter_value = 0.0f;
    total_audio_samples_processed = 0;

    g_sync_state->active = true;

    // Store caller handle so the analysis task can signal us once its main
    // loop is reached — replaces the former vTaskDelay(10ms) approximation.
    s_sync_start_caller = xTaskGetCurrentTaskHandle();

    // Start audio analysis task BEFORE enabling ISR queue operations
    esp_err_t task_ret = start_audio_analysis_task();
    if (task_ret != ESP_OK) {
        s_sync_start_caller = NULL;
        g_sync_state->active = false;
        queue_ready = false;  // Ensure ISR doesn't use queue on failure
        ESP_LOGE(TAG, "Failed to start audio analysis task: %s", esp_err_to_name(task_ret));
        return task_ret;
    }

    // Wait for analysis task to signal it has reached its main loop (replaces
    // the former vTaskDelay(pdMS_TO_TICKS(10)) polling approximation).
    // 100 ms timeout is a safety net: if the task fails to signal, log a
    // warning but continue so callers are not broken.
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    if (notified == 0) {
        ESP_LOGW(TAG, "Analysis task did not signal ready within 100ms; continuing anyway");
    }
    s_sync_start_caller = NULL;

    // Initialize lock-free sync state
    lock_free_set_sync_state(true, mode, 0, 0);

    const char* mode_name =
        (mode == SYNC_MODE_VU_METER) ? "VU_METER" :
        (mode == SYNC_MODE_CUSTOM) ? "CUSTOM" : "DISABLED";

    ESP_LOGI(TAG, "Audio-LED sync started in %s mode (queue_ready=%s, task_handle=%p)",
             mode_name, queue_ready ? "true" : "false", audio_analysis_task_handle);
    return ESP_OK;
}

esp_err_t audio_led_sync_stop(void) {
    if (!g_sync_state) {
        return ESP_ERR_INVALID_STATE;
    }

    // Mark queue as not ready for ISR use FIRST to prevent new ISR queue operations
    queue_ready = false;

    // Stop audio analysis task
    stop_audio_analysis_task();

    // Lock-free update of sync state
    g_sync_state->active = false;
    g_sync_state->mode = SYNC_MODE_DISABLED;

    // Update lock-free sync state
    lock_free_set_sync_state(false, SYNC_MODE_DISABLED, 0, g_sync_state->sync_errors);

    ESP_LOGI(TAG, "Audio-LED sync stopped");
    return ESP_OK;
}

esp_err_t audio_led_sync_deinit(void) {
    if (!g_sync_state) {
        return ESP_ERR_INVALID_STATE;
    }

    // Stop if active
    if (g_sync_state->active) {
        audio_led_sync_stop();
    }

    // Clean up audio sample queue
    if (audio_sample_queue) {
        // Ensure ISR cannot use queue during deletion
        queue_ready = false;
        // Small delay to ensure any ongoing ISR completes
        vTaskDelay(pdMS_TO_TICKS(10));
        vQueueDeleteWithCaps(audio_sample_queue);
        audio_sample_queue = NULL;
    }

    // Clean up lock-free communication system
    esp_err_t ret = lock_free_comm_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to deinitialize lock-free communication: %s", esp_err_to_name(ret));
    }

    free(g_sync_state);
    g_sync_state = NULL;

    ESP_LOGI(TAG, "Audio-LED sync deinitialized");
    return ESP_OK;
}

esp_err_t audio_led_sync_register_i2s_callback(i2s_chan_handle_t i2s_handle) {
    if (!g_sync_state) {
        ESP_LOGE(TAG, "Sync not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!i2s_handle) {
        ESP_LOGE(TAG, "Invalid I2S handle");
        return ESP_ERR_INVALID_ARG;
    }

    // Store I2S handle for synchronization
    g_sync_state->i2s_handle = i2s_handle;

    // Register DMA callback for sample-accurate synchronization
    i2s_event_callbacks_t cbs = {
        .on_recv = NULL,
        .on_recv_q_ovf = NULL,
        .on_sent = audio_led_sync_i2s_callback,  // LED sync callback
        .on_send_q_ovf = NULL,
    };

    esp_err_t ret = i2s_channel_register_event_callback(i2s_handle, &cbs, g_sync_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register I2S callbacks: %s", esp_err_to_name(ret));
        g_sync_state->i2s_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "I2S callback registered for audio-LED synchronization");
    return ESP_OK;
}

// I2S DMA Callback - CRITICAL: Sample-accurate synchronization
bool IRAM_ATTR audio_led_sync_i2s_callback(i2s_chan_handle_t handle,
                                           i2s_event_data_t *event,
                                           void *user_ctx) {
    ISR_PROFILE_BEGIN(0);
    audio_led_sync_state_t *sync_state = (audio_led_sync_state_t*)user_ctx;

    if (!sync_state || !sync_state->active || !event) {
        ISR_PROFILE_END(0);
        return false; // Early exit for invalid parameters
    }

    uint64_t current_time = esp_timer_get_time();

    // Track timing between callbacks for performance monitoring
    if (last_i2s_callback_time > 0) {
        uint64_t interval = current_time - last_i2s_callback_time;
        // Expected interval derived from hardware config: bytes_per_callback / bytes_per_frame / sample_rate.
        // AUDIO_DMA_BUFFER_SIZE=1024 bytes, stereo int16 = 4 bytes/frame → 256 frames → ~5805µs at 44.1kHz.
        // ±50% window covers DMA buffer size variance and both 44.1/48kHz sample rates.
        #define EXPECTED_CALLBACK_US (AUDIO_DMA_BUFFER_SIZE / (AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8)) * 1000000ULL / AUDIO_SAMPLE_RATE)
        if (interval > EXPECTED_CALLBACK_US * 3 / 2 || interval < EXPECTED_CALLBACK_US / 2) {
            sync_state->sync_errors++;
        }
        #undef EXPECTED_CALLBACK_US
    }
    last_i2s_callback_time = current_time;

    // Update sample counter for precise timing reference
    size_t samples_processed = event->size / (2 * sizeof(int16_t)); // Stereo samples
    sync_state->sample_count += samples_processed;
    total_audio_samples_processed += samples_processed;

    // Send audio samples to analysis task (no floating-point operations in ISR)
    // ISR-safe: Check both queue handle and ready flag to prevent race conditions
    if (event->dma_buf && samples_processed > 0 && queue_ready && audio_sample_queue != NULL) {
        // DMA buffers are always in DRAM; catch misconfigurations in debug builds
#if !defined(NDEBUG)
        assert(esp_ptr_in_dram(event->dma_buf));
#endif

        // Prepare sample message for task
        audio_sample_msg_t sample_msg = {
            .sample_count = (samples_processed > 256) ? 256 : samples_processed,
            .timestamp_us = current_time,
            // Snapshot avoids 64-bit torn read in the task's phase-lock check.
            .total_samples_after_batch = total_audio_samples_processed
        };

        // Single memcpy replaces the per-sample loop; copy_count bounded by samples[512]
        size_t copy_count = sample_msg.sample_count * 2; // Stereo pairs, max 512
        memcpy(sample_msg.samples, event->dma_buf, copy_count * sizeof(int16_t));

        // Send to analysis task (non-blocking, ISR-safe)
        BaseType_t higher_priority_task_woken = pdFALSE;
        if (xQueueSendFromISR(audio_sample_queue, &sample_msg, &higher_priority_task_woken) != pdTRUE) {
            // Queue full — only error path for queue failures
            sync_state->queue_errors++;
        }

        // Yield to higher priority task if necessary
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }

        sync_state->led_update_counter++;
    }

    // Update lock-free sync state with performance data // ISR-OK: word "free" is in comment only, no malloc/free call
    lock_free_set_sync_state(sync_state->active, sync_state->mode,
                              sync_state->led_update_counter, sync_state->sync_errors);

    // Keep audio sample counter live regardless of VU mode; amplitude updated by calculate_vu_meter when active.
    lock_free_set_audio_state(true, 0xFF, total_audio_samples_processed, 0);

    ISR_PROFILE_END(0);
    return true; // Continue processing
}

// VU meter calculation - now safely executed in task context
static void calculate_vu_meter(const float *audio_samples, size_t sample_count) {
    if (!audio_samples || sample_count == 0) {
        return;
    }

    // Calculate RMS amplitude of current audio samples
    float rms = 0.0f;
    for (size_t i = 0; i < sample_count; i++) {
        rms += audio_samples[i] * audio_samples[i];
    }
    rms = sqrtf(rms / sample_count);

    // Apply VU meter smoothing with attack/decay
    if (rms > vu_meter_value) {
        // Attack phase - quick response to increases
        vu_meter_value += (rms - vu_meter_value) * vu_attack_coeff;
    } else {
        // Decay phase - slow response to decreases
        vu_meter_value += (rms - vu_meter_value) * vu_decay_coeff;
    }

    g_sync_state->current_amplitude = vu_meter_value;

    // Update lock-free audio state with current amplitude (pre-scale to milli-units in task context)
    lock_free_set_audio_state(true, 0xFF, total_audio_samples_processed,
                              (uint32_t)(vu_meter_value * 1000.0f));

    // Update LED brightness based on VU meter value
    float normalized_amplitude = vu_meter_value * g_sync_state->vu_config.sensitivity;
    if (normalized_amplitude > 1.0f) normalized_amplitude = 1.0f;

    // Scale to brightness range
    uint8_t brightness = (uint8_t)(g_sync_state->vu_config.brightness_min +
                                  normalized_amplitude * (g_sync_state->vu_config.brightness_max -
                                                        g_sync_state->vu_config.brightness_min));

    // Update LEDs with new brightness (non-blocking call)
    // Apply to ALL currently-active channels, not only channel 0.  The previous
    // implementation used the mask=0x01 trampoline which left channels 1-3
    // un-modulated — see bug_led_multichannel_state_2026-06-17.md (Bug B).
    uint8_t active_mask = led_matrix_get_active_mask();
    if (active_mask) {
        led_matrix_update_flicker_params_masked(active_mask,
                                                led_matrix_get_current_frequency(),
                                                50, brightness);
    }
}

// detect_beat_frequency() removed in Layer 5 cleanup: it was a hardcoded 20 Hz
// placeholder (no real FFT beat detection) called only from the
// SYNC_MODE_BEAT_FREQUENCY case that was also removed.  The beat_config struct
// and audio_led_sync_set_beat_sync_config() API are preserved for future use.

static void update_led_from_sync_data(void) {
    if (!g_sync_state || !g_sync_state->active) {
        return;
    }

    // Update LEDs based on current synchronization data — broadcast to every
    // active channel, not just channel 0 (Bug B fix).
    uint8_t active_mask = led_matrix_get_active_mask();
    if (active_mask) {
        float current_freq = led_matrix_get_current_frequency();
        uint8_t brightness = (uint8_t)(g_sync_state->vu_config.brightness_min +
                                      g_sync_state->current_amplitude *
                                      (g_sync_state->vu_config.brightness_max -
                                       g_sync_state->vu_config.brightness_min));

        led_matrix_update_flicker_params_masked(active_mask, current_freq, 50, brightness);
    }
}

static esp_err_t configure_vu_meter_timing(const vu_meter_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Calculate exponential smoothing coefficients for attack/decay
    // Sample rate assumed to be 44.1kHz, update rate ~86Hz (every 512 samples)
    float update_rate_hz = (float)AUDIO_SAMPLE_RATE / 512.0f; // Approximate update rate

    vu_attack_coeff = 1.0f - expf(-1.0f / (config->attack_time_ms * update_rate_hz / 1000.0f));
    vu_decay_coeff = 1.0f - expf(-1.0f / (config->decay_time_ms * update_rate_hz / 1000.0f));

    ESP_LOGD(TAG, "VU meter timing: attack_coeff=%.4f, decay_coeff=%.4f, update_rate=%.1fHz",
             vu_attack_coeff, vu_decay_coeff, update_rate_hz);

    return ESP_OK;
}

// Configuration API implementations
esp_err_t audio_led_sync_set_vu_meter_config(const vu_meter_config_t *config) {
    if (!g_sync_state || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Lock-free update of VU meter configuration
    g_sync_state->vu_config = *config;
    esp_err_t ret = configure_vu_meter_timing(config);

    ESP_LOGI(TAG, "Updated VU meter config: attack=%.1fms, decay=%.1fms, sensitivity=%.2f",
             config->attack_time_ms, config->decay_time_ms, config->sensitivity);

    return ret;
}

esp_err_t audio_led_sync_set_beat_sync_config(const beat_sync_config_t *config) {
    if (!g_sync_state || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Lock-free update of beat sync configuration
    g_sync_state->beat_config = *config;

    ESP_LOGI(TAG, "Updated beat sync config: frequency=%.1fHz, auto_detect=%s",
             config->beat_frequency_hz, config->auto_detect ? "enabled" : "disabled");

    return ESP_OK;
}

esp_err_t audio_led_sync_set_mode(audio_led_sync_mode_t mode) {
    if (!g_sync_state) {
        return ESP_ERR_INVALID_STATE;
    }

    // Lock-free update of sync mode
    g_sync_state->mode = mode;

    // Update lock-free sync state
    lock_free_set_sync_state(g_sync_state->active, mode, g_sync_state->led_update_counter, g_sync_state->sync_errors);

    const char* mode_name =
        (mode == SYNC_MODE_VU_METER) ? "VU_METER" :
        (mode == SYNC_MODE_CUSTOM) ? "CUSTOM" : "DISABLED";

    ESP_LOGI(TAG, "Sync mode changed to %s", mode_name);
    return ESP_OK;
}

// Status API implementations
bool audio_led_sync_is_active(void) {
    return g_sync_state && g_sync_state->active;
}

audio_led_sync_mode_t audio_led_sync_get_mode(void) {
    return g_sync_state ? g_sync_state->mode : SYNC_MODE_DISABLED;
}

float audio_led_sync_get_current_amplitude(void) {
    return g_sync_state ? g_sync_state->current_amplitude : 0.0f;
}

float audio_led_sync_get_beat_frequency(void) {
    return g_sync_state ? g_sync_state->beat_frequency_detected : 0.0f;
}

uint64_t audio_led_sync_get_sync_errors(void) {
    return g_sync_state ? g_sync_state->sync_errors : 0;
}

uint32_t audio_led_sync_get_queue_errors(void) {
    return g_sync_state ? g_sync_state->queue_errors : 0;
}

// Debug/diagnostic function to validate ISR queue safety
bool audio_led_sync_validate_queue_safety(void) {
    if (!g_sync_state) {
        ESP_LOGW(TAG, "Queue validation: sync not initialized");
        return false;
    }

    bool is_safe = (queue_ready && audio_sample_queue != NULL &&
                    audio_analysis_task_handle != NULL);

    ESP_LOGD(TAG, "Queue safety validation: queue_ready=%s, queue=%p, task=%p, result=%s",
             queue_ready ? "true" : "false",
             audio_sample_queue,
             audio_analysis_task_handle,
             is_safe ? "SAFE" : "UNSAFE");

    return is_safe;
}

// Audio analysis task - handles floating-point operations safely
static void audio_analysis_task(void *pvParameters) {
    audio_sample_msg_t sample_msg;
    float audio_samples[256];

    ESP_LOGI(TAG, "Audio analysis task started");

    // Signal audio_led_sync_start() that this task has reached its main loop.
    // s_sync_start_caller is set by the spawner before xTaskCreate and cleared
    // after the notification is received, so the window is narrow and safe.
    if (s_sync_start_caller != NULL) {
        xTaskNotifyGive(s_sync_start_caller);
    }

    while (1) {
        // Wait for audio samples from ISR (blocking with timeout)
        if (xQueueReceive(audio_sample_queue, &sample_msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!g_sync_state || !g_sync_state->active) {
                continue; // Skip processing if sync is disabled
            }

            // Convert I2S samples to float for analysis (safe in task context)
            size_t sample_count = sample_msg.sample_count;
            for (size_t i = 0; i < sample_count; i++) {
                // Mix left and right channels for analysis
                float left = (float)sample_msg.samples[i * 2] / 32768.0f;
                float right = (float)sample_msg.samples[i * 2 + 1] / 32768.0f;
                audio_samples[i] = (left + right) * 0.5f;
            }

            // Perform floating-point analysis based on sync mode
            switch (g_sync_state->mode) {
                case SYNC_MODE_VU_METER:
                    calculate_vu_meter(audio_samples, sample_count);
                    break;

                // SYNC_MODE_BEAT_FREQUENCY removed (Layer 5): was a hardcoded 20 Hz
                // placeholder with no real beat detection.  Superseded by structural
                // phase lock from Layer 3 (DMA lag + Q32 pre-advance).

                // SYNC_MODE_PHASE_LOCK removed (Layer 5): called
                // led_matrix_update_flicker_params_masked every 23 ms but never
                // adjusted cycle_start_time_us — no real phase locking.  Superseded
                // by Layer 3 structural phase lock.

                case SYNC_MODE_CUSTOM:
                    // Custom synchronization pattern
                    calculate_vu_meter(audio_samples, sample_count);
                    update_led_from_sync_data();
                    break;

                default:
                    break;
            }

            // Send lock-free message for LED updates if significant change detected
            static float last_amplitude = 0.0f;
            if (fabsf(g_sync_state->current_amplitude - last_amplitude) > 0.05f) { // 5% change threshold
                lock_free_message_t msg = {
                    .type = MSG_TYPE_SYNC_CONFIG,
                    .timestamp_us = sample_msg.timestamp_us,
                    .data_size = sizeof(float),
                };
                memcpy(msg.data, &g_sync_state->current_amplitude, sizeof(float));

                lock_free_ring_buffer_t *queue = lock_free_get_audio_to_led_queue();
                if (queue) {
                    lock_free_send_message(queue, &msg);
                }
                last_amplitude = g_sync_state->current_amplitude;
            }
        }

        // Check if task should exit
        if (!g_sync_state || !g_sync_state->active) {
            // Allow graceful shutdown when sync is stopped
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// Start the audio analysis task
static esp_err_t start_audio_analysis_task(void) {
    if (audio_analysis_task_handle) {
        ESP_LOGW(TAG, "Audio analysis task already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        audio_analysis_task,
        "audio_analysis",
        4096,                    // Stack size
        NULL,                    // Parameters
        5,                       // Priority (below ISR, above normal tasks)
        &audio_analysis_task_handle,
        1                        // Pin to core 1 (opposite of most other tasks)
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio analysis task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio analysis task created successfully");
    return ESP_OK;
}

// Stop the audio analysis task
static esp_err_t stop_audio_analysis_task(void) {
    if (audio_analysis_task_handle) {
        ESP_LOGD(TAG, "Stopping audio analysis task...");

        // Task will exit naturally when g_sync_state->active becomes false
        // Wait for it to finish gracefully
        vTaskDelay(pdMS_TO_TICKS(200));

        // Force delete if still running
        if (audio_analysis_task_handle) {
            ESP_LOGW(TAG, "Force deleting audio analysis task");
            vTaskDelete(audio_analysis_task_handle);
            audio_analysis_task_handle = NULL;
        }

        // Clear the queue to prevent stale data
        if (audio_sample_queue) {
            xQueueReset(audio_sample_queue);
        }

        ESP_LOGI(TAG, "Audio analysis task stopped");
    }

    return ESP_OK;
}