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
#include "esp_timer.h"           /* esp_timer_get_time for hot-path profiling */
#include "esp_system.h"          /* esp_get_free_heap_size */
#include "esp_heap_caps.h"       /* MALLOC_CAP_SPIRAM */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <string.h>

#if CONFIG_BG_SUPPORT_MP3
#include "minimp3.h"             /* prototypes/types only; impl in mp3_decoder.c */
#endif

static const char *TAG = "bg_player";

/* ---------------------------------------------------------------------------
 * Compile-time constants
 * --------------------------------------------------------------------------- */

/** Ring buffer capacity in bytes (float stereo interleaved).
 *  32 KB ≈ 93 ms at 44 100 Hz / stereo / float.
 *  Must be a power-of-2 multiple for stream-buffer alignment.  */
#define BG_RING_BYTES         1048576u  /* 1 MB float stereo ≈ 3 s headroom. */

/** Trigger level: xStreamBufferCreate wakes a blocking receiver when at least
 *  this many bytes are available.  1024 = 128 stereo float samples.  We use
 *  non-blocking receive (timeout=0), so this only matters if a future caller
 *  uses a blocking receive.                                                   */
#define BG_RING_TRIGGER_BYTES   1024u

/** 5 ms amplitude ramp — mirrors AUDIO_AMP_RAMP_SAMPLES in audio_generator.c.
 *  220 samples at 44 100 Hz = 4.99 ms.  Eliminates click on BG start/stop.  */
#define BG_AMP_RAMP_SAMPLES     220u

/** Producer task stack depth in bytes.
 *  12 KB: enough for the WAV path (~4 KB) plus minimp3's non-scratch decode
 *  frames. minimp3's big ~16 KB mp3dec_scratch_t is NOT on this stack — it was
 *  patched to `static` in minimp3.h (moved to .bss) precisely so this task stack
 *  stays small enough to allocate from the ESP32's fragmented internal DRAM.
 *  (24 KB — needed if the scratch were on-stack — fails to allocate mid-session.) */
#define BG_STREAMER_STACK_BYTES 12288u

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
#define BG_HTTP_RECV_BUF_BYTES  16384u  /* match BG_HTTP_CHUNK_BYTES so each
                                         * esp_http_client_read of one chunk
                                         * needs only ONE underlying recv()
                                         * instead of looping 4× through a
                                         * smaller internal buffer. */

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
#define BG_HTTP_CHUNK_BYTES     16384u  /* 16 KB per HTTP read. */

/** Maximum consecutive URL re-open failures before the streamer task gives up.
 *  On each success the counter resets to 0, so transient failures during a
 *  long session (e.g. brief WiFi drop, server restart) are tolerated.            */
#define BG_HTTP_MAX_RETRIES      3

/* ---------------------------------------------------------------------------
 * Internal state (single static instance — one BG track per session)
 * --------------------------------------------------------------------------- */

/* Which producer is feeding the ring. PULL (default, .bss zero) = bg_streamer_task
 * fetching a URL; PUSH = the /api/bg-stream HTTP handler pushing decoded PCM.
 * The consumer/mixer is identical either way; this only affects start/stop
 * (PUSH never spawns/joins a producer task). */
typedef enum {
    BG_PRODUCER_PULL = 0,
    BG_PRODUCER_PUSH = 1,
} bg_producer_kind_t;

typedef struct {
    StreamBufferHandle_t ring;          /* 32 KB stream buffer (producer→consumer) */
    TaskHandle_t         producer_task; /* bg_streamer_task handle (NULL in PUSH)  */
    SemaphoreHandle_t    state_mutex;   /* protects start/stop/active transitions   */

    bg_producer_kind_t   producer_kind; /* PULL (url fetch) or PUSH (browser feed) */

    volatile bool active;               /* consumer gate: true → mix_into is live   */
    volatile bool streaming;            /* producer gate: true → keep producing      */
    volatile bool hold;                 /* prime gate: true → BUFFER but don't drain/
                                         * play (mix_into outputs silence, ring keeps
                                         * filling) until the timeline releases it so
                                         * BG sample 0 aligns with session t=0.      */

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

/* Ring buffer: 1 MB storage in PSRAM, control struct in DRAM .bss. */
static uint8_t *s_bg_ring_storage = NULL;
static StaticStreamBuffer_t s_bg_ring_ctrl;

#if CONFIG_BG_SUPPORT_PUSH
/** Push-mode conversion scratch: bg_player_push_pcm() processes input in
 *  batches of this many int16 frames so a single PSRAM buffer covers any caller
 *  chunk size. Worst-case float output = BATCH * 2 (upsample) * 2 ch = 4× floats. */
#define BG_PUSH_BATCH_FRAMES  2048u
static float *s_bg_push_scratch = NULL;   /* BG_PUSH_BATCH_FRAMES*4 floats, PSRAM */
#endif

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */

static void bg_streamer_task(void *pvParameters);

static esp_err_t bg_stream_from_http(const char *url);
static void      bg_stream_http_wav(esp_http_client_handle_t client,
                                    const wav_format_t *fmt,
                                    const uint8_t *leftover, size_t leftover_len);

/* Shared int16 -> 44.1 kHz stereo float conversion used by every BG decoder
 * (WAV today, MP3 below). Returns the number of stereo output frames written. */
static size_t    bg_convert_to_stereo_float(const int16_t *pcm, size_t frames,
                                            unsigned channels, bool upsample_2x,
                                            float *flt_buf);

#if CONFIG_BG_SUPPORT_MP3
static bool      bg_looks_like_mp3(const uint8_t *buf, size_t len);
static void      bg_stream_http_mp3(esp_http_client_handle_t client,
                                    const uint8_t *seed, size_t seed_len);
#endif

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

    } else if (strncmp(url, "push://", 7) == 0) {
        /* Browser-driven source: never fetched. bg_player_start() short-circuits
         * push:// before spawning the streamer task, so this branch should be
         * unreachable — return ESP_ERR_NOT_SUPPORTED so that, if it ever IS
         * reached, the outer retry loop bails out rather than spinning on a
         * spurious "EOF → re-open" (ESP_OK) forever.                            */
        ESP_LOGW(TAG, "BG dispatch: push:// reached streamer (unexpected) '%s'", url);
        return ESP_ERR_NOT_SUPPORTED;

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
 * bg_convert_to_stereo_float — the shared decode tail
 *
 * Converts `frames` interleaved int16 input samples (channels == 1 or 2) into
 * interleaved 44.1 kHz stereo float in flt_buf. Mono is duplicated (L = R).
 * When upsample_2x is set, each input frame is written twice (22050 -> 44100
 * sample-and-hold; the spectral mirror lands above 11 kHz, inaudible for
 * ambient BG). Returns the number of stereo output frames written; the caller
 * sends out_frames * 2 * sizeof(float) bytes to the ring. flt_buf must hold at
 * least frames * (upsample_2x ? 2 : 1) * 2 floats.
 *
 * This is the ONLY place raw PCM becomes ring-format audio, so every decoder
 * (WAV, MP3, future formats) funnels through it and inherits mono handling,
 * upsampling, and the /32768 normalization for free.
 * --------------------------------------------------------------------------- */
static size_t bg_convert_to_stereo_float(const int16_t *pcm, size_t frames,
                                         unsigned channels, bool upsample_2x,
                                         float *flt_buf)
{
    size_t out_idx = 0u;
    for (size_t i = 0; i < frames; i++) {
        const int16_t *p = pcm + i * channels;
        float L = (float)p[0] / 32768.0f;
        float R = (channels == 2u) ? (float)p[1] / 32768.0f : L;
        flt_buf[out_idx * 2u]      = L;
        flt_buf[out_idx * 2u + 1u] = R;
        out_idx++;
        if (upsample_2x) {
            flt_buf[out_idx * 2u]      = L;
            flt_buf[out_idx * 2u + 1u] = R;
            out_idx++;
        }
    }
    return out_idx;
}

/* ---------------------------------------------------------------------------
 * bg_stream_http_wav — convert and stream PCM from an open HTTP connection
 *
 * Called by bg_stream_from_http after the WAV header has been parsed.
 * Receives:
 *   client       — open esp_http_client handle positioned at PCM bytes.
 *   fmt          — parsed WAV format.
 *   leftover     — bytes already read past the WAV header (may be NULL).
 *   leftover_len — count of leftover; may be 0.
 *
 * Returns when esp_http_client_read returns 0 / negative, or when
 * s_bg.streaming goes false.
 * --------------------------------------------------------------------------- */
static void bg_stream_http_wav(esp_http_client_handle_t client,
                                const wav_format_t *fmt,
                                const uint8_t *leftover,
                                size_t leftover_len)
{
    /* raw_buf in PSRAM. It was INTERNAL DRAM (faster for the tight int16->float
     * loop), but internal DRAM is scarce on this build (WiFi/LWIP + async pool +
     * diagnostics) and this 16 KB alloc was failing outright after long uptime,
     * killing all BG. PSRAM is plenty fast here — the convert loop runs at ~24%
     * producer-busy with headroom to spare — and never fails. */
    uint8_t *raw_buf = heap_caps_malloc(BG_HTTP_CHUNK_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t frame_bytes  = (size_t)(fmt->channels) * (fmt->bits_per_sample / 8u);
    const size_t max_frames   = BG_HTTP_CHUNK_BYTES / frame_bytes;

    /* The output is always 44100 Hz stereo float. When the source is 22050
     * Hz, each input frame becomes TWO output frames (sample-and-hold
     * upsample), so the flt_buf must be sized for 2x max_frames to cover
     * the worst case (64 KB at chunk 16 KB + 22 kHz upsample).
     *
     * flt_buf is allocated in PSRAM: internal DRAM is already squeezed by
     * the WiFi/LWIP buffer config (WiFi RX 64, BA win 32, no SPIRAM
     * routing). PSRAM write speed (~10 cy/word) is plenty for ~60 KB/s
     * conversion throughput — only ~1 ms extra per chunk — but freeing
     * 64 KB of internal DRAM is essential to keep network buffers fast. */
    const bool   upsample_2x  = (fmt->sample_rate == 22050u);
    const size_t out_frames_max = upsample_2x ? (max_frames * 2u) : max_frames;
    float       *flt_buf      = heap_caps_malloc(out_frames_max * 2u * sizeof(float),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!flt_buf) {
        /* PSRAM exhausted (very rare); fall back to any available heap. */
        flt_buf = heap_caps_malloc(out_frames_max * 2u * sizeof(float),
                                   MALLOC_CAP_8BIT);
    }

    if (!raw_buf || !flt_buf) {
        ESP_LOGE(TAG, "BG HTTP: PCM buffer malloc failed (raw=%p flt=%p)",
                 raw_buf, flt_buf);
        free(raw_buf);
        free(flt_buf);
        return;
    }

    ESP_LOGI(TAG, "BG: PCM convert ready — input %u Hz %u ch, upsample=%s",
             (unsigned)fmt->sample_rate, (unsigned)fmt->channels,
             upsample_2x ? "yes (22→44 kHz sample-and-hold)" : "no");

    /* -----------------------------------------------------------------------
     * 1. Push leftover bytes from the header read (PCM data that was already
     *    fetched when we read the WAV header buffer).
     * ----------------------------------------------------------------------- */
    if (leftover != NULL && leftover_len > 0u) {
        /* Align to frame boundary so we don't split a sample.                 */
        size_t aligned_len = (leftover_len / frame_bytes) * frame_bytes;
        size_t lo_frames   = aligned_len / frame_bytes;

        size_t out_idx = bg_convert_to_stereo_float((const int16_t *)leftover,
                                                    lo_frames, fmt->channels,
                                                    upsample_2x, flt_buf);

        size_t bytes_to_send = out_idx * 2u * sizeof(float);
        if (bytes_to_send > 0u && s_bg.ring != NULL) {
            xStreamBufferSend(s_bg.ring, flt_buf, bytes_to_send, portMAX_DELAY);
            s_bg.bytes_streamed += (uint32_t)bytes_to_send;
        }
    }

    /* -----------------------------------------------------------------------
     * 2. Main streaming loop: read BG_HTTP_CHUNK_BYTES at a time from HTTP,
     *    convert int16→float, push to ring.
     * ----------------------------------------------------------------------- */
    /* Per-stage timing accumulators (microseconds). */
    uint64_t total_http_us = 0, total_conv_us = 0, total_send_us = 0;
    uint32_t chunk_count = 0;
    uint64_t last_report_us = esp_timer_get_time();

    while (s_bg.streaming) {
        if (s_bg.ring == NULL) {
            break;
        }

        /* ----- Producer pacing (high watermark) ----------------------------
         * Critical for sustained streaming: NEVER let a full ring buffer
         * block esp_http_client_read(). When we stop reading from the socket
         * for >100 ms, LWIP's receive buffer fills, advertises a zero window,
         * the server's RTO fires (~200-500 ms), and the server's cwnd resets
         * to 1 MSS. The connection then operates in a permanently throttled
         * state — exactly the "stable degraded after ~2 min" pattern we hit.
         *
         * Fix: stop calling recv() when the ring is over ~80% full and yield
         * briefly so the consumer can drain. This paces the producer to
         * consumer rate without ever blocking the socket-read path. LWIP's
         * receive buffer stays small, TCP window stays healthy, no zero-
         * window event ever happens.
         *
         * One chunk's float output at 22 kHz w/ 2x upsample =
         *   (BG_HTTP_CHUNK_BYTES / 4) * 2 * 2 * sizeof(float) = 65,536 bytes.
         * Earlier value (BG_HTTP_CHUNK_BYTES * 4u = 65,536) equalled exactly
         * one chunk's output, so the watermark re-fired after every send,
         * spinning vTaskDelay(10ms) ~17 times between chunks (~170ms). That
         * dropped effective throughput to ~86 KB/s (below the 88.2 KB/s
         * consumer demand at 22 kHz stereo) and intermittently stalled the
         * TCP socket long enough to leak server-side cwnd via WiFi retransmit
         * events — the documented "stable degraded after ~80 s" pattern.
         * Reserve 4 chunk-outputs of headroom so the producer can read
         * multiple back-to-back chunks before the watermark fires once. */
        const size_t WATERMARK_FREE_BYTES = BG_HTTP_CHUNK_BYTES * 16u;  /* 256 KB = 4 chunk-outputs */
        size_t free_space = xStreamBufferSpacesAvailable(s_bg.ring);
        if (free_space < WATERMARK_FREE_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint64_t t0 = esp_timer_get_time();
        int bytes_read = esp_http_client_read(client, (char *)raw_buf,
                                              (int)BG_HTTP_CHUNK_BYTES);
        uint64_t t1 = esp_timer_get_time();
        total_http_us += (t1 - t0);
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

        /* Convert raw bytes to stereo float frames; optionally upsample 2x
         * by sample-and-hold when source is 22 kHz.                         */
        uint64_t t2 = esp_timer_get_time();
        size_t frames = (size_t)bytes_read / frame_bytes;
        size_t out_idx = bg_convert_to_stereo_float((const int16_t *)raw_buf,
                                                    frames, fmt->channels,
                                                    upsample_2x, flt_buf);
        uint64_t t3 = esp_timer_get_time();
        total_conv_us += (t3 - t2);

        size_t bytes_to_send = out_idx * 2u * sizeof(float);
        if (bytes_to_send == 0u) {
            /* Partial frame at end of stream — discard and let read loop end. */
            continue;
        }

        /* Send to ring buffer. We pre-checked watermark above, so there's
         * room for at least 4 chunk-outputs — this send normally completes
         * in microseconds. Use a bounded 100ms timeout instead of
         * portMAX_DELAY: if the ring is unexpectedly full (e.g. consumer
         * is briefly stalled by a higher-priority task), we'd rather log a
         * short write and continue reading the socket than block the
         * producer indefinitely. Blocking here for &gt;100ms would let the
         * LWIP receive buffer fill up and re-introduce the zero-window
         * cascade the watermark above is designed to prevent. */
        uint64_t t4 = esp_timer_get_time();
        size_t sent = xStreamBufferSend(s_bg.ring, flt_buf, bytes_to_send,
                                        pdMS_TO_TICKS(100));
        uint64_t t5 = esp_timer_get_time();
        total_send_us += (t5 - t4);
        chunk_count++;
        if (sent < bytes_to_send) {
            ESP_LOGW(TAG,
                     "BG HTTP: ring send short write (wanted %zu, sent %zu)",
                     bytes_to_send, sent);
        }
        s_bg.bytes_streamed += (uint32_t)sent;

        /* Periodic diagnostic — every ~3 seconds, log ring health AND per-stage
         * timing breakdown so we can see whether the bottleneck is HTTP reads,
         * the conversion loop, or the ring send. */
        uint64_t now_us = esp_timer_get_time();
        static uint32_t s_last_underrun_count = 0u;
        if ((now_us - last_report_us) >= 3000000ull) {
            uint64_t window_us = now_us - last_report_us;
            uint32_t now_underruns = s_bg.underrun_count;
            uint32_t delta_underruns = now_underruns - s_last_underrun_count;
            size_t ring_filled = xStreamBufferBytesAvailable(s_bg.ring);
            uint64_t avg_http_us = chunk_count ? total_http_us / chunk_count : 0;
            uint64_t avg_conv_us = chunk_count ? total_conv_us / chunk_count : 0;
            uint64_t avg_send_us = chunk_count ? total_send_us / chunk_count : 0;
            uint64_t total_busy_us = total_http_us + total_conv_us + total_send_us;
            uint32_t busy_pct = (uint32_t)((total_busy_us * 100ull) / window_us);
            ESP_LOGI(TAG,
                     "BG diag: ring=%zu/%u B (%.0f%% full), underruns=%u (+%u in 3s), "
                     "bytes_streamed=%u",
                     ring_filled, BG_RING_BYTES,
                     100.0f * (float)ring_filled / (float)BG_RING_BYTES,
                     now_underruns, delta_underruns,
                     s_bg.bytes_streamed);
            ESP_LOGI(TAG,
                     "BG timing: %u chunks in %ums | avg per chunk: http=%uus conv=%uus send=%uus | producer busy=%u%%",
                     (unsigned)chunk_count,
                     (unsigned)(window_us / 1000),
                     (unsigned)avg_http_us,
                     (unsigned)avg_conv_us,
                     (unsigned)avg_send_us,
                     busy_pct);
            s_last_underrun_count = now_underruns;
            last_report_us = now_us;
            total_http_us = total_conv_us = total_send_us = 0;
            chunk_count = 0;
        }
    }

    free(raw_buf);
    free(flt_buf);
}

#if CONFIG_BG_SUPPORT_MP3
/* ---------------------------------------------------------------------------
 * bg_looks_like_mp3 — magic-byte container sniff
 *
 * Returns true for an 'ID3' tag (ID3v2 metadata precedes the audio) or an MPEG
 * audio frame sync (0xFF followed by a byte whose top three bits are all set).
 * This is deliberately loose — minimp3 does the real validation frame by frame;
 * we only need to route away from the WAV path. Called on the first bytes read.
 * --------------------------------------------------------------------------- */
static bool bg_looks_like_mp3(const uint8_t *buf, size_t len)
{
    if (len >= 3u && buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        return true;                         /* ID3v2 tag */
    }
    if (len >= 2u && buf[0] == 0xFFu && (buf[1] & 0xE0u) == 0xE0u) {
        return true;                         /* MPEG frame sync */
    }
    return false;
}

/* MP3 carry buffer capacity. Must hold one full HTTP read PLUS the un-consumed
 * tail of a frame that straddles two reads (max MP3 frame ~1441 B). Sizing it a
 * full chunk + 2 KB guarantees we can always append a fresh read to whatever
 * partial frame is left over, so no frame is ever lost at a chunk boundary. */
#define BG_MP3_CARRY_BYTES  (BG_HTTP_CHUNK_BYTES + 2048u)

/* ---------------------------------------------------------------------------
 * bg_stream_http_mp3 — decode and stream MP3 from an open HTTP connection
 *
 * Unlike WAV (fixed-size frames, byte-copy), MP3 frames are variable length and
 * routinely straddle the 16 KB HTTP reads. The loop keeps a carry buffer: each
 * pass appends a fresh read to the un-consumed tail, then decodes every complete
 * frame minimp3 can find, using info.frame_bytes (bytes consumed) to memmove the
 * remainder to the front. Decoded int16 PCM funnels through the SAME
 * bg_convert_to_stereo_float() tail as WAV, so the ring format is identical and
 * the consumer needs no knowledge of the source codec.
 *
 *   client   — open esp_http_client positioned just past the sniffed header.
 *   seed     — header bytes already read for detection (fed into the carry buf).
 *   seed_len — count of seed bytes (may be 0).
 * --------------------------------------------------------------------------- */
static void bg_stream_http_mp3(esp_http_client_handle_t client,
                               const uint8_t *seed, size_t seed_len)
{
    /* ALL decode buffers live in PSRAM. Internal DRAM is scarce (WiFi/LWIP) and
     * an 18 KB carry alloc there fails intermittently; MP3 decode is CPU-bound
     * (IMDCT), not memory-bandwidth-bound like the WAV per-sample loop, so PSRAM
     * is plenty fast here and keeps internal DRAM free for the task stack (which
     * must hold minimp3's ~16 KB stack-resident scratch — see stack sizing).    */
    mp3dec_t *dec     = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    uint8_t  *carry   = heap_caps_malloc(BG_MP3_CARRY_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t  *pcm     = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Worst case per frame: 1152 samples/ch, 2x upsample, stereo = 4608 floats. */
    const size_t flt_floats = 1152u * 2u /*upsample*/ * 2u /*stereo*/;
    float    *flt_buf = heap_caps_malloc(flt_floats * sizeof(float),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!flt_buf) {
        flt_buf = heap_caps_malloc(flt_floats * sizeof(float), MALLOC_CAP_8BIT);
    }
    if (!dec || !carry || !pcm || !flt_buf) {
        ESP_LOGE(TAG, "BG MP3: buffer malloc failed (dec=%p carry=%p pcm=%p flt=%p)",
                 dec, carry, pcm, flt_buf);
        free(dec); free(carry); free(pcm); free(flt_buf);
        return;
    }
    mp3dec_init(dec);

    /* Seed the carry buffer with the bytes already consumed for detection. */
    size_t carry_len = 0u;
    if (seed != NULL && seed_len > 0u) {
        if (seed_len > BG_MP3_CARRY_BYTES) {
            seed_len = BG_MP3_CARRY_BYTES;
        }
        memcpy(carry, seed, seed_len);
        carry_len = seed_len;
    }

    bool format_locked = false;
    bool upsample_2x   = false;
    bool eof           = false;

    ESP_LOGI(TAG, "BG MP3: decoder ready (carry=%u B)", (unsigned)BG_MP3_CARRY_BYTES);

    while (s_bg.streaming) {
        if (s_bg.ring == NULL) {
            break;
        }

        /* Producer pacing — identical rationale to the WAV loop: never let a
         * full ring block esp_http_client_read (avoids the TCP zero-window
         * cascade documented in bg_stream_http_wav).                          */
        const size_t WATERMARK_FREE_BYTES = BG_HTTP_CHUNK_BYTES * 16u;
        if (xStreamBufferSpacesAvailable(s_bg.ring) < WATERMARK_FREE_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 1. Refill: read into the free tail of the carry buffer. */
        if (!eof && carry_len < BG_MP3_CARRY_BYTES) {
            int want = (int)(BG_MP3_CARRY_BYTES - carry_len);
            int n = esp_http_client_read(client, (char *)(carry + carry_len), want);
            if (n > 0) {
                carry_len += (size_t)n;
            } else {
                if (n < 0) {
                    ESP_LOGW(TAG, "BG MP3: read error %d mid-stream", n);
                } else {
                    ESP_LOGI(TAG, "BG MP3: EOF reached");
                }
                eof = true;    /* stop reading; drain remaining frames, then exit */
            }
        }

        /* 2. Decode every complete frame currently buffered. */
        bool made_progress = false;
        while (carry_len > 0u) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(dec, carry, (int)carry_len, pcm, &info);

            if (info.frame_bytes == 0) {
                /* No complete frame at the head — need more input; go refill. */
                break;
            }
            /* Consume what this call used (a real frame, or skipped ID3/junk). */
            memmove(carry, carry + info.frame_bytes,
                    carry_len - (size_t)info.frame_bytes);
            carry_len -= (size_t)info.frame_bytes;
            made_progress = true;

            if (samples <= 0) {
                continue;      /* skipped metadata / non-audio frame */
            }

            /* Lock + validate format on the first decoded frame (Step 6). */
            if (!format_locked) {
                if (info.hz == 44100) {
                    upsample_2x = false;
                } else if (info.hz == 22050) {
                    upsample_2x = true;
                } else {
                    ESP_LOGE(TAG,
                             "BG MP3: unsupported sample rate %d Hz (need 44100 or "
                             "22050) — re-encode the source. Aborting BG stream.",
                             info.hz);
                    goto mp3_done;
                }
                if (info.channels != 1 && info.channels != 2) {
                    ESP_LOGE(TAG, "BG MP3: unsupported channel count %d — aborting.",
                             info.channels);
                    goto mp3_done;
                }
                ESP_LOGI(TAG, "BG MP3: %d Hz / %d ch / %d kbps — upsample=%s",
                         info.hz, info.channels, info.bitrate_kbps,
                         upsample_2x ? "yes (22->44 kHz S&H)" : "no");
                format_locked = true;
            }

            size_t out_frames = bg_convert_to_stereo_float(pcm, (size_t)samples,
                                                           (unsigned)info.channels,
                                                           upsample_2x, flt_buf);
            size_t bytes_to_send = out_frames * 2u * sizeof(float);
            if (bytes_to_send > 0u && s_bg.ring != NULL) {
                size_t sent = xStreamBufferSend(s_bg.ring, flt_buf, bytes_to_send,
                                                pdMS_TO_TICKS(100));
                if (sent < bytes_to_send) {
                    ESP_LOGW(TAG, "BG MP3: ring short write (%zu/%zu)",
                             sent, bytes_to_send);
                }
                s_bg.bytes_streamed += (uint32_t)sent;
            }
        }

        /* 3a. Corrupt-stream guard: a full carry buffer with no decodable frame
         *     and no EOF would otherwise spin forever (can't refill, can't
         *     decode). Valid MP3 frames are <1.5 KB so this never trips on good
         *     data; abort defensively on garbage.                             */
        if (!eof && !made_progress && carry_len >= BG_MP3_CARRY_BYTES) {
            ESP_LOGE(TAG, "BG MP3: no frame in a full carry buffer — corrupt "
                          "stream, aborting.");
            break;
        }
        /* 3b. Normal termination: EOF reached and the tail is drained (or only a
         *     truncated partial frame remains that can never complete).        */
        if (eof && (!made_progress || carry_len < 4u)) {
            break;
        }
    }

mp3_done:
    free(dec);
    free(carry);
    free(pcm);
    free(flt_buf);
}
#endif /* CONFIG_BG_SUPPORT_MP3 */

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

    esp_http_client_config_t http_cfg = {
        .url                       = url,
        .method                    = HTTP_METHOD_GET,
        .timeout_ms                = BG_HTTP_TIMEOUT_MS,
        .buffer_size               = BG_HTTP_RECV_BUF_BYTES,
        .buffer_size_tx            = BG_HTTP_TX_BUF_BYTES,
        .disable_auto_redirect     = false,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach           = NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "BG HTTP: esp_http_client_init failed for '%s'", url);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BG HTTP: esp_http_client_open failed: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int     http_status    = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "BG HTTP: '%s' → status=%d content_length=%lld",
             url, http_status, content_length);

    if (http_status != 200) {
        ESP_LOGE(TAG, "BG HTTP: unexpected HTTP status %d (expected 200)",
                 http_status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t hdr_buf[BG_HTTP_HDR_BUF_BYTES];
    int hdr_bytes = esp_http_client_read(client, (char *)hdr_buf,
                                         (int)sizeof(hdr_buf));
    if (hdr_bytes < 44) {
        ESP_LOGE(TAG, "BG HTTP: response too short for WAV header (%d)",
                 hdr_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Container detection by magic bytes (NOT file extension): the WAV path
     * needs 'RIFF'; MP3 streams start with an 'ID3' tag or an MPEG frame sync.
     * The header bytes are already in hand from the read above.               */
#if CONFIG_BG_SUPPORT_MP3
    if (bg_looks_like_mp3(hdr_buf, (size_t)hdr_bytes)) {
        ESP_LOGI(TAG, "BG HTTP: detected MP3 stream for '%s'", url);
        bg_stream_http_mp3(client, hdr_buf, (size_t)hdr_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ESP_LOGI(TAG,
                 "BG HTTP: stream ended for '%s' (bytes_streamed=%u, streaming=%d)",
                 url, s_bg.bytes_streamed, (int)s_bg.streaming);
        return ESP_OK;
    }
#endif

    wav_format_t fmt;
    size_t consumed_bytes = 0u;
    esp_err_t wav_err = wav_parse_header(hdr_buf, (size_t)hdr_bytes,
                                         &fmt, &consumed_bytes);
    if (wav_err != ESP_OK) {
        ESP_LOGE(TAG, "BG HTTP: WAV header parse failed (err=%s)",
                 esp_err_to_name(wav_err));
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

    const uint8_t *leftover     = hdr_buf + consumed_bytes;
    size_t         leftover_len = (size_t)hdr_bytes > consumed_bytes
                                  ? (size_t)hdr_bytes - consumed_bytes
                                  : 0u;

    bg_stream_http_wav(client, &fmt, leftover, leftover_len);

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
        ESP_LOGI(TAG, "bg_player_init: PSRAM free = %u bytes, DRAM free = %u bytes",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_bg_ring_storage = heap_caps_malloc(BG_RING_BYTES + 1,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_bg_ring_storage == NULL) {
            ESP_LOGE(TAG, "bg_player_init: PSRAM ring storage malloc failed");
            return ESP_ERR_NO_MEM;
        }
        s_bg.ring = xStreamBufferCreateStatic(BG_RING_BYTES, BG_RING_TRIGGER_BYTES,
                                              s_bg_ring_storage, &s_bg_ring_ctrl);
        if (s_bg.ring == NULL) {
            heap_caps_free(s_bg_ring_storage);
            s_bg_ring_storage = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "bg_player_init: ring buffer initialized (%u bytes, storage PSRAM, ctrl DRAM)",
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

    /* push:// is a browser-sourced clip: the device does NOT fetch it. Playback
     * is driven separately by the web UI via POST /api/bg-stream (which calls
     * bg_player_start_push). So a timeline pre-rolling a push:// BG is a no-op
     * here — never spawn a pull task for it. The play flow's own auto-stop
     * already tears down any prior BG, so we don't need to stop here.          */
    if (strncmp(bg->url, "push://", 7) == 0) {
#if CONFIG_BG_SUPPORT_PUSH
        ESP_LOGI(TAG, "bg_player_start: '%s' is browser-driven (push://) — "
                 "awaiting POST /api/bg-stream, no pull task spawned", bg->url);
        return ESP_OK;
#else
        ESP_LOGW(TAG, "bg_player_start: push:// BG requested but "
                 "CONFIG_BG_SUPPORT_PUSH=n — ignoring '%s'", bg->url);
        return ESP_ERR_NOT_SUPPORTED;
#endif
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
    __sync_synchronize();   /* release: step+target visible before remaining (see bg_player_stop) */
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

    /* This is a URL-pull session (the default producer). Explicit for clarity
     * and so a prior push session's kind can't leak in.                       */
    s_bg.producer_kind = BG_PRODUCER_PULL;

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
/* Shared implementation for bg_player_stop() and bg_player_stop_async() — the
 * two differ only in how long they wait for the producer task to gracefully
 * exit before force-deleting it (and potentially leaking its HTTP socket). */
static esp_err_t bg_player_stop_impl(uint32_t producer_join_timeout_ms)
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
        // Cross-core ordering: the consumer (audio_test_output_task on core 1)
        // reads loudness_ramp_remaining FIRST and gates step/target reads on
        // (remaining > 0).  Therefore we must publish step and target BEFORE
        // remaining, with a release barrier between, so the consumer cannot
        // see (remaining=220 + stale step=0) — which would run 220 no-op
        // iterations then snap to silence in one sample.
        // Reference: bug_stop_click_deep_investigation_2026-06-17.md (Inv 16 #3).
        s_bg.target_loudness         = 0.0f;
        s_bg.loudness_step           = -(s_bg.current_loudness / (float)BG_AMP_RAMP_SAMPLES);
        __sync_synchronize();   /* release: step+target visible before remaining */
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
     * Poll loudness_ramp_remaining instead of sleeping a fixed 30 ms — if the
     * audio output task is briefly starved (e.g., by a higher-priority task),
     * a fixed 30 ms can elapse without the ramp ever running, in which case
     * the next `active=false` cuts BG abruptly while current_loudness is still
     * non-zero — a click.  Polling at 2 ms intervals with a 100 ms ceiling
     * waits exactly as long as needed (typically one buffer ≈ 6 ms) and is a
     * safety net against the consumer being completely stalled.
     * Reference: bug_stop_click_bg_i2s_state_2026-06-17.md (Inv 17 #2).        */
    xSemaphoreGive(s_bg.state_mutex);
    for (int waited = 0; waited < 100; waited += 2) {
        if (s_bg.loudness_ramp_remaining == 0u) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);

    /* Now safe to stop the consumer.                                          */
    s_bg.active = false;

    /* Wait for producer task to exit (it sets producer_task = NULL on exit).
     * Caller-controlled timeout — 2000 ms for the blocking variant
     * (bg_player_stop), 200 ms for the async/fast variant
     * (bg_player_stop_async). Poll at 10 ms steps. */
    if (s_bg.producer_task != NULL) {
        const uint32_t poll_step_ms = 10u;
        uint32_t iters = (producer_join_timeout_ms + poll_step_ms - 1u) / poll_step_ms;
        if (iters == 0u) iters = 1u;
        for (uint32_t i = 0; i < iters && s_bg.producer_task != NULL; i++) {
            xSemaphoreGive(s_bg.state_mutex);
            vTaskDelay(pdMS_TO_TICKS(poll_step_ms));
            xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);
        }
        if (s_bg.producer_task != NULL) {
            /* Force-delete on timeout. This will leak the producer's HTTP
             * client socket; esp_http_client doesn't get its cleanup path,
             * so one socket descriptor stays open until reboot. Acceptable
             * for the async variant (rare, only on slow networks). */
            ESP_LOGW(TAG, "bg_player_stop: producer task did not exit in %u ms — force deleting",
                     (unsigned)producer_join_timeout_ms);
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

#if CONFIG_BG_SUPPORT_PUSH
    /* Free the push-mode scratch (allocated per session by bg_player_start_push)
     * and revert to the default PULL producer kind so a subsequent URL play
     * behaves exactly as before push was ever used. Harmless for a pull stop
     * (scratch is NULL, kind already PULL). */
    if (s_bg_push_scratch != NULL) {
        heap_caps_free(s_bg_push_scratch);
        s_bg_push_scratch = NULL;
    }
#endif
    s_bg.producer_kind = BG_PRODUCER_PULL;

    ESP_LOGI(TAG,
             "bg_player_stop: done (underruns=%u, bytes_streamed=%u)",
             s_bg.underrun_count, s_bg.bytes_streamed);

    xSemaphoreGive(s_bg.state_mutex);
    return ESP_OK;
}

esp_err_t bg_player_stop(void)
{
    /* Blocking variant — give the producer task up to 2 s to exit gracefully
     * so its HTTP client gets a clean teardown (no socket leak). Use this
     * for explicit user-driven shutdown via the /api/stop endpoint. */
    return bg_player_stop_impl(2000u);
}

esp_err_t bg_player_stop_async(void)
{
    /* Fast variant — cap the producer-join wait at 200 ms so the caller
     * (typically play_config_handler doing an implicit auto-stop before
     * starting a new config) returns quickly enough to feel responsive in
     * the web UI. Force-deletes the producer on timeout, which leaks one
     * HTTP socket descriptor; rare in practice and acceptable for the
     * snappy "PLAY replaces PLAY" UX. */
    return bg_player_stop_impl(200u);
}

#if CONFIG_BG_SUPPORT_PUSH
/* ---------------------------------------------------------------------------
 * Browser-push producer (bg_browser_push_plan.md, Phase 1)
 * --------------------------------------------------------------------------- */

uint32_t bg_player_push_buffered_ms(void)
{
    /* Only count once push mode is genuinely active — before the browser's
     * /api/bg-stream arrives (which resets the ring in bg_player_start_push),
     * any leftover ring bytes are stale and must NOT prematurely trip the gate. */
    if (s_bg.ring == NULL || !s_bg.active || s_bg.producer_kind != BG_PRODUCER_PUSH) {
        return 0u;
    }
    /* Ring holds 44.1 kHz stereo float: 2 floats * 4 bytes = 8 bytes per frame,
     * 44100 frames per second. ms = frames * 1000 / 44100. */
    size_t bytes  = xStreamBufferBytesAvailable(s_bg.ring);
    size_t frames = bytes / (2u * sizeof(float));
    return (uint32_t)(((uint64_t)frames * 1000ULL) / 44100ULL);
}

uint32_t bg_player_push_bytes_streamed(void)
{
    /* Ring-format (float-stereo) bytes handed to the ring so far this push
     * session. The WS back-channel reports this so the browser can detect a
     * stall (no progress) and estimate the consumed offset. */
    return s_bg.bytes_streamed;
}

/* Prime gate: buffer the pushed BG but DON'T play it yet (mix_into stays silent
 * and leaves the ring untouched). Set from config_parser when a push:// session
 * is armed, cleared when the timeline actually starts (or on stop).            */
void bg_player_push_hold(void)    { s_bg.hold = true;  ESP_LOGD(TAG, "BGDBG prime: hold ON (buffering, mix silent)"); }
void bg_player_push_release(void) { s_bg.hold = false; ESP_LOGD(TAG, "BGDBG prime: hold RELEASED (start draining, buffered=%ums)", (unsigned)bg_player_push_buffered_ms()); }

esp_err_t bg_player_start_push(float pan, float loudness)
{
    if (s_bg.state_mutex == NULL) {
        ESP_LOGE(TAG, "bg_player_start_push: not initialised — call bg_player_init first");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);

    /* Enforce the single-producer invariant: tear down any active BG (pull OR
     * push) before arming a new push session. bg_player_stop() joins/kills a
     * pull producer task and resets the ring, guaranteeing no other producer
     * touches s_bg.ring once we return.                                       */
    if (s_bg.active) {
        xSemaphoreGive(s_bg.state_mutex);
        bg_player_stop();
        xSemaphoreTake(s_bg.state_mutex, portMAX_DELAY);
    }

    if (s_bg.ring == NULL) {
        ESP_LOGE(TAG, "bg_player_start_push: ring not allocated — bg_player_init failed at boot?");
        xSemaphoreGive(s_bg.state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate the per-session conversion scratch (PSRAM; freed in stop). */
    if (s_bg_push_scratch == NULL) {
        s_bg_push_scratch = heap_caps_malloc(BG_PUSH_BATCH_FRAMES * 4u * sizeof(float),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_bg_push_scratch == NULL) {
            /* PSRAM exhausted (very rare) — fall back to any heap. */
            s_bg_push_scratch = heap_caps_malloc(BG_PUSH_BATCH_FRAMES * 4u * sizeof(float),
                                                 MALLOC_CAP_8BIT);
        }
        if (s_bg_push_scratch == NULL) {
            ESP_LOGE(TAG, "bg_player_start_push: scratch malloc failed");
            xSemaphoreGive(s_bg.state_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    /* Mark this a push session. Note: no URL fetch, no WiFi gate (works on
     * SoftAP), and no producer task — the HTTP handler thread is the producer. */
    s_bg.producer_kind = BG_PRODUCER_PUSH;
    s_bg.producer_task = NULL;
    snprintf(s_bg.url, sizeof(s_bg.url), "push://");
    s_bg.pan      = pan;
    s_bg.loudness = loudness;

    /* Arm the 220-sample fade-in ramp — identical to bg_player_start. */
    s_bg.current_loudness        = 0.0f;
    s_bg.target_loudness         = loudness;
    s_bg.loudness_step           = loudness / (float)BG_AMP_RAMP_SAMPLES;
    __sync_synchronize();   /* release: step+target visible before remaining */
    s_bg.loudness_ramp_remaining = BG_AMP_RAMP_SAMPLES;

    s_bg.underrun_count = 0u;
    s_bg.bytes_streamed = 0u;

    xStreamBufferReset(s_bg.ring);
    s_bg.streaming = true;
    s_bg.active    = true;

    xSemaphoreGive(s_bg.state_mutex);

    ESP_LOGI(TAG, "bg_player_start_push: OK — pan=%.2f loudness=%.2f (push producer)",
             pan, loudness);
    return ESP_OK;
}

size_t bg_player_push_pcm(const int16_t *pcm, size_t frames,
                          unsigned channels, bool upsample_2x)
{
    if (pcm == NULL || frames == 0u) return 0u;
    if (s_bg.producer_kind != BG_PRODUCER_PUSH) return 0u;

    ESP_LOGD(TAG, "BGDBG push_pcm: %zu frames ch=%u up2x=%d ring_free=%zuB",
             frames, channels, upsample_2x ? 1 : 0,
             s_bg.ring ? xStreamBufferSpacesAvailable(s_bg.ring) : 0u);

    /* Same watermark as the pull path: never issue a ring send that could
     * block for long. When the ring is near-full we yield and retry the SAME
     * batch; because the HTTP handler thread is parked here (not calling
     * recv()), TCP flow control throttles the browser upload to playback rate. */
    const size_t WATERMARK_FREE_BYTES = BG_HTTP_CHUNK_BYTES * 16u;  /* 256 KB */
    size_t off = 0u;   /* input frames consumed */

    while (off < frames) {
        if (!s_bg.streaming || s_bg.ring == NULL) {
            break;   /* push mode ended (stop / supersede) — tell caller via off<frames */
        }
        if (xStreamBufferSpacesAvailable(s_bg.ring) < WATERMARK_FREE_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t batch = frames - off;
        if (batch > BG_PUSH_BATCH_FRAMES) batch = BG_PUSH_BATCH_FRAMES;

        size_t out_frames = bg_convert_to_stereo_float(pcm + off * channels,
                                                       batch, channels,
                                                       upsample_2x, s_bg_push_scratch);
        size_t bytes_to_send = out_frames * 2u * sizeof(float);
        if (bytes_to_send > 0u) {
            /* Post-watermark the ring has >=256 KB free and one batch tops out
             * at 32 KB, so this completes in microseconds. The 100 ms bound is
             * a safety net (consumer momentarily starved), matching the WAV path. */
            size_t sent = xStreamBufferSend(s_bg.ring, s_bg_push_scratch,
                                            bytes_to_send, pdMS_TO_TICKS(100));
            s_bg.bytes_streamed += (uint32_t)sent;
            if (sent < bytes_to_send) {
                ESP_LOGW(TAG, "BG push: ring send short write (wanted %zu, sent %zu)",
                         bytes_to_send, sent);
            }
        }
        off += batch;
    }
    return off;
}

esp_err_t bg_player_end_push(void)
{
    /* Natural completion: the browser closed the request body. Let the audio
     * already buffered in the ring play out before the clean fade-out, so we
     * don't clip the tail. We poll from the caller's (HTTP handler) thread —
     * blocking here is fine and, for a client-driven loop, makes the fetch
     * promise resolve exactly when playback finishes.                          */
    if (s_bg.producer_kind != BG_PRODUCER_PUSH || !s_bg.active) {
        return ESP_OK;
    }

    /* Stop accepting/pushing more data, then wait for the ring to drain (or an
     * external stop to flip active=false). Cap the wait at ~5 s (ring holds
     * ≈3 s) so a stalled consumer can't hang the handler forever. */
    for (int waited_ms = 0; waited_ms < 5000; waited_ms += 20) {
        if (!s_bg.active) break;                       /* superseded / stopped */
        if (s_bg.ring == NULL) break;
        if (xStreamBufferBytesAvailable(s_bg.ring) < (2u * sizeof(float) * 64u)) {
            break;                                     /* ring effectively empty */
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Clean fade-out + teardown (also frees the push scratch and reverts to
     * PULL). Idempotent if an external stop already ran.                       */
    return bg_player_stop();
}
#endif // CONFIG_BG_SUPPORT_PUSH

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
    if (!s_bg.active || s_bg.ring == NULL || s_bg.hold) {
        /* hold: prime gate armed — leave the ring untouched (buffering) and add
         * no BG to the output, so playback begins exactly when the timeline
         * releases the hold (see bg_player_push_release).                       */
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
        /* Cross-core acquire: pair with the release barrier in bg_player_stop
         * (and the channel-start path) so that when we observe ramp_remaining > 0
         * we also see the latest loudness_step and target_loudness — not stale
         * values that would cause a 220-sample no-op then a hard cut.         */
        uint32_t remaining = s_bg.loudness_ramp_remaining;
        __sync_synchronize();
        if (remaining > 0u) {
            s_bg.current_loudness += s_bg.loudness_step;
            s_bg.loudness_ramp_remaining = remaining - 1u;
            if (remaining - 1u == 0u) {
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
