/*
 * Speech playback from the SD card. See speech_player.h for the design and
 * why this loads whole phrases instead of streaming them.
 */

#include "sdkconfig.h"

#if CONFIG_BG_SDCARD_ENABLED

#include "speech_player.h"
#include "sdcard.h"
#include "wav_parser.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "speech";

/* 3 MB ≈ 17 s of 44.1 kHz stereo int16. The longest phrase in the shipped
 * library is ~12 s; anything longer is truncated with a warning rather than
 * refused, so a session still runs. */
#define SPEECH_MAX_BYTES   (3u * 1024u * 1024u)
#define SPEECH_MAX_FRAMES  (SPEECH_MAX_BYTES / (2u * sizeof(int16_t)))

#define SPEECH_LOAD_CHUNK  4096u   /* multiple of the 512 B SD block */

typedef struct {
    int16_t *pcm;              /* interleaved stereo @ 44.1 kHz, PSRAM        */
    size_t   frames;           /* valid frames in pcm                          */
    _Atomic size_t play_pos;   /* consumer cursor, frames                      */
    _Atomic bool  active;      /* consumer gate                                */
    float    gain;             /* 0..1                                         */

    /* loader handoff */
    SemaphoreHandle_t req;     /* signalled when a new request is pending      */
    char     req_file[160];
    float    req_gain;
    _Atomic bool req_pending;
} speech_state_t;

static speech_state_t s_sp;
static bool s_inited = false;

/* Read buffer for loading a phrase. NOT a stack local (a 4 KB array on the
 * loader task's stack overflowed it and panicked the device), and NOT static
 * internal RAM either: internal DRAM is the scarce pool on this board — WiFi
 * and the web server leave only ~17 KB free, and bg_player needs a contiguous
 * 12 KB of it for its streamer task. PSRAM costs this path a slower sdmmc
 * transfer (512-byte blocks), which is fine because loading a phrase is a
 * one-off, unlike the real-time BG stream. */
static uint8_t *s_load_chunk = NULL;

/* ---------------------------------------------------------------- hash ---- */

/* FNV-1a 32. Chosen because it is four lines in both C and JavaScript with no
 * library, and has no endianness or wordsize subtleties to get wrong across
 * the two implementations. Identity only — not security. */
static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    for (; s && *s; s++) {
        h ^= (uint8_t)(*s);
        h *= 16777619u;
    }
    return h;
}

void speech_player_filename(const char *voice, const char *text,
                            char *out, size_t cap)
{
    /* Hash "voice|text" exactly — the browser builds the same string. An empty
     * or absent voice normalises to "default" on both sides. */
    char key[512];
    snprintf(key, sizeof(key), "%s|%s",
             (voice && *voice) ? voice : "default",
             text ? text : "");
    snprintf(out, cap, "speech/sp_%08lx.wav", (unsigned long)fnv1a32(key));
}

/* --------------------------------------------------------------- loader --- */

/* Read a WAV off the card into the PSRAM buffer, converting to the device's
 * native 44.1 kHz stereo int16. Mono is duplicated to both channels; 22.05 kHz
 * is doubled (the same two rates bg_player accepts). */
static bool speech_load_file(const char *rel_path)
{
    char path[224];
    snprintf(path, sizeof(path), "/sdcard/%s", rel_path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "phrase not on card: %s (skipping)", path);
        return false;
    }

    uint8_t hdr[512];            /* small enough for the loader task's stack */
    size_t hdr_n = fread(hdr, 1, sizeof(hdr), f);
    wav_format_t fmt;
    size_t consumed = 0;
    if (hdr_n < 44u || wav_parse_header(hdr, hdr_n, &fmt, &consumed) != ESP_OK) {
        ESP_LOGW(TAG, "%s is not a parseable WAV", path);
        fclose(f);
        return false;
    }
    if (fmt.bits_per_sample != 16 || fmt.channels < 1 || fmt.channels > 2) {
        ESP_LOGW(TAG, "%s: need 16-bit mono/stereo, got %u-bit/%uch",
                 path, (unsigned)fmt.bits_per_sample, (unsigned)fmt.channels);
        fclose(f);
        return false;
    }
    const bool upsample_2x = (fmt.sample_rate == 22050u);
    if (fmt.sample_rate != 44100u && !upsample_2x) {
        ESP_LOGW(TAG, "%s: unsupported rate %u (need 44100 or 22050)",
                 path, (unsigned)fmt.sample_rate);
        fclose(f);
        return false;
    }

    if (fseek(f, (long)fmt.data_offset, SEEK_SET) != 0) { fclose(f); return false; }

    const unsigned in_ch = fmt.channels;
    size_t out_frames = 0;
    bool truncated = false;

    uint8_t *raw = s_load_chunk;   /* PSRAM — see the declaration */

    for (;;) {
        size_t n = fread(raw, 1, SPEECH_LOAD_CHUNK, f);   /* NOT sizeof(raw): it is a pointer */
        if (n == 0) break;
        const size_t in_frames = n / (in_ch * sizeof(int16_t));
        const int16_t *src = (const int16_t *)raw;

        for (size_t i = 0; i < in_frames; i++) {
            int16_t l = src[i * in_ch];
            int16_t r = (in_ch == 2) ? src[i * in_ch + 1] : l;
            const unsigned reps = upsample_2x ? 2u : 1u;
            for (unsigned k = 0; k < reps; k++) {
                if (out_frames >= SPEECH_MAX_FRAMES) { truncated = true; break; }
                s_sp.pcm[out_frames * 2 + 0] = l;
                s_sp.pcm[out_frames * 2 + 1] = r;
                out_frames++;
            }
            if (truncated) break;
        }
        if (truncated) break;
    }
    fclose(f);

    if (truncated) {
        ESP_LOGW(TAG, "%s longer than %u s — truncated",
                 path, (unsigned)(SPEECH_MAX_FRAMES / 44100u));
    }
    if (out_frames == 0) {
        ESP_LOGW(TAG, "%s decoded to 0 frames", path);
        return false;
    }

    s_sp.frames = out_frames;
    atomic_store(&s_sp.play_pos, 0u);
    ESP_LOGI(TAG, "loaded %s — %u frames (%.1f s)", rel_path,
             (unsigned)out_frames, (double)out_frames / 44100.0);
    return true;
}

static void speech_loader_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_sp.req, portMAX_DELAY) != pdTRUE) continue;
        if (!atomic_load(&s_sp.req_pending)) continue;

        char file[160];
        strlcpy(file, s_sp.req_file, sizeof(file));
        float gain = s_sp.req_gain;
        atomic_store(&s_sp.req_pending, false);

        /* Silence the consumer while the buffer is rewritten. */
        atomic_store(&s_sp.active, false);

        if (speech_load_file(file)) {
            s_sp.gain = gain;
            atomic_store(&s_sp.active, true);
        }
    }
}

/* ----------------------------------------------------------- public API --- */

esp_err_t speech_player_init(void)
{
    if (s_inited) return ESP_OK;

    s_sp.pcm = heap_caps_malloc(SPEECH_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_sp.pcm) {
        ESP_LOGE(TAG, "cannot allocate %u B phrase buffer in PSRAM",
                 (unsigned)SPEECH_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_load_chunk = heap_caps_malloc(SPEECH_LOAD_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_load_chunk) {
        heap_caps_free(s_sp.pcm); s_sp.pcm = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_sp.req = xSemaphoreCreateBinary();
    if (!s_sp.req) {
        heap_caps_free(s_load_chunk); s_load_chunk = NULL;
        heap_caps_free(s_sp.pcm);     s_sp.pcm = NULL;
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_sp.active, false);
    atomic_store(&s_sp.req_pending, false);
    s_sp.frames = 0;

    /* Core 0 with the other I/O work: core 1 is reserved for the timing-
     * critical LED flicker and audio output tasks. */
    /* 4 KB. The read buffer lives in PSRAM and only a 512-byte header sits on
     * this stack, so the depth is the VFS/FATFS/sdmmc call chain. Kept tight
     * because every byte here comes out of the same internal-DRAM pool that
     * bg_player's streamer task needs a contiguous 12 KB of. */
    BaseType_t rc = xTaskCreatePinnedToCore(speech_loader_task, "speech_load",
                                            4096, NULL, 5, NULL, 0);
    if (rc != pdPASS) {
        vSemaphoreDelete(s_sp.req);   s_sp.req = NULL;
        heap_caps_free(s_load_chunk); s_load_chunk = NULL;
        heap_caps_free(s_sp.pcm);     s_sp.pcm = NULL;
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "speech_player ready (%u KB phrase buffer, max %.1f s)",
             (unsigned)(SPEECH_MAX_BYTES / 1024u),
             (double)SPEECH_MAX_FRAMES / 44100.0);
    return ESP_OK;
}

esp_err_t speech_player_play(const char *voice, const char *text, float volume)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "no SD card — speech skipped");
        return ESP_ERR_INVALID_STATE;
    }

    speech_player_filename(voice, text, s_sp.req_file, sizeof(s_sp.req_file));
    if (volume < 0.0f)   volume = 0.0f;
    if (volume > 100.0f) volume = 100.0f;
    s_sp.req_gain = volume / 100.0f;

    atomic_store(&s_sp.req_pending, true);
    xSemaphoreGive(s_sp.req);          /* loader picks it up; never blocks here */
    ESP_LOGI(TAG, "play '%.40s%s' -> %s", text, strlen(text) > 40 ? "..." : "",
             s_sp.req_file);
    return ESP_OK;
}

void speech_player_stop(void)
{
    if (!s_inited) return;
    atomic_store(&s_sp.active, false);
    atomic_store(&s_sp.req_pending, false);
}

bool speech_player_is_active(void)
{
    return s_inited && atomic_load(&s_sp.active);
}

void speech_player_mix_into(float *output_buffer, size_t samples)
{
    if (!s_inited || !output_buffer || samples == 0) return;
    if (!atomic_load(&s_sp.active)) return;

    size_t pos = atomic_load(&s_sp.play_pos);
    const size_t total = s_sp.frames;
    if (pos >= total) { atomic_store(&s_sp.active, false); return; }

    size_t n = samples;
    if (pos + n > total) n = total - pos;

    const float g = s_sp.gain * (1.0f / 32768.0f);
    const int16_t *src = s_sp.pcm + pos * 2;
    for (size_t i = 0; i < n; i++) {
        output_buffer[i * 2 + 0] += (float)src[i * 2 + 0] * g;
        output_buffer[i * 2 + 1] += (float)src[i * 2 + 1] * g;
    }

    pos += n;
    atomic_store(&s_sp.play_pos, pos);
    if (pos >= total) {
        atomic_store(&s_sp.active, false);
        ESP_LOGI(TAG, "phrase finished");
    }
}

#endif /* CONFIG_BG_SDCARD_ENABLED */
