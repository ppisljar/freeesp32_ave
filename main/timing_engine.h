#ifndef TIMING_ENGINE_H
#define TIMING_ENGINE_H

#include "esp_err.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct timing_engine timing_engine_t;
typedef void (*timing_callback_t)(uint64_t timestamp_us, void *user_data);

// Event types for different subsystems
typedef enum {
    TIMING_EVENT_TIMELINE,
    TIMING_EVENT_AUDIO,
    TIMING_EVENT_LED,
    TIMING_EVENT_SYNC,
    TIMING_EVENT_MAX
} timing_event_type_t;

// Event structure for scheduling
typedef struct {
    uint64_t timestamp_us;
    timing_event_type_t type;
    timing_callback_t callback;
    void *user_data;
    bool active;
} timing_event_t;

// Maximum number of concurrently pending events
#define TIMING_QUEUE_SIZE 64

// Public API

esp_err_t timing_engine_init(void);
esp_err_t timing_engine_start(void);
esp_err_t timing_engine_stop(void);
esp_err_t timing_engine_deinit(void);

// Schedule a future event.
//
// CALLBACK CONTRACT (changed in Step 3 of ISR Safety Remediation):
//   Callbacks run in TASK context (timing_dispatch_task, priority 22, core 1),
//   NOT in ISR context.  Expected dispatch latency: <= 50 µs typical after the
//   scheduled timestamp.  ISR-context callbacks are no longer supported.
//
//   Callers may safely use FreeRTOS task notifications, mutexes, and logging
//   inside their callbacks.  Re-entrant scheduling (calling
//   timing_engine_schedule_event again from within a callback) is also safe
//   because the dispatch task releases the ready-ring slot before invoking
//   the callback.
//
// timestamp_us: absolute time from esp_timer_get_time() at which to fire.
// type:         event category (TIMELINE / AUDIO / LED / SYNC).
// callback:     function to invoke at dispatch time.
// user_data:    opaque pointer forwarded to callback unchanged.
esp_err_t timing_engine_schedule_event(uint64_t timestamp_us,
                                       timing_event_type_t type,
                                       timing_callback_t callback,
                                       void *user_data);

esp_err_t timing_engine_cancel_events_by_type(timing_event_type_t type);
uint64_t  timing_engine_get_time_us(void);
bool      timing_engine_is_running(void);

// Performance monitoring
uint64_t timing_engine_get_events_processed(void);
size_t   timing_engine_get_queue_utilization(void);

#endif // TIMING_ENGINE_H
