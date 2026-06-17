/**
 * @file bg_player.c
 * @brief Background audio player — ring buffer producer/consumer implementation.
 *
 * Step map:
 *   Step 1 — Struct types, bg_player.h, empty stubs
 *   Step 2 — Kconfig flag + URL scheme dispatch structure
 *   Step 3 — BG line parser (config_parser.c side)
 *   Step 4 — WAV header parser (wav_parser.c)
 *   Step 5 — Ring buffer, producer/consumer, bg_player_mix_into
 *   Step 6 — HTTP/HTTPS streaming path (bg_stream_from_http real impl)  ← THIS STEP
 *   Step 7 — Timeline pre-roll integration (config_parser.c side)
 *
 * See Plan 006 (plans/006_background_audio.md) for full architecture.
 *
 * ---------------------------------------------------------------------------
 * Step 6 implementation notes
 * ---------------------------------------------------------------------------
 *
 * HTTP/HTTPS streaming path
 * -------------------------
 * bg_stream_from_http() opens an HTTP/HTTPS connection via esp_http_client,
 * reads the WAV header, then calls bg_stream_http_pcm() which streams PCM
 * data into the ring buffer in 2 KB chunks until EOF or until s_bg.streaming
 * goes false (signalled by bg_player_stop).
 *
 * HTTP client configuration:
 *   - timeout_ms = 5000 ms (connect + read timeout)
 *   - buffer_size = 4096 bytes (receive buffer; accommodates WAV header read
 *     and subsequent PCM chunk reads without excessive per-call overhead)
 *   - buffer_size_tx = 1024 bytes (transmit buffer; GET has no body so 1 KB
 *     is sufficient for the request line + headers)
 *   - skip_cert_common_name_check = true; crt_bundle_attach = NULL
 *     (see design decision 4 in plans/006_background_audio.md: self-signed
 *     or internal HTTPS servers are common in therapeutic device deployments;
 *     the risk of MITM is accepted in exchange for deployment simplicity.
 *     For production use with public HTTPS servers, enable
 *     CONFIG_MBEDTLS_CERTIFICATE_BUNDLE in sdkconfig and remove this flag.
 *     See ledc_spec.md Section 3 security note.)
 *
 * WAV parse → PCM stream flow:
 *   1. Read first BG_HTTP_HDR_BUF_BYTES (512) from the HTTP response.
 *   2. Call wav_parse_header(); reject anything that is not 44.1 kHz / 16-bit
 *      / stereo PCM with a descriptive error log.
 *   3. Any bytes in the local header buffer past consumed_bytes (data_offset)
 *      are the first PCM bytes; push them into the ring before the main loop.
 *   4. bg_stream_http_pcm() reads BG_HTTP_CHUNK_BYTES at a time, converts
 *      int16 → float (scale = 1/32768.0f), writes stereo-interleaved floats
 *      to the ring via xStreamBufferSend (blocks when ring is full, throttling
 *      the producer to the consumer's 44.1 kHz drain rate naturally).
 *
 * EOF / loop semantics:
 *   bg_stream_from_http returns when either:
 *     a) esp_http_client_read returns 0 or negative (server closed connection),
 *     b) s_bg.streaming goes false (stop was called).
 *   bg_streamer_task wraps bg_dispatch_url in an outer retry loop (max 3
 *   consecutive failures) so that a completed HTTP stream (EOF = server closed)
 *   automatically re-opens the URL for seamless looping.
 *
 * Error handling:
 *   - WiFi disconnected:  bg_player_start checks wifi_manager_get_state()
 *     before spawning the task; returns ESP_ERR_INVALID_STATE if not connected.
 *   - HTTP init failure:  log + return (ring stays empty → silence underrun).
 *   - HTTP open failure:  log + close + return.
 *   - Non-200 status:     log + close + cleanup + return.
 *   - WAV parse failure:  log + close + cleanup + return.
 *   - Mid-stream drop:    read returns ≤ 0; log + break; close + cleanup.
 *     Consumer handles underrun with zero-fill (silence) — no crash.
 *
 * Ring buffer
 * -----------
 * We use FreeRTOS Stream Buffers (xStreamBuffer) — the correct primitive for
 * single-producer / single-consumer byte streaming with no copy overhead.
 *
 * Size: BG_RING_BYTES = 32 768 bytes of float stereo samples.
 * Math: 44100 Hz × 2 ch × 4 bytes/float = 352 800 bytes/s.
 *       32 768 / 352 800 ≈ 92.8 ms of audio.
 * Trigger level: BG_RING_TRIGGER_BYTES = 1024 bytes (minimum to wake a blocking
 * receive; we use non-blocking receive so this only matters for potential
 * future blocking callers).
 *
 * Producer task (bg_streamer_task)
 * ---------------------------------
 * Priority 18, pinned to core 0.  Rationale:
 *   - LED task runs at 23, timing dispatch at 22.  Audio output task at 5.
 *   - The producer must outrun the consumer (audio output at 5) so the ring
 *     buffer stays filled; priority 18 achieves this without interfering with
 *     the LED/timing tasks at 22–23.
 *   - Core 0 keeps the producer off core 1 where the I2S DMA interrupt fires,
 *     minimising scheduling jitter on the audio output path.
 * Stack: 6 144 bytes (matches the plan spec).
 *
 * Amp ramp
 * --------
 * Mirrors audio_generator.c (see reports/non_planned_reports/
 * fix_amp_step_click_2026-06-15.md).  220 samples = 5 ms at 44.1 kHz.
 * Constant: AUDIO_AMP_RAMP_SAMPLES 220u (defined in audio_generator.c).
 * We duplicate the value as BG_AMP_RAMP_SAMPLES to keep bg_player.c independent
 * of audio_generator.h internals.
 *
 * Consumer (bg_player_mix_into)
 * ------------------------------
 * Called from audio_test_output_task (priority 5) after fill_buffer and before
 * float→int16.  Non-blocking: if the ring buffer has fewer bytes than needed,
 * the remainder is zero-filled (underrun).  NO logging in the hot path —
 * underrun_count is incremented only; logging happens in bg_player_stop.
 *
 * Pan law
 * -------
 * Linear pan law matched to audio_generator.c's apply_panning:
 *   pan_l = (pan <= 0) ? 1.0 : (1.0 - pan)
 *   pan_r = (pan >= 0) ? 1.0 : (1.0 + pan)
 * This attenuates one side while keeping the other at unity, matching what
 * the user hears from the synthesized channels.
 */

#include "bg_player.h"
#include "wav_parser.h"
#include "wifi_manager.h"
#include "audio_generator.h"    /* AUDIO_GEN_BUFFER_SIZE */
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"          /* esp_get_free_heap_size */
#include "esp_heap_caps.h"       /* MALLOC_CAP_SPIRAM */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "bg_player";

/* ---------------------------------------------------------------------------
 * Compile-time constants
 * --------------------------------------------------------------------------- */

/** Ring buffer capacity in bytes (float stereo interleaved).
 *  32 KB ≈ 93 ms at 44 100 Hz / stereo / float.
 *  Must be a power-of-2 multiple for stream-buffer alignment.  */
#define BG_RING_BYTES           65536u  /* 64 KB float stereo ≈ 186 ms headroom.
                                            * Stays in INTERNAL DRAM (fits the
                                            * largest ~111 KB region).  Tried
                                            * 128 KB in PSRAM but the per-sample
                                            * consumer read jitter from PSRAM
                                            * caused more clicking, not less.
                                            * 186 ms is plenty for typical WiFi
                                            * micro-stalls on a quiet LAN. */

/** Trigger level: xStreamBufferCreate wakes a blocking receiver when at least
 *  this many bytes are available.  1024 = 128 stereo float samples.  We use
 *  non-blocking receive (timeout=0), so this only matters if a future caller
 *  uses a blocking receive.                                                   */
#define BG_RING_TRIGGER_BYTES   1024u

/** 5 ms amplitude ramp — mirrors AUDIO_AMP_RAMP_SAMPLES in audio_generator.c.
 *  220 samples at 44 100 Hz = 4.99 ms.  Eliminates click on BG start/stop.  */
#define BG_AMP_RAMP_SAMPLES     220u

/** Producer task stack depth in bytes.  6 KB is the plan spec (Section 2.7).  */
#define BG_STREAMER_STACK_BYTES 4096u

/** Producer task FreeRTOS priority.
 *  LED task = 23, timing dispatch = 22, audio output = 5.
 *  18 outranks the consumer (5) so the ring stays filled; lower than LED/timing
 *  so flicker and scheduling precision are unaffected.                         */
#define BG_STREAMER_PRIORITY    18u

/** Core affinity: pin to core 0, away from the I2S DMA interrupt on core 1.   */
#define BG_STREAMER_CORE        0

/* ---------------------------------------------------------------------------
 * Step 6 HTTP streaming constants
 * --------------------------------------------------------------------------- */

/** HTTP receive buffer size (bytes).
 *  4 KB balances TCP segment coalescing with memory pressure.  The esp_http_client
 *  internal buffer holds at most this many bytes before returning to the caller.  */
#define BG_HTTP_RECV_BUF_BYTES   4096u

/** HTTP transmit buffer size (bytes).
 *  GET request has no body; 1 KB is more than sufficient for the request line
 *  plus all request headers (User-Agent, Host, Connection, Range etc.).          */
#define BG_HTTP_TX_BUF_BYTES     1024u

/** HTTP connect/read timeout in milliseconds.
 *  5 s covers slow WiFi handshake + TCP connection + first HTTP response byte.
 *  The read loop itself uses portMAX_DELAY on the ring buffer; TCP stalls are
 *  handled by the underlying socket timeout.                                     */
#define BG_HTTP_TIMEOUT_MS       5000

/** WAV header read buffer size (bytes).
 *  512 bytes accommodates standard 44-byte headers plus extra chunks (LIST,
 *  INFO, fact, bext) commonly added by DAWs and audio editors.                   */
#define BG_HTTP_HDR_BUF_BYTES    512u

/** PCM streaming chunk size (raw bytes from HTTP).
 *  2 KB = 512 stereo int16 frames.  Small enough to keep the ring buffer
 *  filling in fine-grained increments without excessive call overhead.           */
#define BG_HTTP_CHUNK_BYTES      2048u

/** Maximum consecutive URL re-open failures before the streamer task gives up.
 *  On each success the counter resets to 0, so transient failures during a
 *  long session (e.g. brief WiFi drop, server restart) are tolerated.            */
#define BG_HTTP_MAX_RETRIES      3

/* ---------------------------------------------------------------------------
 * Internal state (single static instance — one BG track per session)
 * --------------------------------------------------------------------------- */

typedef struct {
    StreamBufferHandle_t ring;          /* 32 KB stream buffer (producer→consumer) */
    TaskHandle_t         producer_task; /* bg_streamer_task handle                 */
    SemaphoreHandle_t    state_mutex;   /* protects start/stop/active transitions   */

    volatile bool active;               /* consumer gate: true → mix_into is live   */
    volatile bool streaming;            /* producer gate: true → keep producing      */

    char  url[256];                     /* copy of source URL from config_bg_entry_t */
    float pan;                          /* [-1.0, +1.0]; 0 = centre                 */
    float loudness;                     /* [0.0, 1.0] gain multiplier                */

    /* Amplitude ramp — mirrors fix_amp_step_click_2026-06-15 design.
     * Read and written ONLY by the consumer (bg_player_mix_into), which is
     * called from a single task (audio_test_output_task).  No locking needed. */
    float    current_loudness;          /* live gain, advances toward target          */
    float    target_loudness;           /* final gain after ramp completes            */
    float    loudness_step;             /* per-sample delta (may be negative on stop) */
    uint32_t loudness_ramp_remaining;   /* countdown; 0 = ramp complete               */

    /* Diagnostics (written by consumer, read by stop — harmless data race on
     * uint32_t on the LX6 which has 32-bit atomic loads/stores).              */
    uint32_t underrun_count;
    uint32_t bytes_streamed;
} bg_player_t;

static bg_player_t s_bg;   /* zero-initialised by the linker (.bss)                */

/* Static .bss storage for the ring buffer.
 * +1 byte: FreeRTOS reserves one extra byte for the stream buffer's internal
 * empty/full disambiguation.  Static allocation eliminates heap fragmentation
 * failures — if BG_RING_BYTES is ever too large for internal DRAM, the build
 * fails at link time (not at runtime). */
static uint8_t s_bg_ring_storage[BG_RING_BYTES + 1];
static StaticStreamBuffer_t s_bg_ring_ctrl;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */

static void bg_streamer_task(void *pvParameters);

static esp_err_t bg_stream_from_http(const char *url);
static void      bg_stream_http_pcm(esp_http_client_handle_t client,
                                    const wav_format_t *fmt,
                                    const uint8_t *leftover, size_t leftover_len);

#ifdef CONFIG_BG_SDCARD_ENABLED
static void __attribute__((unused)) bg_stream_from_sdcard(const char *path);
#endif

/* ---------------------------------------------------------------------------
 * URL-scheme dispatch (called from bg_streamer_task)
 * --------------------------------------------------------------------------- */

/**
 * @brief Dispatch to the correct streaming back-end based on URL scheme.
 *
 * HTTP/HTTPS:  calls bg_stream_from_http(), which opens the connection, parses
 *              the WAV header, and streams PCM data into the ring buffer until
 *              EOF or until s_bg.streaming goes false.
 * sdcard://:   calls bg_stream_from_sdcard() if CONFIG_BG_SDCARD_ENABLED=y;
 *              otherwise logs an error and returns ESP_ERR_NOT_SUPPORTED.
 *
 * Returns ESP_OK on clean completion (including EOF), or an error code on
 * failure.  The outer retry loop in bg_streamer_task uses the return value to
 * decide whether to re-open the URL (ESP_OK = EOF → re-open for looping) or
 * bail out after BG_HTTP_MAX_RETRIES consecutive non-OK returns.
 */
static esp_err_t bg_dispatch_url(const char *url)
{
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        /* HTTP/HTTPS streaming — primary URL scheme for Plan 006.              */
        ESP_LOGI(TAG, "BG dispatch: HTTP/HTTPS '%s'", url);
        return bg_stream_from_http(url);

    } else if (strncmp(url, "sdcard://", 9) == 0) {

#ifdef CONFIG_BG_SDCARD_ENABLED
        /* SD card streaming (CONFIG_BG_SDCARD_ENABLED=y).
         * TODO: see bg_stream_from_sdcard() SD bring-up checklist below.      */
        ESP_LOGI(TAG, "BG dispatch: sdcard:// '%s' (stub)", url);
        bg_stream_from_sdcard(url + 9);
        return ESP_OK;
#else
        ESP_LOGE(TAG,
                 "BG: SD card support not enabled in this build "
                 "(CONFIG_BG_SDCARD_ENABLED=n). Ignoring BG entry: %s",
                 url);
        return ESP_ERR_NOT_SUPPORTED;
#endif

    } else {
        ESP_LOGE(TAG, "BG: unknown URL scheme: %s", url);
        return ESP_ERR_INVALID_ARG;
    }
}

/* ---------------------------------------------------------------------------
 * bg_stream_http_pcm — convert and stream PCM from an open HTTP connection
 *
 * Called by bg_stream_from_http after the WAV header has been parsed.
 * Receives:
 *   client       — open esp_http_client handle positioned just past the WAV
 *                  header (i.e. next read will return PCM bytes).
 *   fmt          — parsed WAV format (channels, bits_per_sample verified by
 *                  wav_parse_header to be 2 / 16 respectively).
 *   leftover     — bytes already read from the header buffer that fall AFTER
 *                  the WAV data_offset (the first actual PCM bytes).
 *   leftover_len — byte count of leftover; may be 0.
 *
 * Converts each int16 sample to float via sample / 32768.0f, writes stereo
 * interleaved floats [L, R, L, R, …] into the ring buffer.
 *
 * Blocks on xStreamBufferSend when the ring is full, which throttles the
 * producer to the consumer's drain rate (~352 800 bytes/s at 44.1 kHz stereo
 * float) without busy-waiting.
 *
 * Returns when:
 *   a) esp_http_client_read returns 0 or negative (EOF / server-side close), or
 *   b) s_bg.streaming becomes false (bg_player_stop was called).
 * --------------------------------------------------------------------------- */
static void bg_stream_http_pcm(esp_http_client_handle_t client,
                                const wav_format_t *fmt,
                                const uint8_t *leftover, size_t leftover_len)
{
    /* Allocate raw int16 read buffer on heap (avoids 2 KB stack pressure).     */
    uint8_t *raw_buf = malloc(BG_HTTP_CHUNK_BYTES);
    /* Float output buffer: worst case BG_HTTP_CHUNK_BYTES / 2 int16 samples,
     * each converted to a stereo pair of floats.  For 16-bit stereo the frame
     * size is 4 bytes, so max_frames = BG_HTTP_CHUNK_BYTES / 4 = 512 frames
     * = 1024 floats = 4096 bytes.                                              */
    const size_t frame_bytes  = (size_t)(fmt->channels) * (fmt->bits_per_sample / 8u);
    const size_t max_frames   = BG_HTTP_CHUNK_BYTES / frame_bytes;
    float       *flt_buf      = malloc(max_frames * 2u * sizeof(float));

    if (!raw_buf || !flt_buf) {
        ESP_LOGE(TAG, "BG HTTP: PCM buffer malloc failed (raw=%p flt=%p)",
                 raw_buf, flt_buf);
        free(raw_buf);
        free(flt_buf);
        return;
    }

    /* -----------------------------------------------------------------------
     * 1. Push leftover bytes from the header read (PCM data that was already
     *    fetched when we read the WAV header buffer).
     * ----------------------------------------------------------------------- */
    if (leftover_len > 0u) {
        /* Align to frame boundary so we don't split a sample.                 */
        size_t aligned_len = (leftover_len / frame_bytes) * frame_bytes;
        size_t lo_frames   = aligned_len / frame_bytes;

        for (size_t i = 0; i < lo_frames; i++) {
            const int16_t *p = (const int16_t *)(leftover + i * frame_bytes);
            flt_buf[i * 2u]       = (float)p[0] / 32768.0f;
            flt_buf[i * 2u + 1u]  = (fmt->channels == 2u) ? (float)p[1] / 32768.0f
                                                            : flt_buf[i * 2u];
        }

        size_t bytes_to_send = lo_frames * 2u * sizeof(float);
        if (bytes_to_send > 0u && s_bg.ring != NULL) {
            xStreamBufferSend(s_bg.ring, flt_buf, bytes_to_send, portMAX_DELAY);
            s_bg.bytes_streamed += (uint32_t)bytes_to_send;
        }
    }

    /* -----------------------------------------------------------------------
     * 2. Main streaming loop: read BG_HTTP_CHUNK_BYTES at a time from HTTP,
     *    convert int16→float, push to ring.
     * ----------------------------------------------------------------------- */
    while (s_bg.streaming) {
        if (s_bg.ring == NULL) {
            break;
        }

        int bytes_read = esp_http_client_read(client, (char *)raw_buf,
                                              (int)BG_HTTP_CHUNK_BYTES);
        if (bytes_read <= 0) {
            /* bytes_read == 0: clean EOF (server closed connection).
             * bytes_read  < 0: socket error / timeout.
             * Both cases terminate the stream; the caller decides whether to
             * re-open the URL (loop) or abort.                                */
            if (bytes_read < 0) {
                ESP_LOGW(TAG,
                         "BG HTTP: read error %d mid-stream — dropping connection",
                         bytes_read);
            } else {
                ESP_LOGI(TAG, "BG HTTP: EOF reached");
            }
            break;
        }

        /* Convert raw bytes to stereo float frames.                          */
        size_t frames = (size_t)bytes_read / frame_bytes;
        for (size_t i = 0; i < frames; i++) {
            const int16_t *p = (const int16_t *)(raw_buf + i * frame_bytes);
            flt_buf[i * 2u]       = (float)p[0] / 32768.0f;
            flt_buf[i * 2u + 1u]  = (fmt->channels == 2u) ? (float)p[1] / 32768.0f
                                                            : flt_buf[i * 2u];
        }

        size_t bytes_to_send = frames * 2u * sizeof(float);
        if (bytes_to_send == 0u) {
            /* Partial frame at end of stream — discard and let read loop end. */
            continue;
        }

        /* Block until the ring buffer has space.  This is the natural back-
         * pressure mechanism: if the consumer (audio output task at priority 5)
         * falls behind, this send blocks and the producer waits rather than
         * over-filling the ring.                                              */
        size_t sent = xStreamBufferSend(s_bg.ring, flt_buf, bytes_to_send,
                                        pdMS_TO_TICKS(500));
        if (sent < bytes_to_send) {
            /* Timeout on send — ring should not stay full for 500 ms.
             * This can happen if the consumer task was suspended.  Log once
             * and continue; do not treat as a fatal error.                   */
            ESP_LOGW(TAG,
                     "BG HTTP: ring send timeout (wanted %zu, sent %zu)",
                     bytes_to_send, sent);
        }
        s_bg.bytes_streamed += (uint32_t)sent;

        /* Periodic diagnostic — every ~5 s log underrun count and ring level.
         * 5 s at 176 KB/s file rate = ~880 KB → ~440 chunks of 2 KB.           */
        static uint32_t s_diag_counter = 0u;
        static uint32_t s_last_underrun_count = 0u;
        if (++s_diag_counter >= 440u) {
            uint32_t now_underruns = s_bg.underrun_count;
            uint32_t delta_underruns = now_underruns - s_last_underrun_count;
            size_t ring_filled = xStreamBufferBytesAvailable(s_bg.ring);
            ESP_LOGI(TAG,
                     "BG diag: ring=%zu/%u B (%.0f%% full), underruns=%u (+%u in last 5s), "
                     "bytes_streamed=%u",
                     ring_filled, BG_RING_BYTES,
                     100.0f * (float)ring_filled / (float)BG_RING_BYTES,
                     now_underruns, delta_underruns,
                     s_bg.bytes_streamed);
            s_last_underrun_count = now_underruns;
            s_diag_counter = 0u;
        }
    }

    free(raw_buf);
    free(flt_buf);
}

/* ---------------------------------------------------------------------------
 * bg_stream_from_http — open HTTP/HTTPS connection and stream WAV audio
 *
 * Opens an esp_http_client connection to the supplied URL, validates the
 * HTTP response status, parses the WAV header, then calls bg_stream_http_pcm
 * to convert and stream PCM data into the ring buffer.
 *
 * HTTP client configuration:
 *   - skip_cert_common_name_check = true: accepts self-signed HTTPS certs.
 *     For production use with public HTTPS servers, enable
 *     CONFIG_MBEDTLS_CERTIFICATE_BUNDLE in sdkconfig and remove this flag.
 *     See ledc_spec.md Section 3 security note.
 *
 * Returns:
 *   ESP_OK              — streaming finished cleanly (EOF reached or stop
 *                         requested by caller).  The outer retry loop in
 *                         bg_streamer_task treats ESP_OK as "re-open for loop".
 *   ESP_FAIL            — HTTP init / open / read failed; retry loop increments
 *                         the failure counter.
 *   ESP_ERR_INVALID_ARG — URL is NULL.
 * --------------------------------------------------------------------------- */
static esp_err_t bg_stream_from_http(const char *url)
{
    if (!url) {
        return ESP_ERR_INVALID_ARG;
    }

    /* -----------------------------------------------------------------------
     * 1. Initialise the HTTP client.
     * ----------------------------------------------------------------------- */
    esp_http_client_config_t http_cfg = {
        .url                       = url,
        .method                    = HTTP_METHOD_GET,
        .timeout_ms                = BG_HTTP_TIMEOUT_MS,
        .buffer_size               = BG_HTTP_RECV_BUF_BYTES,
        .buffer_size_tx            = BG_HTTP_TX_BUF_BYTES,
        .disable_auto_redirect     = false,
        /* For production use with public HTTPS servers, enable
         * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE in sdkconfig and remove this flag.
         * See ledc_spec.md Section 3 security note.                           */
        .skip_cert_common_name_check = true,
        .crt_bundle_attach           = NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "BG HTTP: esp_http_client_init failed for '%s'", url);
        return ESP_FAIL;
    }

    /* -----------------------------------------------------------------------
     * 2. Open the connection and issue the GET request.
     * ----------------------------------------------------------------------- */
    esp_err_t err = esp_http_client_open(client, 0 /* write_len = 0 for GET */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BG HTTP: esp_http_client_open failed: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* -----------------------------------------------------------------------
     * 3. Fetch response headers and validate status code.
     * ----------------------------------------------------------------------- */
    int64_t content_length = esp_http_client_fetch_headers(client);
    int     http_status    = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG,
             "BG HTTP: '%s' → status=%d content_length=%lld",
             url, http_status, content_length);

    if (http_status != 200) {
        ESP_LOGE(TAG,
                 "BG HTTP: unexpected HTTP status %d for '%s' (expected 200)",
                 http_status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* -----------------------------------------------------------------------
     * 4. Read the WAV header from the first bytes of the response body.
     * ----------------------------------------------------------------------- */
    uint8_t hdr_buf[BG_HTTP_HDR_BUF_BYTES];
    int hdr_bytes = esp_http_client_read(client, (char *)hdr_buf,
                                         (int)sizeof(hdr_buf));
    if (hdr_bytes < 44) {
        ESP_LOGE(TAG,
                 "BG HTTP: response too short for WAV header "
                 "(got %d bytes, need >= 44)",
                 hdr_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    wav_format_t fmt;
    size_t consumed_bytes = 0u;
    esp_err_t wav_err = wav_parse_header(hdr_buf, (size_t)hdr_bytes,
                                         &fmt, &consumed_bytes);
    if (wav_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "BG HTTP: WAV header parse failed (err=%s) for '%s'",
                 esp_err_to_name(wav_err), url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "BG HTTP: WAV OK — %u ch / %u Hz / %u-bit, "
             "data_offset=%u data_size=%u consumed=%zu",
             (unsigned)fmt.channels, (unsigned)fmt.sample_rate,
             (unsigned)fmt.bits_per_sample,
             (unsigned)fmt.data_offset, (unsigned)fmt.data_size_bytes,
             consumed_bytes);

    /* Any bytes we already read that are past the header are the first PCM
     * bytes.  Pass them to bg_stream_http_pcm as "leftover".                 */
    const uint8_t *leftover     = hdr_buf + consumed_bytes;
    size_t         leftover_len = (size_t)hdr_bytes > consumed_bytes
                                  ? (size_t)hdr_bytes - consumed_bytes
                                  : 0u;

    /* -----------------------------------------------------------------------
     * 5. Stream PCM data into the ring buffer.
     * ----------------------------------------------------------------------- */
    bg_stream_http_pcm(client, &fmt, leftover, leftover_len);

    /* -----------------------------------------------------------------------
     * 6. Clean up the HTTP connection.
     * ----------------------------------------------------------------------- */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG,
             "BG HTTP: stream ended for '%s' (bytes_streamed=%u, streaming=%d)",
             url, s_bg.bytes_streamed, (int)s_bg.streaming);

    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * bg_stream_from_sdcard — SD card streaming stub (CONFIG_BG_SDCARD_ENABLED=y)
 *
 * SD bring-up checklist (see Plan 006, Section 2.4 for full detail):
 *
 *  1. Wire SD card to ESP32 via SDMMC or SPI-to-SD.
 *       SDMMC (4-bit):  CLK=GPIO14, CMD=GPIO15, D0=GPIO2, D1=GPIO4,
 *                       D2=GPIO12, D3=GPIO13 (classic ESP32 SDMMC mapping).
 *       SDSPI:          CLK=GPIO18, MOSI=GPIO23, MISO=GPIO19, CS=GPIO5
 *                       (adjust to the actual board layout).
 *
 *  2. Enable the SD host driver in sdkconfig / menuconfig:
 *       SDMMC host:  Component config → SD/MMC → CONFIG_SDMMC_HOST_SLOT1
 *       SDSPI host:  Component config → SPI → CONFIG_SPI_MASTER_ISR_IN_IRAM
 *       Also set CONFIG_FATFS_VOLUME_COUNT >= 2 (already set in this project).
 *
 *  3. Add the required headers to this file:
 *       #include "driver/sdmmc_host.h"   // or driver/sdspi_host.h
 *       #include "esp_vfs_fat.h"
 *       #include "sdmmc_cmd.h"
 *
 *  4. Implement bg_player_sdcard_mount() and call it from bg_player_init():
 *       sdmmc_host_t host = SDMMC_HOST_DEFAULT();
 *       sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
 *       esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
 *           .format_if_mount_failed = false, .max_files = 4,
 *           .allocation_unit_size = 16 * 1024,
 *       };
 *       sdmmc_card_t *card;
 *       esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);
 *       (For SDSPI: use esp_vfs_fat_sdspi_mount() with sdspi_device_config_t.)
 *
 *  5. Translate "sdcard://path.wav" → "/sdcard/path.wav":
 *       The `path` argument here is the part AFTER "sdcard://", so prepend
 *       "/sdcard/" to get the VFS path.  Open with fopen, pass to the WAV
 *       parser (Step 4), then stream into the ring buffer — identical to the
 *       HTTP path.  Call fclose() when done or on error.
 *
 * @param path  File path after "sdcard://" prefix (e.g. "rain.wav").
 * --------------------------------------------------------------------------- */
#ifdef CONFIG_BG_SDCARD_ENABLED
static void __attribute__((unused)) bg_stream_from_sdcard(const char *path)
{
    ESP_LOGW(TAG,
             "BG: bg_stream_from_sdcard('%s') — SD streaming not yet implemented. "
             "See bg_player.c SD bring-up checklist (Plan 006 Section 2.4).",
             path);
    (void)path;
}
#endif /* CONFIG_BG_SDCARD_ENABLED */

/* ---------------------------------------------------------------------------
 * bg_streamer_task — producer (FreeRTOS task, priority BG_STREAMER_PRIORITY)
 *
 * Dispatches to the correct streaming back-end (HTTP/HTTPS or SD card) via
 * bg_dispatch_url().  For HTTP/HTTPS, bg_stream_from_http() blocks inside
 * esp_http_client_read() for the full stream duration and returns when either:
 *   a) the server closes the connection (EOF), or
 *   b) s_bg.streaming becomes false (bg_player_stop was called).
 *
 * Outer retry / loop logic:
 *   When bg_dispatch_url returns ESP_OK (clean EOF), the outer loop immediately
 *   re-opens the URL to implement seamless looping.  This means an HTTP WAV
 *   stream automatically replays from the beginning when it reaches the end.
 *
 *   When bg_dispatch_url returns a non-OK error (HTTP failure, parse error,
 *   network drop), the failure counter is incremented.  If the counter reaches
 *   BG_HTTP_MAX_RETRIES consecutive failures, the task logs a warning and exits.
 *   A successful play resets the counter to 0, so transient errors during a
 *   long session (brief WiFi drop, server restart) are tolerated.
 *
 *   A brief delay (200 ms) before each re-open prevents a tight spin loop if
 *   the server is temporarily unavailable.
 *
 * Priority / affinity:
 *   Priority 18, core 0.  See compile-time constant comments and the Step 5
 *   design section at the top of this file for full rationale.
 * --------------------------------------------------------------------------- */
static void bg_streamer_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "bg_streamer_task: started (priority %u, core %d)",
             BG_STREAMER_PRIORITY, xPortGetCoreID());

    int consecutive_failures = 0;

    while (s_bg.streaming) {

        esp_err_t rc = bg_dispatch_url(s_bg.url);

        if (!s_bg.streaming) {
            /* bg_player_stop() was called — exit cleanly regardless of rc.   */
            break;
        }

        if (rc == ESP_OK) {
            /* Clean EOF: stream completed, reset failure counter and loop.
             * Brief pause before re-opening to avoid hammering the server on
             * very short files (< 200 ms duration).                           */
            consecutive_failures = 0;
            ESP_LOGI(TAG,
                     "bg_streamer: EOF on '%s', re-opening for loop",
                     s_bg.url);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            /* Error path: increment failure counter.                          */
            consecutive_failures++;
            ESP_LOGW(TAG,
                     "bg_streamer: dispatch error %s for '%s' "
                     "(failure %d/%d)",
                     esp_err_to_name(rc), s_bg.url,
                     consecutive_failures, BG_HTTP_MAX_RETRIES);

            if (consecutive_failures >= BG_HTTP_MAX_RETRIES) {
                ESP_LOGE(TAG,
                         "bg_streamer: %d consecutive failures for '%s' — "
                         "aborting BG stream",
                         consecutive_failures, s_bg.url);
                break;
            }

            /* Back-off before next retry attempt.                             */
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "bg_streamer_task: exiting (bytes_streamed=%u, underruns=%u)",
             s_bg.bytes_streamed, s_bg.underrun_count);

    /* Signal that the task has exited so bg_player_stop can unblock.          */
    s_bg.producer_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public API implementation
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the BG player subsystem.
 *
 * Creates the state mutex.  Does NOT allocate the ring buffer or start the
 * streamer task — that happens in bg_player_start().
 *
 * Also runs the WAV parser self-test (Step 4) so any parser regression is
 * caught at boot rather than mid-session.
 */
esp_err_t bg_player_init(void)
{
    ESP_LOGI(TAG, "bg_player_init");

    /* Run WAV parser built-in self-test (implemented in Step 4).             */
    esp_err_t wav_test = wav_parser_self_test();
    if (wav_test != ESP_OK) {
        ESP_LOGE(TAG,
                 "bg_player_init: WAV parser self-test FAILED (err 0x%x) "
                 "— BG audio will not work correctly on malformed WAV files",
                 wav_test);
        /* Propagate: caller (app_main) can decide whether to halt.            */
        return wav_test;
    }

    if (s_bg.state_mutex == NULL) {
        s_bg.state_mutex = xSemaphoreCreateMutex();
        if (s_bg.state_mutex == NULL) {
            ESP_LOGE(TAG, "bg_player_init: failed to create state mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Initialize the ring buffer ONCE at boot from STATIC storage.
     * Rationale: xStreamBufferCreate's heap-based allocation requires a
     * contiguous internal DRAM block (pvPortMalloc is hardcoded to
     * MALLOC_CAP_INTERNAL), and with PSRAM enabled the available internal
     * DRAM is fragmented across many small consumers (WiFi static buffers,
     * audio_led_sync queue, SPIRAM DMA reserve).  Every runtime allocation
     * fights every other for the same finite contiguous space, producing
     * order-dependent failures.  Static .bss allocation eliminates the
     * problem at link time: if the buffer doesn't fit, the build fails
     * (not the runtime).  Same pattern as memory_pool's static entry pool
     * and audio_generator's static sine LUT.                                 */
    if (s_bg.ring == NULL) {
        ESP_LOGI(TAG, "bg_player_init: free heap = %u bytes (ring is static .bss)",
                 (unsigned)esp_get_free_heap_size());
        s_bg.ring = xStreamBufferCreateStatic(BG_RING_BYTES, BG_RING_TRIGGER_BYTES,
                                              s_bg_ring_storage, &s_bg_ring_ctrl);
        if (s_bg.ring == NULL) {
            /* Cannot actually fail with static storage — kept for defensiveness. */
            ESP_LOGE(TAG, "bg_player_init: xStreamBufferCreateStatic returned NULL?!");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "bg_player_init: ring buffer initialized (%u bytes, static)",
                 BG_RING_BYTES);
    }

    /*
     * When CONFIG_BG_SDCARD_ENABLED=y, call bg_player_sdcard_mount() here.
     * That function is not yet implemented.  See bg_stream_from_sdcard()
     * comment block for the full SD bring-up checklist.
     */

    ESP_LOGI(TAG, "bg_player_init: OK");
    return ESP_OK;
}

/**
 * @brief Start background audio playback.
 *
 * Copies config, arms the 220-sample fade-in ramp, allocates the 32 KB ring
 * buffer, and spawns bg_streamer_task.
 */
esp_err_t bg_player_start(const config_bg_entry_t *bg)
{
    if (!bg) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Serialize concurrent start/stop calls.                                  */
    if (s_bg.state_mutex == NULL) {
        ESP_LOGE(TAG, "bg_player_start: not initialised — call bg_player_init first");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);

    /* If already active, stop the previous session first.                     */
    if (s_bg.active) {
        xSemaphoreGive(s_bg.state_mutex);   /* bg_player_stop takes the mutex */
        bg_player_stop();
        xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);
    }

    /* For HTTP/HTTPS URLs, require WiFi connectivity before spawning the task.
     * If WiFi is not connected the esp_http_client_open call inside
     * bg_stream_from_http will immediately fail with a socket error, which
     * would count as a retry failure and exhaust BG_HTTP_MAX_RETRIES before
     * the user has a chance to connect.  Failing fast here gives a cleaner
     * error path.
     *
     * SD card URLs bypass this check — they do not require WiFi.              */
    if (strncmp(bg->url, "http://", 7) == 0 ||
        strncmp(bg->url, "https://", 8) == 0) {
        if (wifi_manager_get_state() != WIFI_STATE_CONNECTED) {
            ESP_LOGE(TAG,
                     "bg_player_start: WiFi not connected — cannot stream '%s'",
                     bg->url);
            xSemaphoreGive(s_bg.state_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Copy configuration from caller's struct.                                */
    strncpy(s_bg.url, bg->url, sizeof(s_bg.url) - 1);
    s_bg.url[sizeof(s_bg.url) - 1] = '\0';
    s_bg.pan      = bg->pan;
    s_bg.loudness = bg->loudness;

    /* Arm 220-sample (5 ms) fade-in ramp — mirrors audio_generator.c
     * start_channel_locked to eliminate click on BG start.
     * See fix_amp_step_click_2026-06-15.md, Site 3.                           */
    s_bg.current_loudness       = 0.0f;
    s_bg.target_loudness        = bg->loudness;
    s_bg.loudness_step          = bg->loudness / (float)BG_AMP_RAMP_SAMPLES;
    s_bg.loudness_ramp_remaining = BG_AMP_RAMP_SAMPLES;

    /* Reset diagnostics for this session.                                     */
    s_bg.underrun_count  = 0u;
    s_bg.bytes_streamed  = 0u;

    /* Ring buffer is allocated at boot in bg_player_init.  Drain any stale
     * data from a previous session so the new stream starts at byte 0.        */
    if (s_bg.ring == NULL) {
        ESP_LOGE(TAG, "bg_player_start: ring not allocated — bg_player_init failed at boot?");
        xSemaphoreGive(s_bg.state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xStreamBufferReset(s_bg.ring);

    /* Set streaming flag BEFORE spawning the task so the task's first check
     * of s_bg.streaming sees true.                                            */
    s_bg.streaming = true;

    /* Spawn producer task pinned to core BG_STREAMER_CORE.                   */
    ESP_LOGI(TAG, "bg_player_start: free heap before task spawn = %u bytes",
             (unsigned)esp_get_free_heap_size());
    BaseType_t rc = xTaskCreatePinnedToCore(
        bg_streamer_task,
        "bg_stream",
        BG_STREAMER_STACK_BYTES,
        NULL,                       /* pvParameters — task reads s_bg directly */
        BG_STREAMER_PRIORITY,
        &s_bg.producer_task,
        BG_STREAMER_CORE
    );

    if (rc != pdPASS) {
        ESP_LOGE(TAG,
                 "bg_player_start: xTaskCreatePinnedToCore failed (stack=%u, free heap=%u)",
                 BG_STREAMER_STACK_BYTES,
                 (unsigned)esp_get_free_heap_size());
        s_bg.streaming = false;
        /* Ring stays allocated for the next attempt. */
        xSemaphoreGive(s_bg.state_mutex);
        return ESP_FAIL;
    }

    /* Arm active flag — consumer starts mixing on next bg_player_mix_into().  */
    s_bg.active = true;

    xSemaphoreGive(s_bg.state_mutex);

    ESP_LOGI(TAG,
             "bg_player_start: OK — url='%s' pan=%.2f loudness=%.2f "
             "(ring=%u B, ramp=%u samples, task prio=%u core=%d)",
             s_bg.url, s_bg.pan, s_bg.loudness,
             BG_RING_BYTES, BG_AMP_RAMP_SAMPLES,
             BG_STREAMER_PRIORITY, BG_STREAMER_CORE);
    return ESP_OK;
}

/**
 * @brief Stop background audio playback.
 *
 * Arms a 220-sample fade-out ramp, signals the streamer task to exit,
 * waits up to 2 s for it to terminate, then frees the ring buffer.
 *
 * The fade-out ramp is applied in the NEXT calls to bg_player_mix_into().
 * Because bg_player_stop() may race with the consumer, we clear s_bg.active
 * only after the ramp window has been drained.  In practice the output task
 * drains the ramp in 220/44100 ≈ 5 ms, negligible for a stop operation.
 *
 * Note: setting s_bg.active = false before deleting the ring buffer is
 * sufficient because bg_player_mix_into() checks active at the top and returns
 * immediately; there is no TOCTOU risk since both paths are on the same CPU
 * and there is only one consumer task.
 */
esp_err_t bg_player_stop(void)
{
    if (s_bg.state_mutex == NULL) {
        return ESP_OK;   /* Not initialised — nothing to stop. */
    }

    xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);

    if (!s_bg.active) {
        xSemaphoreGive(s_bg.state_mutex);
        return ESP_OK;
    }

    /* Arm 220-sample fade-out ramp from current amplitude to 0.
     * Mirrors audio_generator.c's approach: step is negative, ramp counts down.
     * This is read by bg_player_mix_into() (consumer task), which is safe because
     * active remains true through the ramp window.                               */
    if (s_bg.current_loudness > 1.0e-4f) {
        s_bg.target_loudness         = 0.0f;
        s_bg.loudness_step           = -(s_bg.current_loudness / (float)BG_AMP_RAMP_SAMPLES);
        s_bg.loudness_ramp_remaining  = BG_AMP_RAMP_SAMPLES;
    } else {
        /* Already silent — no ramp needed.                                    */
        s_bg.loudness_ramp_remaining = 0u;
        s_bg.current_loudness        = 0.0f;
        s_bg.target_loudness         = 0.0f;
    }

    /* Signal the producer to exit its loop.                                   */
    s_bg.streaming = false;

    /* Gate the consumer AFTER the ramp is complete.
     * Wait for the fade-out to drain (220 samples / 44100 Hz ≈ 5 ms → allow 30 ms
     * to account for task scheduling jitter at 1 ms tick rate).               */
    xSemaphoreGive(s_bg.state_mutex);
    vTaskDelay(pdMS_TO_TICKS(30));
    xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);

    /* Now safe to stop the consumer.                                          */
    s_bg.active = false;

    /* Wait for producer task to exit (it sets producer_task = NULL on exit).
     * Timeout: 200 × 10 ms = 2 s.                                            */
    if (s_bg.producer_task != NULL) {
        for (int i = 0; i < 200 && s_bg.producer_task != NULL; i++) {
            xSemaphoreGive(s_bg.state_mutex);
            vTaskDelay(pdMS_TO_TICKS(10));
            xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);
        }
        if (s_bg.producer_task != NULL) {
            /* Force-delete if it did not exit cleanly within 2 s.             */
            ESP_LOGW(TAG, "bg_player_stop: producer task did not exit in 2 s — force deleting");
            vTaskDelete(s_bg.producer_task);
            s_bg.producer_task = NULL;
        }
    }

    /* Drain (don't free) the ring buffer — kept across sessions to avoid
     * heap fragmentation churn.  Allocation lives for the lifetime of the
     * device, owned by bg_player_init.                                        */
    if (s_bg.ring != NULL) {
        xStreamBufferReset(s_bg.ring);
    }

    ESP_LOGI(TAG,
             "bg_player_stop: done (underruns=%u, bytes_streamed=%u)",
             s_bg.underrun_count, s_bg.bytes_streamed);

    xSemaphoreGive(s_bg.state_mutex);
    return ESP_OK;
}

/**
 * @brief Query whether BG playback is currently active.
 *
 * Called from audio_test_output_task to gate the bg_player_mix_into() call.
 * Reading a volatile bool is atomic on the LX6 — no mutex required.
 */
bool bg_player_is_active(void)
{
    return s_bg.active;
}

/**
 * @brief Mix BG audio into the shared output buffer (consumer / hot path).
 *
 * Called from audio_test_output_task after audio_generator_fill_buffer() and
 * before the float→int16 conversion.  This function MUST be fast:
 *   - Non-blocking ring buffer read (timeout = 0).
 *   - No heap allocation (uses static .bss scratch buffer).
 *   - No logging in the hot path (underrun_count only).
 *   - No mutex (single consumer, single producer, stream buffer is SP/SC safe).
 *
 * Underrun handling:
 *   If the ring buffer has fewer bytes than needed, the remainder is zero-filled
 *   (silence).  underrun_count is incremented for diagnostics; the actual log
 *   appears in bg_player_stop() rather than here to keep the hot path lean.
 *
 * Pan law (matches audio_generator.c apply_panning):
 *   pan_l = (pan <= 0) ? 1.0f : (1.0f - pan)
 *   pan_r = (pan >= 0) ? 1.0f : (1.0f + pan)
 *
 * @param output_buffer  Stereo interleaved float [L0,R0,L1,R1,...], length=samples*2.
 * @param samples        Number of stereo frames to mix (== AUDIO_GEN_BUFFER_SIZE).
 */
void bg_player_mix_into(float *output_buffer, size_t samples)
{
    /* Guard: return immediately if BG is not active.
     * The output task SHOULD gate with bg_player_is_active() before calling,
     * but this check provides a safety net.                                    */
    if (!s_bg.active || s_bg.ring == NULL) {
        return;
    }

    /* Static .bss scratch buffer — avoids 8 KB stack allocation.
     * Safe: bg_player_mix_into is only ever called from one task.             */
    static float s_mix_scratch[AUDIO_GEN_BUFFER_SIZE * 2];

    size_t bytes_needed = samples * 2u * sizeof(float);

    /* Non-blocking read: returns however many bytes are available right now.   */
    size_t bytes_got = xStreamBufferReceive(s_bg.ring, s_mix_scratch,
                                            bytes_needed, 0 /* ticks timeout */);

    if (bytes_got < bytes_needed) {
        /* Underrun: zero-fill the missing portion (substitute silence).
         * No log here — hot path.  Diagnostics collected by stop().           */
        memset((uint8_t *)s_mix_scratch + bytes_got, 0, bytes_needed - bytes_got);
        /* Increment only on a complete underrun (got nothing) to avoid
         * flooding the counter on partial reads.                               */
        if (bytes_got == 0u) {
            s_bg.underrun_count++;
        }
    }

    /* -----------------------------------------------------------------------
     * Per-sample loop: apply amplitude ramp, pan law, and sum into output.
     *
     * Amplitude ramp:
     *   Mirrors audio_generator.c per-sample loop (fix_amp_step_click, Site 5).
     *   current_loudness advances by loudness_step until ramp_remaining hits 0,
     *   then snaps to target_loudness to eliminate float-accumulation drift.
     *
     * Pan law (linear, matches apply_panning in audio_generator.c):
     *   pan in [-1.0, +1.0]; 0 = centre.
     *   pan_l = (pan <= 0) → 1.0;  (pan > 0)  → 1.0 - pan   (attenuate left)
     *   pan_r = (pan >= 0) → 1.0;  (pan < 0)  → 1.0 + pan   (attenuate right)
     * ----------------------------------------------------------------------- */
    const float pan      = s_bg.pan;
    const float pan_l    = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
    const float pan_r    = (pan >= 0.0f) ? 1.0f : (1.0f + pan);

    for (size_t i = 0; i < samples; i++) {

        /* Advance amplitude ramp if active.                                   */
        if (s_bg.loudness_ramp_remaining > 0u) {
            s_bg.current_loudness += s_bg.loudness_step;
            s_bg.loudness_ramp_remaining--;
            if (s_bg.loudness_ramp_remaining == 0u) {
                /* Snap to exact target — eliminates float-accumulation drift.  */
                s_bg.current_loudness = s_bg.target_loudness;
            }
        }

        const float gain = s_bg.current_loudness;

        float bg_l = s_mix_scratch[i * 2u]       * gain;
        float bg_r = s_mix_scratch[i * 2u + 1u]  * gain;

        /* Accumulate into caller's buffer (post-mix stage, not replacing).    */
        output_buffer[i * 2u]       += bg_l * pan_l;
        output_buffer[i * 2u + 1u]  += bg_r * pan_r;
    }
}
