#ifndef SPEECH_PLAYER_H
#define SPEECH_PLAYER_H

/*
 * Speech playback from the SD card — the device half of offline sessions.
 *
 * An `S` row in a .ledc timeline names a phrase by TEXT. The browser has
 * already synthesized that phrase (Puter TTS) and uploaded it to the card as
 *     /sdcard/speech/sp_<hash>.wav
 * so the device never does TTS; it resolves the text to a filename and plays
 * the file.
 *
 * WHY A HASH AND NOT A MANIFEST
 * The filename is derived from the phrase itself, with the SAME function in
 * the firmware and in the browser (web/src/js/gen/sdsync.js). That keeps the
 * `.ledc` format unchanged and avoids a manifest file that could drift out of
 * sync with the card. The cost is that the two implementations MUST agree
 * exactly — see speech_player_filename() and its unit test.
 *
 * WHY WHOLE-FILE, NOT STREAMED
 * bg_player streams because a background track runs for the whole session and
 * cannot fit in RAM. A spoken line is a few seconds (the longest in the
 * shipped library is ~12 s ≈ 2 MB as 44.1 kHz stereo int16), so it is loaded
 * into PSRAM in one go. That removes the ring buffer, the producer task
 * watermarks and the underrun class of bug entirely, which is worth far more
 * here than the memory it costs. Speech also tolerates latency that LED/audio
 * sync does not: a phrase arriving 100 ms late is imperceptible.
 *
 * Mixing is a THIRD stage in the output task, after the tone channels and the
 * background stream (see audio_test.c).
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocate the phrase buffer and start the loader task. Safe to call when no
 *  SD card is present — playback requests then simply log and do nothing. */
esp_err_t speech_player_init(void);

/** Derive the card filename for a phrase. Writes "speech/sp_xxxxxxxx.wav".
 *  MUST match the browser's implementation byte for byte. */
void speech_player_filename(const char *voice, const char *text,
                            char *out, size_t cap);

/** Request playback of a phrase. Returns immediately: the file is loaded by a
 *  background task, so this never blocks the timeline executor. A missing file
 *  is logged and skipped — a phrase that was never uploaded must not abort a
 *  session. `volume` is 0..100. */
esp_err_t speech_player_play(const char *voice, const char *text, float volume);

/** Stop any phrase immediately (timeline stop). */
void speech_player_stop(void);

/** True while a phrase is audible. */
bool speech_player_is_active(void);

/** Mix the current phrase into a 44.1 kHz stereo float buffer.
 *  `samples` is the FRAME count. No-op when idle. */
void speech_player_mix_into(float *output_buffer, size_t samples);

#ifdef __cplusplus
}
#endif

#endif // SPEECH_PLAYER_H
