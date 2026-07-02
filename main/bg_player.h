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
 * @brief Stop background audio playback (blocking, up to ~2.1 s).
 *
 * Arms a 220-sample (5 ms) fade-out ramp, signals the streamer task to exit,
 * waits up to 2 s for it to terminate gracefully, force-deletes the task on
 * timeout, and resets the ring buffer.
 *
 * Use this for explicit user-driven shutdown where you want maximum chance of
 * the HTTP producer cleaning up its own socket before exit.
 *
 * Safe to call when BG is not active (returns ESP_OK immediately).
 *
 * @return ESP_OK on success.
 */
esp_err_t bg_player_stop(void);

/**
 * @brief Stop background audio playback (fast, ~5–200 ms).
 *
 * Same fade-out + signal sequence as bg_player_stop(), but caps the producer-
 * task join timeout at ~200 ms instead of ~2 s. If the producer is mid-recv
 * on a slow HTTP connection, it gets force-deleted sooner — costing one
 * potentially-leaked socket (closed lazily by the next esp_http_client
 * teardown) in exchange for a snappy "PLAY replaces PLAY" UX where a new
 * BG would start immediately after.
 *
 * Use this from the auto-stop path inside config_parser_stop_timeline()
 * and any other latency-sensitive caller.
 *
 * Safe to call when BG is not active (returns ESP_OK immediately).
 *
 * @return ESP_OK on success.
 */
esp_err_t bg_player_stop_async(void);

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

#if CONFIG_BG_SUPPORT_PUSH
/* ---------------------------------------------------------------------------
 * Browser-push producer API (bg_browser_push_plan.md, Phase 1).
 *
 * Instead of the device PULLING BG audio from a URL (bg_player_start +
 * bg_streamer_task), the browser PUSHES already-decoded PCM into the same ring
 * over the /api/bg-stream HTTP endpoint. The caller's thread (the HTTP request
 * handler) becomes the sole producer; bg_streamer_task is NOT spawned.
 *
 * Single-producer invariant: bg_player_start_push() stops any active pull
 * first, and the consumer / mixer / I2S path is byte-identical to the pull
 * path — only the producer changes.
 * --------------------------------------------------------------------------- */

/**
 * @brief Arm push mode.
 *
 * Stops any active BG (pull or push), resets the ring, arms the fade-in ramp,
 * and marks the player active+streaming WITHOUT spawning a producer task. The
 * WiFi-connected gate is skipped (push works in SoftAP-only mode). After this
 * returns ESP_OK the caller feeds audio via bg_player_push_pcm().
 *
 * @param pan       [-1.0, +1.0]; 0 = centre.
 * @param loudness  [0.0, 1.0] gain multiplier.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if not initialised / no ring.
 */
esp_err_t bg_player_start_push(float pan, float loudness);

/**
 * @brief Feed interleaved 16-bit PCM into the ring (blocking / backpressured).
 *
 * Converts `frames` int16 frames (channels 1 or 2) to 44.1 kHz stereo float via
 * the shared decode tail and pushes to the ring. Paces itself against a high
 * watermark: when the ring is near-full it yields (10 ms) and retries, which —
 * because the HTTP handler thread is not calling recv() meanwhile — throttles
 * the browser upload to playback rate via TCP flow control.
 *
 * @param pcm          Interleaved int16 samples, `frames * channels` long.
 * @param frames       Number of input frames.
 * @param channels     1 (mono, duplicated L=R) or 2 (stereo).
 * @param upsample_2x  true for 22050 Hz source (sample-and-hold to 44100).
 * @return Number of INPUT frames consumed. A value < `frames` means push mode
 *         was ended (stop / supersede) mid-call — the caller should stop reading.
 */
size_t bg_player_push_pcm(const int16_t *pcm, size_t frames,
                          unsigned channels, bool upsample_2x);

/**
 * @brief End a push stream on natural completion (browser closed the body).
 *
 * Lets the already-buffered ring audio play out (up to ~a few seconds), then
 * performs the same clean fade-out + teardown as bg_player_stop(). Blocks in
 * the caller's (HTTP handler) thread until drained. For an immediate, tail-
 * dropping stop (user Stop / a superseding POST), call bg_player_stop() instead.
 *
 * @return ESP_OK.
 */
esp_err_t bg_player_end_push(void);
#endif // CONFIG_BG_SUPPORT_PUSH

#endif // BG_PLAYER_H
