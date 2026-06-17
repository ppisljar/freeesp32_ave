/**
 * @file wav_parser.h
 * @brief WAV/RIFF header parser — standalone, no I/O, no dynamic allocation.
 *
 * Parses a WAV header from a caller-supplied byte buffer.  The caller is
 * responsible for providing at least the header bytes (44 bytes for a
 * minimal PCM file; 64+ recommended to accommodate extra chunks like LIST or
 * fact before the data chunk).
 *
 * Only PCM WAV files with the following parameters are accepted (the
 * format used by all therapeutic audio sessions in this firmware):
 *   - Audio format : PCM (0x0001)
 *   - Channels     : stereo (2)
 *   - Sample rate  : 44 100 Hz
 *   - Bit depth    : 16-bit signed integer
 *
 * All other formats (mono, 8-bit, 32-bit float, multi-channel, non-44100,
 * WAVE_FORMAT_EXTENSIBLE 0xFFFE) are rejected with ESP_ERR_NOT_SUPPORTED
 * and a descriptive log message so the operator knows exactly what to
 * re-encode the source file to.
 *
 * Path to support WAVE_FORMAT_EXTENSIBLE (0xFFFE) in the future:
 *   The extensible header embeds a "SubFormat" GUID starting at byte 8 of
 *   the fmt chunk body (after the 18-byte base).  The first two bytes of
 *   the GUID are the actual codec (1 = PCM, 3 = float).  To add support:
 *   1. Check audio_format == 0xFFFE.
 *   2. Require fmt chunk size >= 40.
 *   3. Read SubFormat GUID bytes 0-1.  If those == 0x01 0x00, treat as PCM.
 *   4. Validate channels / sample_rate / bits_per_sample as usual.
 *
 * This module is intentionally free of any BG-player or audio-generator
 * headers; it may be used by any WAV-consuming code in the firmware.
 */

#ifndef WAV_PARSER_H
#define WAV_PARSER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parsed WAV format information.
 *
 * Populated by wav_parse_header() on success.  All fields are in host byte
 * order (little-endian already decoded from the RIFF stream).
 */
typedef struct {
    uint16_t channels;          /**< Number of audio channels; always 2 for this firmware. */
    uint32_t sample_rate;       /**< Samples per second; always 44100 for this firmware. */
    uint16_t bits_per_sample;   /**< Bits per sample per channel; always 16 for this firmware. */
    uint32_t data_offset;       /**< Byte offset from the start of the input buffer to the
                                 *   first PCM sample byte.  Equal to the position of the
                                 *   'data' chunk body (chunk header consumed). */
    uint32_t data_size_bytes;   /**< Size of the PCM data in bytes as reported by the 'data'
                                 *   chunk size field.  May be 0xFFFFFFFF for streams of
                                 *   unknown length (e.g. HTTP live stream); the caller is
                                 *   responsible for treating this as "read until EOF". */
} wav_format_t;

/**
 * @brief Parse a WAV header from an in-memory buffer.
 *
 * Walks the RIFF chunk list starting at byte 12 (after the 12-byte RIFF/WAVE
 * preamble).  Unknown chunks (LIST, INFO, bext, fact, …) are skipped using
 * their embedded size field.  Parsing stops as soon as the 'data' chunk is
 * located — PCM data is expected to follow immediately.
 *
 * The function validates:
 *   - RIFF magic and WAVE format identifier.
 *   - Presence and validity of the 'fmt ' chunk.
 *   - audio_format == 1 (PCM); rejects 3 (IEEE float) and 0xFFFE (extensible).
 *   - channels == 2 (stereo).
 *   - sample_rate == 44100.
 *   - bits_per_sample == 16.
 *   - Every byte access is within buf[0..buf_len-1].
 *
 * @param buf            Input buffer containing at least the WAV header.
 *                       A minimum of 44 bytes is required for a standard PCM
 *                       header; 64 bytes is recommended for files with
 *                       additional chunks before 'data'.
 * @param buf_len        Length of buf in bytes.
 * @param out_format     Output struct populated on success.  Undefined on
 *                       error return.
 * @param consumed_bytes Output: number of bytes consumed from buf (i.e. the
 *                       byte offset at which PCM data begins).  This equals
 *                       out_format->data_offset.  May be NULL if the caller
 *                       already reads data_offset from out_format.
 *
 * @return ESP_OK                 WAV is valid, 44.1 kHz / 16-bit / stereo PCM.
 * @return ESP_ERR_INVALID_ARG   buf is NULL, out_format is NULL, buf_len is
 *                                too small, RIFF/WAVE magic is wrong, a chunk
 *                                header or body extends past buf_len, or the
 *                                'data' chunk was not found in buf.
 * @return ESP_ERR_NOT_SUPPORTED  WAV is structurally valid but uses a format
 *                                not supported by this firmware (wrong sample
 *                                rate, wrong channel count, wrong bit depth,
 *                                IEEE float, or extensible format).
 */
esp_err_t wav_parse_header(const uint8_t *buf, size_t buf_len,
                           wav_format_t *out_format,
                           size_t *consumed_bytes);

/**
 * @brief Run the built-in self-test for the WAV parser.
 *
 * Parses a hard-coded 44-byte minimal WAV header and asserts each field of
 * the returned wav_format_t matches expected values.  Logs "WAV parser
 * self-test PASSED" on success or "WAV parser self-test FAILED" + details on
 * failure.
 *
 * Intended to be called once from bg_player_init().  Uses ESP_LOGE / ESP_LOGI
 * only — never aborts; the caller may check the return value and halt if
 * desired.
 *
 * @return ESP_OK if all assertions passed; ESP_FAIL otherwise.
 */
esp_err_t wav_parser_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* WAV_PARSER_H */
