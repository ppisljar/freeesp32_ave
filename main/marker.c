/**
 * @file marker.c
 * @brief Stimulus marker bus — see marker.h for the design and the reasoning
 *        behind the three timestamps.
 */

#include "marker.h"
#include "settings.h"
#include "audio_config.h"
#include "sdkconfig.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"   /* MALLOC_CAP_SPIRAM */
#include "driver/gpio.h"
#include <sys/stat.h>
#include <unistd.h>

#if CONFIG_BG_SDCARD_ENABLED
#include "sdcard.h"
#endif

#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "marker";

/* Firmware identity for the log header. The ELF hash is the only field that
 * actually changes per build: esp_app_desc's date/time come from __DATE__ in a
 * translation unit IDF does not force to recompile, so they can be days stale
 * on a freshly flashed image. A log that cannot be tied to an exact firmware is
 * most of the way to useless, so we record the hash. */
static const char *fw_version_str(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "unknown";
}

static const char *fw_elf_sha_str(void)
{
    static char hex[17];
    const esp_app_desc_t *d = esp_app_get_description();
    if (!d) return "";
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        hex[i * 2]     = hexd[(d->app_elf_sha256[i] >> 4) & 0xF];
        hex[i * 2 + 1] = hexd[d->app_elf_sha256[i] & 0xF];
    }
    hex[16] = '\0';
    return hex;
}

/* ------------------------------------------------------------------ state --- */

/* Ring. Single producer (the timeline dispatcher) and single consumer (the
 * flusher), so head/tail need no lock — only the mutex around the SINK list,
 * which a settings POST can rewrite underneath us.
 *
 * IN PSRAM, NOT INTERNAL DRAM, and this is not an optimisation. A
 * marker_record_t is 272 bytes, so 64 ring slots plus 24 recent slots is
 * ~23 KB of static .bss — and as statics they cost that whether or not anyone
 * ever configures a marker sink. Declared that way, this build reached
 * `E web_server: Failed to start HTTP server: ESP_ERR_HTTPD_TASK`: httpd could
 * no longer create its task, so the device booted, joined WiFi, got an IP, and
 * served nothing. The same failure mode the speech player caused when it
 * fragmented DRAM out from under bg_player's streamer task.
 *
 * Safe here because nothing touches these from an ISR: marker_emit() runs in
 * the timeline task and the flusher in its own, so the cache is always
 * available. The GPIO sink's state (s_pulses / s_fire_arg), which IS reached
 * from a timer callback, deliberately stays in internal DRAM below. */
#define MARKER_RECENT_SLOTS 24
static marker_record_t *s_ring;       /* [MARKER_RING_SLOTS]   */
static marker_record_t *s_recent;     /* [MARKER_RECENT_SLOTS] */
static uint32_t s_ring_slots;         /* actual, after any fallback */
static uint32_t s_recent_slots;
static volatile uint32_t s_head;      /* producer writes */
static volatile uint32_t s_tail;      /* consumer reads  */
static uint32_t s_recent_count;       /* total ever, index with % slots */

static marker_stats_t s_stats;
static uint32_t s_seq;

static int64_t s_origin_mono_us;      /* session transport origin, esp_timer domain */
static char    s_session_name[48];
static char    s_session_sha[65];

static SemaphoreHandle_t s_lock;      /* guards the sink list + the SD file */
static TaskHandle_t      s_flusher;
static volatile bool     s_run;

/* Resolved sink state, rebuilt from settings on init/reconfigure. */
typedef struct {
    marker_sink_cfg_t cfg;
    bool              armed;
    int               sock;           /* udp */
    struct sockaddr_in dest;          /* udp */
    FILE             *fp;             /* sdjsonl */
} sink_state_t;

static sink_state_t s_sinks[SETTINGS_MAX_MARKER_SINKS];

/* ------------------------------------------------------- pipeline latency --- */

/* LED strip serialisation. WS2812 is 1.25 us/bit (T0H+T0L == T1H+T1L, see
 * main/led_strip.c) and 24 bits per pixel, so each pixel costs 30.0 us and the
 * whole strip costs led_count x 30 us.
 *
 * A marker announces a zone, not a pixel, and a zone's pixels are scattered
 * across the chain by channel_map — so the honest figure for "when do photons
 * appear for this zone" is a RANGE, not a number. We use the MIDPOINT of the
 * strip write, which is the least-wrong single value, and say so. Task #24
 * covers measuring the per-zone offset properly; until then this term carries
 * up to +/- half a strip-write of error (+/-1.1 ms on 74 px).
 *
 * This is also exactly why marker_pipeline_lag_us() exists instead of a
 * compile-time constant: the LED term depends on the configured led_count. */
#define LED_US_PER_PIXEL 30

int64_t marker_pipeline_lag_us(marker_modality_t modality)
{
    const int64_t audio_us = (int64_t)AUDIO_DMA_PIPELINE_LAG_US;

    const device_settings_t *st = settings_get();
    int count = (st && st->led_count > 0) ? st->led_count : 1;
    /* LED entries are anchored at demanded + the same DMA lag (that symmetry is
     * what makes A/V sync hold), then the strip write happens on top. */
    const int64_t led_us = audio_us + (int64_t)count * LED_US_PER_PIXEL / 2;

    switch (modality) {
        case MARKER_MODALITY_AUDIO: return audio_us;
        case MARKER_MODALITY_LED:   return led_us;
        case MARKER_MODALITY_BOTH:  return led_us > audio_us ? led_us : audio_us;
        case MARKER_MODALITY_NONE:
        default:                    return 0;
    }
}

/* ------------------------------------------------------------- gpio sink ---- */

/* One-shot timers, one per gpio sink, so a pulse scheduled at the predicted
 * onset does not need a task to sit and wait for it. */
typedef struct {
    int  pin;
    int  active_low;
    esp_timer_handle_t off_timer;
} gpio_pulse_t;

static gpio_pulse_t s_pulses[SETTINGS_MAX_MARKER_SINKS];

static void gpio_pulse_off(void *arg)
{
    gpio_pulse_t *g = (gpio_pulse_t *)arg;
    gpio_set_level((gpio_num_t)g->pin, g->active_low ? 1 : 0);
}

typedef struct {
    int slot;
    int pulse_us;
} gpio_fire_arg_t;

static esp_timer_handle_t s_fire_timer[SETTINGS_MAX_MARKER_SINKS];
static gpio_fire_arg_t    s_fire_arg[SETTINGS_MAX_MARKER_SINKS];

static void gpio_pulse_on(void *arg)
{
    gpio_fire_arg_t *a = (gpio_fire_arg_t *)arg;
    gpio_pulse_t *g = &s_pulses[a->slot];
    if (g->pin < 0) return;
    gpio_set_level((gpio_num_t)g->pin, g->active_low ? 0 : 1);
    /* Re-arming a one-shot that may still be pending is an error, so stop first. */
    esp_timer_stop(g->off_timer);
    esp_timer_start_once(g->off_timer, a->pulse_us);
}

/* Fire (or schedule) the pulse for one gpio sink. Called from the DISPATCH
 * path, so it must not block: everything here is a register write or an
 * esp_timer arm. */
static void gpio_sink_mark(int slot, const marker_record_t *r)
{
    sink_state_t *s = &s_sinks[slot];
    if (!s->armed) return;

    if (s->cfg.gpio_timing == MARKER_GPIO_AT_DISPATCH) {
        s_fire_arg[slot].pulse_us = s->cfg.gpio_pulse_us;
        gpio_pulse_on(&s_fire_arg[slot]);
        return;
    }

    /* AT_ONSET: schedule for the predicted output instant. The delay is
     * measured from NOW, not from the record's dispatch stamp, because the
     * batch stamp can already be a few hundred us old by the time we get here
     * and we want the pulse at the predicted onset, not that minus the skew. */
    int64_t onset_mono = r->t_disp_mono_us +
                         (r->t_onset_pred_us - r->t_disp_us);
    int64_t delay = onset_mono - esp_timer_get_time();
    if (delay < 1000) {
        /* Already due (or so close that the timer's own latency dominates):
         * fire now rather than pretend to schedule. */
        s_fire_arg[slot].pulse_us = s->cfg.gpio_pulse_us;
        gpio_pulse_on(&s_fire_arg[slot]);
        return;
    }
    s_fire_arg[slot].pulse_us = s->cfg.gpio_pulse_us;
    esp_timer_stop(s_fire_timer[slot]);
    esp_timer_start_once(s_fire_timer[slot], (uint64_t)delay);
}

/* ------------------------------------------------------------- serialise ---- */

/* One JSON object per marker, shared by the UDP and SD sinks so the two
 * recordings are byte-comparable. `t_onset_pred_us` is named "pred" on purpose:
 * a consumer must not be able to mistake it for a measured onset. */
static int marker_to_json(const marker_record_t *r, char *out, size_t cap)
{
    int n = snprintf(out, cap,
        "{\"rec\":\"marker\",\"seq\":%" PRIu32 ",\"label\":\"%s\""
        ",\"t_sched_us\":%" PRId64
        ",\"t_disp_us\":%" PRId64
        ",\"t_onset_pred_us\":%" PRId64
        ",\"t_disp_mono_us\":%" PRId64
        ",\"modality\":\"%s\"",
        r->seq, r->label, r->t_sched_us, r->t_disp_us, r->t_onset_pred_us,
        r->t_disp_mono_us,
        r->modality == MARKER_MODALITY_AUDIO ? "audio" :
        r->modality == MARKER_MODALITY_LED   ? "led"   :
        r->modality == MARKER_MODALITY_BOTH  ? "both"  : "none");
    if (n < 0 || (size_t)n >= cap) return -1;

    if (r->kv_count) {
        int m = snprintf(out + n, cap - n, ",\"kv\":{");
        if (m < 0 || (size_t)(n + m) >= cap) return -1;
        n += m;
        for (uint8_t i = 0; i < r->kv_count; i++) {
            m = snprintf(out + n, cap - n, "%s\"%s\":\"%s\"",
                         i ? "," : "", r->kv[i].key, r->kv[i].val);
            if (m < 0 || (size_t)(n + m) >= cap) return -1;
            n += m;
        }
        m = snprintf(out + n, cap - n, "}");
        if (m < 0 || (size_t)(n + m) >= cap) return -1;
        n += m;
    }
    int m = snprintf(out + n, cap - n, "}\n");
    if (m < 0 || (size_t)(n + m) >= cap) return -1;
    return n + m;
}

/* ------------------------------------------------------------- udp sink ----- */

static void udp_sink_write(sink_state_t *s, const char *json, int len)
{
    if (!s->armed || s->sock < 0) return;
    int sent = sendto(s->sock, json, len, 0,
                      (struct sockaddr *)&s->dest, sizeof(s->dest));
    if (sent != len) {
        s_stats.sink_errors++;
    }
}

/* ------------------------------------------------------------- sd sink ------ */

static void sd_sink_write(sink_state_t *s, const char *json, int len)
{
    if (!s->armed || !s->fp) return;
    if (fwrite(json, 1, (size_t)len, s->fp) != (size_t)len) {
        s_stats.sink_errors++;
        return;
    }
    /* Flush AND fsync every record. A session is long and a crash mid-run is
     * exactly when the log matters most; the write rate is a handful of records
     * per minute, so there is nothing to gain by buffering.
     *
     * fflush alone is NOT enough on FATFS: it pushes the libc buffer down, but
     * the directory entry — and therefore the file's SIZE — is only committed on
     * close or fsync. A log written with fflush only reads back as a 0-byte file
     * until the session ends, which is precisely the case the per-record flush
     * exists to cover. Measured: session-001.jsonl was 200-but-empty over
     * /api/sd for the whole run. */
    fflush(s->fp);
    fsync(fileno(s->fp));
}

/* ------------------------------------------------------------- flusher ------ */

static void marker_flusher(void *arg)
{
    (void)arg;
    char json[512];
    while (s_run) {
        bool did = false;
        while (s_tail != s_head) {
            marker_record_t r = s_ring[s_tail % s_ring_slots];
            s_tail++;
            int len = marker_to_json(&r, json, sizeof(json));
            if (len < 0) { s_stats.sink_errors++; continue; }

            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
                for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
                    switch (s_sinks[i].cfg.type) {
                        case MARKER_SINK_UDP:     udp_sink_write(&s_sinks[i], json, len); break;
                        case MARKER_SINK_SDJSONL: sd_sink_write(&s_sinks[i], json, len);  break;
                        default: break;   /* gpio already fired at dispatch */
                    }
                }
                xSemaphoreGive(s_lock);
            } else {
                s_stats.sink_errors++;
            }
            did = true;
        }
        /* 20 ms idle poll: markers are sparse, and the GPIO sink (the only one
         * with timing requirements) never comes through here. */
        vTaskDelay(pdMS_TO_TICKS(did ? 1 : 20));
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- arm/disarm --- */

static void sink_disarm(sink_state_t *s)
{
    if (s->sock >= 0) { close(s->sock); s->sock = -1; }
    if (s->fp)        { fclose(s->fp);  s->fp = NULL; }
    s->armed = false;
}

static void sink_arm(int slot, sink_state_t *s)
{
    s->armed = false;
    switch (s->cfg.type) {
    case MARKER_SINK_GPIO: {
        if (s->cfg.gpio_pin < 0) return;
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s->cfg.gpio_pin,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK) {
            ESP_LOGE(TAG, "sink %d: gpio_config(%d) failed", slot, s->cfg.gpio_pin);
            return;
        }
        gpio_set_level((gpio_num_t)s->cfg.gpio_pin, s->cfg.gpio_active_low ? 1 : 0);

        s_pulses[slot].pin        = s->cfg.gpio_pin;
        s_pulses[slot].active_low = s->cfg.gpio_active_low;
        if (!s_pulses[slot].off_timer) {
            const esp_timer_create_args_t off = {
                .callback = gpio_pulse_off, .arg = &s_pulses[slot],
                .dispatch_method = ESP_TIMER_TASK, .name = "mk_off",
            };
            if (esp_timer_create(&off, &s_pulses[slot].off_timer) != ESP_OK) return;
        }
        s_fire_arg[slot].slot = slot;
        if (!s_fire_timer[slot]) {
            const esp_timer_create_args_t on = {
                .callback = gpio_pulse_on, .arg = &s_fire_arg[slot],
                .dispatch_method = ESP_TIMER_TASK, .name = "mk_on",
            };
            if (esp_timer_create(&on, &s_fire_timer[slot]) != ESP_OK) return;
        }
        ESP_LOGI(TAG, "sink %d: GPIO %d, %d us pulse, %s, fires at %s",
                 slot, s->cfg.gpio_pin, s->cfg.gpio_pulse_us,
                 s->cfg.gpio_active_low ? "active-low" : "active-high",
                 s->cfg.gpio_timing == MARKER_GPIO_AT_ONSET
                     ? "PREDICTED onset" : "dispatch");
        s->armed = true;
        break;
    }
    case MARKER_SINK_UDP: {
        if (s->cfg.udp_host[0] == '\0' || s->cfg.udp_port == 0) return;
        /* Refuse to touch lwip before it exists. socket() without a tcpip task
         * does not fail, it ASSERTS:
         *   assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)
         * and since arming happens during init, the reboot re-arms and asserts
         * again — a boot loop, which is the one failure that also blocks OTA
         * recovery. marker_init() is called after the network stack for exactly
         * this reason; this guard means a future reordering degrades to "sink
         * not armed" instead of bricking the device until someone finds a
         * cable. The STA netif exists from wifi_manager_init() onward,
         * regardless of whether it ever gets an IP. */
        if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") &&
            !esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
            ESP_LOGW(TAG, "sink %d: UDP not armed — network stack is not up yet",
                     slot);
            return;
        }
        s->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s->sock < 0) {
            ESP_LOGE(TAG, "sink %d: socket() failed", slot);
            return;
        }
        memset(&s->dest, 0, sizeof(s->dest));
        s->dest.sin_family = AF_INET;
        s->dest.sin_port   = htons((uint16_t)s->cfg.udp_port);
        if (inet_pton(AF_INET, s->cfg.udp_host, &s->dest.sin_addr) != 1) {
            /* Not a dotted quad — resolve it. Done here, at arm time, so the
             * dispatch path never does a DNS lookup. */
            struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
            struct addrinfo *res = NULL;
            if (getaddrinfo(s->cfg.udp_host, NULL, &hints, &res) != 0 || !res) {
                ESP_LOGE(TAG, "sink %d: cannot resolve '%s'", slot, s->cfg.udp_host);
                close(s->sock); s->sock = -1;
                return;
            }
            s->dest.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
        ESP_LOGI(TAG, "sink %d: UDP -> %s:%d", slot, s->cfg.udp_host, s->cfg.udp_port);
        s->armed = true;
        break;
    }
    case MARKER_SINK_SDJSONL:
        /* The file is opened by marker_session_begin (it is named after the
         * session), so arming here only records that the slot is live. */
#if CONFIG_BG_SDCARD_ENABLED
        if (!sdcard_is_mounted()) {
            ESP_LOGW(TAG, "sink %d: SD log requested but no card mounted", slot);
            return;
        }
        ESP_LOGI(TAG, "sink %d: SD JSONL in /%s", slot, s->cfg.sd_dir);
        s->armed = true;
#else
        ESP_LOGW(TAG, "sink %d: SD log requested but SD support is compiled out", slot);
#endif
        break;
    default:
        break;
    }
}

/* Does any sink actually need the flusher? GPIO fires inline from the dispatch
 * path, so a GPIO-only configuration needs neither the task nor the ring. */
static bool needs_flusher(void)
{
    for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
        if (s_sinks[i].cfg.type == MARKER_SINK_UDP ||
            s_sinks[i].cfg.type == MARKER_SINK_SDJSONL) return true;
    }
    return false;
}

/* Start the flusher on demand, NOT at init.
 *
 * Every sink ships disabled, so on a device nobody has configured for markers
 * this keeps the feature's cost at zero. That is not tidiness: a FreeRTOS task
 * stack comes out of internal DRAM, and this build has so little spare that the
 * first version of this module — which created the task unconditionally and
 * held its buffers in .bss — left lwip unable to allocate for a large response.
 * /api/version answered fine while /api/state and /api/logs returned HTTP 200
 * with an EMPTY BODY, because the header went out and the body send failed. */
static esp_err_t ensure_flusher(void)
{
    if (s_flusher) return ESP_OK;
    s_run = true;
    /* Priority 3: above idle, well below the audio/LED real-time work. Markers
     * are sparse and latency-insensitive on this path — the GPIO sink, which
     * is not latency-insensitive, never uses it. */
    if (xTaskCreate(marker_flusher, "mk_flush", 3072, NULL, 3, &s_flusher) != pdPASS) {
        ESP_LOGE(TAG, "flusher task create failed — record sinks will not write");
        s_run = false;
        s_flusher = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t marker_reconfigure(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    const device_settings_t *st = settings_get();
    if (!st) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
        sink_disarm(&s_sinks[i]);
        s_sinks[i].cfg = st->marker_sinks[i];
        sink_arm(i, &s_sinks[i]);
    }
    bool want = needs_flusher();
    xSemaphoreGive(s_lock);

    if (want) ensure_flusher();
    /* The task is deliberately NOT torn down when the last record sink goes
     * away: stopping it safely mid-drain is more risk than the 3 KB is worth,
     * and a device that has had a sink configured once will have one again. */
    return ESP_OK;
}

esp_err_t marker_init(void)
{
    if (s_lock) return ESP_OK;             /* idempotent */

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* PSRAM first (see the declaration comment: these buffers in .bss cost
     * httpd its task). Fall back to internal DRAM with a much smaller ring on a
     * board without PSRAM, rather than refusing to run. */
    s_ring_slots   = MARKER_RING_SLOTS;
    s_recent_slots = MARKER_RECENT_SLOTS;
    s_ring   = heap_caps_calloc(s_ring_slots, sizeof(marker_record_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_recent = heap_caps_calloc(s_recent_slots, sizeof(marker_record_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring || !s_recent) {
        free(s_ring); free(s_recent);
        s_ring_slots   = 8;
        s_recent_slots = 6;
        s_ring   = calloc(s_ring_slots, sizeof(marker_record_t));
        s_recent = calloc(s_recent_slots, sizeof(marker_record_t));
        if (!s_ring || !s_recent) {
            free(s_ring); free(s_recent);
            s_ring = s_recent = NULL;
            ESP_LOGE(TAG, "no memory for the marker ring");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "marker ring in internal DRAM (%u slots) — no PSRAM; "
                 "a dense marker burst is likelier to overflow",
                 (unsigned)s_ring_slots);
    }

    for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
        s_sinks[i].sock = -1;
        s_pulses[i].pin = -1;
    }
    memset(&s_stats, 0, sizeof(s_stats));

    marker_reconfigure();      /* also starts the flusher iff one is needed */

    ESP_LOGI(TAG, "marker bus up (audio lag %lld us, led lag %lld us)",
             (long long)marker_pipeline_lag_us(MARKER_MODALITY_AUDIO),
             (long long)marker_pipeline_lag_us(MARKER_MODALITY_LED));
    return ESP_OK;
}

/* --------------------------------------------------------------- session ---- */

esp_err_t marker_session_begin(const char *session_name,
                               const char *session_sha256,
                               int64_t origin_mono_us)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    s_origin_mono_us = origin_mono_us ? origin_mono_us : esp_timer_get_time();
    strlcpy(s_session_name, session_name ? session_name : "session",
            sizeof(s_session_name));
    strlcpy(s_session_sha, session_sha256 ? session_sha256 : "",
            sizeof(s_session_sha));

    s_stats.emitted = 0;
    s_stats.dropped = 0;
    s_stats.sink_errors = 0;
    s_stats.max_sched_disp_delta_us = 0;
    s_stats.session_open = true;
    s_seq = 0;
    s_head = s_tail = 0;
    s_recent_count = 0;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

#if CONFIG_BG_SDCARD_ENABLED
    for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
        sink_state_t *s = &s_sinks[i];
        if (s->cfg.type != MARKER_SINK_SDJSONL || !s->armed) continue;
        if (s->fp) { fclose(s->fp); s->fp = NULL; }

        char dir[80];
        snprintf(dir, sizeof(dir), "/sdcard/%s", s->cfg.sd_dir);
        mkdir(dir, 0775);     /* ignore EEXIST */

        /* Never overwrite a previous run: find the first free index. A lost
         * recording is not recoverable, so this errs toward clutter. */
        char path[160];
        for (int n = 1; n < 1000; n++) {
            snprintf(path, sizeof(path), "%s/%s-%03d.jsonl", dir, s_session_name, n);
            FILE *probe = fopen(path, "r");
            if (!probe) break;
            fclose(probe);
        }
        s->fp = fopen(path, "w");
        if (!s->fp) {
            ESP_LOGE(TAG, "sink %d: cannot open %s", i, path);
            s_stats.sink_errors++;
            continue;
        }
        ESP_LOGI(TAG, "sink %d: logging to %s", i, path);
    }
#endif

    /* Provenance header, to every record sink. Without this a log is just a
     * list of times with no way to tie it back to what produced it. */
    char hdr[512];
    const device_settings_t *st = settings_get();
    int len = snprintf(hdr, sizeof(hdr),
        "{\"rec\":\"header\",\"session\":\"%s\",\"session_sha256\":\"%s\""
        ",\"fw_version\":\"%s\",\"fw_elf_sha256\":\"%s\""
        ",\"origin_mono_us\":%" PRId64
        ",\"pipeline\":{\"audio_lag_us\":%" PRId64 ",\"led_lag_us\":%" PRId64
        ",\"led_count\":%d,\"led_us_per_pixel\":%d"
        ",\"note\":\"onset times are PREDICTED from these lags, not measured\"}}\n",
        s_session_name, s_session_sha,
        fw_version_str(), fw_elf_sha_str(),
        s_origin_mono_us,
        marker_pipeline_lag_us(MARKER_MODALITY_AUDIO),
        marker_pipeline_lag_us(MARKER_MODALITY_LED),
        st ? st->led_count : 0, LED_US_PER_PIXEL);

    if (len > 0 && (size_t)len < sizeof(hdr)) {
        for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
            switch (s_sinks[i].cfg.type) {
                case MARKER_SINK_UDP:     udp_sink_write(&s_sinks[i], hdr, len); break;
                case MARKER_SINK_SDJSONL: sd_sink_write(&s_sinks[i], hdr, len);  break;
                default: break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t marker_session_end(void)
{
    if (!s_lock || !s_stats.session_open) return ESP_ERR_INVALID_STATE;

    /* Drain before the footer so its counters describe everything that was
     * actually written, and so the last markers are not lost to the close. */
    for (int spin = 0; spin < 100 && s_tail != s_head; spin++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    char ftr[256];
    int len = snprintf(ftr, sizeof(ftr),
        "{\"rec\":\"footer\",\"markers\":%" PRIu32 ",\"dropped\":%" PRIu32
        ",\"sink_errors\":%" PRIu32
        ",\"max_sched_disp_delta_us\":%" PRId64 "}\n",
        s_stats.emitted, s_stats.dropped, s_stats.sink_errors,
        s_stats.max_sched_disp_delta_us);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (len > 0 && (size_t)len < sizeof(ftr)) {
        for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
            switch (s_sinks[i].cfg.type) {
                case MARKER_SINK_UDP:     udp_sink_write(&s_sinks[i], ftr, len); break;
                case MARKER_SINK_SDJSONL: sd_sink_write(&s_sinks[i], ftr, len);  break;
                default: break;
            }
        }
    }
    for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
        if (s_sinks[i].fp) { fclose(s_sinks[i].fp); s_sinks[i].fp = NULL; }
    }
    xSemaphoreGive(s_lock);

    if (s_stats.dropped) {
        ESP_LOGE(TAG, "session end: %" PRIu32 " marker(s) DROPPED (ring overflow) "
                 "— the log has gaps", s_stats.dropped);
    }
    s_stats.session_open = false;
    return ESP_OK;
}

/* ------------------------------------------------------------------ emit ---- */

esp_err_t marker_emit(const char *label,
                      const marker_kv_t *kv, uint8_t kv_count,
                      marker_modality_t modality,
                      int64_t t_sched_us,
                      int64_t t_disp_mono_us)
{
    if (!s_lock || !label) return ESP_ERR_INVALID_STATE;

    if (!s_ring || !s_recent) return ESP_ERR_INVALID_STATE;

    const int64_t mono = t_disp_mono_us ? t_disp_mono_us : esp_timer_get_time();

    /* Ring full? Count it and return. Dropping the NEWEST keeps the existing
     * records intact and the gap at a known place; either way the footer and
     * /api/state report it, because a marker that silently never existed is the
     * one failure that quietly invalidates an experiment. */
    if ((uint32_t)(s_head - s_tail) >= s_ring_slots) {
        s_stats.dropped++;
        ESP_LOGE(TAG, "marker ring full — DROPPED '%s'", label);
        return ESP_ERR_NO_MEM;
    }

    marker_record_t *r = &s_ring[s_head % s_ring_slots];
    memset(r, 0, sizeof(*r));
    strlcpy(r->label, label, sizeof(r->label));
    r->kv_count = (kv && kv_count) ? (kv_count > MARKER_KV_MAX ? MARKER_KV_MAX : kv_count) : 0;
    for (uint8_t i = 0; i < r->kv_count; i++) r->kv[i] = kv[i];
    r->modality       = modality;
    r->t_sched_us     = t_sched_us;
    r->t_disp_us      = mono - s_origin_mono_us;
    r->t_onset_pred_us = r->t_disp_us + marker_pipeline_lag_us(modality);
    r->t_disp_mono_us = mono;
    r->seq            = s_seq++;

    int64_t delta = r->t_disp_us - r->t_sched_us;
    if (delta < 0) delta = -delta;
    if (delta > s_stats.max_sched_disp_delta_us) {
        s_stats.max_sched_disp_delta_us = delta;
    }

    s_recent[s_recent_count % s_recent_slots] = *r;
    s_recent_count++;

    /* GPIO fires from HERE, not from the flusher: its whole purpose is timing
     * precision, and a 20 ms task poll would throw that away. */
    for (int i = 0; i < SETTINGS_MAX_MARKER_SINKS; i++) {
        if (s_sinks[i].cfg.type == MARKER_SINK_GPIO) gpio_sink_mark(i, r);
    }

    s_head++;                 /* publish AFTER the record is fully written */
    s_stats.emitted++;

    /* With no record sink there is no flusher, so nothing would ever advance
     * the tail: the ring would fill after s_ring_slots markers and start
     * counting DROPS that mean nothing. Discard immediately instead, and keep
     * `dropped` meaning what it says — a record a sink should have had and
     * didn't. s_recent (the UI tail) is written above and is unaffected. */
    if (!s_flusher) s_tail = s_head;

    return ESP_OK;
}

/* ------------------------------------------------------------------ query --- */

void marker_get_stats(marker_stats_t *out)
{
    if (out) *out = s_stats;
}

size_t marker_get_recent(marker_record_t *out, size_t max)
{
    if (!out || !max) return 0;
    uint32_t have = s_recent_count < s_recent_slots
                        ? s_recent_count : s_recent_slots;
    if (have > max) have = (uint32_t)max;
    uint32_t first = s_recent_count - have;
    for (uint32_t i = 0; i < have; i++) {
        out[i] = s_recent[(first + i) % s_recent_slots];
    }
    return have;
}
