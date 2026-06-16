#include "config_parser.h"
#include "audio_manager.h"
#include "audio_led_sync.h"
#include "led_strip.h"
#include "led_matrix_example.h"
#include "audio_config.h"
#include "timing_engine.h"
#include "lock_free_comm.h"
#include "memory_pool.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char* TAG = "config_parser";

// Timeline execution state
static config_timeline_t *current_timeline = NULL;
static config_timeline_t persistent_timeline = {0}; // Persistent copy for execution
static TaskHandle_t timeline_task_handle = NULL;
static SemaphoreHandle_t timeline_mutex = NULL;
static uint32_t timeline_start_time = 0;
static size_t current_entry_index = 0;
static bool timeline_running = false;
static bool timeline_loop = false;

// Forward declarations
static esp_err_t parse_line(const char *line, size_t line_number, config_entry_t *entry);
static esp_err_t parse_led_line(const char *tokens[], size_t token_count, config_led_entry_t *led_entry);
static esp_err_t parse_audio_line(const char *tokens[], size_t token_count, config_audio_entry_t *audio_entry);
static float parse_value_with_interpolation(const char *str, config_interpolation_t *interp);
static void timeline_timing_callback(uint64_t timestamp_us, void *user_data);
static void timeline_execution_task(void *pvParameters);
// execute_timeline_entry_ctx has full timeline context for sweep wiring.
// When lock_held=true the caller already holds audio_gen_mutex; the function
// uses _locked audio_generator variants to avoid a recursive mutex take.
// execute_timeline_entry is a compat wrapper for the startup-call site that
// passes the persistent timeline and a sentinel index (SIZE_MAX means "unknown").
static esp_err_t execute_timeline_entry_ctx(const config_timeline_t *timeline,
                                            size_t entry_idx, bool lock_held);
static esp_err_t execute_timeline_entry(const config_entry_t *entry);

// Per-bit forward/backward lookup helpers (substep 3.6)
static const config_audio_entry_t *find_prev_audio_for_bit(const config_timeline_t *timeline,
                                                            size_t current_idx,
                                                            uint8_t channel_bit);
static const config_audio_entry_t *find_next_audio_for_bit(const config_timeline_t *timeline,
                                                            size_t current_idx,
                                                            uint8_t channel_bit);
static const config_led_entry_t   *find_prev_led_for_bit(const config_timeline_t *timeline,
                                                          size_t current_idx,
                                                          uint8_t channel_bit);
static const config_led_entry_t   *find_next_led_for_bit(const config_timeline_t *timeline,
                                                          size_t current_idx,
                                                          uint8_t channel_bit);

esp_err_t config_parser_init(void)
{
    ESP_LOGI(TAG, "Initializing config parser");

    // Create timeline mutex for thread-safe timeline operations
    timeline_mutex = xSemaphoreCreateMutex();
    if (!timeline_mutex) {
        ESP_LOGE(TAG, "Failed to create timeline mutex");
        return ESP_ERR_NO_MEM;
    }

    // Timeline execution will use lock-free communication
    ESP_LOGI(TAG, "Timeline execution configured for lock-free operation");

    // Create timeline execution task
    BaseType_t result = xTaskCreate(
        timeline_execution_task,
        "timeline_exec",
        4096,                   // Stack size
        NULL,                   // Parameters
        4,                      // Priority (lower than audio)
        &timeline_task_handle
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create timeline execution task");
        return ESP_FAIL;
    }

    // Timing engine integration - no need to create separate timer
    // The timing engine provides hardware-precision scheduling

    ESP_LOGI(TAG, "Config parser initialized");
    return ESP_OK;
}

esp_err_t config_parser_parse_content(const char *content, size_t content_length,
                                      config_timeline_t *timeline)
{
    if (!content || !timeline) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Parsing config content (%zu bytes)", content_length);

    // Initialize timeline with reasonable initial capacity
    memset(timeline, 0, sizeof(config_timeline_t));
    timeline->capacity = 50; // Start with 50 entries, grow as needed
    timeline->entries = calloc(timeline->capacity, sizeof(config_entry_t));

    if (!timeline->entries) {
        ESP_LOGE(TAG, "Failed to allocate initial timeline entries (%zu entries)", timeline->capacity);
        return ESP_ERR_NO_MEM;
    }

    // Store source content
    timeline->source_content = malloc(content_length + 1);
    if (timeline->source_content) {
        memcpy(timeline->source_content, content, content_length);
        timeline->source_content[content_length] = '\0';
    }

    // Parse line by line
    const char *line_start = content;
    const char *line_end;
    size_t line_number = 1;
    char line_buffer[CONFIG_PARSER_MAX_LINE_LENGTH];

    while (line_start < content + content_length) {
        // Find end of line
        line_end = strchr(line_start, '\n');
        if (!line_end) {
            line_end = content + content_length;
        }

        // Copy line to buffer
        size_t line_length = line_end - line_start;
        if (line_length >= sizeof(line_buffer)) {
            ESP_LOGW(TAG, "Line %zu too long, truncating", line_number);
            line_length = sizeof(line_buffer) - 1;
        }

        strncpy(line_buffer, line_start, line_length);
        line_buffer[line_length] = '\0';

        // Remove carriage return if present
        if (line_length > 0 && line_buffer[line_length - 1] == '\r') {
            line_buffer[line_length - 1] = '\0';
        }

        // Parse line
        config_entry_t entry;
        esp_err_t ret = parse_line(line_buffer, line_number, &entry);

        if (ret == ESP_OK) {
            // Check if we need to grow the timeline array
            if (timeline->count >= timeline->capacity) {
                size_t new_capacity = timeline->capacity * 2;
                if (new_capacity > CONFIG_PARSER_MAX_ENTRIES) {
                    new_capacity = CONFIG_PARSER_MAX_ENTRIES;
                }

                if (timeline->count >= new_capacity) {
                    ESP_LOGW(TAG, "Timeline capacity limit reached (%zu entries max)", CONFIG_PARSER_MAX_ENTRIES);
                } else {
                    ESP_LOGI(TAG, "Growing timeline capacity from %zu to %zu entries", timeline->capacity, new_capacity);

                    config_entry_t *new_entries = realloc(timeline->entries, new_capacity * sizeof(config_entry_t));
                    if (new_entries) {
                        timeline->entries = new_entries;
                        timeline->capacity = new_capacity;
                    } else {
                        ESP_LOGE(TAG, "Failed to grow timeline capacity to %zu entries", new_capacity);
                    }
                }
            }

            if (timeline->count < timeline->capacity) {
                timeline->entries[timeline->count++] = entry;
            }
        } else if (ret != ESP_ERR_NOT_FOUND) { // ESP_ERR_NOT_FOUND means comment/empty line
            ESP_LOGW(TAG, "Failed to parse line %zu: %s", line_number, line_buffer);
        }

        // Move to next line
        line_start = line_end;
        if (*line_start == '\n') {
            line_start++;
        }
        line_number++;
    }

    ESP_LOGI(TAG, "Parsed %zu entries from config", timeline->count);
    return ESP_OK;
}

esp_err_t config_parser_parse_file(const char *file_path, config_timeline_t *timeline)
{
    if (!file_path || !timeline) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Parsing config file: %s", file_path);

    FILE *file = fopen(file_path, "r");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        ESP_LOGE(TAG, "Empty or invalid file: %s", file_path);
        return ESP_ERR_INVALID_SIZE;
    }

    // Read file content
    char *content = malloc(file_size + 1);
    if (!content) {
        fclose(file);
        ESP_LOGE(TAG, "Failed to allocate memory for file content");
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = fread(content, 1, file_size, file);
    fclose(file);

    if (bytes_read != file_size) {
        free(content);
        ESP_LOGE(TAG, "Failed to read complete file");
        return ESP_ERR_INVALID_SIZE;
    }

    content[file_size] = '\0';

    // Parse content
    esp_err_t ret = config_parser_parse_content(content, file_size, timeline);
    free(content);

    return ret;
}

esp_err_t config_parser_execute_timeline(config_timeline_t *timeline, bool loop)
{
    if (!timeline || timeline->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (timeline_running) {
        ESP_LOGW(TAG, "Timeline already running, stopping previous");
        config_parser_stop_timeline();
    }

    ESP_LOGI(TAG, "Executing timeline with %zu entries (loop=%s)", timeline->count, loop ? "yes" : "no");

    // Lock-free timeline execution

    // Make a deep copy of the timeline to avoid use-after-free when stack timeline goes out of scope
    config_parser_free_timeline(&persistent_timeline); // Release any previous timeline back to the pool

    config_entry_t *pool_entries = NULL;
    size_t pool_capacity = 0;
    esp_err_t pool_ret = memory_pool_timeline_claim(&pool_entries, &pool_capacity);
    if (pool_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to claim timeline pool: %s", esp_err_to_name(pool_ret));
        return pool_ret;
    }

    if (timeline->count > pool_capacity) {
        ESP_LOGE(TAG, "Timeline count %zu exceeds pool capacity %zu",
                 timeline->count, pool_capacity);
        memory_pool_timeline_release(0);
        return ESP_ERR_INVALID_SIZE;
    }

    persistent_timeline.capacity = pool_capacity;
    persistent_timeline.count    = timeline->count;
    persistent_timeline.entries  = pool_entries;

    // Deep copy entries
    memcpy(persistent_timeline.entries, timeline->entries, timeline->count * sizeof(config_entry_t));

    // Copy source content if it exists
    if (timeline->source_content) {
        size_t content_len = strlen(timeline->source_content);
        persistent_timeline.source_content = malloc(content_len + 1);
        if (persistent_timeline.source_content) {
            strcpy(persistent_timeline.source_content, timeline->source_content);
        }
    }

    current_timeline = &persistent_timeline;
    current_entry_index = 0;
    timeline_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    timeline_running = true;
    timeline_loop = loop;

    // Send lock-free message for timeline start
    lock_free_message_t start_msg = {
        .type = MSG_TYPE_TIMELINE_EVENT,
        .timestamp_us = esp_timer_get_time(),
        .data_size = sizeof(uint32_t),
    };
    uint32_t timeline_count = timeline->count;
    memcpy(start_msg.data, &timeline_count, sizeof(uint32_t));

    lock_free_ring_buffer_t *timeline_queue = lock_free_get_timeline_queue();
    if (timeline_queue) {
        lock_free_send_message(timeline_queue, &start_msg);
    }

    // Dispatch ALL entries at t=0 as one batch — use ctx variant so sweep wiring works too
    if (timeline->count > 0) {
        uint32_t batch_timestamp = timeline->entries[0].type == CONFIG_ENTRY_LED ?
                                   timeline->entries[0].data.led.time_ms :
                                   timeline->entries[0].data.audio.time_ms;

        const size_t MAX_BATCH_SIZE = 50;
        size_t entries_executed = 0;

        // Hold audio_gen_mutex for the entire batch so fill_buffer sees all
        // same-timestamp channels become active in the same DMA buffer.
        audio_generator_lock();

        for (size_t i = 0; i < timeline->count && entries_executed < MAX_BATCH_SIZE; i++) {
            uint32_t entry_time = timeline->entries[i].type == CONFIG_ENTRY_LED ?
                                  timeline->entries[i].data.led.time_ms :
                                  timeline->entries[i].data.audio.time_ms;

            if (entry_time != batch_timestamp) {
                break;
            }

            esp_err_t ret = execute_timeline_entry_ctx(&persistent_timeline, i, true);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute timeline entry %zu at t=%u: %s",
                         i, batch_timestamp, esp_err_to_name(ret));
            }

            entries_executed++;
            current_entry_index = i;
        }

        audio_generator_unlock();

        ESP_LOGI(TAG, "Dispatched batch of %zu entries at t=%u ms", entries_executed, batch_timestamp);

        // Find the next entry with a strictly later timestamp
        if (current_entry_index + 1 < timeline->count) {
            uint32_t next_time = batch_timestamp;
            size_t next_index = current_entry_index + 1;

            while (next_index < timeline->count) {
                next_time = timeline->entries[next_index].type == CONFIG_ENTRY_LED ?
                            timeline->entries[next_index].data.led.time_ms :
                            timeline->entries[next_index].data.audio.time_ms;

                if (next_time > batch_timestamp) {
                    break;
                }
                next_index++;
            }

            if (next_index < timeline->count && next_time > batch_timestamp) {
                uint32_t delay_ms = next_time - batch_timestamp;
                ESP_LOGD(TAG, "Setting timer for %u ms (from %u to %u)", delay_ms, batch_timestamp, next_time);

                uint64_t target_time = timing_engine_get_time_us() + (delay_ms * 1000);
                esp_err_t ret = timing_engine_schedule_event(target_time, TIMING_EVENT_TIMELINE,
                                                             timeline_timing_callback, NULL);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to schedule timeline event: %s", esp_err_to_name(ret));
                }
            }
        }
    }

    return ESP_OK;
}

esp_err_t config_parser_stop_timeline(void)
{
    // Lock-free timeline operation
    if (!timeline_task_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timeline_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire timeline mutex in stop_timeline");
        return ESP_FAIL;
    }

    if (!timeline_running) {
        // Must release the mutex before returning — bug: previously leaked it,
        // permanently locking subsequent stop_timeline / timeline_task calls.
        xSemaphoreGive(timeline_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping timeline execution");

    timeline_running = false;
    current_timeline = NULL;
    current_entry_index = 0;

    xSemaphoreGive(timeline_mutex);

    // Cancel any pending timeline events in timing engine
    timing_engine_cancel_events_by_type(TIMING_EVENT_TIMELINE);

    // Clean up persistent timeline (do this outside mutex)
    config_parser_free_timeline(&persistent_timeline);

    return ESP_OK;
}

uint32_t config_parser_get_timeline_position(void)
{
    if (!timeline_running) {
        return 0;
    }

    return (xTaskGetTickCount() * portTICK_PERIOD_MS) - timeline_start_time;
}

void config_parser_free_timeline(config_timeline_t *timeline)
{
    if (timeline) {
        if (timeline->entries) {
            if (timeline == &persistent_timeline) {
                memory_pool_timeline_release(timeline->count);
            } else {
                free(timeline->entries);
            }
            timeline->entries = NULL;
        }
        if (timeline->source_content) {
            free(timeline->source_content);
            timeline->source_content = NULL;
        }
        timeline->count = 0;
        timeline->capacity = 0;
    }
}

esp_err_t config_parser_create_example(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *example_content =
        "# 30-second demo: LED zones + binaural beat sweep\n"
        "#\n"
        "# LED line formats:\n"
        "#   5-field (old):  time freq duty bright channel_mask\n"
        "#   9-field (new):  time freq duty bright R G B W channel_mask\n"
        "#\n"
        "# Channel mask bits (LED): 1=r1 inner-left, 2=r2 outer-left frame,\n"
        "#                          4=r3 outer-right frame, 8=r4 inner-right.\n"
        "#                          Combine bits: 9 = r1+r4, 15 = all four zones.\n"
        "#\n"
        "# Audio line:  A time freq pan volume mod channel  (channel index 1-8; 0 is rejected)\n"
        "#\n"
        "# Interpolation prefixes:  >value linear sweep,  *value quadratic ease,\n"
        "#                          (no prefix) immediate step\n"
        "\n"
        "# t = 0 — start binaural beat on ch1 (left pan) + ch2 (right pan),\n"
        "# LED inner zones (r1+r4) BLUE at 8 Hz 30% brightness\n"
        "0 8 50 30 0 0 255 0 9                # 9-field: channels 1+4, blue\n"
        "A 0 200 -100 60 0 1                  # ch1 audio: 200 Hz, left\n"
        "A 0 208 100 60 0 2                   # ch2 audio: 208 Hz, right (= 8 Hz binaural)\n"
        "\n"
        "# t = 10 s — sweep LED color blue→green and frequency 8 Hz→12 Hz,\n"
        "# binaural beat sweeps from 8 Hz to 12 Hz (carrier stays 200 Hz)\n"
        "10000 >12 50 30 0 >255 >0 0 9        # linear: freq 8→12, color blue→green\n"
        "A 10000 200 -100 60 0 1               # ch1 holds at 200 Hz\n"
        "A 10000 >212 100 60 0 2              # ch2 sweeps 208→212 Hz\n"
        "\n"
        "# t = 20 s — quadratic ease back to slow alpha-band 8 Hz, color WHITE\n"
        "20000 *8 50 30 *255 *255 *255 0 9    # quadratic ease freq + color to white\n"
        "A 20000 200 -100 60 0 1\n"
        "A 20000 *208 100 60 0 2\n"
        "\n"
        "# t = 30 s — end: LEDs off, audio fades out linearly\n"
        "30000 0 0 0 0 0 0 0 15               # all 4 LED zones off (mask 15)\n"
        "A 30000 200 -100 >0 0 1              # ch1 fade volume to 0\n"
        "A 30000 208 100 >0 0 2               # ch2 fade volume to 0\n";

    size_t example_len = strlen(example_content);
    if (example_len >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    strcpy(buffer, example_content);
    return ESP_OK;
}

// Internal helper functions

static esp_err_t parse_line(const char *line, size_t line_number, config_entry_t *entry)
{
    if (!line || !entry) {
        return ESP_ERR_INVALID_ARG;
    }

    // Skip whitespace
    while (isspace((unsigned char)*line)) {
        line++;
    }

    // Skip empty lines and comments
    if (*line == '\0' || *line == '#') {
        return ESP_ERR_NOT_FOUND;
    }

    // Tokenize line
    char line_copy[CONFIG_PARSER_MAX_LINE_LENGTH];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';

    // Strip inline comments — anything after `#` is a comment regardless of
    // position. Must happen BEFORE tokenization, or the comment tokens get
    // counted as fields and fail the 5/9-field length check.
    char *comment = strchr(line_copy, '#');
    if (comment) {
        *comment = '\0';
    }

    const char *tokens[16];
    size_t token_count = 0;
    char *token = strtok(line_copy, " \t");

    while (token && token_count < sizeof(tokens) / sizeof(tokens[0])) {
        tokens[token_count++] = token;
        token = strtok(NULL, " \t");
    }

    if (token_count == 0) {
        return ESP_ERR_NOT_FOUND; // Empty line
    }

    // Check if this is an audio command (starts with 'A')
    if (tokens[0][0] == 'A' || tokens[0][0] == 'a') {
        entry->type = CONFIG_ENTRY_AUDIO;
        return parse_audio_line(tokens + 1, token_count - 1, &entry->data.audio); // Skip 'A'
    } else {
        entry->type = CONFIG_ENTRY_LED;
        return parse_led_line(tokens, token_count, &entry->data.led);
    }
}

// Clamp a float to [0,255] and warn if it was out of range.
static uint8_t clamp_u8_field(float v, const char *field_name)
{
    if (v < 0.0f) {
        ESP_LOGW(TAG, "LED %s value %.1f clamped to 0", field_name, v);
        return 0;
    }
    if (v > 255.0f) {
        ESP_LOGW(TAG, "LED %s value %.1f clamped to 255", field_name, v);
        return 255;
    }
    return (uint8_t)v;
}

static esp_err_t parse_led_line(const char *tokens[], size_t token_count, config_led_entry_t *led_entry)
{
    // Old format: time freq duty bright mask           (5 tokens)
    // New format: time freq duty bright R G B W mask  (9 tokens)
    // Anything else is rejected.
    if (token_count != 5 && token_count != 9) {
        ESP_LOGW(TAG, "LED line has %zu tokens; expected 5 (old) or 9 (new) — skipping", token_count);
        return ESP_ERR_INVALID_ARG;
    }

    memset(led_entry, 0, sizeof(config_led_entry_t));

    // time — always plain integer
    led_entry->time_ms = (uint32_t)atol(tokens[0]);

    // freq, duty, brightness — support interpolation prefixes on all three
    led_entry->frequency  = parse_value_with_interpolation(tokens[1], &led_entry->freq_interp);
    float duty_f          = parse_value_with_interpolation(tokens[2], &led_entry->duty_interp);
    float bright_f        = parse_value_with_interpolation(tokens[3], &led_entry->brightness_interp);
    led_entry->duty_cycle = (uint8_t)duty_f;
    led_entry->brightness = (uint8_t)bright_f;

    if (token_count == 9) {
        // New 9-token format: R G B W carry independent interp prefixes
        float r_f = parse_value_with_interpolation(tokens[4], &led_entry->r_interp);
        float g_f = parse_value_with_interpolation(tokens[5], &led_entry->g_interp);
        float b_f = parse_value_with_interpolation(tokens[6], &led_entry->b_interp);
        float w_f = parse_value_with_interpolation(tokens[7], &led_entry->w_interp);
        led_entry->r = clamp_u8_field(r_f, "R");
        led_entry->g = clamp_u8_field(g_f, "G");
        led_entry->b = clamp_u8_field(b_f, "B");
        led_entry->w = clamp_u8_field(w_f, "W");
        led_entry->channel_mask = (uint8_t)atoi(tokens[8]);
    } else {
        // Old 5-token format: default RGBW = full white, W off; no interp on colors
        led_entry->r = 255;
        led_entry->g = 255;
        led_entry->b = 255;
        led_entry->w = 0;
        led_entry->r_interp = CONFIG_INTERP_NONE;
        led_entry->g_interp = CONFIG_INTERP_NONE;
        led_entry->b_interp = CONFIG_INTERP_NONE;
        led_entry->w_interp = CONFIG_INTERP_NONE;
        led_entry->channel_mask = (uint8_t)atoi(tokens[4]);
    }

    // Reject channel_mask == 0 — no channel to drive
    if (led_entry->channel_mask == 0) {
        ESP_LOGW(TAG, "LED line at %u ms has channel_mask=0 — skipping", led_entry->time_ms);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t parse_audio_line(const char *tokens[], size_t token_count, config_audio_entry_t *audio_entry)
{
    if (token_count < 5) {
        return ESP_ERR_INVALID_ARG; // Need at least time, freq, pan, volume, modulation
    }

    memset(audio_entry, 0, sizeof(config_audio_entry_t));

    // Parse time (always numeric)
    audio_entry->time_ms = atol(tokens[0]);

    // Parse frequency with interpolation support
    audio_entry->frequency = parse_value_with_interpolation(tokens[1], &audio_entry->freq_interp);

    // Parse pan with interpolation support
    audio_entry->pan = parse_value_with_interpolation(tokens[2], &audio_entry->pan_interp);

    // Parse volume with interpolation support
    audio_entry->volume = parse_value_with_interpolation(tokens[3], &audio_entry->volume_interp);

    // Parse modulation with interpolation support
    audio_entry->modulation = parse_value_with_interpolation(tokens[4], &audio_entry->mod_interp);

    // Parse channel (optional, defaults to 0)
    if (token_count >= 6) {
        audio_entry->channel = atoi(tokens[5]);
    } else {
        audio_entry->channel = 0;
    }

    // Token 6 (optional): freq_r — right channel frequency for binaural beat.
    // If absent or <= 0 or out of range, set to 0.0 which means "same as left"
    // (audio_generator_start_channel_locked interprets 0 as copy of freq_l).
    if (token_count >= 7) {
        float freq_r = atof(tokens[6]);
        audio_entry->frequency_r = (freq_r > 0.0f && freq_r <= (AUDIO_GEN_SAMPLE_RATE / 2.0f))
                                   ? freq_r : 0.0f;
    } else {
        audio_entry->frequency_r = 0.0f;
    }

    // Token 7 (optional): wave_type — integer 0-6 mapped to audio_wave_type_t.
    // Defaults to 0 (AUDIO_WAVE_SINE) if absent or out of range.
    if (token_count >= 8) {
        int wave_type_int = atoi(tokens[7]);
        audio_entry->wave_type = (wave_type_int >= 0 && wave_type_int < AUDIO_WAVE_COUNT)
                                 ? (uint8_t)wave_type_int : 0;
    } else {
        audio_entry->wave_type = 0;
    }

    return ESP_OK;
}

static float parse_value_with_interpolation(const char *str, config_interpolation_t *interp)
{
    if (!str || !interp) {
        *interp = CONFIG_INTERP_NONE;
        return 0.0f;
    }

    // Check for interpolation prefixes
    if (str[0] == '>') {
        *interp = CONFIG_INTERP_LINEAR;
        return atof(&str[1]);
    } else if (str[0] == '*') {
        *interp = CONFIG_INTERP_QUADRATIC;
        return atof(&str[1]);
    } else {
        *interp = CONFIG_INTERP_NONE;
        return atof(str);
    }
}

static void timeline_timing_callback(uint64_t timestamp_us, void *user_data)
{
    // Hardware-precision timing callback - notify the task
    if (timeline_task_handle) {
        xTaskNotifyGive(timeline_task_handle);
    }
}

static void timeline_execution_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Timeline execution task started");

    while (1) {
        // Wait for timer notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Acquire mutex to safely access timeline state
        if (xSemaphoreTake(timeline_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to acquire timeline mutex");
            continue;
        }

        // Check if timeline is still running. Release the mutex before
        // continuing — the previous code leaked it on the not-running path.
        if (!timeline_running || !current_timeline) {
            xSemaphoreGive(timeline_mutex);
            continue;
        }

        current_entry_index++;

        if (current_entry_index >= current_timeline->count) {
            if (timeline_loop) {
                // Restart timeline
                current_entry_index = 0;
                timeline_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                ESP_LOGI(TAG, "Timeline loop restarting");
            } else {
                // Timeline finished
                timeline_running = false;
                ESP_LOGI(TAG, "Timeline execution completed");

                // Send lock-free message for timeline completion
                lock_free_message_t complete_msg = {
                    .type = MSG_TYPE_TIMELINE_EVENT,
                    .timestamp_us = esp_timer_get_time(),
                    .data_size = 0,
                };
                lock_free_ring_buffer_t *timeline_queue = lock_free_get_timeline_queue();
                if (timeline_queue) {
                    lock_free_send_message(timeline_queue, &complete_msg);
                }
                xSemaphoreGive(timeline_mutex);
                continue;
            }
        }

        // ========== NEW BATCH PROCESSING LOGIC ==========

        // Get timestamp of current entry
        if (current_entry_index >= current_timeline->count) {
            ESP_LOGE(TAG, "Timeline task: current_entry_index %zu >= count %zu",
                     current_entry_index, current_timeline->count);
            timeline_running = false;
            xSemaphoreGive(timeline_mutex);
            continue;
        }

        uint32_t batch_timestamp = current_timeline->entries[current_entry_index].type == CONFIG_ENTRY_LED ?
                                   current_timeline->entries[current_entry_index].data.led.time_ms :
                                   current_timeline->entries[current_entry_index].data.audio.time_ms;

        // Execute ALL entries at this timestamp in one batch
        size_t batch_start_index = current_entry_index;
        size_t entries_executed = 0;

        ESP_LOGI(TAG, "Executing batch at timestamp %u ms starting from index %zu",
                 batch_timestamp, batch_start_index);

        // Add timing measurement for batch execution
        uint64_t batch_start_time = esp_timer_get_time();

        // Hold audio_gen_mutex for the entire batch so fill_buffer sees all
        // same-timestamp channels become active in the same DMA buffer.
        audio_generator_lock();

        // Process all entries at the same timestamp (with safety limit)
        const size_t MAX_BATCH_SIZE = 50; // Safety limit to prevent infinite loops
        for (size_t i = batch_start_index; i < current_timeline->count && entries_executed < MAX_BATCH_SIZE; i++) {
            uint32_t entry_time = current_timeline->entries[i].type == CONFIG_ENTRY_LED ?
                                 current_timeline->entries[i].data.led.time_ms :
                                 current_timeline->entries[i].data.audio.time_ms;

            if (entry_time != batch_timestamp) {
                // Different timestamp - stop batch processing
                break;
            }

            // Execute this entry
            // ESP_LOGD — runs inside audio_gen_mutex held by this batch loop.
            // ESP_LOGI at 115200 baud blocks fill_buffer ~10 ms per line, causing
            // audible click at DMA buffer boundary.  See fix_clicks_logging_2026-06-15.md.
            ESP_LOGD(TAG, "Executing entry %zu: type=%s, time=%u ms",
                     i,
                     (current_timeline->entries[i].type == CONFIG_ENTRY_LED) ? "LED" : "AUDIO",
                     entry_time);

            uint64_t entry_start_time = esp_timer_get_time();
            esp_err_t ret = execute_timeline_entry_ctx(current_timeline, i, true);
            uint64_t entry_execution_time = esp_timer_get_time() - entry_start_time;

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute timeline entry %zu: %s", i, esp_err_to_name(ret));
            } else {
                ESP_LOGD(TAG, "Entry %zu executed in %llu μs (offset +%llu μs from batch start)",
                         i, entry_execution_time, entry_start_time - batch_start_time);
            }

            entries_executed++;
            current_entry_index = i; // Update to last processed entry
        }

        audio_generator_unlock();

        uint64_t batch_total_time = esp_timer_get_time() - batch_start_time;
        ESP_LOGI(TAG, "Batch complete: executed %zu entries at timestamp %u ms in %llu μs",
                 entries_executed, batch_timestamp, batch_total_time);

        // Schedule next timer for the next different timestamp
        if (current_entry_index + 1 < current_timeline->count) {
            // Find next entry with different timestamp
            uint32_t next_time = batch_timestamp;
            size_t next_index = current_entry_index + 1;

            while (next_index < current_timeline->count) {
                next_time = current_timeline->entries[next_index].type == CONFIG_ENTRY_LED ?
                           current_timeline->entries[next_index].data.led.time_ms :
                           current_timeline->entries[next_index].data.audio.time_ms;

                if (next_time > batch_timestamp) {
                    break; // Found next different timestamp
                }
                next_index++;
            }

            // Only set timer if we found a future entry
            if (next_index < current_timeline->count && next_time > batch_timestamp) {
                uint32_t delay_ms = next_time - batch_timestamp;
                ESP_LOGI(TAG, "Scheduling next batch in %u ms (t=%u → t=%u)",
                         delay_ms, batch_timestamp, next_time);

                // Release mutex before timing operations
                // Lock-free operation - no mutex needed

                // Schedule next event with hardware precision timing engine
                uint64_t target_time = timing_engine_get_time_us() + (delay_ms * 1000); // Convert ms to μs
                esp_err_t ret = timing_engine_schedule_event(target_time, TIMING_EVENT_TIMELINE,
                                                           timeline_timing_callback, NULL);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to schedule timeline event: %s", esp_err_to_name(ret));
                }
            } else {
                ESP_LOGI(TAG, "No more timeline entries to schedule");
                // Lock-free operation - no mutex needed
            }
        } else {
            ESP_LOGI(TAG, "Timeline execution completed (no more entries)");
            timeline_running = false;
            // Lock-free operation - no mutex needed
        }

        // Release the mutex at the end of every successful iteration. Earlier
        // versions of this code left the mutex held until the next iteration,
        // which deadlocked the task on its own second take attempt.
        xSemaphoreGive(timeline_mutex);
    }
}

// ---------------------------------------------------------------------------
// Per-bit lookup helpers (substep 3.6)
// "channel_bit" is a single-bit mask (e.g. 0x01, 0x02, 0x04, 0x08).
// For audio entries the concept of "channel_mask" doesn't exist — audio uses
// a plain channel number (0-7).  We adapt by treating channel N as bit (1<<N)
// for the purpose of these helpers so the same pattern works.
//
// find_prev_* are not called by the current forward-only sweep wiring but are
// part of the Step 3.6 API surface; Step 5 will use them for update-vs-start
// decisions.  The unused attribute prevents the compiler warning.
// ---------------------------------------------------------------------------

static __attribute__((unused))
const config_audio_entry_t *find_prev_audio_for_bit(const config_timeline_t *timeline,
                                                     size_t current_idx,
                                                     uint8_t channel_bit)
{
    if (!timeline || current_idx == 0) {
        return NULL;
    }
    for (size_t i = current_idx - 1; ; i--) {
        const config_entry_t *e = &timeline->entries[i];
        if (e->type == CONFIG_ENTRY_AUDIO) {
            // channel_bit here is (1 << channel_number)
            if ((uint8_t)(1u << e->data.audio.channel) & channel_bit) {
                return &e->data.audio;
            }
        }
        if (i == 0) {
            break;
        }
    }
    return NULL;
}

static const config_audio_entry_t *find_next_audio_for_bit(const config_timeline_t *timeline,
                                                            size_t current_idx,
                                                            uint8_t channel_bit)
{
    if (!timeline) {
        return NULL;
    }
    for (size_t i = current_idx + 1; i < timeline->count; i++) {
        const config_entry_t *e = &timeline->entries[i];
        if (e->type == CONFIG_ENTRY_AUDIO) {
            if ((uint8_t)(1u << e->data.audio.channel) & channel_bit) {
                return &e->data.audio;
            }
        }
    }
    return NULL;
}

static __attribute__((unused))
const config_led_entry_t *find_prev_led_for_bit(const config_timeline_t *timeline,
                                                 size_t current_idx,
                                                 uint8_t channel_bit)
{
    if (!timeline || current_idx == 0) {
        return NULL;
    }
    for (size_t i = current_idx - 1; ; i--) {
        const config_entry_t *e = &timeline->entries[i];
        if (e->type == CONFIG_ENTRY_LED) {
            if (e->data.led.channel_mask & channel_bit) {
                return &e->data.led;
            }
        }
        if (i == 0) {
            break;
        }
    }
    return NULL;
}

static const config_led_entry_t *find_next_led_for_bit(const config_timeline_t *timeline,
                                                        size_t current_idx,
                                                        uint8_t channel_bit)
{
    if (!timeline) {
        return NULL;
    }
    for (size_t i = current_idx + 1; i < timeline->count; i++) {
        const config_entry_t *e = &timeline->entries[i];
        if (e->type == CONFIG_ENTRY_LED) {
            if (e->data.led.channel_mask & channel_bit) {
                return &e->data.led;
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Map config interpolation type to audio generator sweep type
// ---------------------------------------------------------------------------
static audio_gen_sweep_type_t interp_to_audio_curve(config_interpolation_t interp)
{
    switch (interp) {
        case CONFIG_INTERP_LINEAR:    return AUDIO_GEN_SWEEP_LINEAR;
        case CONFIG_INTERP_QUADRATIC: return AUDIO_GEN_SWEEP_QUADRATIC;
        default:                      return AUDIO_GEN_SWEEP_NONE;
    }
}

// ---------------------------------------------------------------------------
// Map config interpolation type to LED interpolation type
// ---------------------------------------------------------------------------
static led_interp_t interp_to_led_curve(config_interpolation_t interp)
{
    switch (interp) {
        case CONFIG_INTERP_LINEAR:    return LED_INTERP_LINEAR;
        case CONFIG_INTERP_QUADRATIC: return LED_INTERP_QUADRATIC;
        default:                      return LED_INTERP_NONE;
    }
}

// ---------------------------------------------------------------------------
// Context-aware entry execution — has access to the full timeline so it can
// look forward/backward for sweep wiring (substep 3.4 and 3.5).
// lock_held: caller already holds audio_gen_mutex; use _locked audio_generator
// variants to avoid a recursive mutex take.
// ---------------------------------------------------------------------------
static esp_err_t execute_timeline_entry_ctx(const config_timeline_t *timeline,
                                            size_t entry_idx, bool lock_held)
{
    if (!timeline || entry_idx >= timeline->count) {
        ESP_LOGE(TAG, "execute_timeline_entry_ctx: bad args");
        return ESP_ERR_INVALID_ARG;
    }

    const config_entry_t *entry = &timeline->entries[entry_idx];

    if (entry->type != CONFIG_ENTRY_AUDIO && entry->type != CONFIG_ENTRY_LED) {
        ESP_LOGE(TAG, "execute_timeline_entry_ctx: invalid type %d", entry->type);
        return ESP_ERR_INVALID_ARG;
    }

    // ------------------------------------------------------------------
    // AUDIO entry
    // ------------------------------------------------------------------
    if (entry->type == CONFIG_ENTRY_AUDIO) {
        const config_audio_entry_t *audio = &entry->data.audio;

        // Interp prefix glyph: NONE="", LINEAR=">", QUADRATIC="*"
        // ESP_LOGD here — runs inside audio_gen_mutex held by batch loop.
        // ESP_LOGI at 115200 baud blocks fill_buffer long enough to cause
        // I2S underrun (audible click).  Raise log level via menuconfig to debug.
        #define INTERP_GLYPH(x) ((x) == CONFIG_INTERP_LINEAR ? ">" : (x) == CONFIG_INTERP_QUADRATIC ? "*" : "")
        ESP_LOGD(TAG, "Executing audio entry: t=%u  freq=%s%.1f  freq_r=%.1f  pan=%s%.0f  vol=%s%.0f  mod=%s%.1f  ch=%u",
                 audio->time_ms,
                 INTERP_GLYPH(audio->freq_interp),    audio->frequency,
                 audio->frequency_r,
                 INTERP_GLYPH(audio->pan_interp),     audio->pan,
                 INTERP_GLYPH(audio->volume_interp),  audio->volume,
                 INTERP_GLYPH(audio->mod_interp),     audio->modulation,
                 (unsigned)audio->channel);
        #undef INTERP_GLYPH

        audio_gen_params_t gen_params = {
            .frequency   = audio->frequency,
            .frequency_r = audio->frequency_r,
            .amplitude   = audio->volume / 100.0f,   // 0-100 → 0.0-1.0
            .pan         = audio->pan / 100.0f,      // -100/+100 → -1.0/+1.0
            .mod_frequency = audio->modulation,
            // mod_depth hardcoded per spec (no field defined); Phase 4 may
            // expose a Kconfig override.
            .mod_depth   = 0.1f,
            .wave_type   = (audio_wave_type_t)audio->wave_type,
            // Legacy sweep fields unused — sweep is driven via
            // audio_generator_start_sweep() after channel start.
            .sweep_type  = AUDIO_GEN_SWEEP_NONE,
            .sweep_target = 0.0f,
            .duration_ms = 86400000   // 24 h — effectively continuous
        };

        // Smart update-vs-start (Step 5.3): if the channel is already running,
        // update its params without restarting so there is no audible click or
        // phase reset.  Only start from scratch when the channel is inactive.
        esp_err_t ret;
        bool ch_active;
        if (lock_held) {
            ch_active = audio_generator_is_active_locked(audio->channel);
        } else {
            ch_active = audio_manager_is_channel_active(audio->channel);
        }

        if (ch_active) {
            if (lock_held) {
                ret = audio_generator_update_params_locked(audio->channel, &gen_params);
            } else {
                ret = audio_manager_update_generation(audio->channel, &gen_params);
            }
        } else {
            if (lock_held) {
                ret = audio_generator_start_channel_locked(audio->channel, &gen_params);
            } else {
                ret = audio_manager_start_generation(audio->channel, &gen_params);
            }
        }

        if (ret == ESP_OK) {
            bool ch_active_after;
            if (lock_held) {
                ch_active_after = audio_generator_is_active_locked(audio->channel);
            } else {
                ch_active_after = audio_manager_is_channel_active(audio->channel);
            }
            ESP_LOGD(TAG, "Audio channel %d %s successfully", audio->channel,
                     ch_active_after ? "updated" : "started");

            // ---- Sweep wiring (substep 3.4) ----
            // For each sweep-capable parameter, look ahead to the next entry
            // on the SAME channel to determine the window duration and target.
            // Per-bit independent per the Q1 decision (multi-bit channel maps
            // are not used for audio, but the helper is written generically).
            uint8_t ch_bit = (uint8_t)(1u << audio->channel);
            const config_audio_entry_t *next = find_next_audio_for_bit(timeline, entry_idx, ch_bit);

            if (next != NULL) {
                uint32_t window_ms = next->time_ms - audio->time_ms;
                // 64-bit multiply guards against the integer overflow that
                // caused the original 10-second bug.
                uint64_t dur_samples = ((uint64_t)window_ms * AUDIO_GEN_SAMPLE_RATE) / 1000ULL;

                // Frequency sweep
                if (next->freq_interp != CONFIG_INTERP_NONE) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_FREQUENCY,
                            audio->frequency, next->frequency,
                            dur_samples, interp_to_audio_curve(next->freq_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_FREQUENCY,
                            audio->frequency, next->frequency,
                            dur_samples, interp_to_audio_curve(next->freq_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=FREQ %.2f→%.2f over %ums curve=%d",
                             audio->channel, audio->frequency, next->frequency,
                             window_ms, (int)next->freq_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "freq sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }

                // Amplitude sweep — volume 0-100 → amplitude 0.0-1.0
                if (next->volume_interp != CONFIG_INTERP_NONE) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_AMPLITUDE,
                            audio->volume / 100.0f, next->volume / 100.0f,
                            dur_samples, interp_to_audio_curve(next->volume_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_AMPLITUDE,
                            audio->volume / 100.0f, next->volume / 100.0f,
                            dur_samples, interp_to_audio_curve(next->volume_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=AMP %.2f→%.2f over %ums curve=%d",
                             audio->channel,
                             audio->volume / 100.0f, next->volume / 100.0f,
                             window_ms, (int)next->volume_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "amp sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }

                // Pan sweep — pan -100/+100 → -1.0/+1.0
                if (next->pan_interp != CONFIG_INTERP_NONE) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_PAN,
                            audio->pan / 100.0f, next->pan / 100.0f,
                            dur_samples, interp_to_audio_curve(next->pan_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_PAN,
                            audio->pan / 100.0f, next->pan / 100.0f,
                            dur_samples, interp_to_audio_curve(next->pan_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=PAN %.2f→%.2f over %ums curve=%d",
                             audio->channel,
                             audio->pan / 100.0f, next->pan / 100.0f,
                             window_ms, (int)next->pan_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "pan sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }

                // Modulation frequency sweep
                if (next->mod_interp != CONFIG_INTERP_NONE) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_MOD_FREQ,
                            audio->modulation, next->modulation,
                            dur_samples, interp_to_audio_curve(next->mod_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_MOD_FREQ,
                            audio->modulation, next->modulation,
                            dur_samples, interp_to_audio_curve(next->mod_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=MOD %.2f→%.2f over %ums curve=%d",
                             audio->channel,
                             audio->modulation, next->modulation,
                             window_ms, (int)next->mod_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "mod sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }
            }

        }

        return ret;
    }

    // ------------------------------------------------------------------
    // LED entry
    // ------------------------------------------------------------------
    const config_led_entry_t *led = &entry->data.led;

    // ESP_LOGD — same audio_gen_mutex blocking concern as the audio entry log above.
    // The lock is released a few lines below for the LED dispatch itself, but the
    // log line currently fires while the lock is still held.
    #define INTERP_GLYPH(x) ((x) == CONFIG_INTERP_LINEAR ? ">" : (x) == CONFIG_INTERP_QUADRATIC ? "*" : "")
    ESP_LOGD(TAG, "Executing LED entry: t=%u  freq=%s%.1f  duty=%s%d%%  bright=%s%d%%  RGB=(%s%d,%s%d,%s%d)  W=%s%d  mask=0x%02x",
             led->time_ms,
             INTERP_GLYPH(led->freq_interp),       led->frequency,
             INTERP_GLYPH(led->duty_interp),       led->duty_cycle,
             INTERP_GLYPH(led->brightness_interp), led->brightness,
             INTERP_GLYPH(led->r_interp), led->r,
             INTERP_GLYPH(led->g_interp), led->g,
             INTERP_GLYPH(led->b_interp), led->b,
             INTERP_GLYPH(led->w_interp), led->w,
             led->channel_mask);
    #undef INTERP_GLYPH

    // Release audio lock around LED dispatch — LED path calls audio_led_sync_stop/start
    // which contains vTaskDelays that would otherwise block fill_buffer (see
    // reports/non_planned_reports/fix_clicks_boundary_2026-06-15.md).
    if (lock_held) {
        audio_generator_unlock();
    }

    esp_err_t led_ret = ESP_OK;

    // Stop audio-LED synchronization before reconfiguring LEDs
    if (audio_led_sync_is_active()) {
        esp_err_t sync_ret = audio_led_sync_stop();
        if (sync_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop audio-LED sync: %s", esp_err_to_name(sync_ret));
        }
    }

    if (led->frequency <= 0.0f) {
        // freq=0 means stop flicker on these channels
        esp_err_t ret = led_matrix_stop_flicker_masked(led->channel_mask);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop LED flicker on mask 0x%02x: %s",
                     led->channel_mask, esp_err_to_name(ret));
        }
        /* led_ret stays ESP_OK — re-acquire lock and return via led_done below */
    } else {
    /* ---------- freq > 0: sweep wiring + LED matrix dispatch ---------- */

    // ---- Sweep wiring (substep 3.5) ----
    // For each bit in channel_mask, look forward independently to find the
    // next entry that covers that bit (Q1: per-bit independent lookback).
    // Build a sweep spec that reflects the "union" of all bits needing sweep.
    // In practice most entries cover a single bit or all bits with the same
    // target, so we build one spec per mask and call start_sweep_masked once.
    // If bits disagree on targets, the last bit's values win for the shared
    // spec — multi-target-per-call is not part of the spec.
    bool any_sweep = false;
    led_sweep_spec_t sweep_spec = {0};

    // Compute next entry for the lowest-set bit; use that for duration.
    uint8_t first_bit = led->channel_mask & (uint8_t)(-(int8_t)led->channel_mask);
    const config_led_entry_t *next_first = find_next_led_for_bit(timeline, entry_idx, first_bit);

    if (next_first != NULL) {
        uint32_t window_ms = next_first->time_ms - led->time_ms;
        sweep_spec.duration_ms = window_ms;

        // Walk each bit in channel_mask and merge sweep params
        for (int bit = 0; bit < 4; bit++) {
            uint8_t bitmask = (uint8_t)(1u << bit);
            if (!(led->channel_mask & bitmask)) {
                continue;
            }

            const config_led_entry_t *next_bit =
                find_next_led_for_bit(timeline, entry_idx, bitmask);
            if (next_bit == NULL) {
                continue;
            }

            // Start values from current entry, targets from next entry
            if (next_bit->freq_interp != CONFIG_INTERP_NONE) {
                sweep_spec.freq_milliHz_start  = (uint32_t)(led->frequency  * 1000.0f);
                sweep_spec.freq_milliHz_target = (uint32_t)(next_bit->frequency * 1000.0f);
                sweep_spec.freq_curve          = interp_to_led_curve(next_bit->freq_interp);
                any_sweep = true;
            }
            if (next_bit->duty_interp != CONFIG_INTERP_NONE) {
                sweep_spec.duty_start   = led->duty_cycle;
                sweep_spec.duty_target  = next_bit->duty_cycle;
                sweep_spec.duty_curve   = interp_to_led_curve(next_bit->duty_interp);
                any_sweep = true;
            }
            if (next_bit->brightness_interp != CONFIG_INTERP_NONE) {
                sweep_spec.bright_start  = led->brightness;
                sweep_spec.bright_target = next_bit->brightness;
                sweep_spec.bright_curve  = interp_to_led_curve(next_bit->brightness_interp);
                any_sweep = true;
            }
            if (next_bit->r_interp != CONFIG_INTERP_NONE) {
                sweep_spec.r_start  = led->r;
                sweep_spec.r_target = next_bit->r;
                sweep_spec.r_curve  = interp_to_led_curve(next_bit->r_interp);
                any_sweep = true;
            }
            if (next_bit->g_interp != CONFIG_INTERP_NONE) {
                sweep_spec.g_start  = led->g;
                sweep_spec.g_target = next_bit->g;
                sweep_spec.g_curve  = interp_to_led_curve(next_bit->g_interp);
                any_sweep = true;
            }
            if (next_bit->b_interp != CONFIG_INTERP_NONE) {
                sweep_spec.b_start  = led->b;
                sweep_spec.b_target = next_bit->b;
                sweep_spec.b_curve  = interp_to_led_curve(next_bit->b_interp);
                any_sweep = true;
            }
            if (next_bit->w_interp != CONFIG_INTERP_NONE) {
                sweep_spec.w_start  = led->w;
                sweep_spec.w_target = next_bit->w;
                sweep_spec.w_curve  = interp_to_led_curve(next_bit->w_interp);
                any_sweep = true;
            }
        }
    }

    esp_err_t ret;
    if (any_sweep) {
        // Fill non-swept fields so start_sweep has a complete snapshot
        if (sweep_spec.freq_curve == LED_INTERP_NONE) {
            sweep_spec.freq_milliHz_start  = (uint32_t)(led->frequency * 1000.0f);
            sweep_spec.freq_milliHz_target = sweep_spec.freq_milliHz_start;
        }
        if (sweep_spec.duty_curve == LED_INTERP_NONE) {
            sweep_spec.duty_start  = led->duty_cycle;
            sweep_spec.duty_target = led->duty_cycle;
        }
        if (sweep_spec.bright_curve == LED_INTERP_NONE) {
            sweep_spec.bright_start  = led->brightness;
            sweep_spec.bright_target = led->brightness;
        }
        if (sweep_spec.r_curve == LED_INTERP_NONE) {
            sweep_spec.r_start  = led->r;
            sweep_spec.r_target = led->r;
        }
        if (sweep_spec.g_curve == LED_INTERP_NONE) {
            sweep_spec.g_start  = led->g;
            sweep_spec.g_target = led->g;
        }
        if (sweep_spec.b_curve == LED_INTERP_NONE) {
            sweep_spec.b_start  = led->b;
            sweep_spec.b_target = led->b;
        }
        if (sweep_spec.w_curve == LED_INTERP_NONE) {
            sweep_spec.w_start  = led->w;
            sweep_spec.w_target = led->w;
        }

#ifdef CONFIG_TIMELINE_DEBUG
        ESP_LOGI(TAG, "Timeline LED sweep: mask=0x%02x freq %.2f→%.2f dur=%ums",
                 led->channel_mask, led->frequency,
                 (float)sweep_spec.freq_milliHz_target / 1000.0f,
                 sweep_spec.duration_ms);
#endif
        ret = led_matrix_start_sweep_masked(led->channel_mask, &sweep_spec);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start LED sweep on mask 0x%02x: %s",
                     led->channel_mask, esp_err_to_name(ret));
            led_ret = ret;
        }
    } else {
        // No sweep — immediate flicker with current entry's color.
        // Route based on whether the channel(s) are already active:
        //   - Inactive: start_flicker_masked activates and initialises cycle state.
        //   - Active:   update_flicker_params_masked updates timing without resetting
        //               the cycle origin (rhythm-preserving), followed by
        //               set_flicker_color_masked for the new color.
        // This prevents the stutter caused by an unconditional start that resets
        // cycle_start_time_us on an already-running channel.
        led_matrix_set_flicker_color_masked(led->channel_mask, led->r, led->g, led->b);
        if (led_matrix_is_flickering_masked(led->channel_mask)) {
            ret = led_matrix_update_flicker_params_masked(led->channel_mask,
                                                          led->frequency,
                                                          led->duty_cycle,
                                                          led->brightness);
        } else {
            ret = led_matrix_start_flicker_masked(led->channel_mask,
                                                  led->frequency,
                                                  led->duty_cycle,
                                                  led->brightness);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start/update LED flicker on mask 0x%02x: %s",
                     led->channel_mask, esp_err_to_name(ret));
            led_ret = ret;
        }
    }

    if (led_ret == ESP_OK) {
        ESP_LOGI(TAG, "LED mask 0x%02x started: %.1f Hz, %d%% duty, %d%% brightness %s",
                 led->channel_mask, led->frequency, led->duty_cycle, led->brightness,
                 any_sweep ? "(with sweep)" : "");

        // Re-enable audio-LED synchronization in VU-meter mode
        if (!audio_led_sync_is_active()) {
            esp_err_t sync_ret = audio_led_sync_start(SYNC_MODE_VU_METER);
            if (sync_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start audio-LED sync: %s", esp_err_to_name(sync_ret));
            }
        } else {
            audio_led_sync_set_mode(SYNC_MODE_VU_METER);
        }
    }

    } /* end else (freq > 0) */

    // Re-acquire audio lock now that LED dispatch (including any vTaskDelays) is complete.
    if (lock_held) {
        audio_generator_lock();
    }
    return led_ret;
}

// ---------------------------------------------------------------------------
// Legacy compat wrapper — kept only so the forward declaration compiles.
// All real call sites now use execute_timeline_entry_ctx().  If this is ever
// reached at runtime, it indicates a missing conversion of a call site.
// ---------------------------------------------------------------------------
static __attribute__((unused)) esp_err_t execute_timeline_entry(const config_entry_t *entry)
{
    (void)entry;
    ESP_LOGW(TAG, "execute_timeline_entry called without timeline context — BUG");
    return ESP_ERR_NOT_SUPPORTED;
}
