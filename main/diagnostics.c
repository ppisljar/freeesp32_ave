/**
 * @file diagnostics.c
 * @brief Implementation of WiFi-accessible diagnostics (see diagnostics.h).
 */

#include "diagnostics.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_partition.h"
#endif

static const char *TAG = "diag";

/* ---------------------------------------------------------------------------
 * Layer 1 — reset reason
 * --------------------------------------------------------------------------- */

static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;

esp_reset_reason_t diagnostics_reset_reason(void)
{
    return s_reset_reason;
}

const char *diagnostics_reset_reason_str(void)
{
    switch (s_reset_reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * Layer 2 — runtime log ring + esp_log vprintf hook
 *
 * A byte ring in PSRAM mirrors every formatted ESP_LOGx line. The vprintf hook
 * runs in the context of whatever task logged, so the ring is guarded by a
 * FreeRTOS mutex taken NON-BLOCKING in the writer (a lost log beats a stall or
 * deadlock) and with a short timeout in the reader (no torn copy). We never hold
 * the lock across network I/O.
 * --------------------------------------------------------------------------- */

#define DIAG_LOG_RING_BYTES   (32u * 1024u)
#define DIAG_FMT_BUF_BYTES     256u

static char             *s_ring;                 /* PSRAM, DIAG_LOG_RING_BYTES */
static size_t            s_head;                  /* next write index          */
static size_t            s_count;                 /* valid bytes (<= ring size) */
static SemaphoreHandle_t s_lock;
static vprintf_like_t    s_prev_vprintf;          /* original (UART) sink       */

/* Append bytes to the ring, overwriting the oldest on wrap. Caller holds s_lock. */
static void ring_append_locked(const char *data, size_t len)
{
    if (len > DIAG_LOG_RING_BYTES) {         /* keep only the tail of a huge line */
        data += len - DIAG_LOG_RING_BYTES;
        len   = DIAG_LOG_RING_BYTES;
    }
    size_t first = DIAG_LOG_RING_BYTES - s_head;
    if (first >= len) {
        memcpy(s_ring + s_head, data, len);
    } else {
        memcpy(s_ring + s_head, data, first);
        memcpy(s_ring, data + first, len - first);
    }
    s_head = (s_head + len) % DIAG_LOG_RING_BYTES;
    s_count += len;
    if (s_count > DIAG_LOG_RING_BYTES) {
        s_count = DIAG_LOG_RING_BYTES;
    }
}

static int diag_vprintf(const char *fmt, va_list ap)
{
    /* 1. Forward to the original UART sink so a wired console still works. Use a
     *    copy — a va_list is single-pass. */
    va_list ap_fwd;
    va_copy(ap_fwd, ap);
    int n = s_prev_vprintf ? s_prev_vprintf(fmt, ap_fwd) : 0;
    va_end(ap_fwd);

    /* 2. Format into a local buffer for the ring. */
    if (s_ring && s_lock) {
        char buf[DIAG_FMT_BUF_BYTES];
        int m = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (m > 0) {
            size_t len = (m < (int)sizeof(buf)) ? (size_t)m : (sizeof(buf) - 1u);
            /* Non-blocking: if a reader holds the lock, drop this line. */
            if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
                ring_append_locked(buf, len);
                xSemaphoreGive(s_lock);
            }
        }
    }
    return n;
}

size_t diagnostics_get_logs(char *out, size_t out_size)
{
    if (!s_ring || !s_lock || !out || out_size == 0u) {
        return 0u;
    }
    size_t written = 0u;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0u;
    }
    size_t n = s_count;
    /* If the buffer can't hold everything, drop the oldest — keep recent output. */
    size_t skip = (n > out_size) ? (n - out_size) : 0u;
    size_t start = (s_head + DIAG_LOG_RING_BYTES - s_count + skip) % DIAG_LOG_RING_BYTES;
    size_t to_copy = n - skip;
    size_t first = DIAG_LOG_RING_BYTES - start;
    if (first >= to_copy) {
        memcpy(out, s_ring + start, to_copy);
    } else {
        memcpy(out, s_ring + start, first);
        memcpy(out + first, s_ring, to_copy - first);
    }
    written = to_copy;
    xSemaphoreGive(s_lock);
    return written;
}

size_t diagnostics_logs_size(void)
{
    return s_count;
}

void diagnostics_clear_logs(void)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_head = 0u;
        s_count = 0u;
        xSemaphoreGive(s_lock);
    }
}

/* ---------------------------------------------------------------------------
 * Subsystem health — boot is non-fatal; each subsystem records ok/failed here
 * so the web server always comes up and the diagnostics page can warn about
 * anything that didn't start. Written once per subsystem at boot (single-
 * threaded app_main), read from the httpd task; a plain array + no lock is
 * safe because writes finish before the web server (hence any reader) exists.
 * ------------------------------------------------------------------------- */
#define DIAG_HEALTH_MAX 12
static struct { char name[16]; bool ok; char msg[64]; bool used; } s_health[DIAG_HEALTH_MAX];

void diagnostics_health_set(const char *name, bool ok, const char *msg)
{
    if (!name) return;
    int free_slot = -1;
    for (int i = 0; i < DIAG_HEALTH_MAX; i++) {
        if (s_health[i].used && strncmp(s_health[i].name, name, sizeof(s_health[i].name)) == 0) {
            free_slot = i; break;                 /* update existing */
        }
        if (free_slot < 0 && !s_health[i].used) free_slot = i;
    }
    if (free_slot < 0) return;                     /* table full — drop */
    s_health[free_slot].used = true;
    s_health[free_slot].ok = ok;
    strlcpy(s_health[free_slot].name, name, sizeof(s_health[free_slot].name));
    strlcpy(s_health[free_slot].msg, msg ? msg : "", sizeof(s_health[free_slot].msg));
    if (!ok) ESP_LOGW(TAG, "subsystem '%s' DEGRADED: %s", name, msg ? msg : "(failed)");
}

size_t diagnostics_health_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    size_t n = 0;
    n += snprintf(out + n, out_size - n, "{");
    bool first = true;
    for (int i = 0; i < DIAG_HEALTH_MAX && n < out_size; i++) {
        if (!s_health[i].used) continue;
        if (s_health[i].ok) {
            n += snprintf(out + n, out_size - n, "%s\"%s\":{\"ok\":true}",
                          first ? "" : ",", s_health[i].name);
        } else {
            n += snprintf(out + n, out_size - n, "%s\"%s\":{\"ok\":false,\"msg\":\"%s\"}",
                          first ? "" : ",", s_health[i].name, s_health[i].msg);
        }
        first = false;
    }
    if (n < out_size) n += snprintf(out + n, out_size - n, "}");
    return n;
}

/* ---------------------------------------------------------------------------
 * Layer 3 — core dump retrieval (compiled in only with coredump-to-flash)
 * --------------------------------------------------------------------------- */

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

static const esp_partition_t *coredump_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
}

bool diagnostics_coredump_present(size_t *size_out)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        return false;
    }
    if (size_out) {
        *size_out = size;
    }
    return true;
}

size_t diagnostics_coredump_read(size_t offset, void *out, size_t len)
{
    size_t addr = 0, size = 0;
    if (!out || esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        return 0u;
    }
    if (offset >= size) {
        return 0u;
    }
    if (offset + len > size) {
        len = size - offset;
    }
    const esp_partition_t *p = coredump_partition();
    if (!p) {
        return 0u;
    }
    /* esp_core_dump_image_get returns a flash address; the image lives at the
     * start of the coredump partition, so read at `offset` within it. */
    if (esp_partition_read(p, offset, out, len) != ESP_OK) {
        return 0u;
    }
    return len;
}

esp_err_t diagnostics_coredump_erase(void)
{
    return esp_core_dump_image_erase();
}

#else  /* coredump-to-flash not compiled in */

bool diagnostics_coredump_present(size_t *size_out)
{
    (void)size_out;
    return false;
}

size_t diagnostics_coredump_read(size_t offset, void *out, size_t len)
{
    (void)offset; (void)out; (void)len;
    return 0u;
}

esp_err_t diagnostics_coredump_erase(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */

/* ---------------------------------------------------------------------------
 * Init
 * --------------------------------------------------------------------------- */

void diagnostics_early_init(void)
{
    static bool s_inited = false;
    if (s_inited) {
        return;
    }
    s_inited = true;

    s_reset_reason = esp_reset_reason();

    /* Log ring in PSRAM (falls back to any heap; disabled if both fail). */
    s_lock = xSemaphoreCreateMutex();
    s_ring = heap_caps_malloc(DIAG_LOG_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        s_ring = heap_caps_malloc(DIAG_LOG_RING_BYTES, MALLOC_CAP_8BIT);
    }
    if (s_ring && s_lock) {
        s_head = 0u;
        s_count = 0u;
        s_prev_vprintf = esp_log_set_vprintf(diag_vprintf);
    } else {
        ESP_LOGW(TAG, "log ring unavailable (ring=%p lock=%p) — /api/logs disabled",
                 s_ring, s_lock);
    }

    ESP_LOGI(TAG, "diagnostics init: reset_reason=%s, log ring=%s",
             diagnostics_reset_reason_str(),
             (s_ring && s_prev_vprintf) ? "on" : "off");

    bool cd = diagnostics_coredump_present(NULL);
    if (cd) {
        ESP_LOGW(TAG, "a core dump is stored in flash — retrieve via GET /api/coredump");
    }
}
