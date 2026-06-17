/**
 * @file bg_player.h
 * @brief Background audio player — public API.
 *
 * bg_player streams a WAV/PCM file from an HTTP/HTTPS URL (or, when
 * CONFIG_BG_SDCARD_ENABLED=y, from an SD card path) into the I2S output
 * buffer as a post-mix stage that runs independently of the synthesized
 * channel pool.
 *
 * Integration point:
 *   audio_test_output_task() calls bg_player_mix_into() AFTER
 *   audio_generator_fill_buffer() and BEFORE the float→int16 conversion.
 *
 * Thread safety:
 *   bg_player_start / bg_player_stop / bg_player_is_active are called from
 *   the timeline execution task (FreeRTOS task, not ISR).
 *   bg_player_mix_into is called exclusively from the audio output task.
 *   The ring buffer (FreeRTOS stream buffer) is the only shared resource
 *   between those two tasks; it is single-producer / single-consumer safe.
 *
 * See Plan 006 (plans/006_background_audio.md) for full architecture.
 */

#ifndef BG_PLAYER_H
#define BG_PLAYER_H

#include "config_parser.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Initialise the BG player subsystem.
 *
 * Must be called once from app_main before any bg_player_start() call.
 * When CONFIG_BG_SDCARD_ENABLED=y this will also attempt to mount the SD
 * card via bg_player_sdcard_mount().
 *
 * @return ESP_OK on success.
 */
esp_err_t bg_player_init(void);

/**
 * @brief Start background audio playback.
 *
 * Allocates the 32 KB ring buffer, spawns the bg_streamer_task (priority 3,
 * 6 KB stack), and arms a 220-sample (5 ms) fade-in ramp.  BG audio will
 * begin mixing into the output buffer on the next bg_player_mix_into() call.
 *
 * For http:// or https:// URLs, bg_player_start() checks WiFi connectivity
 * first and returns ESP_ERR_INVALID_STATE if WiFi is not connected.
 *
 * For sdcard:// URLs, the function requires CONFIG_BG_SDCARD_ENABLED=y;
 * otherwise it logs an error and returns ESP_ERR_NOT_SUPPORTED.
 *
 * @param bg  Pointer to the config_bg_entry_t parsed from the .led file.
 *            The struct is copied internally; the caller may free it after
 *            bg_player_start() returns.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t bg_player_start(const config_bg_entry_t *bg);

/**
 * @brief Stop background audio playback.
 *
 * Arms a 220-sample (5 ms) fade-out ramp, signals the streamer task to exit,
 * waits up to 2 s for it to terminate, and frees the ring buffer.
 *
 * Safe to call when BG is not active (returns ESP_OK immediately).
 *
 * @return ESP_OK on success.
 */
esp_err_t bg_player_stop(void);

/**
 * @brief Query whether BG playback is currently active.
 *
 * @return true if bg_player_start() has been called and bg_player_stop()
 *         has not yet completed; false otherwise.
 */
bool bg_player_is_active(void);

/**
 * @brief Mix BG audio into the shared output buffer.
 *
 * Called from audio_test_output_task() after audio_generator_fill_buffer()
 * and before the float→int16 conversion.  Reads from the internal ring
 * buffer (non-blocking), applies the amplitude ramp and linear pan law, and
 * accumulates the result into output_buffer.
 *
 * If the ring buffer has insufficient data (underrun), silence (0.0) is
 * substituted for the missing samples and a warning is logged.
 *
 * @param output_buffer  Stereo interleaved float buffer of length samples*2.
 *                       Layout: [L0, R0, L1, R1, ...].
 * @param samples        Number of stereo sample frames (== AUDIO_GEN_BUFFER_SIZE).
 */
void bg_player_mix_into(float *output_buffer, size_t samples);

#endif // BG_PLAYER_H
