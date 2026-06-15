#include <stdio.h>
#include "esp_log.h"
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

static const char* TAG = "main";

// Timing precision test variables
static uint64_t last_test_event_time = 0;
static uint32_t test_event_count = 0;
static int64_t total_jitter_us = 0;
static int64_t max_jitter_us = 0;

// Timing precision test callback
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

        ESP_LOGI(TAG, "Timing test #%lu: jitter = %lld μs (expected %lu μs, actual %llu μs)",
                test_event_count, jitter, *expected_interval_us, actual_interval);
    }

    last_test_event_time = current_time;

    // Schedule next test event if count < 10
    if (test_event_count < 10) {
        uint64_t next_time = current_time + *expected_interval_us;
        timing_engine_schedule_event(next_time, TIMING_EVENT_TIMELINE,
                                   timing_precision_test_callback, user_data);
    } else {
        // Print final statistics
        int64_t avg_jitter = test_event_count > 0 ? total_jitter_us / test_event_count : 0;
        ESP_LOGI(TAG, "Timing precision test complete:");
        ESP_LOGI(TAG, "  Events: %lu", test_event_count);
        ESP_LOGI(TAG, "  Average jitter: %lld μs", avg_jitter);
        ESP_LOGI(TAG, "  Maximum jitter: %lld μs", max_jitter_us);
        ESP_LOGI(TAG, "  Target precision: <1000 μs (current: %lld μs avg)", avg_jitter);
    }
}

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

    // Initialize WiFi manager
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
        return;
    }

    // Connect to WiFi
    ESP_LOGI(TAG, "Connecting to WiFi...");
    ret = wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi: %s", esp_err_to_name(ret));
    } else {
        // Get and display IP address
        char ip_str[16];
        if (wifi_manager_get_ip_string(ip_str, sizeof(ip_str)) == ESP_OK) {
            ESP_LOGI(TAG, "Connected to WiFi! IP address: %s", ip_str);
        }
    }

    // Initialize config parser
    ret = config_parser_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize config parser: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize web server
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

    // Initialize LED matrix
    ret = led_matrix_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED matrix: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LED matrix initialized on GPIO 12");
        // Quick LED test
        led_matrix_test_pattern();
    }

    // Initialize audio manager
    ret = audio_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio manager: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "ESP32 Audio Player initialized successfully");

    // Start audio output task for real-time audio generation
    ret = audio_test_start_output_task();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio output task: %s", esp_err_to_name(ret));
        return;
    }

    // Test audio generation after a short delay
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Starting audio generation tests...");

    // Test 1: Basic 440Hz tone for 3 seconds
    audio_test_basic_generation();
    vTaskDelay(pdMS_TO_TICKS(4000));

    // Test 2: Binaural beats (440Hz base with 10Hz beat frequency) for 5 seconds
    audio_test_binaural_beats(440.0f, 10.0f, 5000);
    vTaskDelay(pdMS_TO_TICKS(6000));

    // Test 3: Frequency sweep from 200Hz to 800Hz over 4 seconds
    audio_test_frequency_sweep(200.0f, 800.0f, 4000, AUDIO_GEN_SWEEP_LINEAR);

    ESP_LOGI(TAG, "Audio generation tests started. System running...");
    ESP_LOGI(TAG, "Web interface: Upload .led config files and control audio remotely");

    // Start timing precision test
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait for audio tests to stabilize
    ESP_LOGI(TAG, "Starting timing engine precision test...");
    static uint32_t test_interval_us = 50000; // 50ms test interval
    uint64_t first_test_time = timing_engine_get_time_us() + 1000000; // Start in 1 second
    timing_engine_schedule_event(first_test_time, TIMING_EVENT_TIMELINE,
                               timing_precision_test_callback, &test_interval_us);

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
