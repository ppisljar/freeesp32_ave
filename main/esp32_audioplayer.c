#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "audio_manager.h"
#include "audio_generator.h"
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

// --- Debug snapshot button (GPIO 5) -------------------------------------
// Momentary push button wired between GPIO 5 and GND with the chip's
// internal pull-up keeping the line HIGH at rest. Falling edge fires the
// ISR which notifies snapshot_button_task; the task does the actual logging
// in task context so ESP_LOGI is safe and slow UART writes can't extend
// ISR latency.
//
// GPIO 5 is a strapping pin for the SDIO slave timing config — the
// internal pull-up holds it HIGH at boot so unpressed = normal boot. If
// the user happens to hold the button during reset they get a different
// SDIO timing latch; harmless for this project (no SDIO slave).
#define SNAPSHOT_BTN_GPIO        GPIO_NUM_5
#define SNAPSHOT_BTN_DEBOUNCE_US 200000ULL
#define SNAPSHOT_BTN_LOG_SIZE    64   // ring buffer of recent press timestamps

static TaskHandle_t s_snapshot_btn_task_handle = NULL;
static volatile uint64_t s_snapshot_btn_last_us = 0;

// Ring buffer of absolute esp_timer_get_time() values, one per debounced
// button press. /api/report filters these by session origin so only presses
// within the current session are returned.
static uint64_t s_btn_press_log[SNAPSHOT_BTN_LOG_SIZE] = {0};
static volatile uint32_t s_btn_press_count = 0;   // monotonic, total presses

// Snapshot the press log into caller buffer. Returns presses recorded since
// boot (capped at SNAPSHOT_BTN_LOG_SIZE most-recent). The caller is expected
// to filter against the current session origin.
size_t snapshot_button_get_presses(uint64_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    uint32_t total = s_btn_press_count;
    size_t n_in_log = (total < SNAPSHOT_BTN_LOG_SIZE) ? total : SNAPSHOT_BTN_LOG_SIZE;
    size_t n_copy = (n_in_log < max) ? n_in_log : max;
    // Copy in chronological order: oldest first.
    uint32_t start_idx = (total >= SNAPSHOT_BTN_LOG_SIZE)
                         ? (total - SNAPSHOT_BTN_LOG_SIZE) : 0;
    for (size_t i = 0; i < n_copy; i++) {
        out[i] = s_btn_press_log[(start_idx + i) % SNAPSHOT_BTN_LOG_SIZE];
    }
    return n_copy;
}

static void IRAM_ATTR snapshot_button_isr(void *arg)
{
    (void)arg;
    uint64_t now = esp_timer_get_time();
    if (now - s_snapshot_btn_last_us < SNAPSHOT_BTN_DEBOUNCE_US) return;
    s_snapshot_btn_last_us = now;

    // Record press in ring buffer (single producer = this ISR; reader uses
    // s_btn_press_count to find current head, so torn read of one slot is
    // bounded — worst case the reader gets stale or partial slot for one
    // press, never crashes).
    uint32_t idx = s_btn_press_count % SNAPSHOT_BTN_LOG_SIZE;
    s_btn_press_log[idx] = now;
    s_btn_press_count++;

    if (s_snapshot_btn_task_handle) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(s_snapshot_btn_task_handle, &hpw);
        if (hpw) portYIELD_FROM_ISR();
    }
}

static void snapshot_button_task(void *pv)
{
    (void)pv;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "=== BUTTON: live state snapshot ===");
        audio_generator_log_full_state();
        led_matrix_log_full_state();
        ESP_LOGI(TAG, "=== end snapshot ===");
    }
}

static esp_err_t snapshot_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SNAPSHOT_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) return ret;

    // ISR service may have been installed already by another component.
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) return isr_ret;

    return gpio_isr_handler_add(SNAPSHOT_BTN_GPIO, snapshot_button_isr, NULL);
}

// Periodic logger: every 15 s, dump current value + progress for every
// active audio and LED sweep. Stays silent when nothing is sweeping, so it
// adds no log noise outside of actual transitions.
static void sweep_progress_task(void *pv)
{
    (void)pv;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        int n_audio = audio_generator_log_sweep_progress();
        int n_led   = led_matrix_log_sweep_progress();
        if (n_audio + n_led > 0) {
            ESP_LOGI(TAG, "  (%d sweep%s in progress)",
                     n_audio + n_led, (n_audio + n_led) == 1 ? "" : "s");
        }
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

    // Start audio output task for real-time audio generation
    ret = audio_test_start_output_task();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio output task: %s", esp_err_to_name(ret));
        return;
    }

    // Periodic 15 s logger for active sweeps. Low priority — strictly diagnostic.
    // Disabled: button-driven snapshot (GPIO 5) is sufficient for normal use;
    // uncomment to re-enable continuous progress logging.
    // xTaskCreatePinnedToCore(sweep_progress_task, "sweep_log", 4096, NULL, 3, NULL, 0);

    // Push-button on GPIO 5: pressing it logs a snapshot of all active audio
    // and LED channels (current params and any sweep details).
    xTaskCreatePinnedToCore(snapshot_button_task, "snapshot_btn", 4096, NULL, 4,
                            &s_snapshot_btn_task_handle, 0);
    esp_err_t btn_ret = snapshot_button_init();
    if (btn_ret != ESP_OK) {
        ESP_LOGW(TAG, "Snapshot button on GPIO %d not available: %s",
                 SNAPSHOT_BTN_GPIO, esp_err_to_name(btn_ret));
    } else {
        ESP_LOGI(TAG, "Snapshot button ready on GPIO %d (press for live state log)",
                 SNAPSHOT_BTN_GPIO);
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

            // Timing precision test results — disabled. The scheduler has been
            // validated; the report adds noise on every boot. Uncomment to
            // re-enable a one-shot precision check.
            // if (test_event_count >= 10 && last_test_event_time > 0) {
            //     static bool results_reported = false;
            //     if (!results_reported) {
            //         int64_t avg_jitter = total_jitter_us / test_event_count;
            //         ESP_LOGI(TAG, "Timing precision test complete:");
            //         ESP_LOGI(TAG, "  Events: %lu", test_event_count);
            //         ESP_LOGI(TAG, "  Average jitter: %lld μs", avg_jitter);
            //         ESP_LOGI(TAG, "  Maximum jitter: %lld μs", max_jitter_us);
            //         ESP_LOGI(TAG, "  Target precision: <1000 μs (current: %lld μs avg)", avg_jitter);
            //         results_reported = true;
            //     }
            // }
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

