#include "timing_engine.h"
#include "isr_profiling.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <assert.h>
#include <stdatomic.h>

static const char* TAG = "timing_engine";

// ---------------------------------------------------------------------------
// SPSC ring buffer for the event store (producer = task, consumer = ISR)
//
// Layout: a flat array of TIMING_QUEUE_SIZE slots.
//   producer_tail: written only by the producer (task context).
//   consumer_head: written only by the ISR (consumer context).
//   Both are atomic so reads from the other side are safe without locks.
//   The buffer is "full" when (tail+1) % SIZE == head.
// ---------------------------------------------------------------------------

#define SPSC_EVENT_SLOTS TIMING_QUEUE_SIZE

typedef struct {
    timing_event_t slots[SPSC_EVENT_SLOTS];
    _Atomic uint32_t head;  // ISR advances after consuming
    _Atomic uint32_t tail;  // producer advances after inserting
} spsc_event_ring_t;

// ---------------------------------------------------------------------------
// Ready ring: ISR pushes slot indices here; dispatch task drains it.
// Max concurrent ready events per ISR tick is bounded by the event ring size.
// Using the same size for simplicity; in practice only a handful fire per ms.
// ---------------------------------------------------------------------------

#define READY_RING_SIZE TIMING_QUEUE_SIZE

typedef struct {
    uint8_t  indices[READY_RING_SIZE];
    _Atomic uint32_t head;  // dispatch task advances
    _Atomic uint32_t tail;  // ISR advances
} spsc_ready_ring_t;

// ---------------------------------------------------------------------------
// Per-engine state (all fields except the SPSCs accessed in task context only
// unless marked with "ISR reads/writes").
// ---------------------------------------------------------------------------

struct timing_engine {
    gptimer_handle_t master_timer;

    // ISR reads this to know whether to process events.
    volatile bool running;

    // ISR writes via esp_timer_get_time(); task reads for reporting.
    volatile uint64_t master_timestamp_us;

    // Event store shared between producer (task) and consumer (ISR).
    spsc_event_ring_t event_ring;

    // Ready queue shared between ISR (producer) and dispatch task (consumer).
    spsc_ready_ring_t ready_ring;

    // Dispatch task handle — notify target for the ISR.
    TaskHandle_t dispatch_task;

    // Updated in dispatch task only.
    uint64_t total_events_processed;
};

static timing_engine_t *g_engine = NULL;

// Mutex guards the PRODUCER side of event_ring (schedule / cancel / start / stop).
// The ISR never touches this mutex.
static SemaphoreHandle_t timing_mutex = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

// IRAM_ATTR is on the definition only; declaring it here without the attribute
// avoids the "conflicting section" warning from GCC when the definition follows.
static bool timing_engine_alarm_callback(gptimer_handle_t timer,
                                         const gptimer_alarm_event_data_t *edata,
                                         void *user_data);
static void timing_dispatch_task(void *pvParameters);

// ---------------------------------------------------------------------------
// SPSC helpers — all inlined, no function-call overhead in ISR.
// ---------------------------------------------------------------------------

// Producer-side enqueue (task context, mutex already held by caller).
static inline esp_err_t spsc_enqueue(spsc_event_ring_t *r, const timing_event_t *ev) {
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t next = (tail + 1) % SPSC_EVENT_SLOTS;
    uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (next == head) {
        return ESP_ERR_NO_MEM; // ring full
    }
    r->slots[tail] = *ev;
    // publish: store to tail after writing the slot so ISR sees consistent data.
    atomic_store_explicit(&r->tail, next, memory_order_release);
    return ESP_OK;
}

// ISR-side linear scan: find all slots with timestamp <= deadline, copy them
// to the ready ring.  Returns number of events fired.
//
// Linear scan is chosen over a sorted queue because:
//   - Max 64 slots → at most 64 comparisons → ~192 cycles, well under 1 ms tick.
//   - No sort overhead on the producer side keeps schedule_event cheap.
static inline uint32_t IRAM_ATTR spsc_scan_and_fire(spsc_event_ring_t *r,
                                                      spsc_ready_ring_t *ready,
                                                      uint64_t deadline) {
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t fired = 0;

    uint32_t idx = head;
    while (idx != tail) {
        timing_event_t *ev = &r->slots[idx];
        if (ev->active && ev->timestamp_us <= deadline) {
            ev->active = false;  // mark consumed before pushing to ready ring

            // Push slot index to ready ring for dispatch task.
            uint32_t rtail = atomic_load_explicit(&ready->tail, memory_order_relaxed);
            uint32_t rnext = (rtail + 1) % READY_RING_SIZE;
            uint32_t rhead = atomic_load_explicit(&ready->head, memory_order_acquire);
            if (rnext != rhead) {
                // safe: only ISR writes ready->tail
                ready->indices[rtail] = (uint8_t)idx;
                atomic_store_explicit(&ready->tail, rnext, memory_order_release);
                fired++;
            }
            // Whether we pushed or dropped, advance head past this consumed slot.
            uint32_t new_head = (idx + 1) % SPSC_EVENT_SLOTS;
            atomic_store_explicit(&r->head, new_head, memory_order_release);
            tail = atomic_load_explicit(&r->tail, memory_order_acquire);
            // restart scan from new head — we just moved head, so re-read it
            head = new_head;
            idx  = head;
        } else {
            idx = (idx + 1) % SPSC_EVENT_SLOTS;
        }
    }
    return fired;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t timing_engine_init(void) {
    if (g_engine != NULL) {
        ESP_LOGW(TAG, "Timing engine already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    g_engine = calloc(1, sizeof(timing_engine_t));
    if (!g_engine) {
        ESP_LOGE(TAG, "Failed to allocate timing engine");
        return ESP_ERR_NO_MEM;
    }

    atomic_init(&g_engine->event_ring.head, 0);
    atomic_init(&g_engine->event_ring.tail, 0);
    atomic_init(&g_engine->ready_ring.head, 0);
    atomic_init(&g_engine->ready_ring.tail, 0);

    // Producer-side mutex only — ISR never takes this.
    timing_mutex = xSemaphoreCreateMutex();
    if (!timing_mutex) {
        free(g_engine);
        g_engine = NULL;
        ESP_LOGE(TAG, "Failed to create timing mutex");
        return ESP_ERR_NO_MEM;
    }

    gptimer_config_t timer_config = {
        .clk_src      = GPTIMER_CLK_SRC_DEFAULT,
        .direction    = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };

    esp_err_t ret = gptimer_new_timer(&timer_config, &g_engine->master_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create hardware timer: %s", esp_err_to_name(ret));
        vSemaphoreDelete(timing_mutex);
        free(g_engine);
        g_engine = NULL;
        return ret;
    }

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timing_engine_alarm_callback,
    };
    ret = gptimer_register_event_callbacks(g_engine->master_timer, &cbs, g_engine);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register timer callback: %s", esp_err_to_name(ret));
        gptimer_del_timer(g_engine->master_timer);
        vSemaphoreDelete(timing_mutex);
        free(g_engine);
        g_engine = NULL;
        return ret;
    }

    ret = gptimer_enable(g_engine->master_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable hardware timer: %s", esp_err_to_name(ret));
        gptimer_del_timer(g_engine->master_timer);
        vSemaphoreDelete(timing_mutex);
        free(g_engine);
        g_engine = NULL;
        return ret;
    }

    g_engine->running = false;
    g_engine->total_events_processed = 0;

    ESP_LOGI(TAG, "Timing engine initialized");
    return ESP_OK;
}

esp_err_t timing_engine_start(void) {
    if (!g_engine) {
        ESP_LOGE(TAG, "Timing engine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire timing mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (g_engine->running) {
        xSemaphoreGive(timing_mutex);
        ESP_LOGW(TAG, "Timing engine already running");
        return ESP_ERR_INVALID_STATE;
    }

    // Start dispatch task before the ISR so it's ready to receive notifications.
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        timing_dispatch_task,
        "timing_dispatch",
        4096,
        g_engine,
        22,      // priority 22: above default tasks, below watchdog
        &g_engine->dispatch_task,
        1        // core 1
    );
    if (task_ret != pdPASS) {
        xSemaphoreGive(timing_mutex);
        ESP_LOGE(TAG, "Failed to create dispatch task");
        return ESP_ERR_NO_MEM;
    }

    g_engine->master_timestamp_us = esp_timer_get_time();
    g_engine->running = true;

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count  = 1000,               // 1000 μs = 1 ms period
        .flags.auto_reload_on_alarm = true,
    };

    esp_err_t ret = gptimer_set_alarm_action(g_engine->master_timer, &alarm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer alarm: %s", esp_err_to_name(ret));
        g_engine->running = false;
        vTaskDelete(g_engine->dispatch_task);
        g_engine->dispatch_task = NULL;
        xSemaphoreGive(timing_mutex);
        return ret;
    }

    ret = gptimer_start(g_engine->master_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start hardware timer: %s", esp_err_to_name(ret));
        g_engine->running = false;
        vTaskDelete(g_engine->dispatch_task);
        g_engine->dispatch_task = NULL;
        xSemaphoreGive(timing_mutex);
        return ret;
    }

    xSemaphoreGive(timing_mutex);
    ESP_LOGI(TAG, "Timing engine started (callbacks dispatched in task context)");
    return ESP_OK;
}

esp_err_t timing_engine_stop(void) {
    if (!g_engine) {
        ESP_LOGE(TAG, "Timing engine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire timing mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (!g_engine->running) {
        xSemaphoreGive(timing_mutex);
        ESP_LOGW(TAG, "Timing engine not running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gptimer_stop(g_engine->master_timer);
    g_engine->running = false;

    // Drain the SPSC rings under the producer mutex so no stale events fire.
    atomic_store_explicit(&g_engine->event_ring.head, 0, memory_order_relaxed);
    atomic_store_explicit(&g_engine->event_ring.tail, 0, memory_order_relaxed);
    atomic_store_explicit(&g_engine->ready_ring.head, 0, memory_order_relaxed);
    atomic_store_explicit(&g_engine->ready_ring.tail, 0, memory_order_relaxed);
    memset(g_engine->event_ring.slots, 0, sizeof(g_engine->event_ring.slots));

    // Stop the dispatch task after the ISR is stopped.
    if (g_engine->dispatch_task) {
        vTaskDelete(g_engine->dispatch_task);
        g_engine->dispatch_task = NULL;
    }

    xSemaphoreGive(timing_mutex);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Timing engine stopped");
    } else {
        ESP_LOGW(TAG, "Timer stop failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t timing_engine_deinit(void) {
    if (!g_engine) {
        ESP_LOGW(TAG, "Timing engine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_engine->running) {
        timing_engine_stop();
    }

    gptimer_disable(g_engine->master_timer);
    esp_err_t ret = gptimer_del_timer(g_engine->master_timer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to delete timer: %s", esp_err_to_name(ret));
    }

    if (timing_mutex) {
        vSemaphoreDelete(timing_mutex);
        timing_mutex = NULL;
    }

    free(g_engine);
    g_engine = NULL;

    ESP_LOGI(TAG, "Timing engine deinitialized");
    return ESP_OK;
}

// Callbacks run in TASK context (see timing_engine.h for the full contract).
esp_err_t timing_engine_schedule_event(uint64_t timestamp_us,
                                       timing_event_type_t type,
                                       timing_callback_t callback,
                                       void *user_data) {
    if (!g_engine || !callback) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_engine->running) {
        ESP_LOGE(TAG, "Timing engine not running");
        return ESP_ERR_INVALID_STATE;
    }

    timing_event_t ev = {
        .timestamp_us = timestamp_us,
        .type         = type,
        .callback     = callback,
        .user_data    = user_data,
        .active       = true,
    };

    // Only the producer side takes the mutex; the ISR never does.
    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = spsc_enqueue(&g_engine->event_ring, &ev);

    xSemaphoreGive(timing_mutex);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Scheduled %s event for %llu μs",
                 (type == TIMING_EVENT_TIMELINE) ? "TIMELINE" :
                 (type == TIMING_EVENT_AUDIO)    ? "AUDIO"    :
                 (type == TIMING_EVENT_LED)       ? "LED"      : "SYNC",
                 timestamp_us);
    } else {
        ESP_LOGW(TAG, "Event ring full, dropping event");
    }

    return ret;
}

esp_err_t timing_engine_cancel_events_by_type(timing_event_type_t type) {
    if (!g_engine) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Walk the live portion of the ring and deactivate matching events.
    // The ISR may read these concurrently, but it only fires events where
    // active==true, so clearing active is safe: it's a single-byte write
    // and the ISR does not write the active field of slots it hasn't yet
    // consumed (head has not advanced past them).
    uint32_t head = atomic_load_explicit(&g_engine->event_ring.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&g_engine->event_ring.tail, memory_order_acquire);
    size_t   cancelled = 0;

    uint32_t idx = head;
    while (idx != tail) {
        timing_event_t *ev = &g_engine->event_ring.slots[idx];
        if (ev->active && ev->type == type) {
            ev->active = false;
            cancelled++;
        }
        idx = (idx + 1) % SPSC_EVENT_SLOTS;
    }

    xSemaphoreGive(timing_mutex);

    ESP_LOGI(TAG, "Cancelled %zu events of type %d", cancelled, type);
    return ESP_OK;
}

uint64_t timing_engine_get_time_us(void) {
    return esp_timer_get_time();
}

bool timing_engine_is_running(void) {
    return (g_engine != NULL && g_engine->running);
}

uint64_t timing_engine_get_events_processed(void) {
    return g_engine ? g_engine->total_events_processed : 0;
}

size_t timing_engine_get_queue_utilization(void) {
    if (!g_engine) {
        return 0;
    }
    uint32_t head = atomic_load_explicit(&g_engine->event_ring.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&g_engine->event_ring.tail, memory_order_acquire);
    uint32_t used = (tail - head + SPSC_EVENT_SLOTS) % SPSC_EVENT_SLOTS;
    return (used * 100) / SPSC_EVENT_SLOTS;
}

// ---------------------------------------------------------------------------
// ISR — must contain ZERO xSemaphore* calls and ZERO ->callback() invocations.
// ---------------------------------------------------------------------------

static bool IRAM_ATTR timing_engine_alarm_callback(gptimer_handle_t timer,
                                                    const gptimer_alarm_event_data_t *edata,
                                                    void *user_data) {
    ISR_PROFILE_BEGIN(0);

    timing_engine_t *engine = (timing_engine_t *)user_data;
    if (!engine || !engine->running) {
        ISR_PROFILE_END(0);
        return false;
    }

    engine->master_timestamp_us = esp_timer_get_time();

    // Find and enqueue ready events; 500 μs look-ahead matches original tolerance.
    uint32_t fired = spsc_scan_and_fire(&engine->event_ring,
                                        &engine->ready_ring,
                                        engine->master_timestamp_us + 500);

    BaseType_t higher_woken = pdFALSE;
    if (fired > 0 && engine->dispatch_task) {
        vTaskNotifyGiveFromISR(engine->dispatch_task, &higher_woken);
    }

    ISR_PROFILE_END(0);
    return (higher_woken == pdTRUE);
}

// ---------------------------------------------------------------------------
// Dispatch task — invokes callbacks in task context.
// ---------------------------------------------------------------------------

static void timing_dispatch_task(void *pvParameters) {
    timing_engine_t *engine = (timing_engine_t *)pvParameters;

    while (1) {
        // Block until the ISR notifies that at least one event is ready.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint64_t now = esp_timer_get_time();

        // Drain the ready ring.
        while (1) {
            uint32_t rhead = atomic_load_explicit(&engine->ready_ring.head, memory_order_relaxed);
            uint32_t rtail = atomic_load_explicit(&engine->ready_ring.tail, memory_order_acquire);
            if (rhead == rtail) {
                break;
            }
            uint8_t slot_idx = engine->ready_ring.indices[rhead];
            // Advance head before calling the callback so re-entrant scheduling
            // during the callback does not deadlock on the ready ring.
            atomic_store_explicit(&engine->ready_ring.head,
                                  (rhead + 1) % READY_RING_SIZE,
                                  memory_order_release);

            timing_event_t *ev = &engine->event_ring.slots[slot_idx];
            if (ev->callback) {
                ev->callback(now, ev->user_data);
            }
            engine->total_events_processed++;
        }
    }
}
