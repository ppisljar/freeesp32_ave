#include "config_parser.h"
#include "audio_manager.h"
#include "led_strip.h"
#include "led_matrix_example.h"
#include "audio_config.h"
#include "timing_engine.h"
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
static esp_err_t execute_timeline_entry(const config_entry_t *entry);

esp_err_t config_parser_init(void)
{
    ESP_LOGI(TAG, "Initializing config parser");

    // Create timeline mutex
    timeline_mutex = xSemaphoreCreateMutex();
    if (!timeline_mutex) {
        ESP_LOGE(TAG, "Failed to create timeline mutex");
        return ESP_FAIL;
    }

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
        vSemaphoreDelete(timeline_mutex);
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
    if (!timeline || timeline->count == 0 || !timeline_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    if (timeline_running) {
        ESP_LOGW(TAG, "Timeline already running, stopping previous");
        config_parser_stop_timeline();
    }

    ESP_LOGI(TAG, "Executing timeline with %zu entries (loop=%s)", timeline->count, loop ? "yes" : "no");

    if (xSemaphoreTake(timeline_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire timeline mutex in execute_timeline");
        return ESP_FAIL;
    }

    // Make a deep copy of the timeline to avoid use-after-free when stack timeline goes out of scope
    config_parser_free_timeline(&persistent_timeline); // Free any previous timeline

    persistent_timeline.capacity = timeline->count; // Only allocate what we need
    persistent_timeline.count = timeline->count;
    persistent_timeline.entries = malloc(persistent_timeline.capacity * sizeof(config_entry_t));

    if (!persistent_timeline.entries) {
        ESP_LOGE(TAG, "Failed to allocate persistent timeline entries");
        xSemaphoreGive(timeline_mutex);
        return ESP_ERR_NO_MEM;
    }

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

    // Start with first entry
    if (timeline->count > 0) {
        esp_err_t ret = execute_timeline_entry(&timeline->entries[0]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to execute first timeline entry");
            timeline_running = false;
            current_timeline = NULL;
            xSemaphoreGive(timeline_mutex);
            return ret;
        }

        // Set timer for next entry if there is one
        if (timeline->count > 1) {
            // Find next entry with different timestamp
            uint32_t current_time = timeline->entries[0].type == CONFIG_ENTRY_LED ?
                                   timeline->entries[0].data.led.time_ms :
                                   timeline->entries[0].data.audio.time_ms;
            uint32_t next_time = current_time;
            size_t next_index = 1;

            // Skip entries at same time
            while (next_index < timeline->count) {
                next_time = timeline->entries[next_index].type == CONFIG_ENTRY_LED ?
                           timeline->entries[next_index].data.led.time_ms :
                           timeline->entries[next_index].data.audio.time_ms;

                if (next_time > current_time) {
                    break; // Found next different timestamp
                }
                next_index++;
            }

            // Only set timer if we found a future entry
            if (next_index < timeline->count && next_time > current_time) {
                uint32_t delay_ms = next_time - current_time;
                ESP_LOGD(TAG, "Setting timer for %u ms (from %u to %u)", delay_ms, current_time, next_time);

                // Release mutex before timing operations
                xSemaphoreGive(timeline_mutex);

                // Schedule event with microsecond precision using timing engine
                uint64_t target_time = timing_engine_get_time_us() + (delay_ms * 1000); // Convert ms to μs
                esp_err_t ret = timing_engine_schedule_event(target_time, TIMING_EVENT_TIMELINE,
                                                           timeline_timing_callback, NULL);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to schedule timeline event: %s", esp_err_to_name(ret));
                }
            } else {
                xSemaphoreGive(timeline_mutex);
            }
        } else {
            xSemaphoreGive(timeline_mutex);
        }
    } else {
        xSemaphoreGive(timeline_mutex);
    }

    return ESP_OK;
}

esp_err_t config_parser_stop_timeline(void)
{
    if (!timeline_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timeline_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire timeline mutex in stop_timeline");
        return ESP_FAIL;
    }

    if (!timeline_running) {
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
            free(timeline->entries);
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
        "# 21-Minute Meditation Session with LED & Binaural Beats\n"
        "# Format: time frequency duty brightness channel (for LED)\n"
        "#         A time frequency pan volume modulation channel (for Audio)\n"
        "#\n"
        "# Phase 1: 1 minute - White LED flicker 20Hz + Binaural 400/420Hz\n"
        "0 20 50 100 1                 # Start white LED flicker at 20Hz\n"
        "A 0 400 -100 70 0 0           # Left channel: 400Hz\n"
        "A 0 420 100 70 0 1            # Right channel: 420Hz (20Hz binaural beat)\n"
        "\n"
        "# Phase 2: 10 minutes - Transition to 8Hz flicker + 400/408Hz binaural\n"
        "60000 >8 50 100 1             # Slow transition: LED 20Hz → 8Hz over 10 minutes\n"
        "A 60000 400 -100 70 0 0       # Left channel stays at 400Hz\n"
        "A 60000 >408 100 70 0 1       # Right channel: 420Hz → 408Hz (20Hz → 8Hz binaural)\n"
        "\n"
        "# Phase 3: 10 minutes - Hold steady at 8Hz flicker + 400/408Hz binaural\n"
        "660000 8 50 100 1             # Hold LED at 8Hz\n"
        "A 660000 400 -100 70 0 0      # Left channel: 400Hz steady\n"
        "A 660000 408 100 70 0 1       # Right channel: 408Hz steady (8Hz binaural)\n"
        "\n"
        "# End session\n"
        "1260000 0 0 0 1               # Turn off LED after 21 minutes\n"
        "A 1260000 0 0 >0 0 0          # Fade out left channel\n"
        "A 1260000 0 0 >0 0 1          # Fade out right channel\n";

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

static esp_err_t parse_led_line(const char *tokens[], size_t token_count, config_led_entry_t *led_entry)
{
    if (token_count < 4) {
        return ESP_ERR_INVALID_ARG; // Need at least time, freq, duty, brightness
    }

    memset(led_entry, 0, sizeof(config_led_entry_t));

    // Parse time (always numeric)
    led_entry->time_ms = atol(tokens[0]);

    // Parse frequency with interpolation support
    led_entry->frequency = parse_value_with_interpolation(tokens[1], &led_entry->freq_interp);

    // Parse duty cycle with interpolation support
    float duty = parse_value_with_interpolation(tokens[2], &led_entry->duty_interp);
    led_entry->duty_cycle = (uint8_t)duty;

    // Parse brightness with interpolation support
    float brightness = parse_value_with_interpolation(tokens[3], &led_entry->brightness_interp);
    led_entry->brightness = (uint8_t)brightness;

    // Parse channel mask (optional, defaults to 1)
    if (token_count >= 5) {
        led_entry->channel_mask = atoi(tokens[4]);
    } else {
        led_entry->channel_mask = 1;
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

    // Set right channel frequency (same as left for now, binaural beats can be added later)
    audio_entry->frequency_r = audio_entry->frequency;

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

        // Check if timeline is still running
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
            ESP_LOGI(TAG, "Executing entry %zu: type=%s, time=%u ms",
                     i,
                     (current_timeline->entries[i].type == CONFIG_ENTRY_LED) ? "LED" : "AUDIO",
                     entry_time);

            uint64_t entry_start_time = esp_timer_get_time();
            esp_err_t ret = execute_timeline_entry(&current_timeline->entries[i]);
            uint64_t entry_execution_time = esp_timer_get_time() - entry_start_time;

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute timeline entry %zu: %s", i, esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Entry %zu executed in %llu μs (offset +%llu μs from batch start)",
                         i, entry_execution_time, entry_start_time - batch_start_time);
            }

            entries_executed++;
            current_entry_index = i; // Update to last processed entry
        }

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
                xSemaphoreGive(timeline_mutex);

                // Schedule next event with hardware precision timing engine
                uint64_t target_time = timing_engine_get_time_us() + (delay_ms * 1000); // Convert ms to μs
                esp_err_t ret = timing_engine_schedule_event(target_time, TIMING_EVENT_TIMELINE,
                                                           timeline_timing_callback, NULL);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to schedule timeline event: %s", esp_err_to_name(ret));
                }
            } else {
                ESP_LOGI(TAG, "No more timeline entries to schedule");
                xSemaphoreGive(timeline_mutex);
            }
        } else {
            ESP_LOGI(TAG, "Timeline execution completed (no more entries)");
            timeline_running = false;
            xSemaphoreGive(timeline_mutex);
        }
    }
}

static esp_err_t execute_timeline_entry(const config_entry_t *entry)
{
    if (!entry) {
        ESP_LOGE(TAG, "execute_timeline_entry: NULL entry pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate entry type before accessing union data
    if (entry->type != CONFIG_ENTRY_AUDIO && entry->type != CONFIG_ENTRY_LED) {
        ESP_LOGE(TAG, "execute_timeline_entry: Invalid entry type %d", entry->type);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "execute_timeline_entry: entry=%p, type=%d", entry, entry->type);

    if (entry->type == CONFIG_ENTRY_AUDIO) {
        const config_audio_entry_t *audio = &entry->data.audio;

        ESP_LOGI(TAG, "Executing audio entry: t=%u, freq=%.1f, pan=%.1f, vol=%.1f",
                 audio->time_ms, audio->frequency, audio->pan, audio->volume);

        // Convert to audio generator parameters
        audio_gen_params_t gen_params = {
            .frequency = audio->frequency,
            .frequency_r = audio->frequency_r,
            .amplitude = audio->volume / 100.0f,  // Convert percentage to 0-1
            .pan = audio->pan / 100.0f,           // Convert -100/+100 to -1/+1
            .mod_frequency = audio->modulation,
            .mod_depth = 0.1f,                    // Default modulation depth
            .sweep_type = AUDIO_GEN_SWEEP_NONE,   // TODO: Handle interpolation
            .sweep_target = 0.0f,
            .duration_ms = 86400000                // 24 hours - effectively continuous
        };

        esp_err_t ret = audio_manager_start_generation(audio->channel, &gen_params);

        // Validation logging for simultaneous execution
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Audio channel %d started successfully", audio->channel);

            // Log all currently active channels for validation
            int active_count = 0;
            for (int i = 0; i < 8; i++) {
                if (audio_manager_is_channel_active(i)) {
                    ESP_LOGI(TAG, "Channel %d is active", i);
                    active_count++;
                }
            }
            ESP_LOGI(TAG, "Total active audio channels: %d", active_count);
        }

        return ret;

    } else if (entry->type == CONFIG_ENTRY_LED) {
        const config_led_entry_t *led = &entry->data.led;

        ESP_LOGI(TAG, "Executing LED entry: t=%u, freq=%.1f, duty=%d, bright=%d",
                 led->time_ms, led->frequency, led->duty_cycle, led->brightness);

        // Stop any existing flicker first
        esp_err_t ret = led_matrix_stop_flicker();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to stop existing LED flicker: %s", esp_err_to_name(ret));
        }

        // Check if we should start LED flicker (frequency > 0)
        if (led->frequency > 0.0f) {
            // Set LED color to white for meditation applications
            led_matrix_set_flicker_color(255, 255, 255);

            // Start LED flicker with specified parameters
            ret = led_matrix_start_flicker(led->frequency, led->duty_cycle, led->brightness);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start LED flicker: %s", esp_err_to_name(ret));
                return ret;
            }

            ESP_LOGI(TAG, "Started LED flicker: %.1f Hz, %d%% duty cycle, %d%% brightness",
                     led->frequency, led->duty_cycle, led->brightness);
        } else {
            // Frequency is 0 - set static LED color with brightness
            uint8_t brightness_scaled = (255 * led->brightness) / 100;
            led_matrix_clear(); // Clear first

            // Set all LEDs to white with specified brightness
            for (uint8_t x = 0; x < LED_MATRIX_WIDTH; x++) {
                for (uint8_t y = 0; y < LED_MATRIX_HEIGHT; y++) {
                    led_matrix_set_pixel(x, y, brightness_scaled, brightness_scaled, brightness_scaled);
                }
            }
            led_matrix_refresh();

            ESP_LOGI(TAG, "Set static LED brightness to %d%%", led->brightness);
        }

        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}
