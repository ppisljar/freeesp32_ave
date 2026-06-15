#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include "esp_err.h"
#include "audio_generator.h"

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
    CONFIG_ENTRY_AUDIO
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
    // RGBW color fields — 8 bytes for color data + 4×4 bytes interp flags = 24 bytes/entry.
    uint8_t r, g, b, w;
    uint8_t channel_mask;
    config_interpolation_t freq_interp;
    config_interpolation_t duty_interp;
    config_interpolation_t brightness_interp;
    config_interpolation_t r_interp;
    config_interpolation_t g_interp;
    config_interpolation_t b_interp;
    config_interpolation_t w_interp;
} config_led_entry_t;

typedef struct {
    uint32_t time_ms;
    float frequency;
    float frequency_r;        // Right channel frequency (for binaural)
    float pan;               // Pan position (-100 to +100)
    float volume;            // Volume (0 to 100)
    float modulation;        // Modulation frequency
    uint8_t channel;
    config_interpolation_t freq_interp;
    config_interpolation_t pan_interp;
    config_interpolation_t volume_interp;
    config_interpolation_t mod_interp;
} config_audio_entry_t;

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
 * @brief Create example config content
 *
 * @param buffer Output buffer for example content
 * @param buffer_size Size of output buffer
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_parser_create_example(char *buffer, size_t buffer_size);

#endif // CONFIG_PARSER_H
