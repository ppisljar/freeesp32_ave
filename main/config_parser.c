#include "config_parser.h"
#include "audio_manager.h"
#include "led_strip.h"
#include "led_matrix_example.h"
#include "audio_config.h"
#include "timing_engine.h"
#include "lock_free_comm.h"
#include "memory_pool.h"
#include "bg_player.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

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

// Transport clock (Layer 2 — Plan 007 Step 2.1):
// Single canonical wall-clock anchor captured at the moment timeline playback
// begins.  All event deadlines and LED cycle origins are computed relative to
// this value so that accumulated dispatch lag can never produce phase drift
// between channels or between successive batches.
// Reset to 0 by config_parser_stop_timeline() and re-captured on each
// config_parser_execute_timeline() call (including loop restarts).
static uint64_t transport_origin_us = 0;

// Diagnostic-only mirror of transport_origin_us. Set whenever a timeline
// starts; NOT reset by stop_timeline so /api/report can still correlate
// button-press timestamps to the just-finished session. Overwritten by the
// next execute_timeline call.
static uint64_t last_session_origin_us = 0;

// Forward declarations
static esp_err_t parse_line(const char *line, size_t line_number, config_entry_t *entry);
static esp_err_t parse_led_line(const char *tokens[], size_t token_count, config_led_entry_t *led_entry);
static esp_err_t parse_audio_line(const char *tokens[], size_t token_count, config_audio_entry_t *audio_entry);
static esp_err_t parse_bg_line(const char *tokens[], size_t token_count, config_bg_entry_t *bg_entry);
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

// One-line human-readable summary of what a timeline entry actually does,
// including any forward-wired sweep target (start→target over duration).
// Called from the batch dispatchers AFTER audio_generator_unlock() so the
// ESP_LOGI calls don't block fill_buffer (UART writes at 115200 baud are
// slow enough to cause I2S underrun if invoked under the audio mutex).
static void log_entry_summary(const config_timeline_t *tl, size_t idx)
{
    if (!tl || idx >= tl->count) return;
    const config_entry_t *e = &tl->entries[idx];

    char sweep_buf[192] = {0};
    size_t sbi = 0;
    #define SWEEP_APPEND(...) do { \
        if (sbi < sizeof(sweep_buf) - 1) { \
            int _w = snprintf(sweep_buf + sbi, sizeof(sweep_buf) - sbi, __VA_ARGS__); \
            if (_w > 0) sbi += (size_t)_w; \
        } \
    } while (0)

    if (e->type == CONFIG_ENTRY_LED) {
        const config_led_entry_t *led = &e->data.led;

        // First-bit sweep lookup — typical configs use single-bit masks.
        // For multi-bit divergent masks the per-bucket dispatcher still does
        // the right thing; this summary just shows the first bit's window.
        for (int bit = 0; bit < NUM_LED_CHANNELS; bit++) {
            uint8_t bitmask = (uint8_t)(1u << bit);
            if (!(led->channel_mask & bitmask)) continue;
            const config_led_entry_t *nb = find_next_led_for_bit(tl, idx, bitmask);
            if (!nb) break;
            uint32_t dur = nb->time_ms - led->time_ms;
            if (nb->freq_interp       != CONFIG_INTERP_NONE) SWEEP_APPEND(" freq:%.1f->%.1fHz/%ums",  led->frequency, nb->frequency, (unsigned)dur);
            if (nb->duty_interp       != CONFIG_INTERP_NONE) SWEEP_APPEND(" duty:%d->%d%%/%ums",      led->duty_cycle, nb->duty_cycle, (unsigned)dur);
            if (nb->brightness_interp != CONFIG_INTERP_NONE) SWEEP_APPEND(" bri:%d->%d%%/%ums",       led->brightness, nb->brightness, (unsigned)dur);
            if (nb->r_interp          != CONFIG_INTERP_NONE) SWEEP_APPEND(" R:%d->%d/%ums",           led->r, nb->r, (unsigned)dur);
            if (nb->g_interp          != CONFIG_INTERP_NONE) SWEEP_APPEND(" G:%d->%d/%ums",           led->g, nb->g, (unsigned)dur);
            if (nb->b_interp          != CONFIG_INTERP_NONE) SWEEP_APPEND(" B:%d->%d/%ums",           led->b, nb->b, (unsigned)dur);
            break;
        }

        ESP_LOGI(TAG, "  LED[mask=0x%02x] t=%u  freq=%.1fHz duty=%d%% bri=%d%% RGB=(%d,%d,%d)%s%s",
                 (unsigned)led->channel_mask, (unsigned)led->time_ms,
                 led->frequency, led->duty_cycle, led->brightness,
                 led->r, led->g, led->b,
                 sweep_buf[0] ? "  sweep:" : "",
                 sweep_buf);
        return;
    }

    if (e->type == CONFIG_ENTRY_AUDIO) {
        const config_audio_entry_t *au = &e->data.audio;
        uint8_t ch_bit = (uint8_t)(1u << au->channel);
        const config_audio_entry_t *nb = find_next_audio_for_bit(tl, idx, ch_bit);
        if (nb) {
            uint32_t dur = nb->time_ms - au->time_ms;
            if (nb->freq_interp   != CONFIG_INTERP_NONE) SWEEP_APPEND(" freq:%.1f->%.1fHz/%ums", au->frequency, nb->frequency, (unsigned)dur);
            if (nb->pan_interp    != CONFIG_INTERP_NONE) SWEEP_APPEND(" pan:%.0f->%.0f/%ums",    au->pan, nb->pan, (unsigned)dur);
            if (nb->volume_interp != CONFIG_INTERP_NONE) SWEEP_APPEND(" vol:%.0f->%.0f/%ums",    au->volume, nb->volume, (unsigned)dur);
            if (nb->mod_interp    != CONFIG_INTERP_NONE) SWEEP_APPEND(" mod:%.1f->%.1f/%ums",    au->modulation, nb->modulation, (unsigned)dur);
        }

        ESP_LOGI(TAG, "  AUDIO[ch=%u] t=%u  freq=%.1fHz pan=%.0f vol=%.0f mod=%.1f%s%s",
                 (unsigned)au->channel, (unsigned)au->time_ms,
                 au->frequency, au->pan, au->volume, au->modulation,
                 sweep_buf[0] ? "  sweep:" : "",
                 sweep_buf);
        return;
    }

    // Other entry types (BG, future): just note the type.
    ESP_LOGI(TAG, "  OTHER[type=%d] idx=%zu", (int)e->type, idx);

    #undef SWEEP_APPEND
}

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

    // Create timeline execution task pinned to core 0.
    // audio_output_task runs on core 1 at priority 23; keeping timeline_exec
    // on a different core prevents the audio task from preempting us during
    // LED dispatch (which would stall the dispatch for a full I2S buffer, ~23 ms).
    BaseType_t result = xTaskCreatePinnedToCore(
        timeline_execution_task,
        "timeline_exec",
        4096,                   // Stack size
        NULL,                   // Parameters
        4,                      // Priority (lower than audio)
        &timeline_task_handle,
        0                       // Core 0 (audio_output_task is on core 1)
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

        // ---------------------------------------------------------------------------
        // BG line pre-check (Plan 006 Step 3)
        //
        // Detect "BG <url> <pan> <loudness>" BEFORE calling parse_line.
        // Rationale: BG is a session-level annotation, not a timeline entry.
        // It must NOT be added to timeline->entries[] and must NOT route through
        // parse_line's union dispatch.  We intercept it here, parse it into
        // timeline->bg, and skip the rest of the per-entry processing.
        //
        // Detection: skip leading whitespace, then check for two-character keyword
        // "BG" (case-insensitive) followed by whitespace or end-of-string.
        // Comments and empty lines will have already been caught by parse_line's
        // early-return check, but we replicate the skip here to be safe.
        // ---------------------------------------------------------------------------
        {
            const char *bg_scan = line_buffer;
            while (isspace((unsigned char)*bg_scan)) { bg_scan++; }

            if ((bg_scan[0] == 'B' || bg_scan[0] == 'b') &&
                (bg_scan[1] == 'G' || bg_scan[1] == 'g') &&
                (bg_scan[2] == '\0' || isspace((unsigned char)bg_scan[2]))) {

                // Tokenize this line to extract url, pan, loudness
                char bg_line_copy[CONFIG_PARSER_MAX_LINE_LENGTH];
                strncpy(bg_line_copy, line_buffer, sizeof(bg_line_copy) - 1);
                bg_line_copy[sizeof(bg_line_copy) - 1] = '\0';

                // Strip inline comments before tokenizing
                char *bg_comment = strchr(bg_line_copy, '#');
                if (bg_comment) { *bg_comment = '\0'; }

                const char *bg_tokens[8];
                size_t bg_token_count = 0;
                char *bg_tok = strtok(bg_line_copy, " \t");
                while (bg_tok && bg_token_count < sizeof(bg_tokens) / sizeof(bg_tokens[0])) {
                    bg_tokens[bg_token_count++] = bg_tok;
                    bg_tok = strtok(NULL, " \t");
                }

                // bg_tokens[0] is "BG"; pass the rest to parse_bg_line
                if (bg_token_count >= 1) {
                    // Save old URL before parse_bg_line overwrites it (for last-wins warning)
                    char old_url[sizeof(timeline->bg.url)];
                    if (timeline->has_bg) {
                        strncpy(old_url, timeline->bg.url, sizeof(old_url) - 1);
                        old_url[sizeof(old_url) - 1] = '\0';
                    } else {
                        old_url[0] = '\0';
                    }

                    esp_err_t bg_ret = parse_bg_line(bg_tokens + 1, bg_token_count - 1,
                                                     &timeline->bg);
                    if (bg_ret == ESP_OK) {
                        if (timeline->has_bg) {
                            // Last-wins: warn that previous BG URL is being replaced
                            ESP_LOGW(TAG, "Line %zu: multiple BG lines — last one wins "
                                     "(replacing previous URL: %s with: %s)",
                                     line_number, old_url, timeline->bg.url);
                        }
                        timeline->has_bg = true;
                        ESP_LOGI(TAG, "Line %zu: BG parsed — url=%s pan=%.2f loudness=%.2f",
                                 line_number, timeline->bg.url, timeline->bg.pan,
                                 timeline->bg.loudness);
                    } else {
                        ESP_LOGW(TAG, "Line %zu: BG parse failed (%s) — skipping",
                                 line_number, esp_err_to_name(bg_ret));
                    }
                }

                // Advance to next line — do NOT add BG to entries[]
                line_start = line_end;
                if (*line_start == '\n') { line_start++; }
                line_number++;
                continue;
            }
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

    // Copy BG descriptor (added in Plan 006 Step 1)
    persistent_timeline.has_bg = timeline->has_bg;
    if (timeline->has_bg) {
        memcpy(&persistent_timeline.bg, &timeline->bg, sizeof(config_bg_entry_t));
    }

    current_timeline = &persistent_timeline;
    current_entry_index = 0;
    timeline_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Layer 2 (Plan 007 Step 2.1): capture the single canonical T0.
    // Must happen BEFORE the first batch dispatch so that every LED entry's
    // logical_anchor_us is computed relative to the same wall-clock instant.
    transport_origin_us = esp_timer_get_time();
    last_session_origin_us = transport_origin_us;  // diagnostic mirror, survives stop_timeline
    timeline_running = true;
    timeline_loop = loop;

    // ---- BG pre-roll (Plan 006 Step 7) ----------------------------------------
    // Start the background audio stream BEFORE dispatching the t=0 batch so that
    // the BG producer task has a head-start filling the ring buffer.  This means
    // BG audio is already streaming when the first synthesized tone fires.
    //
    // Error handling: bg_player_start() returns ESP_ERR_INVALID_STATE when WiFi is
    // not connected (Step 6 guard).  A warning is logged and the timeline continues
    // without BG — the synthesised channels are unaffected.
    if (persistent_timeline.has_bg) {
        ESP_LOGI(TAG, "BG pre-roll: starting %s pan=%.2f loudness=%.2f",
                 persistent_timeline.bg.url,
                 persistent_timeline.bg.pan,
                 persistent_timeline.bg.loudness);
        esp_err_t bg_ret = bg_player_start(&persistent_timeline.bg);
        if (bg_ret != ESP_OK) {
            ESP_LOGW(TAG, "BG player start failed (%s) — continuing without BG",
                     esp_err_to_name(bg_ret));
        }
    }

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

        // Pre-scan to locate the contiguous slice that belongs to this batch.
        // batch_end is one past the last entry with time == batch_timestamp
        // (or MAX_BATCH_SIZE cap, whichever comes first).
        size_t batch_end = 0;
        for (size_t i = 0; i < timeline->count && batch_end < MAX_BATCH_SIZE; i++) {
            uint32_t entry_time = timeline->entries[i].type == CONFIG_ENTRY_LED ?
                                  timeline->entries[i].data.led.time_ms :
                                  timeline->entries[i].data.audio.time_ms;
            if (entry_time != batch_timestamp) break;
            batch_end = i + 1;
        }

        // Layer 4 (Plan 007 Step 4.1): Two-pass batch dispatch for audio-audio
        // phase coherence.
        //
        // Problem: In a mixed same-timestamp batch (audio + LED), the LED
        // dispatch path calls audio_generator_unlock() (to avoid blocking
        // fill_buffer during LED-side vTaskDelays — see fix_clicks_boundary).
        // When the unlock fires between two audio entries, fill_buffer can
        // preempt and advance some audio channels' Q32 phase before subsequent
        // audio channels are activated, creating a stochastic per-session phase
        // offset in multi-channel audio (e.g., binaural beats).
        //
        // Fix: restructure as three passes over the same entry slice.
        //   Pass 1 — all AUDIO entries while mutex held uninterrupted.  All
        //             channels start at the same sample N with phase pre-
        //             advanced uniformly (Layer 3 Step 3.3).  fill_buffer
        //             cannot preempt here because the mutex is held throughout.
        //   Pass 2 — all LED entries.  Their unlock/relock is harmless now
        //             because all audio channels are already running.
        //   Pass 3 — anything else (CONFIG_ENTRY_BG, future types).
        //
        // Reference: bug_audio_multichannel_phase_2026-06-17.md (Inv 11),
        //            bug_audio_multi_phase_coherence_2026-06-17.md (Inv 15).

        audio_generator_lock();

        // Pass 1: dispatch all AUDIO entries in the batch.  The mutex is held
        // continuously across all audio activations — fill_buffer cannot
        // interleave and advance any channel's phase between activations.
        for (size_t i = 0; i < batch_end; i++) {
            if (timeline->entries[i].type != CONFIG_ENTRY_AUDIO) continue;
            esp_err_t ret = execute_timeline_entry_ctx(&persistent_timeline, i, true);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute audio entry %zu at t=%u: %s",
                         i, batch_timestamp, esp_err_to_name(ret));
            }
            entries_executed++;
        }

        // Pass 2: dispatch all LED entries.  Each LED entry releases and
        // re-acquires the mutex internally (fix_clicks_boundary) — that is
        // safe now because all audio channels are already running from pass 1.
        for (size_t i = 0; i < batch_end; i++) {
            if (timeline->entries[i].type != CONFIG_ENTRY_LED) continue;
            esp_err_t ret = execute_timeline_entry_ctx(&persistent_timeline, i, true);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute LED entry %zu at t=%u: %s",
                         i, batch_timestamp, esp_err_to_name(ret));
            }
            entries_executed++;
        }

        // Pass 3: anything else (CONFIG_ENTRY_BG and any future entry types).
        for (size_t i = 0; i < batch_end; i++) {
            if (timeline->entries[i].type == CONFIG_ENTRY_AUDIO ||
                timeline->entries[i].type == CONFIG_ENTRY_LED) continue;
            esp_err_t ret = execute_timeline_entry_ctx(&persistent_timeline, i, true);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute other entry %zu at t=%u: %s",
                         i, batch_timestamp, esp_err_to_name(ret));
            }
            entries_executed++;
        }

        // Advance current_entry_index to the last entry of this batch so the
        // post-loop "find next timestamp" scan starts from the correct position.
        if (batch_end > 0) {
            current_entry_index = batch_end - 1;
        }

        audio_generator_unlock();

        ESP_LOGI(TAG, "Dispatched batch of %zu entries at t=%u ms", entries_executed, batch_timestamp);
        for (size_t i = 0; i < batch_end; i++) {
            log_entry_summary(&persistent_timeline, i);
        }

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
                // Layer 2 (Plan 007 Step 2.2): absolute-deadline scheduling.
                // target_time is anchored to T0, not to "now", so accumulated
                // dispatch lag in previous batches does not compound into future ones.
                uint64_t target_time = transport_origin_us + (uint64_t)next_time * 1000ULL;
                uint64_t now_us      = timing_engine_get_time_us();
                if (target_time <= now_us) {
                    // Already late — fire immediately by scheduling 1 µs in the future.
                    // This can happen if a batch is particularly slow; the next batch
                    // will execute as soon as the task scheduler yields.
                    target_time = now_us + 1ULL;
                }
                ESP_LOGD(TAG, "Scheduling next batch at T0+%u ms (target_us=%llu, now_us=%llu)",
                         next_time, target_time, now_us);
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
    transport_origin_us = 0;  // Layer 2: reset so stale T0 can't leak into next run

    xSemaphoreGive(timeline_mutex);

    // ---- BG shutdown (Plan 006 Step 7) ----------------------------------------
    // Stop the background audio stream on timeline stop.  bg_player_stop() is
    // safe to call when BG is not active (returns ESP_OK immediately), so we
    // call it unconditionally here rather than guarding with bg_player_is_active().
    // Called AFTER releasing timeline_mutex to avoid holding the mutex during the
    // fade-out + streamer-task join (which may block up to 2 s).
    bg_player_stop();

    // Cancel any pending timeline events in timing engine
    timing_engine_cancel_events_by_type(TIMING_EVENT_TIMELINE);

    // Note: previously freed persistent_timeline here, but that nulled out
    // source_content + entries so /api/report couldn't show the just-finished
    // session. The next config_parser_execute_timeline() already frees the
    // previous timeline before installing the new one (see the free_timeline
    // call at the top of that function), so this stop-time free was redundant
    // for memory hygiene anyway — it just lost diagnostic data on stop.

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

const char *config_parser_get_example(void)
{
    static const char example_content[] =
        "# 30-second demo: LED zones + binaural beat sweep\n"
        "#\n"
        "# LED line formats:\n"
        "#   5-field (legacy):    time freq duty bright channel_mask\n"
        "#   8-field (canonical): time freq duty bright R G B channel_mask\n"
        "#\n"
        "# White (W) channel is not supported. Old 9-field files must be re-saved\n"
        "# as 8-field (drop the W column) or they will fail to parse.\n"
        "#\n"
        "# Channel mask bits (LED): bits 0-7 = channels 1-8 (uint8_t, 0x01-0xFF).\n"
        "#   Bits 0-3 map the original four spec regions:\n"
        "#     1=r1 inner-left, 2=r2 outer-left frame,\n"
        "#     4=r3 outer-right frame, 8=r4 inner-right.\n"
        "#   Bits 4-7 (channels 5-8, masks 0x10-0x80) are valid in the format;\n"
        "#   they have no visible effect unless the channel-map (Kconfig) assigns\n"
        "#   LED pixels to those channels.\n"
        "#   Common values: 9=r1+r4, 15=all four legacy zones, 255=all 8 channels.\n"
        "#\n"
        "# Audio line:  A time freq pan volume mod channel  (channel index 1-16; 0 is rejected)\n"
        "#\n"
        "# Interpolation prefixes:  >value linear sweep,  *value quadratic ease,\n"
        "#                          (no prefix) immediate step\n"
        "\n"
        "# t = 0 — start binaural beat on ch1 (left pan) + ch2 (right pan),\n"
        "# LED inner zones (r1+r4) BLUE at 8 Hz 30% brightness\n"
        "0 8 50 30 0 0 255 9                  # 8-field: channels 1+4, blue\n"
        "A 0 200 -100 60 0 1                  # ch1 audio: 200 Hz, left\n"
        "A 0 208 100 60 0 2                   # ch2 audio: 208 Hz, right (= 8 Hz binaural)\n"
        "\n"
        "# t = 10 s — sweep LED color blue→green and frequency 8 Hz→12 Hz,\n"
        "# binaural beat sweeps from 8 Hz to 12 Hz (carrier stays 200 Hz)\n"
        "10000 >12 50 30 0 >255 >0 9          # linear: freq 8→12, color blue→green\n"
        "A 10000 200 -100 60 0 1               # ch1 holds at 200 Hz\n"
        "A 10000 >212 100 60 0 2              # ch2 sweeps 208→212 Hz\n"
        "\n"
        "# t = 20 s — quadratic ease back to slow alpha-band 8 Hz, color WHITE\n"
        "20000 *8 50 30 *255 *255 *255 9      # quadratic ease freq + color to white\n"
        "A 20000 200 -100 60 0 1\n"
        "A 20000 *208 100 60 0 2\n"
        "\n"
        "# t = 30 s — end: LEDs off, audio fades out linearly\n"
        "30000 0 0 0 0 0 0 15                 # all 4 LED zones off (mask 15)\n"
        "A 30000 200 -100 >0 0 1              # ch1 fade volume to 0\n"
        "A 30000 208 100 >0 0 2               # ch2 fade volume to 0\n";

    return example_content;
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
    // Legacy format:   time freq duty bright channel_mask          (5 tokens)
    // Canonical format: time freq duty bright R G B channel_mask   (8 tokens)
    // 9-token (old RGBW) format is no longer accepted — re-save as 8-field.
    if (token_count != 5 && token_count != 8) {
        ESP_LOGW(TAG, "LED line has %zu tokens; expected 5 (legacy) or 8 (canonical: time freq duty bright R G B mask) — skipping", token_count);
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

    if (token_count == 8) {
        // Canonical 8-token format: R G B carry independent interp prefixes
        float r_f = parse_value_with_interpolation(tokens[4], &led_entry->r_interp);
        float g_f = parse_value_with_interpolation(tokens[5], &led_entry->g_interp);
        float b_f = parse_value_with_interpolation(tokens[6], &led_entry->b_interp);
        led_entry->r = clamp_u8_field(r_f, "R");
        led_entry->g = clamp_u8_field(g_f, "G");
        led_entry->b = clamp_u8_field(b_f, "B");
        led_entry->channel_mask = (uint8_t)atoi(tokens[7]);
    } else {
        // Legacy 5-token format: default RGB = full white; no interp on colors
        led_entry->r = 255;
        led_entry->g = 255;
        led_entry->b = 255;
        led_entry->r_interp = CONFIG_INTERP_NONE;
        led_entry->g_interp = CONFIG_INTERP_NONE;
        led_entry->b_interp = CONFIG_INTERP_NONE;
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

// ---------------------------------------------------------------------------
// parse_bg_line — Plan 006 Step 3
//
// Called from config_parser_parse_content when the first token of a line is
// "BG" (case-insensitive).  The BG keyword token is consumed by the caller;
// this function receives only the REMAINING tokens: [0]=url, [1]=pan, [2]=loudness.
//
// Validation summary:
//   - Exactly 3 tokens required (url, pan, loudness).
//   - URL scheme must be "http://", "https://", or "sdcard://" (shallow check only;
//     no DNS resolution or header fetch — that is Step 6).
//   - pan  clamped to [-100, +100] and stored as pan / 100.0f in bg_entry->pan.
//   - loudness clamped to [0, 100] and stored as loudness / 100.0f in bg_entry->loudness.
//   - URL copied with strncpy into bg_entry->url[256]; null-terminated.
//
// Return:
//   ESP_OK              — bg_entry populated; caller sets timeline->has_bg = true.
//   ESP_ERR_INVALID_ARG — wrong token count or invalid URL scheme; bg_entry unchanged.
// ---------------------------------------------------------------------------
static esp_err_t parse_bg_line(const char *tokens[], size_t token_count,
                                config_bg_entry_t *bg_entry)
{
    if (!tokens || !bg_entry) {
        return ESP_ERR_INVALID_ARG;
    }

    // Must have at least 3 tokens: url, pan, loudness
    if (token_count < 3) {
        ESP_LOGW(TAG, "BG line needs 3 tokens (url pan loudness), got %zu -- skipping", token_count);
        return ESP_ERR_INVALID_ARG;
    }

    // ----- URL: validate scheme (shallow prefix check only) -----
    const char *url = tokens[0];
    bool valid_scheme =
        (strncmp(url, "http://",   7) == 0) ||
        (strncmp(url, "https://",  8) == 0) ||
        (strncmp(url, "sdcard://", 9) == 0);

    if (!valid_scheme) {
        ESP_LOGW(TAG, "BG line: unsupported URL scheme in '%s' "
                 "(must be http://, https://, or sdcard://) -- skipping", url);
        return ESP_ERR_INVALID_ARG;
    }

    // Copy URL; warn and truncate if too long for the buffer
    size_t url_len = strlen(url);
    if (url_len >= sizeof(bg_entry->url)) {
        ESP_LOGW(TAG, "BG URL truncated from %zu to %zu chars",
                 url_len, sizeof(bg_entry->url) - 1);
    }
    strncpy(bg_entry->url, url, sizeof(bg_entry->url) - 1);
    bg_entry->url[sizeof(bg_entry->url) - 1] = '\0';

    // ----- pan: range [-100, +100] -> stored as [-1.0, +1.0] -----
    float pan_raw = (float)atof(tokens[1]);
    if (pan_raw < -100.0f || pan_raw > 100.0f) {
        ESP_LOGW(TAG, "BG pan value %.1f clamped to [-100, +100]", pan_raw);
    }
    bg_entry->pan = fmaxf(-1.0f, fminf(1.0f, pan_raw / 100.0f));

    // ----- loudness: range [0, 100] -> stored as [0.0, 1.0] -----
    float loud_raw = (float)atof(tokens[2]);
    if (loud_raw < 0.0f || loud_raw > 100.0f) {
        ESP_LOGW(TAG, "BG loudness value %.1f clamped to [0, 100]", loud_raw);
    }
    bg_entry->loudness = fmaxf(0.0f, fminf(1.0f, loud_raw / 100.0f));

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
                // Restart timeline — re-capture T0 so the new loop iteration's
                // absolute deadlines are anchored to its own start, not the
                // original run's start.  (Layer 2, Plan 007 Step 2.1)
                current_entry_index = 0;
                timeline_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                transport_origin_us = esp_timer_get_time();
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

        // Pre-scan to locate the contiguous slice that belongs to this batch.
        // batch_end is one past the last entry with time == batch_timestamp
        // (capped at MAX_BATCH_SIZE entries for safety).
        const size_t MAX_BATCH_SIZE = 50; // Safety limit to prevent infinite loops
        size_t batch_end_index = batch_start_index;
        for (size_t i = batch_start_index;
             i < current_timeline->count && (i - batch_start_index) < MAX_BATCH_SIZE;
             i++) {
            uint32_t entry_time = current_timeline->entries[i].type == CONFIG_ENTRY_LED ?
                                  current_timeline->entries[i].data.led.time_ms :
                                  current_timeline->entries[i].data.audio.time_ms;
            if (entry_time != batch_timestamp) break;
            batch_end_index = i + 1;
        }

        // Layer 4 (Plan 007 Step 4.1): Two-pass batch dispatch for audio-audio
        // phase coherence.
        //
        // Problem: In a mixed same-timestamp batch (audio + LED), the LED
        // dispatch path calls audio_generator_unlock() (to avoid blocking
        // fill_buffer during LED-side vTaskDelays — see fix_clicks_boundary).
        // When the unlock fires between two audio entries, fill_buffer can
        // preempt and advance some audio channels' Q32 phase before subsequent
        // audio channels are activated, creating a stochastic per-session phase
        // offset in multi-channel audio (e.g., binaural beats).
        //
        // Fix: restructure as three passes over the same entry slice.
        //   Pass 1 — all AUDIO entries while mutex held uninterrupted.  All
        //             channels start at the same sample N with phase pre-
        //             advanced uniformly (Layer 3 Step 3.3).  fill_buffer
        //             cannot preempt here because the mutex is held throughout.
        //   Pass 2 — all LED entries.  Their unlock/relock is harmless now
        //             because all audio channels are already running.
        //   Pass 3 — anything else (CONFIG_ENTRY_BG, future types).
        //
        // Reference: bug_audio_multichannel_phase_2026-06-17.md (Inv 11),
        //            bug_audio_multi_phase_coherence_2026-06-17.md (Inv 15).

        // Hold audio_gen_mutex across all three passes so fill_buffer cannot
        // interleave between audio activations in pass 1.
        audio_generator_lock();

        // Pass 1: dispatch all AUDIO entries in the batch.  The mutex is held
        // continuously — fill_buffer cannot advance any channel's Q32 phase
        // between activations.
        for (size_t i = batch_start_index; i < batch_end_index; i++) {
            if (current_timeline->entries[i].type != CONFIG_ENTRY_AUDIO) continue;

            ESP_LOGD(TAG, "Pass1/AUDIO entry %zu: time=%u ms", i, batch_timestamp);
            uint64_t entry_start_time = esp_timer_get_time();
            esp_err_t ret = execute_timeline_entry_ctx(current_timeline, i, true);
            uint64_t entry_execution_time = esp_timer_get_time() - entry_start_time;

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute audio entry %zu: %s", i, esp_err_to_name(ret));
            } else {
                ESP_LOGD(TAG, "Audio entry %zu executed in %llu μs (offset +%llu μs from batch start)",
                         i, entry_execution_time, entry_start_time - batch_start_time);
            }
            entries_executed++;
        }

        // Pass 2: dispatch all LED entries.  Each LED entry releases and
        // re-acquires the mutex internally (fix_clicks_boundary) — safe now
        // because all audio channels are already running from pass 1.
        for (size_t i = batch_start_index; i < batch_end_index; i++) {
            if (current_timeline->entries[i].type != CONFIG_ENTRY_LED) continue;

            ESP_LOGD(TAG, "Pass2/LED entry %zu: time=%u ms", i, batch_timestamp);
            uint64_t entry_start_time = esp_timer_get_time();
            esp_err_t ret = execute_timeline_entry_ctx(current_timeline, i, true);
            uint64_t entry_execution_time = esp_timer_get_time() - entry_start_time;

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute LED entry %zu: %s", i, esp_err_to_name(ret));
            } else {
                ESP_LOGD(TAG, "LED entry %zu executed in %llu μs (offset +%llu μs from batch start)",
                         i, entry_execution_time, entry_start_time - batch_start_time);
            }
            entries_executed++;
        }

        // Pass 3: anything else (CONFIG_ENTRY_BG and any future entry types).
        for (size_t i = batch_start_index; i < batch_end_index; i++) {
            if (current_timeline->entries[i].type == CONFIG_ENTRY_AUDIO ||
                current_timeline->entries[i].type == CONFIG_ENTRY_LED) continue;

            ESP_LOGD(TAG, "Pass3/OTHER entry %zu: time=%u ms", i, batch_timestamp);
            uint64_t entry_start_time = esp_timer_get_time();
            esp_err_t ret = execute_timeline_entry_ctx(current_timeline, i, true);
            uint64_t entry_execution_time = esp_timer_get_time() - entry_start_time;

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to execute other entry %zu: %s", i, esp_err_to_name(ret));
            } else {
                ESP_LOGD(TAG, "Other entry %zu executed in %llu μs (offset +%llu μs from batch start)",
                         i, entry_execution_time, entry_start_time - batch_start_time);
            }
            entries_executed++;
        }

        // Advance current_entry_index to the last entry of this batch so the
        // post-loop "find next timestamp" scan starts from the correct position.
        if (batch_end_index > batch_start_index) {
            current_entry_index = batch_end_index - 1;
        }

        audio_generator_unlock();

        uint64_t batch_total_time = esp_timer_get_time() - batch_start_time;
        ESP_LOGI(TAG, "Batch complete: executed %zu entries at timestamp %u ms in %llu μs",
                 entries_executed, batch_timestamp, batch_total_time);
        for (size_t i = batch_start_index; i < batch_end_index; i++) {
            log_entry_summary(current_timeline, i);
        }

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
                // Layer 2 (Plan 007 Step 2.2): absolute-deadline scheduling.
                // target_time = T0 + logical_time_ms * 1000 so that drift from
                // earlier-batch overhead never propagates forward in the timeline.
                uint64_t target_time = transport_origin_us + (uint64_t)next_time * 1000ULL;
                uint64_t now_us      = timing_engine_get_time_us();
                if (target_time <= now_us) {
                    // Already late (batch took longer than the inter-event gap).
                    // Fire as soon as possible — 1 µs gives the scheduler a tick.
                    target_time = now_us + 1ULL;
                }
                ESP_LOGI(TAG, "Scheduling next batch at T0+%u ms (target_us=%llu, now_us=%llu)",
                         next_time, target_time, now_us);

                // Lock-free operation - no mutex needed
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

    // Layer 2 (Plan 007 Step 2.3): compute the transport-clock anchor for this
    // entry.  logical_anchor_us = T0 + entry->time_ms * 1000.
    // When transport_origin_us == 0 (timeline not started, or called from a
    // legacy path), pass 0 so the LED API falls back to its peer-piggyback /
    // now_us behaviour without any change.
    //
    // Layer 3 (Plan 007 Step 3.2): offset the anchor forward by AUDIO_DMA_PIPELINE_LAG_US
    // so the LED cycle-origin coincides with the wall-clock instant that the
    // corresponding audio samples actually emerge from the DAC — not the instant
    // the dispatcher writes them into the I2S DMA ring buffer.
    //
    // Symmetry proof:
    //   T0 = transport_origin_us (captured once at timeline start).
    //   T_audio_dispatch ≈ T0  (audio entry dispatched at the first batch tick).
    //   T_audio_DAC = T_audio_dispatch + DMA_LAG ≈ T0 + DMA_LAG.
    //   T_led_anchor = T0 + entry_time_ms * 1000 + DMA_LAG.
    //   For entry_time_ms == 0: T_led_anchor = T0 + DMA_LAG = T_audio_DAC.  ✓
    //   Audio phase is also pre-advanced by DMA_LAG at channel start (Step 3.3),
    //   so the audio phase-0 sample and the LED cycle-0 onset occur at the same
    //   wall-clock instant within ISR/timer quantization.
    //   Reference: bug_led_audio_proof_of_sync_2026-06-17.md (Inv 14).
    uint64_t logical_anchor_us = (transport_origin_us != 0)
                                 ? transport_origin_us + (uint64_t)led->time_ms * 1000ULL
                                   + AUDIO_DMA_PIPELINE_LAG_US
                                 : 0ULL;

    // ESP_LOGD — same audio_gen_mutex blocking concern as the audio entry log above.
    // The lock is released a few lines below for the LED dispatch itself, but the
    // log line currently fires while the lock is still held.
    #define INTERP_GLYPH(x) ((x) == CONFIG_INTERP_LINEAR ? ">" : (x) == CONFIG_INTERP_QUADRATIC ? "*" : "")
    ESP_LOGD(TAG, "Executing LED entry: t=%u  freq=%s%.1f  duty=%s%d%%  bright=%s%d%%  RGB=(%s%d,%s%d,%s%d)  mask=0x%02x",
             led->time_ms,
             INTERP_GLYPH(led->freq_interp),       led->frequency,
             INTERP_GLYPH(led->duty_interp),       led->duty_cycle,
             INTERP_GLYPH(led->brightness_interp), led->brightness,
             INTERP_GLYPH(led->r_interp), led->r,
             INTERP_GLYPH(led->g_interp), led->g,
             INTERP_GLYPH(led->b_interp), led->b,
             led->channel_mask);
    #undef INTERP_GLYPH

    // Release audio lock around LED dispatch. (Previously the LED path also
    // called audio_led_sync_stop/start whose vTaskDelays would have blocked
    // fill_buffer — that VU pipeline has since been removed, but we still
    // release the lock so any future blocking LED helpers stay safe.)
    if (lock_held) {
        audio_generator_unlock();
    }

    esp_err_t led_ret = ESP_OK;

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

    // ---- Sweep wiring (substep 3.5, bucketed) ----
    // Group bits in channel_mask by their next-entry pointer. Bits sharing the
    // same next entry dispatch together with one sweep_spec; bits with
    // divergent next-entries get their own start_sweep_masked call so each
    // bit's sweep window aligns to its OWN next entry — not the lowest-bit's
    // (the previous code used the first-bit's window for all bits, silently
    // compressing sweeps for bits whose own next-entry sat further out).
    // Bits with no next entry at all fall to the no-sweep (immediate flicker)
    // path via orphan_mask below.
    const config_led_entry_t *bucket_next[NUM_LED_CHANNELS] = {0};
    uint8_t bucket_mask[NUM_LED_CHANNELS] = {0};
    int num_buckets = 0;
    uint8_t orphan_mask = 0;

    for (int bit = 0; bit < NUM_LED_CHANNELS; bit++) {
        uint8_t bitmask = (uint8_t)(1u << bit);
        if (!(led->channel_mask & bitmask)) continue;
        const config_led_entry_t *nb = find_next_led_for_bit(timeline, entry_idx, bitmask);
        if (nb == NULL) {
            orphan_mask |= bitmask;
            continue;
        }
        int found = -1;
        for (int b = 0; b < num_buckets; b++) {
            if (bucket_next[b] == nb) { found = b; break; }
        }
        if (found == -1) {
            bucket_next[num_buckets] = nb;
            bucket_mask[num_buckets] = bitmask;
            num_buckets++;
        } else {
            bucket_mask[found] |= bitmask;
        }
    }

    if (num_buckets > 1) {
        ESP_LOGD(TAG, "LED entry t=%u mask=0x%02x: bits diverge across %d next-entries "
                      "— splitting into per-group sweep installs",
                 led->time_ms, led->channel_mask, num_buckets);
    }

    esp_err_t worst_ret = ESP_OK;
    bool any_sweep = false;  // for end-of-function success log

    // Per-bucket sweep dispatch
    for (int b = 0; b < num_buckets; b++) {
        const config_led_entry_t *next_bit = bucket_next[b];
        uint8_t sub_mask = bucket_mask[b];
        led_sweep_spec_t sweep_spec = {0};
        sweep_spec.duration_ms = next_bit->time_ms - led->time_ms;
        bool bucket_has_sweep = false;

        if (next_bit->freq_interp != CONFIG_INTERP_NONE) {
            sweep_spec.freq_milliHz_start  = (uint32_t)(led->frequency  * 1000.0f);
            sweep_spec.freq_milliHz_target = (uint32_t)(next_bit->frequency * 1000.0f);
            sweep_spec.freq_curve          = interp_to_led_curve(next_bit->freq_interp);
            bucket_has_sweep = true;
        }
        if (next_bit->duty_interp != CONFIG_INTERP_NONE) {
            sweep_spec.duty_start   = led->duty_cycle;
            sweep_spec.duty_target  = next_bit->duty_cycle;
            sweep_spec.duty_curve   = interp_to_led_curve(next_bit->duty_interp);
            bucket_has_sweep = true;
        }
        if (next_bit->brightness_interp != CONFIG_INTERP_NONE) {
            sweep_spec.bright_start  = led->brightness;
            sweep_spec.bright_target = next_bit->brightness;
            sweep_spec.bright_curve  = interp_to_led_curve(next_bit->brightness_interp);
            bucket_has_sweep = true;
        }
        if (next_bit->r_interp != CONFIG_INTERP_NONE) {
            sweep_spec.r_start  = led->r;
            sweep_spec.r_target = next_bit->r;
            sweep_spec.r_curve  = interp_to_led_curve(next_bit->r_interp);
            bucket_has_sweep = true;
        }
        if (next_bit->g_interp != CONFIG_INTERP_NONE) {
            sweep_spec.g_start  = led->g;
            sweep_spec.g_target = next_bit->g;
            sweep_spec.g_curve  = interp_to_led_curve(next_bit->g_interp);
            bucket_has_sweep = true;
        }
        if (next_bit->b_interp != CONFIG_INTERP_NONE) {
            sweep_spec.b_start  = led->b;
            sweep_spec.b_target = next_bit->b;
            sweep_spec.b_curve  = interp_to_led_curve(next_bit->b_interp);
            bucket_has_sweep = true;
        }

        if (!bucket_has_sweep) {
            // This bucket's next entry has no `>` fields — fall to no-sweep path
            orphan_mask |= sub_mask;
            continue;
        }
        any_sweep = true;

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

#ifdef CONFIG_TIMELINE_DEBUG
        ESP_LOGI(TAG, "Timeline LED sweep: mask=0x%02x freq %.2f→%.2f dur=%ums",
                 sub_mask, led->frequency,
                 (float)sweep_spec.freq_milliHz_target / 1000.0f,
                 sweep_spec.duration_ms);
#endif
        esp_err_t br = led_matrix_start_sweep_masked(sub_mask, &sweep_spec, logical_anchor_us);
        if (br != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start LED sweep on mask 0x%02x: %s",
                     sub_mask, esp_err_to_name(br));
            worst_ret = br;
        }
    }

    // Bits with no next entry (or with no swept fields) get the no-sweep path:
    // immediate flicker with the current entry's color. Routing:
    //   - Inactive sub-bits: start_flicker_masked activates and inits cycle state.
    //   - Active sub-bits:   update_flicker_params_masked + set_flicker_color_masked
    //                        (rhythm-preserving, no cycle-origin reset).
    if (orphan_mask != 0) {
        led_matrix_set_flicker_color_masked(orphan_mask, led->r, led->g, led->b);
        esp_err_t fr;
        if (led_matrix_is_flickering_masked(orphan_mask)) {
            fr = led_matrix_update_flicker_params_masked(orphan_mask,
                                                         led->frequency,
                                                         led->duty_cycle,
                                                         led->brightness);
        } else {
            fr = led_matrix_start_flicker_masked(orphan_mask,
                                                 led->frequency,
                                                 led->duty_cycle,
                                                 led->brightness,
                                                 logical_anchor_us);
        }
        if (fr != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start/update LED flicker on mask 0x%02x: %s",
                     orphan_mask, esp_err_to_name(fr));
            if (worst_ret == ESP_OK) worst_ret = fr;
        }
    }

    if (worst_ret != ESP_OK) {
        led_ret = worst_ret;
    }

    if (led_ret == ESP_OK) {
        ESP_LOGD(TAG, "LED mask 0x%02x started: %.1f Hz, %d%% duty, %d%% brightness %s",
                 led->channel_mask, led->frequency, led->duty_cycle, led->brightness,
                 any_sweep ? "(with sweep)" : "");
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

const char *config_parser_get_loaded_source(void)
{
    return persistent_timeline.source_content;
}

uint64_t config_parser_get_session_origin_us(void)
{
    // Return the diagnostic mirror so /api/report can still correlate
    // button-press timestamps to the just-finished session after stop.
    return last_session_origin_us;
}
