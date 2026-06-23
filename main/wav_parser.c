/**
 * @file wav_parser.c
 * @brief WAV/RIFF header parser implementation.
 *
 * See wav_parser.h for the full API and design rationale.
 *
 * RIFF/WAV layout reference (all multi-byte fields are little-endian):
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *   0       4     ChunkID      "RIFF"
 *   4       4     ChunkSize    file size - 8
 *   8       4     Format       "WAVE"
 *
 *   12      4     Subchunk1ID  "fmt "
 *   16      4     Subchunk1Size  16 for PCM
 *   20      2     AudioFormat  1 = PCM, 3 = IEEE float, 0xFFFE = extensible
 *   22      2     NumChannels  1 = mono, 2 = stereo
 *   24      4     SampleRate   samples per second
 *   28      4     ByteRate     SampleRate * NumChannels * BitsPerSample/8
 *   32      2     BlockAlign   NumChannels * BitsPerSample/8
 *   34      2     BitsPerSample  8, 16, 24, 32 ...
 *
 *   (For PCM fmt chunk size == 16; for extensible it's 40.)
 *
 *   36      4     Subchunk2ID  "data"
 *   40      4     Subchunk2Size  number of bytes of PCM data
 *   44      ...   raw PCM samples
 *
 * For non-PCM or files with extra metadata (LIST, fact, bext, etc.) the
 * "data" chunk may appear later.  This parser walks all chunks until it
 * finds "data".
 */

#include "wav_parser.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "wav_parser";

/* Minimum buffer length: 12-byte RIFF/WAVE preamble + 8-byte chunk header. */
#define WAV_MIN_BUF_LEN     20U

/* Supported audio format codes. */
#define WAVE_FORMAT_PCM         0x0001U
#define WAVE_FORMAT_IEEE_FLOAT  0x0003U
#define WAVE_FORMAT_EXTENSIBLE  0xFFFEU

/* Expected firmware parameters.
 * Sample rate: 44100 is the native I2S output rate; 22050 is also accepted
 * because the bg_player can sample-and-hold upsample 22 kHz BG content to
 * 44 kHz on the fly. This halves the network bandwidth required for
 * streamed BG audio — useful for long-running 1-hour ambient sounds where
 * TCP throughput collapse on a single long connection has been observed. */
#define WAV_EXPECTED_SAMPLE_RATE_HI 44100U
#define WAV_EXPECTED_SAMPLE_RATE_LO 22050U
#define WAV_EXPECTED_CHANNELS       2U
#define WAV_EXPECTED_BITS           16U

/* Streaming-unknown-length sentinel from the RIFF spec. */
#define WAV_UNKNOWN_DATA_SIZE       0xFFFFFFFFU

/* -------------------------------------------------------------------------
 * Byte-unpacking helpers — explicit byte-by-byte reads, no alignment casts.
 * ------------------------------------------------------------------------- */

/**
 * @brief Read a uint16_t from a little-endian byte stream at offset @p off.
 *
 * @pre off + 2 <= buf_len  (caller must verify before calling)
 */
static inline uint16_t read_u16_le(const uint8_t *buf, size_t off)
{
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
}

/**
 * @brief Read a uint32_t from a little-endian byte stream at offset @p off.
 *
 * @pre off + 4 <= buf_len  (caller must verify before calling)
 */
static inline uint32_t read_u32_le(const uint8_t *buf, size_t off)
{
    return (uint32_t)buf[off]
         | ((uint32_t)buf[off + 1] <<  8)
         | ((uint32_t)buf[off + 2] << 16)
         | ((uint32_t)buf[off + 3] << 24);
}

/**
 * @brief Compare a 4-byte field in buf at @p off with the 4-char string @p id.
 *
 * @pre off + 4 <= buf_len  (caller must verify before calling)
 */
static inline bool chunk_id_eq(const uint8_t *buf, size_t off, const char *id)
{
    return (buf[off]     == (uint8_t)id[0])
        && (buf[off + 1] == (uint8_t)id[1])
        && (buf[off + 2] == (uint8_t)id[2])
        && (buf[off + 3] == (uint8_t)id[3]);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t wav_parse_header(const uint8_t *buf, size_t buf_len,
                           wav_format_t *out_format,
                           size_t *consumed_bytes)
{
    /* --- Validate caller arguments --------------------------------------- */
    if (buf == NULL || out_format == NULL) {
        ESP_LOGE(TAG, "wav_parse_header: buf or out_format is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (buf_len < WAV_MIN_BUF_LEN) {
        ESP_LOGE(TAG, "WAV: buffer too small: %zu bytes (need at least %u)",
                 buf_len, WAV_MIN_BUF_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    /* --- Validate RIFF magic (bytes 0-3) -------------------------------- */
    if (!chunk_id_eq(buf, 0, "RIFF")) {
        ESP_LOGE(TAG, "WAV: bad RIFF magic: %02x %02x %02x %02x",
                 buf[0], buf[1], buf[2], buf[3]);
        return ESP_ERR_INVALID_ARG;
    }

    /* --- Validate WAVE format tag (bytes 8-11) -------------------------- */
    /* Bytes 4-7 are the RIFF chunk size; we read but don't strictly validate
     * it — some encoders write 0 or an incorrect size for streaming output. */
    if (buf_len < 12) {
        ESP_LOGE(TAG, "WAV: buffer too small to contain WAVE identifier");
        return ESP_ERR_INVALID_ARG;
    }
    if (!chunk_id_eq(buf, 8, "WAVE")) {
        ESP_LOGE(TAG, "WAV: bad WAVE identifier: %02x %02x %02x %02x",
                 buf[8], buf[9], buf[10], buf[11]);
        return ESP_ERR_INVALID_ARG;
    }

    /* --- Walk sub-chunks starting at byte 12 ---------------------------- */
    bool found_fmt  = false;
    bool found_data = false;

    /* Decoded fmt fields (filled when "fmt " chunk is found). */
    uint16_t audio_format    = 0;
    uint16_t num_channels    = 0;
    uint32_t sample_rate     = 0;
    uint16_t bits_per_sample = 0;

    /* Decoded data fields (filled when "data" chunk is found). */
    uint32_t data_offset     = 0;
    uint32_t data_size       = 0;

    size_t pos = 12; /* current byte position in buf */

    while (pos < buf_len) {
        /* Need at least 8 bytes for a chunk header (4 ID + 4 size). */
        if (pos + 8 > buf_len) {
            ESP_LOGE(TAG, "WAV: truncated chunk header at offset %zu "
                     "(only %zu bytes remain)", pos, buf_len - pos);
            return ESP_ERR_INVALID_ARG;
        }

        /* Read chunk ID and size — size is little-endian uint32_t. */
        uint32_t chunk_size = read_u32_le(buf, pos + 4);

        /* ---------------------------------------------------------------- */
        if (chunk_id_eq(buf, pos, "fmt ")) {
            /* fmt chunk: minimum 16 bytes for PCM; may be 18 (non-PCM
             * extension field) or 40 (extensible).  We need at least 16. */
            if (chunk_size < 16) {
                ESP_LOGE(TAG, "WAV: fmt chunk size %"PRIu32" < 16 (invalid)",
                         chunk_size);
                return ESP_ERR_INVALID_ARG;
            }
            /* Bounds check: the full fmt body must be inside buf. */
            if (pos + 8 + chunk_size > buf_len) {
                ESP_LOGE(TAG, "WAV: fmt chunk body extends beyond buffer "
                         "(offset %zu + 8 + %"PRIu32" > %zu)",
                         pos, chunk_size, buf_len);
                return ESP_ERR_INVALID_ARG;
            }

            size_t fmt_body = pos + 8; /* start of fmt chunk body */

            audio_format    = read_u16_le(buf, fmt_body + 0);
            num_channels    = read_u16_le(buf, fmt_body + 2);
            sample_rate     = read_u32_le(buf, fmt_body + 4);
            /* byte_rate   = read_u32_le(buf, fmt_body + 8); -- not validated */
            /* block_align = read_u16_le(buf, fmt_body + 12); -- not validated */
            bits_per_sample = read_u16_le(buf, fmt_body + 14);

            /* Validate audio format. ------------------------------------ */
            if (audio_format == WAVE_FORMAT_IEEE_FLOAT) {
                ESP_LOGE(TAG, "WAV: IEEE float format (0x0003) is not "
                         "supported — re-encode as 16-bit signed PCM (0x0001)");
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (audio_format == WAVE_FORMAT_EXTENSIBLE) {
                ESP_LOGE(TAG, "WAV: WAVE_FORMAT_EXTENSIBLE (0xFFFE) is not "
                         "supported — re-encode as 16-bit signed stereo PCM "
                         "(see wav_parser.h for the extension path)");
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (audio_format != WAVE_FORMAT_PCM) {
                ESP_LOGE(TAG, "WAV: unknown audio format 0x%04"PRIx16
                         " — only PCM (0x0001) is supported", audio_format);
                return ESP_ERR_NOT_SUPPORTED;
            }

            /* Validate channel count. ----------------------------------- */
            if (num_channels != WAV_EXPECTED_CHANNELS) {
                ESP_LOGE(TAG, "WAV: %"PRIu16"-channel audio is not supported "
                         "— expected stereo (%u channels)",
                         num_channels, WAV_EXPECTED_CHANNELS);
                return ESP_ERR_NOT_SUPPORTED;
            }

            /* Validate sample rate. ------------------------------------- */
            if (sample_rate != WAV_EXPECTED_SAMPLE_RATE_HI &&
                sample_rate != WAV_EXPECTED_SAMPLE_RATE_LO) {
                ESP_LOGE(TAG, "WAV: sample rate %"PRIu32" Hz is not supported "
                         "— expected %u or %u Hz",
                         sample_rate,
                         WAV_EXPECTED_SAMPLE_RATE_HI,
                         WAV_EXPECTED_SAMPLE_RATE_LO);
                return ESP_ERR_NOT_SUPPORTED;
            }

            /* Validate bit depth. --------------------------------------- */
            if (bits_per_sample != WAV_EXPECTED_BITS) {
                ESP_LOGE(TAG, "WAV: %"PRIu16"-bit depth is not supported "
                         "— expected %u-bit signed PCM",
                         bits_per_sample, WAV_EXPECTED_BITS);
                return ESP_ERR_NOT_SUPPORTED;
            }

            found_fmt = true;

        /* ---------------------------------------------------------------- */
        } else if (chunk_id_eq(buf, pos, "data")) {
            data_size = chunk_size;

            /* data_offset = first PCM byte = chunk header start + 8. */
            data_offset = (uint32_t)(pos + 8);

            /* If data size is unknown (streaming), note it but don't error. */
            if (data_size == WAV_UNKNOWN_DATA_SIZE) {
                ESP_LOGW(TAG, "WAV: 'data' chunk size is 0xFFFFFFFF "
                         "(stream of unknown length) — caller must read "
                         "until server closes the connection");
            }
            /* If data body would exceed the supplied buffer, that's fine —
             * the caller provided only the header portion of the stream.
             * We don't error here; the streaming layer reads the rest. */

            found_data = true;
            /* Stop walking — everything after 'data' is PCM samples. */
            break;

        /* ---------------------------------------------------------------- */
        } else {
            /* Unknown/skippable chunk (LIST, INFO, fact, bext, id3, …).
             * Log at DEBUG level for traceability. */
            ESP_LOGD(TAG, "WAV: skipping chunk '%c%c%c%c' size=%"PRIu32,
                     buf[pos], buf[pos+1], buf[pos+2], buf[pos+3],
                     chunk_size);

            /* Bounds check before skipping. */
            if (pos + 8 + chunk_size > buf_len) {
                /* The skipped chunk extends past buf — that means 'fmt ' or
                 * 'data' must have come before this chunk.  If we already
                 * found fmt and data we're fine; otherwise fail. */
                if (!found_fmt || !found_data) {
                    ESP_LOGE(TAG, "WAV: chunk '%c%c%c%c' at offset %zu "
                             "size=%"PRIu32" extends beyond buffer (%zu bytes) "
                             "and fmt/data not yet found",
                             buf[pos], buf[pos+1], buf[pos+2], buf[pos+3],
                             pos, chunk_size, buf_len);
                    return ESP_ERR_INVALID_ARG;
                }
                break; /* fmt + data already found before this chunk */
            }
        }

        /* Advance to the next chunk.  RIFF chunks are word-aligned: if size
         * is odd, a padding byte follows the chunk body. */
        size_t advance = 8 + chunk_size;
        if (chunk_size & 1U) {
            advance += 1; /* skip padding byte */
        }

        /* Guard against malformed size that would wrap pos. */
        if (advance > buf_len - pos) {
            /* Past end of buffer — stop walking. */
            break;
        }
        pos += advance;
    }

    /* --- Post-walk validation ------------------------------------------- */
    if (!found_fmt) {
        ESP_LOGE(TAG, "WAV: 'fmt ' chunk not found in buffer "
                 "(%zu bytes scanned)", buf_len);
        return ESP_ERR_INVALID_ARG;
    }
    if (!found_data) {
        ESP_LOGE(TAG, "WAV: 'data' chunk not found in buffer "
                 "(%zu bytes scanned) — provide a larger header buffer",
                 buf_len);
        return ESP_ERR_INVALID_ARG;
    }

    /* --- Populate output struct ----------------------------------------- */
    out_format->channels        = num_channels;
    out_format->sample_rate     = sample_rate;
    out_format->bits_per_sample = bits_per_sample;
    out_format->data_offset     = data_offset;
    out_format->data_size_bytes = data_size;

    if (consumed_bytes != NULL) {
        *consumed_bytes = (size_t)data_offset;
    }

    ESP_LOGI(TAG, "WAV: parsed OK — %"PRIu16"ch / %"PRIu32"Hz / %"PRIu16"-bit, "
             "PCM data at offset %"PRIu32" (%"PRIu32" bytes%s)",
             num_channels, sample_rate, bits_per_sample,
             data_offset,
             data_size,
             (data_size == WAV_UNKNOWN_DATA_SIZE) ? ", unknown length" : "");

    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Built-in self-test
 *
 * Minimal 44-byte PCM WAV header — the smallest possible valid WAV file
 * header.  Byte-by-byte breakdown:
 *
 *  Bytes  Value (hex)       Meaning
 *  -----  ----------------  ------
 *  0-3    52 49 46 46       "RIFF"
 *  4-7    24 00 00 00       ChunkSize = 36  (= 44 - 8; no PCM data)
 *  8-11   57 41 56 45       "WAVE"
 *  12-15  66 6D 74 20       "fmt "
 *  16-19  10 00 00 00       Subchunk1Size = 16 (PCM)
 *  20-21  01 00             AudioFormat = 1 (PCM)
 *  22-23  02 00             NumChannels = 2 (stereo)
 *  24-27  44 AC 00 00       SampleRate = 0xAC44 = 44100
 *  28-31  10 B1 02 00       ByteRate = 44100*2*2 = 176400 = 0x02B110
 *  32-33  04 00             BlockAlign = 4 (2ch * 2bytes)
 *  34-35  10 00             BitsPerSample = 16
 *  36-39  64 61 74 61       "data"
 *  40-43  00 00 00 00       Subchunk2Size = 0 (no PCM data — header only)
 * ------------------------------------------------------------------------- */
static const uint8_t s_test_wav_header[44] = {
    /* RIFF preamble */
    'R', 'I', 'F', 'F',             /* 0-3:  ChunkID "RIFF"            */
    0x24, 0x00, 0x00, 0x00,          /* 4-7:  ChunkSize = 36            */
    'W', 'A', 'V', 'E',             /* 8-11: Format "WAVE"             */

    /* fmt sub-chunk */
    'f', 'm', 't', ' ',             /* 12-15: Subchunk1ID "fmt "       */
    0x10, 0x00, 0x00, 0x00,          /* 16-19: Subchunk1Size = 16       */
    0x01, 0x00,                      /* 20-21: AudioFormat = 1 (PCM)    */
    0x02, 0x00,                      /* 22-23: NumChannels = 2          */
    0x44, 0xAC, 0x00, 0x00,          /* 24-27: SampleRate = 44100       */
    0x10, 0xB1, 0x02, 0x00,          /* 28-31: ByteRate = 176400        */
    0x04, 0x00,                      /* 32-33: BlockAlign = 4           */
    0x10, 0x00,                      /* 34-35: BitsPerSample = 16       */

    /* data sub-chunk */
    'd', 'a', 't', 'a',             /* 36-39: Subchunk2ID "data"       */
    0x00, 0x00, 0x00, 0x00           /* 40-43: Subchunk2Size = 0        */
};

esp_err_t wav_parser_self_test(void)
{
    ESP_LOGI(TAG, "WAV parser self-test: running...");

    wav_format_t fmt;
    size_t consumed = 0;

    esp_err_t ret = wav_parse_header(s_test_wav_header,
                                     sizeof(s_test_wav_header),
                                     &fmt, &consumed);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WAV parser self-test FAILED: wav_parse_header returned "
                 "0x%x (expected ESP_OK)", ret);
        return ESP_FAIL;
    }

    bool ok = true;

#define ASSERT_EQ(field, expected, fmt_spec)                                \
    do {                                                                     \
        if ((field) != (expected)) {                                         \
            ESP_LOGE(TAG, "WAV parser self-test FAILED: " #field            \
                     " = %" fmt_spec ", expected %" fmt_spec,               \
                     (field), (expected));                                   \
            ok = false;                                                      \
        }                                                                    \
    } while (0)

    ASSERT_EQ(fmt.channels,        (uint16_t)2,     PRIu16);
    ASSERT_EQ(fmt.sample_rate,     (uint32_t)44100, PRIu32);
    ASSERT_EQ(fmt.bits_per_sample, (uint16_t)16,    PRIu16);
    ASSERT_EQ(fmt.data_offset,     (uint32_t)44,    PRIu32);
    ASSERT_EQ(fmt.data_size_bytes, (uint32_t)0,     PRIu32);
    ASSERT_EQ((uint32_t)consumed,  (uint32_t)44,    PRIu32);

#undef ASSERT_EQ

    if (!ok) {
        ESP_LOGE(TAG, "WAV parser self-test FAILED");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WAV parser self-test PASSED");
    return ESP_OK;
}
