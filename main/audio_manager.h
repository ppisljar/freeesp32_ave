#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "esp_err.h"
#include "esp_log.h"
#include "audio_generator.h"

/**
 * @brief Audio Manager Component
 *
 * Main control logic for the ESP32 Audio Player.
 * Manages audio pipeline, source routing, and playback control.
 */

typedef enum {
    AUDIO_SOURCE_NONE = 0,
    AUDIO_SOURCE_SDCARD,
    AUDIO_SOURCE_RTP,
    AUDIO_SOURCE_VBAN,
    AUDIO_SOURCE_BLUETOOTH,
    AUDIO_SOURCE_GENERATOR  // Add generated audio source
} audio_source_t;

typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED,
    AUDIO_STATE_ERROR
} audio_state_t;

typedef struct {
    audio_source_t current_source;
    audio_state_t state;
    float volume;
    bool muted;
} audio_manager_state_t;

/**
 * @brief Initialize the audio manager
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_init(void);

/**
 * @brief Start playback from specified source
 *
 * @param source Audio source to play from
 * @param uri Source URI (file path, stream URL, etc.)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_play(audio_source_t source, const char* uri);

/**
 * @brief Pause current playback
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_pause(void);

/**
 * @brief Resume paused playback
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_resume(void);

/**
 * @brief Stop current playback
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_stop(void);

/**
 * @brief Set volume level
 *
 * @param volume Volume level (0.0 to 1.0)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_set_volume(float volume);

/**
 * @brief Get current audio manager state
 *
 * @return audio_manager_state_t* Current state
 */
audio_manager_state_t* audio_manager_get_state(void);

/**
 * @brief Start audio generation on a channel
 *
 * @param channel Channel number for audio generation
 * @param params Audio generation parameters
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_start_generation(int channel, const audio_gen_params_t* params);

/**
 * @brief Stop audio generation on a channel
 *
 * @param channel Channel number
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_manager_stop_generation(int channel);

/**
 * @brief Check if audio generation is active
 *
 * @param channel Channel number
 * @return true if active
 */
bool audio_manager_is_generating(int channel);

/**
 * @brief Check if audio channel is active (alias for is_generating)
 *
 * @param channel Channel number
 * @return true if active
 */
bool audio_manager_is_channel_active(int channel);

#endif // AUDIO_MANAGER_H
