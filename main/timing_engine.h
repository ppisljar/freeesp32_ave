#ifndef TIMING_ENGINE_H
#define TIMING_ENGINE_H

#include "esp_err.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct timing_engine timing_engine_t;
typedef void (*timing_callback_t)(uint64_t timestamp_us, void *user_data);

// Event types for different subsystems
typedef enum {
    TIMING_EVENT_TIMELINE,      // Timeline entry execution
    TIMING_EVENT_AUDIO,         // Audio parameter updates
    TIMING_EVENT_LED,           // LED effect changes
    TIMING_EVENT_SYNC,          // Audio-LED synchronization
    TIMING_EVENT_MAX
} timing_event_type_t;

// Event structure for scheduling
typedef struct {
    uint64_t timestamp_us;          // When to execute (microseconds)
    timing_event_type_t type;       // Event type
    timing_callback_t callback;     // Function to call
    void *user_data;               // Data for callback
    bool active;                   // Event is scheduled
} timing_event_t;

// Event queue for coordinated execution
#define TIMING_QUEUE_SIZE 64
typedef struct {
    timing_event_t events[TIMING_QUEUE_SIZE];
    size_t head;                   // Next event to process
    size_t tail;                   // Next free slot
    size_t count;                  // Number of scheduled events
} timing_event_queue_t;

// Main timing engine structure
typedef struct timing_engine {
    esp_timer_handle_t master_timer;    // High-precision hardware timer
    uint64_t master_timestamp_us;       // Current time reference
    timing_event_queue_t event_queue;   // Coordinated event scheduling
    bool running;                       // Engine operational status
    uint64_t total_events_processed;    // Performance monitoring
} timing_engine_t;

// Public API functions
esp_err_t timing_engine_init(void);
esp_err_t timing_engine_start(void);
esp_err_t timing_engine_stop(void);
esp_err_t timing_engine_deinit(void);

// Event scheduling functions
esp_err_t timing_engine_schedule_event(uint64_t timestamp_us,
                                      timing_event_type_t type,
                                      timing_callback_t callback,
                                      void *user_data);

esp_err_t timing_engine_cancel_events_by_type(timing_event_type_t type);
uint64_t timing_engine_get_time_us(void);
bool timing_engine_is_running(void);

// Performance monitoring
uint64_t timing_engine_get_events_processed(void);
size_t timing_engine_get_queue_utilization(void);

#endif // TIMING_ENGINE_H