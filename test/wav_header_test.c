/**
 * @file wav_header_test.c
 * @brief Host-side unit tests for the WAV header parser.
 *
 * This is a pure-host test (no ESP-IDF hardware required).  Minimal ESP-IDF
 * stubs (esp_err.h, esp_log.h) are provided in this same directory; the build
 * command places this directory first in the include search path so the stubs
 * shadow the real ESP-IDF headers.
 *
 * Build and run (from the project root: approach2-scratch/):
 *   gcc -I esp32_audioplayer/test \
 *       -I esp32_audioplayer/main \
 *       -o /tmp/wav_test \
 *       esp32_audioplayer/test/wav_header_test.c \
 *       esp32_audioplayer/main/wav_parser.c -lm
 *   /tmp/wav_test
 *
 * Exit code 0 = all tests passed.
 * Exit code 1 = one or more assertions failed (details printed to stdout).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* Pull in the module under test — stub headers in this directory
 * (esp_err.h, esp_log.h) satisfy wav_parser.h's includes. */
#include "wav_parser.h"

/* --------------------------------------------------------------------------
 * Test harness helpers
 * -------------------------------------------------------------------------- */
static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do {                                                                     \
        g_tests_run++;                                                       \
        if (cond) {                                                          \
            g_tests_passed++;                                                \
            printf("  OK   : %s\n", (msg));                                 \
        } else {                                                             \
            g_tests_failed++;                                                \
            printf("  FAIL : %s  (line %d)\n", (msg), __LINE__);           \
        }                                                                    \
    } while (0)

#define TEST_ASSERT_EQ_U32(a, b, msg)                                       \
    do {                                                                     \
        g_tests_run++;                                                       \
        if ((uint32_t)(a) == (uint32_t)(b)) {                               \
            g_tests_passed++;                                                \
            printf("  OK   : %s (%"PRIu32")\n", (msg), (uint32_t)(a));     \
        } else {                                                             \
            g_tests_failed++;                                                \
            printf("  FAIL : %s — got %"PRIu32" expected %"PRIu32           \
                   "  (line %d)\n", (msg),                                  \
                   (uint32_t)(a), (uint32_t)(b), __LINE__);                 \
        }                                                                    \
    } while (0)

#define TEST_ASSERT_EQ_I32(a, b, msg)                                       \
    do {                                                                     \
        g_tests_run++;                                                       \
        if ((int)(a) == (int)(b)) {                                         \
            g_tests_passed++;                                                \
            printf("  OK   : %s (%d / 0x%x)\n", (msg), (int)(a), (int)(a)); \
        } else {                                                             \
            g_tests_failed++;                                                \
            printf("  FAIL : %s — got 0x%x expected 0x%x  (line %d)\n",   \
                   (msg), (int)(a), (int)(b), __LINE__);                    \
        }                                                                    \
    } while (0)

/* --------------------------------------------------------------------------
 * Minimal 44-byte standard WAV header (PCM, stereo, 44100 Hz, 16-bit).
 *
 * Byte-by-byte layout:
 *   Offset  Bytes  Value (hex)       Meaning
 *   ------  -----  ----------------  ------
 *   0       4      52 49 46 46       "RIFF"
 *   4       4      24 00 00 00       ChunkSize = 36 (= 44 - 8)
 *   8       4      57 41 56 45       "WAVE"
 *   12      4      66 6D 74 20       "fmt "
 *   16      4      10 00 00 00       Subchunk1Size = 16 (PCM)
 *   20      2      01 00             AudioFormat = 1 (PCM)
 *   22      2      02 00             NumChannels = 2 (stereo)
 *   24      4      44 AC 00 00       SampleRate = 44100 (0xAC44)
 *   28      4      10 B1 02 00       ByteRate = 176400 (0x02B110)
 *   32      2      04 00             BlockAlign = 4
 *   34      2      10 00             BitsPerSample = 16
 *   36      4      64 61 74 61       "data"
 *   40      4      00 00 00 00       Subchunk2Size = 0 (empty — header only)
 * -------------------------------------------------------------------------- */
static const uint8_t VALID_44_BYTE_PCM_HEADER[44] = {
    /* RIFF preamble */
    'R', 'I', 'F', 'F',
    0x24, 0x00, 0x00, 0x00,          /* ChunkSize = 36 */
    'W', 'A', 'V', 'E',
    /* fmt sub-chunk */
    'f', 'm', 't', ' ',
    0x10, 0x00, 0x00, 0x00,          /* Subchunk1Size = 16 */
    0x01, 0x00,                      /* AudioFormat = 1 (PCM) */
    0x02, 0x00,                      /* NumChannels = 2 */
    0x44, 0xAC, 0x00, 0x00,          /* SampleRate = 44100 */
    0x10, 0xB1, 0x02, 0x00,          /* ByteRate = 176400 */
    0x04, 0x00,                      /* BlockAlign = 4 */
    0x10, 0x00,                      /* BitsPerSample = 16 */
    /* data sub-chunk */
    'd', 'a', 't', 'a',
    0x00, 0x00, 0x00, 0x00           /* Subchunk2Size = 0 */
};

/* --------------------------------------------------------------------------
 * Helper: copy the base header and patch specific bytes.
 * -------------------------------------------------------------------------- */
static void make_header(uint8_t *out, const uint8_t *src, size_t len)
{
    memcpy(out, src, len);
}

/* --------------------------------------------------------------------------
 * Test cases
 * -------------------------------------------------------------------------- */

static void test_valid_header(void)
{
    printf("\n[TEST] Valid standard 44-byte PCM header\n");
    wav_format_t fmt;
    size_t consumed = 0;
    esp_err_t ret = wav_parse_header(VALID_44_BYTE_PCM_HEADER,
                                     sizeof(VALID_44_BYTE_PCM_HEADER),
                                     &fmt, &consumed);
    TEST_ASSERT_EQ_I32(ret, ESP_OK, "Returns ESP_OK");
    TEST_ASSERT_EQ_U32(fmt.channels,        2,     "channels == 2");
    TEST_ASSERT_EQ_U32(fmt.sample_rate,     44100, "sample_rate == 44100");
    TEST_ASSERT_EQ_U32(fmt.bits_per_sample, 16,    "bits_per_sample == 16");
    TEST_ASSERT_EQ_U32(fmt.data_offset,     44,    "data_offset == 44");
    TEST_ASSERT_EQ_U32(fmt.data_size_bytes, 0,     "data_size_bytes == 0");
    TEST_ASSERT_EQ_U32((uint32_t)consumed,  44,    "consumed == 44");
}

static void test_null_args(void)
{
    printf("\n[TEST] NULL arguments\n");
    wav_format_t fmt;
    esp_err_t ret;

    ret = wav_parse_header(NULL, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_INVALID_ARG, "NULL buf → INVALID_ARG");

    ret = wav_parse_header(VALID_44_BYTE_PCM_HEADER, 44, NULL, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_INVALID_ARG, "NULL out_format → INVALID_ARG");
}

static void test_buf_too_small(void)
{
    printf("\n[TEST] Buffer too small\n");
    wav_format_t fmt;
    esp_err_t ret;

    ret = wav_parse_header(VALID_44_BYTE_PCM_HEADER, 4, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_INVALID_ARG, "4-byte buf → INVALID_ARG");

    ret = wav_parse_header(VALID_44_BYTE_PCM_HEADER, 19, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_INVALID_ARG, "19-byte buf → INVALID_ARG");
}

static void test_bad_riff_magic(void)
{
    printf("\n[TEST] Bad RIFF magic\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    buf[0] = 'X';  /* corrupt first byte */

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_INVALID_ARG, "Corrupt RIFF → INVALID_ARG");
}

static void test_bad_wave_magic(void)
{
    printf("\n[TEST] Bad WAVE format identifier\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    buf[8] = 'X';  /* corrupt "WAVE" → "XAVE" */

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_INVALID_ARG, "Corrupt WAVE tag → INVALID_ARG");
}

static void test_wrong_sample_rate(void)
{
    printf("\n[TEST] Wrong sample rate (22050 Hz)\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    /* SampleRate at offset 24-27: set to 22050 = 0x00005622 */
    buf[24] = 0x22; buf[25] = 0x56; buf[26] = 0x00; buf[27] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "22050 Hz → NOT_SUPPORTED");
}

static void test_wrong_channels_mono(void)
{
    printf("\n[TEST] Mono (1 channel)\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    /* NumChannels at offset 22-23: set to 1 */
    buf[22] = 0x01; buf[23] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "1 channel (mono) → NOT_SUPPORTED");
}

static void test_wrong_channels_surround(void)
{
    printf("\n[TEST] 5.1 surround (6 channels)\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    buf[22] = 0x06; buf[23] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "6 channels → NOT_SUPPORTED");
}

static void test_wrong_bit_depth_8(void)
{
    printf("\n[TEST] 8-bit depth\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    /* BitsPerSample at offset 34-35: set to 8 */
    buf[34] = 0x08; buf[35] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "8-bit → NOT_SUPPORTED");
}

static void test_wrong_bit_depth_24(void)
{
    printf("\n[TEST] 24-bit depth\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    buf[34] = 0x18; buf[35] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "24-bit → NOT_SUPPORTED");
}

static void test_wrong_bit_depth_32(void)
{
    printf("\n[TEST] 32-bit integer depth\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    buf[34] = 0x20; buf[35] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "32-bit int → NOT_SUPPORTED");
}

static void test_ieee_float_format(void)
{
    printf("\n[TEST] IEEE float audio format (0x0003)\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    /* AudioFormat at offset 20-21: set to 3 */
    buf[20] = 0x03; buf[21] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "IEEE float (0x0003) → NOT_SUPPORTED");
}

static void test_extensible_format(void)
{
    printf("\n[TEST] WAVE_FORMAT_EXTENSIBLE (0xFFFE)\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    /* AudioFormat at offset 20-21: set to 0xFFFE */
    buf[20] = 0xFE; buf[21] = 0xFF;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "Extensible (0xFFFE) → NOT_SUPPORTED");
}

static void test_unknown_audio_format(void)
{
    printf("\n[TEST] Unknown audio format (0x0055 = MPEG/MP3)\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    buf[20] = 0x55; buf[21] = 0x00;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_ERR_NOT_SUPPORTED, "MP3 format 0x0055 → NOT_SUPPORTED");
}

static void test_skippable_chunks(void)
{
    printf("\n[TEST] Extra chunk between fmt and data (fact chunk, 12 bytes)\n");
    /*
     * Layout (48 bytes total):
     *   0-11:  RIFF/WAVE preamble (RIFF size field = 40 = 48-8)
     *  12-35:  fmt  chunk (8-byte header + 16-byte PCM body)
     *  36-47:  fact chunk (8-byte header + 4-byte body = 12 bytes) — skipped
     *  (This buffer has no room for the data chunk itself, so we need to make
     *   it bigger.)
     *
     * Full 56-byte layout with data chunk appended:
     *   0-11:  RIFF/WAVE preamble (size = 48)
     *  12-35:  fmt  chunk
     *  36-47:  fact chunk (12 bytes)
     *  48-55:  data chunk (8 bytes, 0 PCM)
     */
    static const uint8_t buf_with_fact[56] = {
        /* RIFF preamble */
        'R', 'I', 'F', 'F',
        0x30, 0x00, 0x00, 0x00,   /* ChunkSize = 48 = 56 - 8 */
        'W', 'A', 'V', 'E',
        /* fmt chunk (24 bytes total) */
        'f', 'm', 't', ' ',
        0x10, 0x00, 0x00, 0x00,   /* size = 16 */
        0x01, 0x00,               /* PCM */
        0x02, 0x00,               /* stereo */
        0x44, 0xAC, 0x00, 0x00,   /* 44100 Hz */
        0x10, 0xB1, 0x02, 0x00,   /* byte rate */
        0x04, 0x00,               /* block align */
        0x10, 0x00,               /* 16-bit */
        /* fact chunk (12 bytes total — skipped) */
        'f', 'a', 'c', 't',
        0x04, 0x00, 0x00, 0x00,   /* size = 4 */
        0x00, 0x00, 0x00, 0x00,   /* dwSampleLength = 0 */
        /* data chunk (8 bytes total — 0 PCM bytes) */
        'd', 'a', 't', 'a',
        0x00, 0x00, 0x00, 0x00    /* 0 bytes of PCM */
    };

    wav_format_t fmt;
    size_t consumed = 0;
    esp_err_t ret = wav_parse_header(buf_with_fact, sizeof(buf_with_fact),
                                     &fmt, &consumed);
    TEST_ASSERT_EQ_I32(ret, ESP_OK, "Header with fact chunk → ESP_OK");
    TEST_ASSERT_EQ_U32(fmt.channels,        2,     "channels == 2");
    TEST_ASSERT_EQ_U32(fmt.sample_rate,     44100, "sample_rate == 44100");
    TEST_ASSERT_EQ_U32(fmt.bits_per_sample, 16,    "bits_per_sample == 16");
    /* data chunk body starts at offset 56: 12 preamble + 24 fmt + 12 fact + 8 data header */
    TEST_ASSERT_EQ_U32(fmt.data_offset,     56,    "data_offset == 56");
    TEST_ASSERT_EQ_U32((uint32_t)consumed,  56,    "consumed == 56");
}

static void test_consumed_bytes_is_null(void)
{
    printf("\n[TEST] consumed_bytes parameter can be NULL\n");
    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(VALID_44_BYTE_PCM_HEADER,
                                     sizeof(VALID_44_BYTE_PCM_HEADER),
                                     &fmt, NULL /* consumed_bytes = NULL */);
    TEST_ASSERT_EQ_I32(ret, ESP_OK, "NULL consumed_bytes ptr is accepted");
    TEST_ASSERT_EQ_U32(fmt.data_offset, 44, "data_offset still correct");
}

static void test_data_size_unknown_length(void)
{
    printf("\n[TEST] data chunk size = 0xFFFFFFFF (streaming unknown length)\n");
    uint8_t buf[44];
    make_header(buf, VALID_44_BYTE_PCM_HEADER, 44);
    /* Subchunk2Size at offset 40-43: set to 0xFFFFFFFF */
    buf[40] = 0xFF; buf[41] = 0xFF; buf[42] = 0xFF; buf[43] = 0xFF;

    wav_format_t fmt;
    esp_err_t ret = wav_parse_header(buf, 44, &fmt, NULL);
    TEST_ASSERT_EQ_I32(ret, ESP_OK, "Unknown-length data → ESP_OK");
    TEST_ASSERT_EQ_U32(fmt.data_size_bytes, 0xFFFFFFFFU,
                       "data_size_bytes == 0xFFFFFFFF");
}

static void test_built_in_self_test(void)
{
    printf("\n[TEST] wav_parser_self_test() built-in function\n");
    esp_err_t ret = wav_parser_self_test();
    TEST_ASSERT_EQ_I32(ret, ESP_OK, "wav_parser_self_test() returns ESP_OK");
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(void)
{
    printf("=== WAV header parser host-side unit tests ===\n");

    test_valid_header();
    test_null_args();
    test_buf_too_small();
    test_bad_riff_magic();
    test_bad_wave_magic();
    test_wrong_sample_rate();
    test_wrong_channels_mono();
    test_wrong_channels_surround();
    test_wrong_bit_depth_8();
    test_wrong_bit_depth_24();
    test_wrong_bit_depth_32();
    test_ieee_float_format();
    test_extensible_format();
    test_unknown_audio_format();
    test_skippable_chunks();
    test_consumed_bytes_is_null();
    test_data_size_unknown_length();
    test_built_in_self_test();

    printf("\n=== Results: %d/%d tests passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf(" ===\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
