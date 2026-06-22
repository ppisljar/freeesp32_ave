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
    CONFIG_INTERP_LINEAR,     // > prefix
    CONFIG_INTERP_QUADRATIC   // * prefix
} config_interpolation_t;

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

#endif // CONFIG_PARSER_H
