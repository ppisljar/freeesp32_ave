#include "config_parser.h"
#include "audio_manager.h"
#include "led_strip.h"
#include "led_matrix_example.h"
#include "mod_engine.h"
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
static void  parse_mod_extras(const char *str, float *out_end, float *out_period_ms);

// v2 skip sentinel: a lone "-" means "leave this field unchanged". A negative
// NUMBER like "-50" has more than one char, so pan/etc. still parse normally.
static inline bool tok_is_dash(const char *t) {
    return t && t[0] == '-' && t[1] == '\0';
}
// Degrees wrap to 0..359; percent clamps to 0..100. Used by the pulse fields on
// both line types so start and mod-end values are bounded the same way.
static inline float wrap_deg(float d) {
    float r = fmodf(d, 360.0f);
    return (r < 0.0f) ? r + 360.0f : r;
}
static inline float clamp_pct(float v) {
    return (v < 0.0f) ? 0.0f : (v > 100.0f ? 100.0f : v);
}
// Parse a v2 pulse-field value whose field is an ENUM, so an interp glyph is
// meaningless and deliberately dropped. Compound pulse fields (phase, attack,
// audio duty) must NOT use this — they capture their interp like the core
// fields do, or a ramp silently plays as its start value.
static inline float parse_v2_enum(const char *t) {
    config_interpolation_t dummy;
    return parse_value_with_interpolation(t, &dummy);
}
// Capture a compound pulse field: value + interp, plus the end/period extras
// when the interp is one of the periodic modulations.
static inline float parse_v2_cell(const char *t, config_interpolation_t *interp,
                                  float *mod_end, float *mod_period_ms) {
    float v = parse_value_with_interpolation(t, interp);
    if (config_interp_is_modulation(*interp)) {
        parse_mod_extras(t, mod_end, mod_period_ms);
    }
    return v;
}
// Parse a jitter token "amp" or "amp:period_ms". Returns amp; *period_ms updated
// only when a ':period' suffix is present (else left at the caller's default).
static inline float parse_jitter_token(const char *t, float *period_ms) {
    config_interpolation_t dummy;
    float amp = parse_value_with_interpolation(t, &dummy);
    const char *colon = strchr(t, ':');
    if (colon) *period_ms = (float)atof(colon + 1);
    return amp;
}
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
            // NEW CONVENTION: prefix lives on THIS entry; target value lives on next.
            if (led->freq_interp       != CONFIG_INTERP_NONE) SWEEP_APPEND(" freq:%.1f->%.1fHz/%ums",  led->frequency, nb->frequency, (unsigned)dur);
            if (led->duty_interp       != CONFIG_INTERP_NONE) SWEEP_APPEND(" duty:%d->%d%%/%ums",      led->duty_cycle, nb->duty_cycle, (unsigned)dur);
            if ((led->brightness_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(led->brightness_interp)) SWEEP_APPEND(" bri:%d->%d%%/%ums",       led->brightness, nb->brightness, (unsigned)dur);
            if (led->r_interp          != CONFIG_INTERP_NONE) SWEEP_APPEND(" R:%d->%d/%ums",           led->r, nb->r, (unsigned)dur);
            if (led->g_interp          != CONFIG_INTERP_NONE) SWEEP_APPEND(" G:%d->%d/%ums",           led->g, nb->g, (unsigned)dur);
            if (led->b_interp          != CONFIG_INTERP_NONE) SWEEP_APPEND(" B:%d->%d/%ums",           led->b, nb->b, (unsigned)dur);
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
            // NEW CONVENTION: prefix lives on THIS entry; target value lives on next.
            if (au->freq_interp   != CONFIG_INTERP_NONE) SWEEP_APPEND(" freq:%.1f->%.1fHz/%ums", au->frequency, nb->frequency, (unsigned)dur);
            if (au->pan_interp    != CONFIG_INTERP_NONE) SWEEP_APPEND(" pan:%.0f->%.0f/%ums",    au->pan, nb->pan, (unsigned)dur);
            if (au->volume_interp != CONFIG_INTERP_NONE) SWEEP_APPEND(" vol:%.0f->%.0f/%ums",    au->volume, nb->volume, (unsigned)dur);
            if (au->mod_interp    != CONFIG_INTERP_NONE) SWEEP_APPEND(" mod:%.1f->%.1f/%ums",    au->modulation, nb->modulation, (unsigned)dur);
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

    /* Stable-sort entries by timestamp. The timeline EXECUTOR (execute_timeline's
     * batch pre-scan) assumes same-timestamp entries are CONTIGUOUS and that
     * timestamps are non-decreasing in array order — but the parser preserves
     * FILE order, and a .ledc authored grouped-by-layer (e.g. one channel's t=0
     * AND t=60000 rows listed before another channel's t=0 row) violates that:
     * the t=0 batch pre-scan hits the t=60000 row, breaks, and dispatches only
     * 1 of N same-time entries — so the rest of the t=0 layer (LED flash, drum
     * carriers) never fire. Stable insertion sort (count<=100, runs once at
     * parse) makes any authoring order correct while preserving the relative
     * order of entries that share a timestamp. */
    for (size_t i = 1; i < timeline->count; i++) {
        config_entry_t key = timeline->entries[i];
        uint32_t key_t = (key.type == CONFIG_ENTRY_LED)   ? key.data.led.time_ms
                       : (key.type == CONFIG_ENTRY_AUDIO) ? key.data.audio.time_ms : 0;
        size_t j = i;
        while (j > 0) {
            const config_entry_t *p = &timeline->entries[j - 1];
            uint32_t pt = (p->type == CONFIG_ENTRY_LED)   ? p->data.led.time_ms
                        : (p->type == CONFIG_ENTRY_AUDIO) ? p->data.audio.time_ms : 0;
            if (pt <= key_t) break;   // '<=' keeps equal-timestamp order stable
            timeline->entries[j] = timeline->entries[j - 1];
            j--;
        }
        timeline->entries[j] = key;
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

/* ---------------------------------------------------------------------------
 * BG push:// prime gate
 * ---------------------------------------------------------------------------
 * When a session's BG is a push:// clip, the browser-pushed audio (often with
 * baked speech) must be sample-aligned to session t=0. So the timeline start is
 * DEFERRED: bg_player buffers the pushed audio but holds it silent
 * (bg_player_push_hold) while a 100 ms poll waits until the ring holds
 * BG_PRIME_THRESHOLD_MS (or BG_PRIME_TIMEOUT_MS elapses — start anyway). Then the
 * timeline task latches T0, releases the hold, and dispatches t=0 in one breath.
 */
// Prebuffer depth before releasing the timeline at t=0. Raised 500 -> 2000 ms:
// during the hold the consumer does NOT drain the ring, so it fills deep (well
// past the steady-state TCP-window limit) — a 2 s cushion lets the pushed BG
// audio ride out the ~60 s WiFi block-ACK (DELBA) dropouts that were underrunning
// the ring. Reachable well within the 5 s timeout at normal WiFi throughput;
// costs ~1-2 s extra startup (hidden behind the Home pre-roll delay).
#define BG_PRIME_THRESHOLD_MS   2000u
#define BG_PRIME_TIMEOUT_MS     5000u
static volatile bool      bg_prime_pending = false;
static uint32_t           bg_prime_deadline_ms = 0;
static esp_timer_handle_t bg_prime_poll_timer = NULL;

static inline bool bg_is_push_url(const char *url)
{
    return url && strncmp(url, "push://", 7) == 0;
}

// esp_timer callback: nudge the timeline task to re-check the prime condition.
static void bg_prime_poll_cb(void *arg)
{
    (void)arg;
    if (timeline_task_handle) {
        xTaskNotifyGive(timeline_task_handle);
    }
}

static void bg_prime_poll_start(void)
{
    if (!bg_prime_poll_timer) {
        const esp_timer_create_args_t args = {
            .callback = bg_prime_poll_cb,
            .name = "bg_prime_poll",
        };
        if (esp_timer_create(&args, &bg_prime_poll_timer) != ESP_OK) {
            bg_prime_poll_timer = NULL;
        }
    }
    if (bg_prime_poll_timer) {
        esp_timer_start_periodic(bg_prime_poll_timer, 100000ULL);   // 100 ms
    }
}

static void bg_prime_poll_stop(void)
{
    if (bg_prime_poll_timer) {
        esp_timer_stop(bg_prime_poll_timer);
    }
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

    // ---- BG push:// prime gate ------------------------------------------------
    // For a push:// BG the browser-pushed audio must be sample-aligned to t=0, so
    // defer the whole start (T0 latch + t=0 dispatch) to the timeline task, which
    // fires it once the pushed audio has primed (see the poll branch in
    // timeline_execution_task). current_entry_index = (size_t)-1 so the task's ++
    // yields 0 and it dispatches the t=0 batch itself. bg_player_push_hold()
    // buffers the push but keeps it silent until that exact moment.
    if (persistent_timeline.has_bg && bg_is_push_url(persistent_timeline.bg.url)) {
        timeline_loop = loop;
        timeline_running = true;
        current_entry_index = (size_t)-1;
        bg_prime_pending = true;
        bg_prime_deadline_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) + BG_PRIME_TIMEOUT_MS;
        bg_player_push_hold();
        bg_prime_poll_start();
        ESP_LOGI(TAG, "BG push:// '%s' — deferring timeline start until %ums buffered (or %ums)",
                 persistent_timeline.bg.url, (unsigned)BG_PRIME_THRESHOLD_MS,
                 (unsigned)BG_PRIME_TIMEOUT_MS);
        return ESP_OK;
    }

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

    // Cancel a pending BG push:// prime gate (stopped before it primed).
    if (bg_prime_pending) {
        bg_prime_pending = false;
        bg_prime_poll_stop();
        bg_player_push_release();
    }

    timeline_running = false;
    current_timeline = NULL;
    current_entry_index = 0;
    transport_origin_us = 0;  // Layer 2: reset so stale T0 can't leak into next run

    xSemaphoreGive(timeline_mutex);

    // ---- BG shutdown (Plan 006 Step 7) ----------------------------------------
    // Stop the background audio stream on timeline stop.  bg_player_stop_async()
    // is safe to call when BG is not active (returns ESP_OK immediately), so we
    // call it unconditionally here rather than guarding with bg_player_is_active().
    // Called AFTER releasing timeline_mutex to avoid holding the mutex during the
    // fade-out + streamer-task join.
    //
    // Async variant caps producer-task join at ~200 ms (vs 2 s for the blocking
    // variant) so both the /api/stop endpoint and play_config_handler's implicit
    // stop-before-play return quickly. Worst case: a slow HTTP connection forces
    // a producer force-delete that leaks one socket — acceptable for snappy UX.
    bg_player_stop_async();

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

/* ============================================================================
 *  config_parser_apply_patch — additive update (no full timeline restart)
 * ============================================================================
 *
 * Patch semantics (different from main-timeline .ledc):
 *
 *   time_ms == 0:
 *     Every field is set instantly via the corresponding per-field setter.
 *     No animation, no sweep.
 *
 *   time_ms > 0, field has NO prefix:
 *     Sweep from the engine's CURRENT value to the patch's value, over
 *     time_ms (LINEAR curve). The UI does not need to know the current
 *     value — the dispatcher reads it from the engine under the same lock
 *     that fill_buffer / the LED ISR observe, so the sweep start is
 *     guaranteed consistent with what the audio/LED stages will see next.
 *
 *   time_ms > 0, field has `>` or `*` prefix:
 *     Same as above (animate current → value over time_ms), but use the
 *     prefix's curve (LINEAR for `>`, QUADRATIC for `*`). Multi-line patches
 *     with cross-entry `>` wiring are not supported yet — each line is
 *     dispatched independently; `>` in a single-line patch behaves like
 *     no-prefix with LINEAR curve.
 *
 *   field has modulation prefix (`^~/\_`):
 *     Arm modulation via mod_engine_start_audio / mod_engine_start_led.
 *     The field's value becomes the modulation start; the parser's per-
 *     field `_mod_end` and `_mod_period_ms` carry the rest.
 *
 * Dispatch is synchronous — all entries fire immediately at patch-receive
 * time. There is no future-scheduling (esp_timer) inside the patch; the UI
 * sends one patch per slider event. Multi-line patches still parse and each
 * line is processed in order against current state.
 *
 * Old comment kept below for completeness:
 *
 * Architecture: a patch is a tiny self-contained timeline that runs in
 * parallel to the main timeline (if any). It owns its own heap-allocated
 * config_timeline_t. Entries at t=0 dispatch synchronously inside the patch
 * call itself; entries at t>0 schedule a one-shot esp_timer whose callback
 * dispatches the entry and decrements the patch's refcount. When refcount
 * reaches zero, the patch's memory is freed.
 *
 * Up to PATCH_MAX_PENDING patches can be live simultaneously (each may have
 * multiple pending entries). Past that, the oldest patch is force-completed
 * (its remaining entries dispatch immediately) to free a slot. This bound is
 * a safety net; the UI sends one patch per slider change, so steady-state is
 * 0–2 patches in flight.
 */

#define PATCH_MAX_ENTRIES_PER    32   /* per-call cap; safety net for malformed input */

/* Resolve curve preference from a parsed interp value + the patch's time_ms.
 *   - time_ms == 0  → always NONE (instant set, never sweep)
 *   - QUADRATIC (*) → QUADRATIC sweep
 *   - LINEAR (>)   → LINEAR sweep
 *   - NONE + time>0 → LINEAR sweep (patch default; UI sliders animate
 *                     smoothly from current value)
 *   - modulation prefixes return NONE here (caller routes them separately
 *     to mod_engine_start_*). */
static led_interp_t patch_field_curve_led(config_interpolation_t interp, uint32_t time_ms)
{
    if (config_interp_is_modulation(interp)) return LED_INTERP_NONE;
    if (time_ms == 0)                        return LED_INTERP_NONE;
    if (interp == CONFIG_INTERP_QUADRATIC)   return LED_INTERP_QUADRATIC;
    return LED_INTERP_LINEAR;
}

static audio_gen_sweep_type_t patch_field_curve_audio(config_interpolation_t interp, uint32_t time_ms)
{
    /* Same logic as patch_field_curve_led but for audio. NONE is encoded as
     * AUDIO_GEN_SWEEP_NONE; caller checks for that and uses set_param_locked
     * instead of start_sweep_locked. */
    (void)interp; (void)time_ms;
    return (time_ms == 0) ? AUDIO_GEN_SWEEP_NONE
           : (interp == CONFIG_INTERP_QUADRATIC) ? AUDIO_GEN_SWEEP_QUADRATIC
           : AUDIO_GEN_SWEEP_LINEAR;
}

/* Dispatch one audio patch entry. Caller holds audio_gen_mutex (via
 * audio_generator_lock) so we can read current values + start sweeps under
 * the same lock fill_buffer observes — guarantees the sweep start matches
 * what the synth stage sees on its next sample. */
static void apply_patch_audio_entry(const config_audio_entry_t *e)
{
    if (e->channel >= NUM_AUDIO_CHANNELS) {
        ESP_LOGW(TAG, "patch: audio channel %d out of range", e->channel);
        return;
    }
    if (!audio_generator_is_active_locked(e->channel)) {
        /* Channel not currently playing — start it from scratch with the
         * patch's values. No "animate from current" because there is no
         * current value to read. */
        audio_gen_params_t p = {0};
        p.frequency   = e->frequency;
        p.frequency_r = e->frequency_r;
        p.pan           = e->pan       / 100.0f;
        p.amplitude     = e->volume    / 100.0f;
        p.mod_frequency = e->modulation;
        p.wave_type   = (audio_wave_type_t)e->wave_type;
        audio_generator_start_channel_locked(e->channel, &p);
        return;
    }

    /* Waveform isn't sweepable, so the field-loop below never touches it. If the
     * patch explicitly carried a wave_type (8th field — e.g. the noise UI
     * switching White↔Pink↔Brown on an already-running channel), apply it
     * directly. Patches without it (the 6-field tone sliders) leave it alone, so
     * a square/saw tone from a .ledc file isn't reset to sine by a slider nudge. */
    if (e->has_wave_type) {
        audio_generator_set_wave_type_locked(e->channel, (audio_wave_type_t)e->wave_type);
    }

    /* freq_r (7th field) — right-ear binaural carrier. Not an AUDIO_PARAM_* sweep
     * field, so it's set directly (instant) when the patch carried it. Guard on the
     * present bit so a plain 6-field slider nudge never zeroes an existing binaural
     * pair. freq_r == 0 (explicitly present) collapses the channel back to mono. */
    if (e->present & AUD_SET_FREQR) {
        audio_generator_set_freq_r_locked(e->channel, e->frequency_r);
    }

    const uint32_t dur_ms = e->time_ms;
    const uint64_t dur_samples = ((uint64_t)dur_ms * AUDIO_GEN_SAMPLE_RATE) / 1000ULL;

    /* For each field: pick the dispatch path based on its interp + dur_ms. */
    struct {
        audio_param_t        param;
        float                target;         /* in INTERNAL units (Hz / -1..1 / 0..1) */
        config_interpolation_t interp;
    } fields[] = {
        { AUDIO_PARAM_FREQUENCY, e->frequency,          e->freq_interp   },
        { AUDIO_PARAM_PAN,       e->pan    / 100.0f,    e->pan_interp    },
        { AUDIO_PARAM_AMPLITUDE, e->volume / 100.0f,    e->volume_interp },
        { AUDIO_PARAM_MOD_FREQ,  e->modulation,         e->mod_interp    },
    };

    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        config_interpolation_t interp = fields[i].interp;
        if (config_interp_is_modulation(interp)) {
            /* Modulation prefix — dispatch via mod_engine, NOT as a sweep.
             * mod_engine.c takes care of param mapping + UI-unit scaling. */
            int wave = mod_wave_from_interp(interp);
            /* Per-field _mod_end + _mod_period_ms live on the entry. */
            float mod_end = 0.0f; uint32_t mod_period = 0;
            switch (fields[i].param) {
                case AUDIO_PARAM_FREQUENCY:
                    mod_end = e->freq_mod_end;
                    mod_period = (uint32_t)e->freq_mod_period_ms; break;
                case AUDIO_PARAM_PAN:
                    mod_end = e->pan_mod_end;
                    mod_period = (uint32_t)e->pan_mod_period_ms; break;
                case AUDIO_PARAM_AMPLITUDE:
                    mod_end = e->vol_mod_end;
                    mod_period = (uint32_t)e->vol_mod_period_ms; break;
                case AUDIO_PARAM_MOD_FREQ:
                    mod_end = e->mod_mod_end;
                    mod_period = (uint32_t)e->mod_mod_period_ms; break;
                default: break;
            }
            mod_audio_field_t mf = (fields[i].param == AUDIO_PARAM_FREQUENCY) ? MOD_AUDIO_FREQ
                                 : (fields[i].param == AUDIO_PARAM_PAN)       ? MOD_AUDIO_PAN
                                 : (fields[i].param == AUDIO_PARAM_AMPLITUDE) ? MOD_AUDIO_VOLUME
                                 :                                              MOD_AUDIO_MOD;
            mod_engine_start_audio(e->channel, mf, (mod_wave_t)wave,
                                   fields[i].target * (fields[i].param == AUDIO_PARAM_PAN || fields[i].param == AUDIO_PARAM_AMPLITUDE ? 100.0f : 1.0f),
                                   mod_end, mod_period);
            continue;
        }

        audio_gen_sweep_type_t curve = patch_field_curve_audio(interp, dur_ms);
        if (curve == AUDIO_GEN_SWEEP_NONE) {
            /* Instant set — use set_param_locked (which also cancels any
             * active sweep on this param). */
            audio_generator_set_param_locked(e->channel, fields[i].param, fields[i].target);
        } else {
            /* Animated patch — read current value as the sweep start. */
            float current = 0.0f;
            audio_generator_get_param_locked(e->channel, fields[i].param, &current);
            audio_generator_start_sweep_locked(e->channel, fields[i].param,
                                               current, fields[i].target,
                                               dur_samples, curve);
        }
    }
}

/* Dispatch one LED patch entry. Reads current values from the FIRST channel
 * in the mask (typically a single-bit mask for UI-driven patches). For
 * multi-bit masks the same start value is broadcast across all matched
 * channels — acceptable for now since the UI sends one channel per patch. */
static void apply_patch_led_entry(const config_led_entry_t *e)
{
    uint8_t mask = e->channel_mask;
    if (mask == 0) return;
    const uint32_t dur_ms = e->time_ms;

    /* Snapshot current LED state so we can pick a sensible sweep start.
     * Snapshot path takes s_flicker_mux briefly — fine from task context. */
    led_matrix_channel_snapshot_t snap[NUM_LED_CHANNELS] = {0};
    led_matrix_get_snapshot(snap, NUM_LED_CHANNELS);
    int first_ch = -1;
    for (int i = 0; i < NUM_LED_CHANNELS; i++) {
        if (mask & (1u << i)) { first_ch = i; break; }
    }
    if (first_ch < 0) return;

    /* Build a sweep spec. NONE curve on a field = "instant set to target,
     * start value ignored" per the spec contract. So for fields the patch
     * just wants set immediately (time_ms == 0 or no prefix + time_ms == 0),
     * we still go through start_sweep_masked with curve=NONE — one API call,
     * uniform path. */
    led_sweep_spec_t spec = {
        .freq_milliHz_start  = (uint32_t)(snap[first_ch].freq * 1000.0f),
        .freq_milliHz_target = (uint32_t)(e->frequency       * 1000.0f),
        .duty_start          = snap[first_ch].duty,
        .duty_target         = e->duty_cycle,
        .bright_start        = snap[first_ch].brightness,
        .bright_target       = e->brightness,
        .r_start             = snap[first_ch].r,
        .r_target            = e->r,
        .g_start             = snap[first_ch].g,
        .g_target            = e->g,
        .b_start             = snap[first_ch].b,
        .b_target            = e->b,
        .freq_curve   = patch_field_curve_led(e->freq_interp,       dur_ms),
        .duty_curve   = patch_field_curve_led(e->duty_interp,       dur_ms),
        .bright_curve = patch_field_curve_led(e->brightness_interp, dur_ms),
        .r_curve      = patch_field_curve_led(e->r_interp,          dur_ms),
        .g_curve      = patch_field_curve_led(e->g_interp,          dur_ms),
        .b_curve      = patch_field_curve_led(e->b_interp,          dur_ms),
        /* Pulse shape. Start == target: the live-patch path has no "next entry"
         * to ramp toward, so these hold at the patch's literal. The timeline
         * path below wires real start→target pairs. */
        .phase_start  = e->phase_deg,
        .phase_target = e->phase_deg,
        .attack_start = e->attack_ms,
        .attack_target= e->attack_ms,
        .phase_curve  = LED_INTERP_NONE,
        .attack_curve = LED_INTERP_NONE,
        .phase_set    = (e->present & LED_SET_PHASE)  != 0,
        .attack_set   = (e->present & LED_SET_ATTACK) != 0,
        .duration_ms  = dur_ms,
    };

    /* If the channel isn't currently flickering AND the patch has a non-zero
     * frequency, start it; otherwise just push the sweep spec. */
    if (!led_matrix_is_flickering_masked(mask) && e->frequency > 0.0f) {
        led_matrix_start_flicker_masked(mask, e->frequency, e->duty_cycle,
                                        e->brightness, 0);
        led_matrix_set_flicker_color_masked(mask, e->r, e->g, e->b);
    } else {
        led_matrix_start_sweep_masked(mask, &spec, 0);
    }

    /* Handle modulation prefixes on each field — dispatched independently
     * of the sweep above. The mod_engine writes into the LED ISR slot.
     * Fields without a modulation prefix get no mod change (existing
     * modulation on that field, if any, is left alone). */
    if (config_interp_is_modulation(e->freq_interp)) {
        int wave = mod_wave_from_interp(e->freq_interp);
        mod_engine_start_led(mask, MOD_LED_FREQ, (mod_wave_t)wave,
                             e->frequency, e->freq_mod_end, (uint32_t)e->freq_mod_period_ms);
    }
    if (config_interp_is_modulation(e->duty_interp)) {
        int wave = mod_wave_from_interp(e->duty_interp);
        mod_engine_start_led(mask, MOD_LED_DUTY, (mod_wave_t)wave,
                             e->duty_cycle, e->duty_mod_end, e->duty_mod_period_ms);
    }
    if (config_interp_is_modulation(e->brightness_interp)) {
        int wave = mod_wave_from_interp(e->brightness_interp);
        mod_engine_start_led(mask, MOD_LED_BRIGHT, (mod_wave_t)wave,
                             e->brightness, e->bright_mod_end, e->bright_mod_period_ms);
    }
    if ((e->present & LED_SET_PHASE) && config_interp_is_modulation(e->phase_interp)) {
        mod_engine_start_led(mask, MOD_LED_PHASE, (mod_wave_t)mod_wave_from_interp(e->phase_interp),
                             (float)e->phase_deg, (float)e->phase_mod_end, e->phase_mod_period_ms);
    }
    if ((e->present & LED_SET_ATTACK) && config_interp_is_modulation(e->attack_interp)) {
        mod_engine_start_led(mask, MOD_LED_ATTACK, (mod_wave_t)mod_wave_from_interp(e->attack_interp),
                             (float)e->attack_ms, (float)e->attack_mod_end, e->attack_mod_period_ms);
    }
    if (config_interp_is_modulation(e->r_interp)) {
        int wave = mod_wave_from_interp(e->r_interp);
        mod_engine_start_led(mask, MOD_LED_R, (mod_wave_t)wave,
                             e->r, e->r_mod_end, e->r_mod_period_ms);
    }
    if (config_interp_is_modulation(e->g_interp)) {
        int wave = mod_wave_from_interp(e->g_interp);
        mod_engine_start_led(mask, MOD_LED_G, (mod_wave_t)wave,
                             e->g, e->g_mod_end, e->g_mod_period_ms);
    }
    if (config_interp_is_modulation(e->b_interp)) {
        int wave = mod_wave_from_interp(e->b_interp);
        mod_engine_start_led(mask, MOD_LED_B, (mod_wave_t)wave,
                             e->b, e->b_mod_end, e->b_mod_period_ms);
    }
}

esp_err_t config_parser_apply_patch(const char *content, size_t content_length)
{
    if (!content || content_length == 0) return ESP_ERR_INVALID_ARG;

    /* Parse into a temporary on-stack timeline. Use the same parser as
     * /api/play-config so the patch grammar is identical to .ledc. */
    config_timeline_t parsed = {0};
    esp_err_t ret = config_parser_parse_content(content, content_length, &parsed);
    if (ret != ESP_OK) {
        config_parser_free_timeline(&parsed);
        ESP_LOGW(TAG, "apply_patch: parse failed");
        return ret;
    }
    if (parsed.count == 0) {
        config_parser_free_timeline(&parsed);
        return ESP_ERR_INVALID_ARG;
    }
    if (parsed.count > PATCH_MAX_ENTRIES_PER) {
        config_parser_free_timeline(&parsed);
        ESP_LOGW(TAG, "apply_patch: too many entries (%zu > %d)",
                 parsed.count, PATCH_MAX_ENTRIES_PER);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Dispatch each entry synchronously. Take audio_generator_lock ONCE
     * around the whole batch so all audio dispatches see a consistent view
     * (and so we don't keep flipping the mutex per entry, which would let
     * fill_buffer interleave). LED entries take their own spinlock per call
     * — no need to coordinate. */
    int n_audio = 0, n_led = 0, n_other = 0;
    audio_generator_lock();
    for (size_t i = 0; i < parsed.count; i++) {
        switch (parsed.entries[i].type) {
            case CONFIG_ENTRY_AUDIO:
                apply_patch_audio_entry(&parsed.entries[i].data.audio);
                n_audio++;
                break;
            case CONFIG_ENTRY_LED:
                apply_patch_led_entry(&parsed.entries[i].data.led);
                n_led++;
                break;
            default:
                n_other++;
                break;
        }
    }
    audio_generator_unlock();

    config_parser_free_timeline(&parsed);
    ESP_LOGI(TAG, "apply_patch: %d audio, %d LED, %d ignored", n_audio, n_led, n_other);
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
    // Format v2 (ledc_format.md): 8..12 = canonical + optional [env phase attack
    // jitter]. Any field may be '-' (leave unchanged): the present-bit stays clear
    // and the execute path substitutes the channel's live value so the write is a
    // no-op. 9-token old RGBW is gone; a v2 line just has more trailing tokens.
    if (token_count != 5 && (token_count < 8 || token_count > 12)) {
        ESP_LOGW(TAG, "LED line has %zu tokens; expected 5 (legacy) or 8..12 (canonical + [env phase attack jitter]) — skipping", token_count);
        return ESP_ERR_INVALID_ARG;
    }

    memset(led_entry, 0, sizeof(config_led_entry_t));
    led_entry->present = 0;

    // time — always plain integer
    led_entry->time_ms = (uint32_t)atol(tokens[0]);

    // freq, duty, brightness — each may be '-' (leave unchanged) or a value with an
    // optional interp prefix. '-' clears the present bit (execute substitutes live).
    if (!tok_is_dash(tokens[1])) {
        led_entry->frequency = parse_value_with_interpolation(tokens[1], &led_entry->freq_interp);
        led_entry->present |= LED_SET_FREQ;
        if (config_interp_is_modulation(led_entry->freq_interp))
            parse_mod_extras(tokens[1], &led_entry->freq_mod_end, &led_entry->freq_mod_period_ms);
    }
    if (!tok_is_dash(tokens[2])) {
        led_entry->duty_cycle = (uint8_t)parse_value_with_interpolation(tokens[2], &led_entry->duty_interp);
        led_entry->present |= LED_SET_DUTY;
        if (config_interp_is_modulation(led_entry->duty_interp)) {
            float end_f, period_f;
            parse_mod_extras(tokens[2], &end_f, &period_f);
            led_entry->duty_mod_end       = (uint8_t)end_f;
            led_entry->duty_mod_period_ms = (uint32_t)period_f;
        }
    }
    if (!tok_is_dash(tokens[3])) {
        led_entry->brightness = (uint8_t)parse_value_with_interpolation(tokens[3], &led_entry->brightness_interp);
        led_entry->present |= LED_SET_BRIGHT;
        if (config_interp_is_modulation(led_entry->brightness_interp)) {
            float end_f, period_f;
            parse_mod_extras(tokens[3], &end_f, &period_f);
            led_entry->bright_mod_end       = (uint8_t)end_f;
            led_entry->bright_mod_period_ms = (uint32_t)period_f;
        }
    }

    if (token_count >= 8) {
        // Canonical: R G B (each '-'-able) then channel_mask.
        if (!tok_is_dash(tokens[4])) {
            led_entry->r = clamp_u8_field(parse_value_with_interpolation(tokens[4], &led_entry->r_interp), "R");
            led_entry->present |= LED_SET_R;
            if (config_interp_is_modulation(led_entry->r_interp)) {
                float end_f, period_f; parse_mod_extras(tokens[4], &end_f, &period_f);
                led_entry->r_mod_end = clamp_u8_field(end_f, "R^end");
                led_entry->r_mod_period_ms = (uint32_t)period_f;
            }
        }
        if (!tok_is_dash(tokens[5])) {
            led_entry->g = clamp_u8_field(parse_value_with_interpolation(tokens[5], &led_entry->g_interp), "G");
            led_entry->present |= LED_SET_G;
            if (config_interp_is_modulation(led_entry->g_interp)) {
                float end_f, period_f; parse_mod_extras(tokens[5], &end_f, &period_f);
                led_entry->g_mod_end = clamp_u8_field(end_f, "G^end");
                led_entry->g_mod_period_ms = (uint32_t)period_f;
            }
        }
        if (!tok_is_dash(tokens[6])) {
            led_entry->b = clamp_u8_field(parse_value_with_interpolation(tokens[6], &led_entry->b_interp), "B");
            led_entry->present |= LED_SET_B;
            if (config_interp_is_modulation(led_entry->b_interp)) {
                float end_f, period_f; parse_mod_extras(tokens[6], &end_f, &period_f);
                led_entry->b_mod_end = clamp_u8_field(end_f, "B^end");
                led_entry->b_mod_period_ms = (uint32_t)period_f;
            }
        }
        led_entry->channel_mask = (uint8_t)atoi(tokens[7]);
    } else {
        // Legacy 5-token format: default RGB = full white (explicitly set).
        led_entry->r = 255;
        led_entry->g = 255;
        led_entry->b = 255;
        led_entry->present |= LED_SET_R | LED_SET_G | LED_SET_B;
        led_entry->channel_mask = (uint8_t)atoi(tokens[4]);
    }

    // ---- v2 trailing pulse fields (positional): [8]=env [9]=phase [10]=attack [11]=jitter ----
    if (token_count >= 9 && !tok_is_dash(tokens[8])) {
        int env = (int)parse_v2_enum(tokens[8]);
        led_entry->env = (env >= 0 && env <= 3) ? (uint8_t)env : 0;   // 0..3 (square/sine/tri/trapezoid)
        led_entry->present |= LED_SET_ENV;
    }
    if (token_count >= 10 && !tok_is_dash(tokens[9])) {
        float end_f = 0.0f, period_f = 0.0f;
        float d = parse_v2_cell(tokens[9], &led_entry->phase_interp, &end_f, &period_f);
        led_entry->phase_deg = (uint16_t)wrap_deg(d);
        led_entry->phase_mod_end = (uint16_t)wrap_deg(end_f);
        led_entry->phase_mod_period_ms = (uint32_t)period_f;
        led_entry->present |= LED_SET_PHASE;
    }
    if (token_count >= 11 && !tok_is_dash(tokens[10])) {
        float end_f = 0.0f, period_f = 0.0f;
        float a = parse_v2_cell(tokens[10], &led_entry->attack_interp, &end_f, &period_f);
        led_entry->attack_ms = (uint16_t)(a < 0.0f ? 0.0f : a);
        led_entry->attack_mod_end = (uint16_t)(end_f < 0.0f ? 0.0f : end_f);
        led_entry->attack_mod_period_ms = (uint32_t)period_f;
        led_entry->present |= LED_SET_ATTACK;
    }
    if (token_count >= 12 && !tok_is_dash(tokens[11])) {
        led_entry->jitter_period_ms = 45000.0f;   // default wander period if only amp given
        led_entry->jitter_amp_hz    = parse_jitter_token(tokens[11], &led_entry->jitter_period_ms);
        led_entry->present |= LED_SET_JITTER;
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
    // Format v2 (ledc_format.md): after [ch freqR waveType], tokens[8..12] carry the
    // optional pulse fields [duty env phase attack jitter]. Any field may be '-'
    // (leave unchanged): its present bit stays clear and the execute path keeps the
    // channel's live value.
    memset(audio_entry, 0, sizeof(config_audio_entry_t));
    audio_entry->present = 0;

    // Parse time (always numeric)
    audio_entry->time_ms = atol(tokens[0]);

    // freq / pan / volume / modulation — each '-'-able, interp-prefix capable.
    if (!tok_is_dash(tokens[1])) {
        audio_entry->frequency = parse_value_with_interpolation(tokens[1], &audio_entry->freq_interp);
        audio_entry->present |= AUD_SET_FREQ;
        if (config_interp_is_modulation(audio_entry->freq_interp))
            parse_mod_extras(tokens[1], &audio_entry->freq_mod_end, &audio_entry->freq_mod_period_ms);
    }
    if (!tok_is_dash(tokens[2])) {
        audio_entry->pan = parse_value_with_interpolation(tokens[2], &audio_entry->pan_interp);
        audio_entry->present |= AUD_SET_PAN;
        if (config_interp_is_modulation(audio_entry->pan_interp))
            parse_mod_extras(tokens[2], &audio_entry->pan_mod_end, &audio_entry->pan_mod_period_ms);
    }
    if (!tok_is_dash(tokens[3])) {
        audio_entry->volume = parse_value_with_interpolation(tokens[3], &audio_entry->volume_interp);
        audio_entry->present |= AUD_SET_VOL;
        if (config_interp_is_modulation(audio_entry->volume_interp))
            parse_mod_extras(tokens[3], &audio_entry->vol_mod_end, &audio_entry->vol_mod_period_ms);
    }
    if (!tok_is_dash(tokens[4])) {
        audio_entry->modulation = parse_value_with_interpolation(tokens[4], &audio_entry->mod_interp);
        audio_entry->present |= AUD_SET_MOD;
        if (config_interp_is_modulation(audio_entry->mod_interp))
            parse_mod_extras(tokens[4], &audio_entry->mod_mod_end, &audio_entry->mod_mod_period_ms);
    }

    // Channel (optional, defaults to 0). Selector — not '-'-able.
    audio_entry->channel = (token_count >= 6 && !tok_is_dash(tokens[5])) ? atoi(tokens[5]) : 0;

    // Token 6 (optional): freq_r — right-ear frequency for binaural beat. '-'/absent
    // → leave (present bit clear); 0/out-of-range → 0.0 ("same as left").
    if (token_count >= 7 && !tok_is_dash(tokens[6])) {
        float freq_r = atof(tokens[6]);
        audio_entry->frequency_r = (freq_r > 0.0f && freq_r <= (AUDIO_GEN_SAMPLE_RATE / 2.0f)) ? freq_r : 0.0f;
        audio_entry->present |= AUD_SET_FREQR;
    }

    // Token 7 (optional): wave_type — 0..6 → audio_wave_type_t. '-'/absent → leave.
    if (token_count >= 8 && !tok_is_dash(tokens[7])) {
        int wt = atoi(tokens[7]);
        audio_entry->wave_type = (wt >= 0 && wt < AUDIO_WAVE_COUNT) ? (uint8_t)wt : 0;
        audio_entry->has_wave_type = true;
        audio_entry->present |= AUD_SET_WAVE;
    }

    // ---- v2 trailing pulse fields: [8]=duty [9]=env [10]=phase [11]=attack [12]=jitter ----
    if (token_count >= 9 && !tok_is_dash(tokens[8])) {
        float end_f = 0.0f, period_f = 0.0f;
        float d = parse_v2_cell(tokens[8], &audio_entry->duty_interp, &end_f, &period_f);
        audio_entry->duty_pct = clamp_pct(d);
        audio_entry->duty_mod_end = clamp_pct(end_f);
        audio_entry->duty_mod_period_ms = period_f;
        audio_entry->present |= AUD_SET_DUTY;
    }
    if (token_count >= 10 && !tok_is_dash(tokens[9])) {
        int env = (int)parse_v2_enum(tokens[9]);
        audio_entry->env = (env >= 0 && env <= 4) ? (uint8_t)env : 4;   // 0..4 (…/tremolo)
        audio_entry->present |= AUD_SET_ENV;
    }
    if (token_count >= 11 && !tok_is_dash(tokens[10])) {
        float end_f = 0.0f, period_f = 0.0f;
        float d = parse_v2_cell(tokens[10], &audio_entry->phase_interp, &end_f, &period_f);
        audio_entry->phase_deg = (uint16_t)wrap_deg(d);
        audio_entry->phase_mod_end = (uint16_t)wrap_deg(end_f);
        audio_entry->phase_mod_period_ms = (uint32_t)period_f;
        audio_entry->present |= AUD_SET_PHASE;
    }
    if (token_count >= 12 && !tok_is_dash(tokens[11])) {
        float end_f = 0.0f, period_f = 0.0f;
        float a = parse_v2_cell(tokens[11], &audio_entry->attack_interp, &end_f, &period_f);
        audio_entry->attack_ms = (a < 0.0f) ? 0.0f : a;
        audio_entry->attack_mod_end = (end_f < 0.0f) ? 0.0f : end_f;
        audio_entry->attack_mod_period_ms = period_f;
        audio_entry->present |= AUD_SET_ATTACK;
    }
    if (token_count >= 13 && !tok_is_dash(tokens[12])) {
        audio_entry->jitter_period_ms = 45000.0f;  // default wander period if only amp given
        audio_entry->jitter_amp_hz    = parse_jitter_token(tokens[12], &audio_entry->jitter_period_ms);
        audio_entry->present |= AUD_SET_JITTER;
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
    // push:// marks a browser-sourced clip (bg_browser_push_plan.md): the
    // device never fetches it — the audio arrives via POST /api/bg-stream.
    // Accepting it here lets a session round-trip losslessly through save/load.
    const char *url = tokens[0];
    bool valid_scheme =
        (strncmp(url, "http://",   7) == 0) ||
        (strncmp(url, "https://",  8) == 0) ||
        (strncmp(url, "sdcard://", 9) == 0) ||
        (strncmp(url, "push://",   7) == 0);

    if (!valid_scheme) {
        ESP_LOGW(TAG, "BG line: unsupported URL scheme in '%s' "
                 "(must be http://, https://, sdcard://, or push://) -- skipping", url);
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

/*
 * Parse a numeric field that may carry an interpolation prefix:
 *   >value             — linear ramp from this entry to next entry
 *   *value             — quadratic ease from this entry to next entry
 *   ^start:end:period  — triangle wave (linear up/down) start..end period ms
 *   ~start:end:period  — sine wave start..end period ms
 *   /start:end:period  — sawtooth (ramp from start to end, jumps back)
 *   \start:end:period  — reverse sawtooth (end < start typically)
 *   _start:end:period  — square wave (half period at start, half at end)
 *   value              — bare value, no interpolation
 *
 * For modulation prefixes, the function returns `start`. The caller must
 * extract `end` and `period` via parse_mod_extras() if they need them.
 */
static float parse_value_with_interpolation(const char *str, config_interpolation_t *interp)
{
    if (!str || !interp) {
        *interp = CONFIG_INTERP_NONE;
        return 0.0f;
    }

    switch (str[0]) {
        case '>':  *interp = CONFIG_INTERP_LINEAR;    return atof(&str[1]);
        case '*':  *interp = CONFIG_INTERP_QUADRATIC; return atof(&str[1]);
        case '^':  *interp = CONFIG_INTERP_TRIANGLE;  return atof(&str[1]);
        case '~':  *interp = CONFIG_INTERP_SINE;      return atof(&str[1]);
        case '/':  *interp = CONFIG_INTERP_SAW_UP;    return atof(&str[1]);
        case '\\': *interp = CONFIG_INTERP_SAW_DOWN;  return atof(&str[1]);
        case '_':  *interp = CONFIG_INTERP_SQUARE;    return atof(&str[1]);
        default:   *interp = CONFIG_INTERP_NONE;      return atof(str);
    }
}

/*
 * Extract the second and third colon-separated values from a modulation
 * token like "^50:75:1000" or "~50:75:1000". Works for any modulation
 * prefix (^, ~, /, \, _) — the leading prefix character is skipped.
 * Defaults: end = start (degenerate, no modulation), period = 1000 ms.
 * Caller has already verified the interp is a modulation type.
 */
static void parse_mod_extras(const char *str, float *out_end, float *out_period_ms)
{
    if (!str || str[0] == '\0') {
        if (out_end)        *out_end = 0.0f;
        if (out_period_ms)  *out_period_ms = 1000.0f;
        return;
    }
    // Skip the prefix character (any of ^~/\_); body starts at str+1.
    const char *body = str + 1;
    const char *p = strchr(body, ':');
    if (!p) {
        // No `:end` — degenerate, leave defaults
        if (out_end)        *out_end = atof(body);
        if (out_period_ms)  *out_period_ms = 1000.0f;
        return;
    }
    if (out_end) *out_end = atof(p + 1);
    const char *p2 = strchr(p + 1, ':');
    if (!p2) {
        // No `:period` — use default
        if (out_period_ms) *out_period_ms = 1000.0f;
        return;
    }
    if (out_period_ms) *out_period_ms = atof(p2 + 1);
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

        // ---- BG push:// prime gate ----
        // Hold the timeline until the pushed BG has buffered enough (or the
        // timeout fires), then latch T0, release the hold, and fall through so the
        // normal batch logic dispatches t=0 (current_entry_index is (size_t)-1, so
        // the ++ below makes it 0). This aligns BG sample 0 with session t=0.
        if (bg_prime_pending) {
            uint32_t buffered = bg_player_push_buffered_ms();
            uint32_t now_ms   = xTaskGetTickCount() * portTICK_PERIOD_MS;
            bool timed_out = (int32_t)(now_ms - bg_prime_deadline_ms) >= 0;
            if (buffered < BG_PRIME_THRESHOLD_MS && !timed_out) {
                xSemaphoreGive(timeline_mutex);
                continue;                       // not primed yet — wait for next poll
            }
            bg_prime_pending = false;
            bg_prime_poll_stop();
            timeline_start_time    = now_ms;
            transport_origin_us    = esp_timer_get_time();
            last_session_origin_us = transport_origin_us;
            bg_player_push_release();            // BG playback begins now, at t=0
            lock_free_message_t start_msg = {
                .type = MSG_TYPE_TIMELINE_EVENT,
                .timestamp_us = esp_timer_get_time(),
                .data_size = sizeof(uint32_t),
            };
            uint32_t tc = current_timeline->count;
            memcpy(start_msg.data, &tc, sizeof(uint32_t));
            lock_free_ring_buffer_t *q = lock_free_get_timeline_queue();
            if (q) lock_free_send_message(q, &start_msg);
            ESP_LOGI(TAG, "BG prime %s (buffered=%ums) — starting timeline",
                     timed_out ? "TIMEOUT" : "ready", (unsigned)buffered);
            // fall through (mutex held) → current_entry_index++ → 0 → dispatch t=0
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

        // Timeline task context — ESP_LOG safe. Greppable trace of the batch span.
        ESP_LOGD(TAG, "CFGDBG dispatch t=%ums entries[%zu..%zu) count=%zu",
                 batch_timestamp, batch_start_index, batch_end_index,
                 batch_end_index - batch_start_index);

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
            // Session ended: stop any LED flicker still running on masks the
            // timeline didn't explicitly zero, so the device returns to idle
            // instead of flickering forever. Real sessions fade brightness to 0
            // at the end, so this is a no-op for them; it fixes sessions/tests
            // that leave a mask active (audio is left as-is — it's already faded
            // to silence and abrupt teardown here would risk a click).
            led_matrix_stop_flicker_masked(0xFF);
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

/* Next audio entry on this channel that actually SETS `present_mask`.
 *
 * The core fields ramp toward the immediately-next entry, which is fine because
 * they are always present on a canonical line. The pulse fields are optional: a
 * later entry that omits duty means "leave unchanged", not "ramp to zero", so
 * ramping toward it would drive the value to a zero-initialised struct member.
 * Scanning for the next entry that sets the field also matches what the web
 * table shows as the ramp target, so the editor and the device agree. */
static const config_audio_entry_t *find_next_audio_with(const config_timeline_t *timeline,
                                                        size_t current_idx,
                                                        uint8_t channel_bit,
                                                        uint16_t present_mask)
{
    if (!timeline) return NULL;
    for (size_t i = current_idx + 1; i < timeline->count; i++) {
        const config_entry_t *e = &timeline->entries[i];
        if (e->type != CONFIG_ENTRY_AUDIO) continue;
        if (!((uint8_t)(1u << e->data.audio.channel) & channel_bit)) continue;
        if (e->data.audio.present & present_mask) return &e->data.audio;
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

        // Smart update-vs-start (Step 5.3): if the channel is already running,
        // update its params without restarting so there is no audible click or
        // phase reset.  Only start from scratch when the channel is inactive.
        // Determine this FIRST — the v2 '-' skip needs the channel's live values.
        esp_err_t ret;
        bool ch_active;
        if (lock_held) {
            ch_active = audio_generator_is_active_locked(audio->channel);
        } else {
            ch_active = audio_manager_is_channel_active(audio->channel);
        }

        // Effective core params. For a v2 '-' field (present bit clear), keep the
        // channel's LIVE value so the write is a no-op ("leave unchanged"). Only the
        // lock_held timeline path can read live values safely (the _locked getters);
        // the rare non-locked path falls back to the parsed default.
        float eff_freq   = audio->frequency;
        float eff_amp    = audio->volume / 100.0f;   // 0-100 → 0.0-1.0
        float eff_pan    = audio->pan / 100.0f;      // -100/+100 → -1.0/+1.0
        float eff_mod    = audio->modulation;
        float eff_freq_r = audio->frequency_r;
        if (ch_active && lock_held) {
            float v;
            if (!(audio->present & AUD_SET_FREQ)  && audio_generator_get_param_locked(audio->channel, AUDIO_PARAM_FREQUENCY, &v) == ESP_OK) eff_freq = v;
            if (!(audio->present & AUD_SET_VOL)   && audio_generator_get_param_locked(audio->channel, AUDIO_PARAM_AMPLITUDE, &v) == ESP_OK) eff_amp  = v;
            if (!(audio->present & AUD_SET_PAN)   && audio_generator_get_param_locked(audio->channel, AUDIO_PARAM_PAN,       &v) == ESP_OK) eff_pan  = v;
            if (!(audio->present & AUD_SET_MOD)   && audio_generator_get_param_locked(audio->channel, AUDIO_PARAM_MOD_FREQ,  &v) == ESP_OK) eff_mod  = v;
            if (!(audio->present & AUD_SET_FREQR) && audio_generator_get_current_freq_r_locked(audio->channel, &v) == ESP_OK) eff_freq_r = v;
        }

        audio_gen_params_t gen_params = {
            .frequency   = eff_freq,
            .frequency_r = eff_freq_r,
            .amplitude   = eff_amp,
            .pan         = eff_pan,
            .mod_frequency = eff_mod,
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

            // ---- v2 pulse-field wiring (per-channel; '-'/absent → leave) ----
            // env/duty/attack go through the per-channel iso override; each field
            // not present is passed as its "leave" sentinel so untouched values
            // stay. phase is per-channel. beat jitter is a session-wide setting
            // (global engine) applied when the field is present.
            if (audio->present & (AUD_SET_ENV | AUD_SET_DUTY | AUD_SET_ATTACK)) {
                int   env_arg    = (audio->present & AUD_SET_ENV)    ? (int)audio->env       : -1;
                float duty_arg   = (audio->present & AUD_SET_DUTY)   ? audio->duty_pct       : -1.0f;
                float attack_arg = (audio->present & AUD_SET_ATTACK) ? audio->attack_ms      : -1.0f;
                audio_generator_set_iso_channel(audio->channel, env_arg, duty_arg, attack_arg, -1.0f);
            }
            if (audio->present & AUD_SET_PHASE) {
                audio_generator_set_phase(audio->channel, audio->phase_deg);
            }
            if (audio->present & AUD_SET_JITTER) {
                audio_generator_set_beat_jitter(audio->jitter_amp_hz, audio->jitter_period_ms);
            }

            // ---- Modulation wiring (new in mod_engine phase) ----
            // Stop any previously-active modulations on this channel's fields.
            // A new entry always preempts whatever was running, regardless of
            // whether the new entry is a step, sweep, or new modulation.
            mod_engine_stop_audio(audio->channel, MOD_AUDIO_FREQ);
            mod_engine_stop_audio(audio->channel, MOD_AUDIO_PAN);
            mod_engine_stop_audio(audio->channel, MOD_AUDIO_VOLUME);
            mod_engine_stop_audio(audio->channel, MOD_AUDIO_MOD);
            mod_engine_stop_audio(audio->channel, MOD_AUDIO_ISO_DUTY);
            mod_engine_stop_audio(audio->channel, MOD_AUDIO_ISO_PHASE);
            mod_engine_stop_audio(audio->channel, MOD_AUDIO_ISO_ATTACK);
            // Start new modulations for fields that use a modulation prefix.
            if (config_interp_is_modulation(audio->freq_interp)) {
                mod_engine_start_audio(audio->channel, MOD_AUDIO_FREQ,
                    (mod_wave_t)mod_wave_from_interp(audio->freq_interp),
                    audio->frequency, audio->freq_mod_end, (uint32_t)audio->freq_mod_period_ms);
            }
            if (config_interp_is_modulation(audio->pan_interp)) {
                mod_engine_start_audio(audio->channel, MOD_AUDIO_PAN,
                    (mod_wave_t)mod_wave_from_interp(audio->pan_interp),
                    audio->pan, audio->pan_mod_end, (uint32_t)audio->pan_mod_period_ms);
            }
            if (config_interp_is_modulation(audio->volume_interp)) {
                mod_engine_start_audio(audio->channel, MOD_AUDIO_VOLUME,
                    (mod_wave_t)mod_wave_from_interp(audio->volume_interp),
                    audio->volume, audio->vol_mod_end, (uint32_t)audio->vol_mod_period_ms);
            }
            if (config_interp_is_modulation(audio->mod_interp)) {
                mod_engine_start_audio(audio->channel, MOD_AUDIO_MOD,
                    (mod_wave_t)mod_wave_from_interp(audio->mod_interp),
                    audio->modulation, audio->mod_mod_end, (uint32_t)audio->mod_mod_period_ms);
            }
            /* Pulse-shape modulations. Natural units all the way through (duty %,
             * degrees, ms) — fill_buffer converts when it applies them. */
            if ((audio->present & AUD_SET_DUTY) && config_interp_is_modulation(audio->duty_interp)) {
                mod_engine_start_audio(audio->channel, MOD_AUDIO_ISO_DUTY,
                    (mod_wave_t)mod_wave_from_interp(audio->duty_interp),
                    audio->duty_pct, audio->duty_mod_end, (uint32_t)audio->duty_mod_period_ms);
            }
            if ((audio->present & AUD_SET_PHASE) && config_interp_is_modulation(audio->phase_interp)) {
                mod_engine_start_audio(audio->channel, MOD_AUDIO_ISO_PHASE,
                    (mod_wave_t)mod_wave_from_interp(audio->phase_interp),
                    (float)audio->phase_deg, (float)audio->phase_mod_end,
                    (uint32_t)audio->phase_mod_period_ms);
            }
            if ((audio->present & AUD_SET_ATTACK) && config_interp_is_modulation(audio->attack_interp)) {
                mod_engine_start_audio(audio->channel, MOD_AUDIO_ISO_ATTACK,
                    (mod_wave_t)mod_wave_from_interp(audio->attack_interp),
                    audio->attack_ms, audio->attack_mod_end,
                    (uint32_t)audio->attack_mod_period_ms);
            }

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

                // NEW CONVENTION (animate-on-start): the interp flag lives on
                // THIS entry (audio->X_interp), not on the next entry. Means:
                // "starting at this entry, run a sweep that reaches the next
                // entry's value over the time window to next". The VALUES
                // still come from this entry (start) and next entry (target).
                // Frequency sweep
                if ((audio->freq_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(audio->freq_interp)) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_FREQUENCY,
                            audio->frequency, next->frequency,
                            dur_samples, interp_to_audio_curve(audio->freq_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_FREQUENCY,
                            audio->frequency, next->frequency,
                            dur_samples, interp_to_audio_curve(audio->freq_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=FREQ %.2f→%.2f over %ums curve=%d",
                             audio->channel, audio->frequency, next->frequency,
                             window_ms, (int)audio->freq_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "freq sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }

                // Amplitude sweep — volume 0-100 → amplitude 0.0-1.0
                if ((audio->volume_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(audio->volume_interp)) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_AMPLITUDE,
                            audio->volume / 100.0f, next->volume / 100.0f,
                            dur_samples, interp_to_audio_curve(audio->volume_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_AMPLITUDE,
                            audio->volume / 100.0f, next->volume / 100.0f,
                            dur_samples, interp_to_audio_curve(audio->volume_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=AMP %.2f→%.2f over %ums curve=%d",
                             audio->channel,
                             audio->volume / 100.0f, next->volume / 100.0f,
                             window_ms, (int)audio->volume_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "amp sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }

                // Pan sweep — pan -100/+100 → -1.0/+1.0
                if ((audio->pan_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(audio->pan_interp)) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_PAN,
                            audio->pan / 100.0f, next->pan / 100.0f,
                            dur_samples, interp_to_audio_curve(audio->pan_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_PAN,
                            audio->pan / 100.0f, next->pan / 100.0f,
                            dur_samples, interp_to_audio_curve(audio->pan_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=PAN %.2f→%.2f over %ums curve=%d",
                             audio->channel,
                             audio->pan / 100.0f, next->pan / 100.0f,
                             window_ms, (int)audio->pan_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "pan sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }

                // Modulation frequency sweep
                if ((audio->mod_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(audio->mod_interp)) {
                    esp_err_t sw;
                    if (lock_held) {
                        sw = audio_generator_start_sweep_locked(
                            audio->channel, AUDIO_PARAM_MOD_FREQ,
                            audio->modulation, next->modulation,
                            dur_samples, interp_to_audio_curve(audio->mod_interp));
                    } else {
                        sw = audio_generator_start_sweep(
                            audio->channel, AUDIO_PARAM_MOD_FREQ,
                            audio->modulation, next->modulation,
                            dur_samples, interp_to_audio_curve(audio->mod_interp));
                    }
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=MOD %.2f→%.2f over %ums curve=%d",
                             audio->channel,
                             audio->modulation, next->modulation,
                             window_ms, (int)audio->mod_interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "mod sweep start failed ch=%d: %s",
                                 audio->channel, esp_err_to_name(sw));
                    }
                }
            }

            /* Pulse-shape ramps (duty / phase / attack).
             *
             * Deliberately outside the block above, which ramps toward the
             * immediately-next entry. These scan forward for the next entry that
             * actually SETS the field: an entry omitting duty means "leave
             * unchanged", so ramping toward its zero-initialised duty_pct would
             * drive the pulse to 0% instead of holding. A field with no later
             * setter simply doesn't ramp — the same "holds" the web table shows. */
            {
                const uint8_t pulse_bit = (uint8_t)(1u << audio->channel);
                const struct {
                    uint16_t               present_bit;
                    config_interpolation_t interp;
                    audio_param_t          param;
                    float                  start;
                    const char            *name;
                } pulse[] = {
                    { AUD_SET_DUTY,   audio->duty_interp,   AUDIO_PARAM_ISO_DUTY,
                      audio->duty_pct,         "duty"   },
                    { AUD_SET_PHASE,  audio->phase_interp,  AUDIO_PARAM_ISO_PHASE,
                      (float)audio->phase_deg, "phase"  },
                    { AUD_SET_ATTACK, audio->attack_interp, AUDIO_PARAM_ISO_ATTACK,
                      audio->attack_ms,        "attack" },
                };
                for (size_t pi = 0; pi < sizeof(pulse) / sizeof(pulse[0]); pi++) {
                    if (!(audio->present & pulse[pi].present_bit))       continue;
                    if (pulse[pi].interp == CONFIG_INTERP_NONE)          continue;
                    if (config_interp_is_modulation(pulse[pi].interp))   continue;  /* started above */

                    const config_audio_entry_t *nx =
                        find_next_audio_with(timeline, entry_idx, pulse_bit, pulse[pi].present_bit);
                    if (nx == NULL) continue;   /* nothing later sets it — hold */

                    float target = (pulse[pi].param == AUDIO_PARAM_ISO_DUTY)  ? nx->duty_pct
                                 : (pulse[pi].param == AUDIO_PARAM_ISO_PHASE) ? (float)nx->phase_deg
                                 :                                              nx->attack_ms;
                    uint32_t win_ms = nx->time_ms - audio->time_ms;
                    uint64_t win_samples = ((uint64_t)win_ms * AUDIO_GEN_SAMPLE_RATE) / 1000ULL;

                    esp_err_t sw = lock_held
                        ? audio_generator_start_sweep_locked(audio->channel, pulse[pi].param,
                                                             pulse[pi].start, target, win_samples,
                                                             interp_to_audio_curve(pulse[pi].interp))
                        : audio_generator_start_sweep(audio->channel, pulse[pi].param,
                                                      pulse[pi].start, target, win_samples,
                                                      interp_to_audio_curve(pulse[pi].interp));
#ifdef CONFIG_TIMELINE_DEBUG
                    ESP_LOGI(TAG, "Timeline sweep: ch=%d param=%s %.2f→%.2f over %ums curve=%d",
                             audio->channel, pulse[pi].name, pulse[pi].start, target,
                             win_ms, (int)pulse[pi].interp);
#endif
                    if (sw != ESP_OK) {
                        ESP_LOGW(TAG, "%s sweep start failed ch=%d: %s",
                                 pulse[pi].name, audio->channel, esp_err_to_name(sw));
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

    // v2 '-' skip: if any core field (freq/duty/bright/R/G/B) is absent, substitute
    // the LIVE value of the lowest channel in the mask so applying it is a no-op
    // ("leave unchanged"). A multi-bit mask with divergent live values collapses to
    // the low bit's value for the '-' fields — an unusual authoring choice.
    config_led_entry_t led_eff;
    if ((led->present & LED_SET_CORE) != LED_SET_CORE) {
        led_eff = *led;
        int lowbit = -1;
        for (int b = 0; b < NUM_LED_CHANNELS; b++) {
            if (led->channel_mask & (1u << b)) { lowbit = b; break; }
        }
        if (lowbit >= 0) {
            led_matrix_channel_snapshot_t snap[NUM_LED_CHANNELS];
            int n = led_matrix_get_snapshot(snap, NUM_LED_CHANNELS);
            if (lowbit < n) {
                const led_matrix_channel_snapshot_t *s = &snap[lowbit];
                if (!(led->present & LED_SET_FREQ))   led_eff.frequency  = s->freq;
                if (!(led->present & LED_SET_DUTY))   led_eff.duty_cycle = s->duty;
                if (!(led->present & LED_SET_BRIGHT)) led_eff.brightness = s->brightness;
                if (!(led->present & LED_SET_R))      led_eff.r = s->r;
                if (!(led->present & LED_SET_G))      led_eff.g = s->g;
                if (!(led->present & LED_SET_B))      led_eff.b = s->b;
            }
        }
        led = &led_eff;
    }

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
    /* ---------- freq > 0: modulation + sweep wiring + LED dispatch ---------- */

    // ---- Modulation wiring (mod_engine phase) ----
    // Preempt any active modulations on this entry's channel-mask fields,
    // then start new ones for fields that carry a modulation prefix.
    // Modulation is mask-wide and self-contained — doesn't depend on the
    // bucketing below (which is for sweep target value lookup).
    mod_engine_stop_led(led->channel_mask, MOD_LED_FREQ);
    mod_engine_stop_led(led->channel_mask, MOD_LED_DUTY);
    mod_engine_stop_led(led->channel_mask, MOD_LED_BRIGHT);
    mod_engine_stop_led(led->channel_mask, MOD_LED_R);
    mod_engine_stop_led(led->channel_mask, MOD_LED_G);
    mod_engine_stop_led(led->channel_mask, MOD_LED_B);
    if (config_interp_is_modulation(led->freq_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_FREQ,
            (mod_wave_t)mod_wave_from_interp(led->freq_interp),
            led->frequency, led->freq_mod_end, (uint32_t)led->freq_mod_period_ms);
    }
    if (config_interp_is_modulation(led->duty_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_DUTY,
            (mod_wave_t)mod_wave_from_interp(led->duty_interp),
            (float)led->duty_cycle, (float)led->duty_mod_end, led->duty_mod_period_ms);
    }
    if (config_interp_is_modulation(led->brightness_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_BRIGHT,
            (mod_wave_t)mod_wave_from_interp(led->brightness_interp),
            (float)led->brightness, (float)led->bright_mod_end, led->bright_mod_period_ms);
    }
    if (config_interp_is_modulation(led->r_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_R,
            (mod_wave_t)mod_wave_from_interp(led->r_interp),
            (float)led->r, (float)led->r_mod_end, led->r_mod_period_ms);
    }
    if (config_interp_is_modulation(led->g_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_G,
            (mod_wave_t)mod_wave_from_interp(led->g_interp),
            (float)led->g, (float)led->g_mod_end, led->g_mod_period_ms);
    }
    if (config_interp_is_modulation(led->b_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_B,
            (mod_wave_t)mod_wave_from_interp(led->b_interp),
            (float)led->b, (float)led->b_mod_end, led->b_mod_period_ms);
    }

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

        // NEW CONVENTION (animate-on-start): the interp flag lives on
        // led (this entry), not next_bit (the next entry). Start value
        // is led->X, target is next_bit->X. See audio sweep block above.
        if ((led->freq_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(led->freq_interp)) {
            sweep_spec.freq_milliHz_start  = (uint32_t)(led->frequency  * 1000.0f);
            sweep_spec.freq_milliHz_target = (uint32_t)(next_bit->frequency * 1000.0f);
            sweep_spec.freq_curve          = interp_to_led_curve(led->freq_interp);
            bucket_has_sweep = true;
        }
        if ((led->duty_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(led->duty_interp)) {
            sweep_spec.duty_start   = led->duty_cycle;
            sweep_spec.duty_target  = next_bit->duty_cycle;
            sweep_spec.duty_curve   = interp_to_led_curve(led->duty_interp);
            bucket_has_sweep = true;
        }
        if ((led->brightness_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(led->brightness_interp)) {
            sweep_spec.bright_start  = led->brightness;
            sweep_spec.bright_target = next_bit->brightness;
            sweep_spec.bright_curve  = interp_to_led_curve(led->brightness_interp);
            bucket_has_sweep = true;
        }
        if ((led->r_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(led->r_interp)) {
            sweep_spec.r_start  = led->r;
            sweep_spec.r_target = next_bit->r;
            sweep_spec.r_curve  = interp_to_led_curve(led->r_interp);
            bucket_has_sweep = true;
        }
        if ((led->g_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(led->g_interp)) {
            sweep_spec.g_start  = led->g;
            sweep_spec.g_target = next_bit->g;
            sweep_spec.g_curve  = interp_to_led_curve(led->g_interp);
            bucket_has_sweep = true;
        }
        if ((led->b_interp) != CONFIG_INTERP_NONE && !config_interp_is_modulation(led->b_interp)) {
            sweep_spec.b_start  = led->b;
            sweep_spec.b_target = next_bit->b;
            sweep_spec.b_curve  = interp_to_led_curve(led->b_interp);
            bucket_has_sweep = true;
        }
        /* Pulse shape. Both ends must actually carry the field: an entry that
         * omits phase means "leave unchanged", so ramping toward its
         * zero-initialised phase_deg would drag the offset to 0 instead of
         * holding. Same rule the audio side uses, and the same "holds" the web
         * table reports when nothing later sets the field. */
        if ((led->present & LED_SET_PHASE) && (next_bit->present & LED_SET_PHASE) &&
            led->phase_interp != CONFIG_INTERP_NONE &&
            !config_interp_is_modulation(led->phase_interp)) {
            sweep_spec.phase_start  = led->phase_deg;
            sweep_spec.phase_target = next_bit->phase_deg;
            sweep_spec.phase_curve  = interp_to_led_curve(led->phase_interp);
            sweep_spec.phase_set    = true;
            bucket_has_sweep = true;
        }
        if ((led->present & LED_SET_ATTACK) && (next_bit->present & LED_SET_ATTACK) &&
            led->attack_interp != CONFIG_INTERP_NONE &&
            !config_interp_is_modulation(led->attack_interp)) {
            sweep_spec.attack_start  = led->attack_ms;
            sweep_spec.attack_target = next_bit->attack_ms;
            sweep_spec.attack_curve  = interp_to_led_curve(led->attack_interp);
            sweep_spec.attack_set    = true;
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

    // ---- v2 pulse-field wiring (per-channel_mask; only fields the line SET) ----
    // Applied in both branches (stop and start): carrier/attack/jitter write
    // per-channel state (latched at next start too), phase is applied at use-time.
    // A field left '-' (present bit clear) is not written → channel keeps its value.
    if (led->present & LED_SET_ENV)    led_matrix_set_carrier_masked(led->channel_mask, led->env);
    if (led->present & LED_SET_PHASE)  led_matrix_set_phase_masked(led->channel_mask, (int16_t)led->phase_deg);
    if (led->present & LED_SET_ATTACK) led_matrix_set_attack_masked(led->channel_mask, led->attack_ms);
    if (led->present & LED_SET_JITTER) led_matrix_set_jitter_masked(led->channel_mask, led->jitter_amp_hz, led->jitter_period_ms);

    /* Pulse-shape modulation. Cleared first so a new entry preempts whatever was
     * running, mirroring the audio path; a plain value then leaves the field on
     * its sweep/step, which is what clear_mod restores. */
    mod_engine_stop_led(led->channel_mask, MOD_LED_PHASE);
    mod_engine_stop_led(led->channel_mask, MOD_LED_ATTACK);
    if ((led->present & LED_SET_PHASE) && config_interp_is_modulation(led->phase_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_PHASE,
                             (mod_wave_t)mod_wave_from_interp(led->phase_interp),
                             (float)led->phase_deg, (float)led->phase_mod_end,
                             led->phase_mod_period_ms);
    }
    if ((led->present & LED_SET_ATTACK) && config_interp_is_modulation(led->attack_interp)) {
        mod_engine_start_led(led->channel_mask, MOD_LED_ATTACK,
                             (mod_wave_t)mod_wave_from_interp(led->attack_interp),
                             (float)led->attack_ms, (float)led->attack_mod_end,
                             led->attack_mod_period_ms);
    }

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
