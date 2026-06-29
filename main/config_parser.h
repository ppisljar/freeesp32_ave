#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include "esp_err.h"
#include "audio_generator.h"
#include <stdbool.h>

/**
 * @brief Config File Parser for .LED format
 *
 * Parses timeline-based configuration files that support:
 * - LED control commands
 * - Audio generation commands (with A prefix)
 * - Interpolation (linear >, quadratic *)
 * - Comments and error handling
 */

#define CONFIG_PARSER_MAX_LINE_LENGTH   256
#define CONFIG_PARSER_MAX_ENTRIES       100

typedef enum {
    CONFIG_ENTRY_LED = 0,
    CONFIG_ENTRY_AUDIO,
    CONFIG_ENTRY_BG          // Session-level background audio descriptor (not a timeline entry)
} config_entry_type_t;

typedef enum {
    CONFIG_INTERP_NONE = 0,
    CONFIG_INTERP_LINEAR,     // > prefix  — linear ramp to next entry's value
    CONFIG_INTERP_QUADRATIC,  // * prefix  — quadratic ease to next entry's value
    // Modulation prefixes — all 5 use the same `prefix start:end:period_ms` syntax.
    // Self-contained: do NOT depend on the next entry's value. Runs continuously
    // until preempted by a new entry on the same channel/field.
    CONFIG_INTERP_TRIANGLE,   // ^ prefix  — triangle wave (linear ramp up then down)
    CONFIG_INTERP_SINE,       // ~ prefix  — smooth sinusoidal oscillation
    CONFIG_INTERP_SAW_UP,     // / prefix  — sawtooth, ramps from start to end then jumps back
    CONFIG_INTERP_SAW_DOWN,   // \ prefix  — sawtooth, ramps from start to end then jumps back (start > end)
    CONFIG_INTERP_SQUARE      // _ prefix  — square wave (half period at start, half at end)
} config_interpolation_t;

/* True iff the given interpolation type is a periodic modulation (one of
 * triangle/sine/saw/square) rather than a one-shot ramp (linear/quadratic).
 * Modulation entries carry extra (`_mod_end`, `_mod_period_ms`) fields. */
static inline bool config_interp_is_modulation(config_interpolation_t i)
{
    return i == CONFIG_INTERP_TRIANGLE
        || i == CONFIG_INTERP_SINE
        || i == CONFIG_INTERP_SAW_UP
        || i == CONFIG_INTERP_SAW_DOWN
        || i == CONFIG_INTERP_SQUARE;
}

/* Modulation extras: each interpolatable field carries two extra floats
 * — the wave's "end" value (other extreme of the oscillation; the entry's
 * regular field value is the "start") and the period in ms (full cycle
 * time start→end→start). Unused for NONE/LINEAR/QUADRATIC. */

typedef struct {
    uint32_t time_ms;
    float frequency;
    uint8_t duty_cycle;
    uint8_t brightness;
    // RGB color fields — 6 bytes for color data + 3×4 bytes interp flags = 18 bytes/entry.
    uint8_t r, g, b;
    uint8_t channel_mask;
    config_interpolation_t freq_interp;
    config_interpolation_t duty_interp;
    config_interpolation_t brightness_interp;
    config_interpolation_t r_interp;
    config_interpolation_t g_interp;
    config_interpolation_t b_interp;
    // Modulation extras (valid only when the matching _interp is a
    // modulation type — see config_interp_is_modulation()).
    float    freq_mod_end,   freq_mod_period_ms;
    uint8_t  duty_mod_end;
    uint32_t duty_mod_period_ms;
    uint8_t  bright_mod_end;
    uint32_t bright_mod_period_ms;
    uint8_t  r_mod_end, g_mod_end, b_mod_end;
    uint32_t r_mod_period_ms, g_mod_period_ms, b_mod_period_ms;
} config_led_entry_t;

typedef struct {
    uint32_t time_ms;
    float frequency;
    float frequency_r;        // Right channel frequency (for binaural)
    float pan;               // Pan position (-100 to +100)
    float volume;            // Volume (0 to 100)
    float modulation;        // Modulation frequency
    uint8_t wave_type;       // Waveform type (0=SINE default); see audio_wave_type_t
    uint8_t channel;
    config_interpolation_t freq_interp;
    config_interpolation_t pan_interp;
    config_interpolation_t volume_interp;
    config_interpolation_t mod_interp;
    // Modulation extras (see notes above config_led_entry_t).
    float    freq_mod_end,   freq_mod_period_ms;
    float    pan_mod_end,    pan_mod_period_ms;
    float    vol_mod_end,    vol_mod_period_ms;
    float    mod_mod_end,    mod_mod_period_ms;
} config_audio_entry_t;

/**
 * @brief Background audio entry descriptor.
 *
 * Holds the session-level BG annotation parsed from a "BG <url> <pan> <loudness>" line.
 * This struct is NOT placed in the config_entry_t.data union (which would inflate the
 * entries array by ~200 bytes per slot).  Instead it lives directly in config_timeline_t,
 * embedded by value, and is valid only when has_bg == true.
 *
 * URL buffer: 256 bytes (null-terminated).  Sufficient for any realistic HTTP/HTTPS URL
 * in a local WiFi session (~230 usable path characters after the scheme).  If a longer
 * URL is ever needed, increase the constant here and recompile — there is no heap
 * allocation, so the change only adds 256 bytes to sizeof(config_timeline_t).
 *
 * pan:      stored as −1.0 … +1.0 (divided by 100 from the BG line's −100 … +100 token).
 * loudness: stored as 0.0 … 1.0 (divided by 100 from the BG line's 0 … 100 token).
 *           Maps directly to the per-sample gain applied by bg_player_mix_into().
 */
typedef struct {
    char    url[256];        // Source URL: http://..., https://..., or sdcard://...
    float   pan;             // −1.0 (hard left) to +1.0 (hard right); 0.0 = center
    float   loudness;        // 0.0 to 1.0 gain multiplier (from BG line loudness / 100.0)
} config_bg_entry_t;

typedef struct {
    config_entry_type_t type;
    union {
        config_led_entry_t led;
        config_audio_entry_t audio;
    } data;
} config_entry_t;

typedef struct {
    config_entry_t *entries;
    size_t count;
    size_t capacity;
    char *source_content;
    config_bg_entry_t bg;    // Background audio descriptor (valid only when has_bg == true)
    bool has_bg;             // true when a valid BG line was parsed from this file
} config_timeline_t;

/**
 * @brief Initialize config parser
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_parser_init(void);

/**
 * @brief Parse .led config file content
 *
 * @param content File content as string
 * @param content_length Length of content
 * @param timeline Output timeline structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_parser_parse_content(const char *content, size_t content_length,
                                      config_timeline_t *timeline);

/**
 * @brief Parse .led config file from filesystem
 *
 * @param file_path Path to .led file
 * @param timeline Output timeline structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_parser_parse_file(const char *file_path, config_timeline_t *timeline);

/**
 * @brief Execute config timeline
 *
 * Starts execution of the parsed timeline
 * @param timeline Timeline to execute
 * @param loop Whether to loop the timeline
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_parser_execute_timeline(config_timeline_t *timeline, bool loop);

/**
 * @brief Stop timeline execution
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_parser_stop_timeline(void);

/**
 * @brief Apply a patch — additive update that does NOT stop the running
 *        timeline or restart channels not mentioned in the patch.
 *
 * Use for live "direct control" UX where the user wants to tweak one or two
 * channels (e.g. adjust audio channel 3's frequency) without disturbing
 * everything else. Channels not mentioned in the patch keep playing exactly
 * as they were.
 *
 * The patch content is parsed using the same .ledc grammar as the main
 * configuration. Each entry is dispatched at time = its time_ms field
 * RELATIVE TO NOW (not to the main timeline's t=0). Entries at t=0 fire
 * immediately; entries at t>0 are scheduled via esp_timer.
 *
 * For animated changes, send two lines: line 1 at t=0 sets the start value
 * (anchor), line 2 at t=Nms with the `>` interpolation prefix triggers the
 * sweep from the channel's current value to the new value over N ms.
 *
 * BG audio (`BG ...` lines) is currently NOT supported in patches — silently
 * ignored. Use /api/play-config for BG changes.
 *
 * @param content       .ledc-format text (1+ lines)
 * @param content_length Length of content in bytes
 * @return ESP_OK if parsed and dispatched successfully, error code otherwise
 */
esp_err_t config_parser_apply_patch(const char *content, size_t content_length);

/**
 * @brief Get current timeline position
 *
 * @return uint32_t Current time in milliseconds
 */
uint32_t config_parser_get_timeline_position(void);

/**
 * @brief Free timeline resources
 *
 * @param timeline Timeline to free
 */
void config_parser_free_timeline(config_timeline_t *timeline);

/**
 * @brief Validate config file syntax
 *
 * @param content File content as string
 * @param content_length Length of content
 * @param errors Output for error messages (can be NULL)
 * @param max_errors Maximum number of errors to report
 * @return esp_err_t ESP_OK if valid, ESP_ERR_INVALID_ARG if syntax errors
 */
esp_err_t config_parser_validate_syntax(const char *content, size_t content_length,
                                        char **errors, size_t max_errors);

/**
 * @brief Get a pointer to the static example .led config string.
 *
 * The returned pointer is to a string literal in flash (.rodata); never NULL,
 * never freed by the caller. Returning the literal directly avoids copying
 * ~3 KB onto a caller's stack — important for HTTPD handlers whose task stack
 * defaults to 4 KB and otherwise overflows when responding.
 */
const char *config_parser_get_example(void);

/**
 * @brief Return the raw text of the currently-loaded timeline (the .led
 *        source as last parsed by config_parser_parse_content or _parse_file),
 *        or NULL if no timeline is loaded.
 *
 * Owned by the parser; do not free. Pointer stays valid until the next
 * parse call replaces the timeline.
 */
const char *config_parser_get_loaded_source(void);

/**
 * @brief Return the canonical transport-clock origin (esp_timer_get_time at
 *        timeline start), or 0 if no timeline is currently running.
 *
 * Used by diagnostic endpoints to convert absolute esp_timer timestamps
 * (e.g. button-press times) into milliseconds relative to session start.
 */
uint64_t config_parser_get_session_origin_us(void);

#endif // CONFIG_PARSER_H
