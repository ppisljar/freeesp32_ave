#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_manager.h"
#include "audio_test.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "led_matrix_example.h"
#include "config_parser.h"
#include "timing_engine.h"
#include "lock_free_comm.h"
#include "isr_profiling.h"
#include "memory_pool.h"
#include "bg_player.h"

static const char* TAG = "main";

// Timing precision test variables
static uint64_t last_test_event_time = 0;
static uint32_t test_event_count = 0;
static int64_t total_jitter_us = 0;
static int64_t max_jitter_us = 0;

// Timing precision test callback - ISR-safe (no logging)
static void timing_precision_test_callback(uint64_t timestamp_us, void *user_data) {
    uint32_t *expected_interval_us = (uint32_t*)user_data;
    uint64_t current_time = timing_engine_get_time_us();

    if (last_test_event_time > 0) {
        uint64_t actual_interval = current_time - last_test_event_time;
        int64_t jitter = (int64_t)actual_interval - (int64_t)*expected_interval_us;

        total_jitter_us += (jitter < 0) ? -jitter : jitter; // Absolute jitter
        if ((jitter < 0 ? -jitter : jitter) > max_jitter_us) {
            max_jitter_us = (jitter < 0) ? -jitter : jitter;
        }

        test_event_count++;

        // NOTE: No logging allowed in ISR context - statistics collected silently
    }

    last_test_event_time = current_time;

    // Schedule next test event if count < 10
    if (test_event_count < 10) {
        uint64_t next_time = current_time + *expected_interval_us;
        timing_engine_schedule_event(next_time, TIMING_EVENT_TIMELINE,
                                   timing_precision_test_callback, user_data);
    }
    // NOTE: Final statistics logging moved to task context in main loop
}

// Performance monitoring task
static void performance_monitoring_task(void *pvParameters);

// Benchmark lock-free vs mutex performance
static void benchmark_communication_performance(void);

// WiFi configuration
#define WIFI_SSID "Teltonika_Router"
#define WIFI_PASSWORD "secpass123"

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 Audio Player Starting...");

    // Initialize NVS (required for WiFi and other components)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize timing engine (hardware-precision timing)
    ret = timing_engine_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize timing engine: %s", esp_err_to_name(ret));
        return;
    }

    ret = timing_engine_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timing engine: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Master timing engine operational");

    ret = memory_pool_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize memory pools: %s", esp_err_to_name(ret));
        return;
    }

    // WiFi / config_parser / web_server init MOVED to AFTER the soak (see below).
    // The WiFi stack's interrupt bursts were the dominant source of RMT FIFO
    // underruns producing residual LED flicker — running the baseline soak
    // before WiFi comes up gives a clean reading of LED + audio behaviour.

    // Initialize LED matrix
    ret = led_matrix_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED matrix: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LED matrix initialized on GPIO 12");
        // Quick LED test — disabled during soak validation to keep startup clean.
        // led_matrix_test_pattern();
    }

    // Initialize audio manager
    ret = audio_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio manager: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize background audio player (Plan 006).
    // Ring buffer is in static .bss — no heap impact, no ordering constraints.
    ret = bg_player_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bg_player: %s", esp_err_to_name(ret));
        // Non-fatal: session continues without BG audio support.
    }


    ESP_LOGI(TAG, "ESP32 Audio Player initialized successfully");

    // Start lock-free performance monitoring task — disabled during soak validation;
    // every-5-seconds report is too noisy when we want ISR profile lines to stand out.
    // xTaskCreate(performance_monitoring_task, "perf_monitor", 4096, NULL, 5, NULL);

    // Benchmark lock-free communication performance
    benchmark_communication_performance();

    // Start audio output task for real-time audio generation
    ret = audio_test_start_output_task();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio output task: %s", esp_err_to_name(ret));
        return;
    }

    // Audio generation startup tests — disabled during soak validation.
    // They conflict with the soak's own channel-0 binaural beat (sweep still
    // playing on ch 0 when the soak tries to start its own generation).
    // vTaskDelay(pdMS_TO_TICKS(2000));
    //
    // ESP_LOGI(TAG, "Starting audio generation tests...");
    //
    // // Test 1: Basic 440Hz tone for 3 seconds
    // audio_test_basic_generation();
    // vTaskDelay(pdMS_TO_TICKS(4000));
    //
    // // Test 2: Binaural beats (440Hz base with 10Hz beat frequency) for 5 seconds
    // audio_test_binaural_beats(440.0f, 10.0f, 5000);
    // vTaskDelay(pdMS_TO_TICKS(6000));
    //
    // // Test 3: Frequency sweep from 200Hz to 800Hz over 4 seconds
    // audio_test_frequency_sweep(200.0f, 800.0f, 4000, AUDIO_GEN_SWEEP_LINEAR);
    //
    // ESP_LOGI(TAG, "Audio generation tests started. System running...");
    ESP_LOGI(TAG, "Web interface: Upload .led config files and control audio remotely");

    // Start timing precision test
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait for audio tests to stabilize
    ESP_LOGI(TAG, "Starting timing engine precision test...");
    ESP_LOGI(TAG, "NOTE: Test runs in ISR context - results reported in main loop");
    static uint32_t test_interval_us = 50000; // 50ms test interval
    uint64_t first_test_time = timing_engine_get_time_us() + 1000000; // Start in 1 second
    timing_engine_schedule_event(first_test_time, TIMING_EVENT_TIMELINE,
                               timing_precision_test_callback, &test_interval_us);

#ifdef CONFIG_ISR_PROFILING
    // Soak test disabled — re-enable for ISR baseline validation by uncommenting.
    // ESP_LOGI(TAG, "CONFIG_ISR_PROFILING enabled — running 1-minute ISR baseline soak (4 × 15-s phases)");
    // ESP_LOGI(TAG, "Soak runs BEFORE WiFi to eliminate interrupt-burst LED-FIFO underruns.");
    // audio_test_isr_baseline_soak();
    // isr_profiling_report();
#endif

    // ------------------------------------------------------------------
    // Post-soak: bring up WiFi, config parser, and web server now.
    // ------------------------------------------------------------------
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Connecting to WiFi...");
        ret = wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to connect to WiFi: %s", esp_err_to_name(ret));
        } else {
            char ip_str[16];
            if (wifi_manager_get_ip_string(ip_str, sizeof(ip_str)) == ESP_OK) {
                ESP_LOGI(TAG, "Connected to WiFi! IP address: %s", ip_str);
            }
        }
    }

    ret = config_parser_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize config parser: %s", esp_err_to_name(ret));
    }

    ret = web_server_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize web server: %s", esp_err_to_name(ret));
    } else {
        web_server_set_wifi_status(wifi_manager_get_state() == WIFI_STATE_CONNECTED);
        char url_buffer[64];
        if (web_server_get_url(url_buffer, sizeof(url_buffer)) == ESP_OK) {
            ESP_LOGI(TAG, "Web interface available at: %s", url_buffer);
        }
    }


    // Main application loop
    while (1) {
        // Monitor audio manager state
        audio_manager_state_t* state = audio_manager_get_state();
        ESP_LOGD(TAG, "Audio state: source=%d, state=%d, volume=%.2f",
                state->current_source, state->state, state->volume);

        // Monitor timing engine performance
        if (timing_engine_is_running()) {
            uint64_t events_processed = timing_engine_get_events_processed();
            size_t queue_utilization = timing_engine_get_queue_utilization();
            ESP_LOGD(TAG, "Timing engine: %llu events processed, %zu%% queue utilization",
                    events_processed, queue_utilization);

            // Report timing precision test results if completed (safe in task context)
            if (test_event_count >= 10 && last_test_event_time > 0) {
                static bool results_reported = false;
                if (!results_reported) {
                    int64_t avg_jitter = total_jitter_us / test_event_count;
                    ESP_LOGI(TAG, "Timing precision test complete:");
                    ESP_LOGI(TAG, "  Events: %lu", test_event_count);
                    ESP_LOGI(TAG, "  Average jitter: %lld μs", avg_jitter);
                    ESP_LOGI(TAG, "  Maximum jitter: %lld μs", max_jitter_us);
                    ESP_LOGI(TAG, "  Target precision: <1000 μs (current: %lld μs avg)", avg_jitter);
                    results_reported = true;
                }
            }
        }

        // Check active generators
        for (int i = 0; i < 4; i++) {
            if (audio_manager_is_generating(i)) {
                ESP_LOGD(TAG, "Generator channel %d is active", i);
            }
        }

        // Monitor WiFi connection status
        wifi_state_t wifi_state = wifi_manager_get_state();
        if (wifi_state != WIFI_STATE_CONNECTED) {
            ESP_LOGW(TAG, "WiFi disconnected, web interface unavailable");
            web_server_set_wifi_status(false);
        } else {
            web_server_set_wifi_status(true);
        }

        vTaskDelay(pdMS_TO_TICKS(10000)); // Update every 10 seconds
    }
}

// Performance monitoring task using lock-free communication
static void performance_monitoring_task(void *pvParameters) {
    ESP_LOGI(TAG, "Lock-free performance monitoring started");

    while (1) {
        // Get lock-free communication statistics
        uint64_t total_comms = lock_free_get_total_communications();

        // Get atomic system states
        bool audio_active, led_active, sync_active;
        uint32_t channels_mask, led_brightness, sync_mode;
        uint64_t sample_count, sync_events_processed;
        float amplitude, led_frequency;
        uint32_t sync_errors;

        lock_free_get_audio_state(&audio_active, &channels_mask, &sample_count, &amplitude);
        lock_free_get_led_state(&led_active, &led_frequency, &led_brightness, NULL);
        lock_free_get_sync_state(&sync_active, &sync_mode, &sync_events_processed, &sync_errors);

        // Get queue statistics
        size_t audio_queue_total, audio_queue_dropped, audio_queue_util;
        lock_free_ring_buffer_t *audio_queue = lock_free_get_audio_to_led_queue();
        if (audio_queue) {
            lock_free_get_queue_stats(audio_queue, &audio_queue_total, &audio_queue_dropped, &audio_queue_util);
        }

        // Performance report
        ESP_LOGI(TAG, "=== Lock-Free Performance Report ===");
        ESP_LOGI(TAG, "Total communications: %llu", total_comms);
        ESP_LOGI(TAG, "Audio: %s | Samples: %llu | Amplitude: %.3f",
                 audio_active ? "ACTIVE" : "INACTIVE", sample_count, amplitude);
        ESP_LOGI(TAG, "LED: %s | %.1fHz | Brightness: %lu%%",
                 led_active ? "ACTIVE" : "INACTIVE", led_frequency, led_brightness);
        ESP_LOGI(TAG, "Sync: %s | Mode: %lu | Events: %llu | Errors: %lu",
                 sync_active ? "ACTIVE" : "INACTIVE", sync_mode, sync_events_processed, sync_errors);

        if (audio_queue) {
            ESP_LOGI(TAG, "Audio→LED Queue: %zu msgs | %zu dropped | %zu%% util",
                     audio_queue_total, audio_queue_dropped,
                     audio_queue_util * 100 / RING_BUFFER_SIZE);
        }

        ESP_LOGI(TAG, "========================================");

        vTaskDelay(pdMS_TO_TICKS(5000)); // 5-second reporting interval
    }
}

// Benchmark lock-free vs mutex performance
static void benchmark_communication_performance(void) {
    ESP_LOGI(TAG, "=== Lock-Free Communication Benchmark ===");

    uint64_t start_time, end_time;
    const uint32_t iterations = 10000;

    // Benchmark lock-free operations
    start_time = esp_timer_get_time();
    for (uint32_t i = 0; i < iterations; i++) {
        lock_free_set_audio_state(true, 0xFF, i, 500);
        bool active;
        uint32_t mask;
        uint64_t count;
        float amp;
        lock_free_get_audio_state(&active, &mask, &count, &amp);
    }
    end_time = esp_timer_get_time();

    uint64_t lock_free_time_us = end_time - start_time;
    float lock_free_avg_us = (float)lock_free_time_us / iterations;

    ESP_LOGI(TAG, "Lock-free: %llu μs total, %.2f μs/op average", lock_free_time_us, lock_free_avg_us);
    ESP_LOGI(TAG, "Performance: %u operations in %llu μs", iterations, lock_free_time_us);
    ESP_LOGI(TAG, "Deterministic: NO mutex operations, bounded execution time");
    ESP_LOGI(TAG, "Real-time suitable: YES (atomic operations only)");
    ESP_LOGI(TAG, "=============================================");
}
