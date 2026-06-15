#include "timing_engine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <assert.h>

static const char* TAG = "timing_engine";

// Global timing engine instance
static timing_engine_t *g_timing_engine = NULL;
static SemaphoreHandle_t timing_mutex = NULL;

// High-precision timer callback
static void timing_engine_callback(void* arg);

// Event queue management
static esp_err_t enqueue_event(const timing_event_t *event);
static esp_err_t dequeue_event(timing_event_t *event);
static bool has_pending_events(void);
static void process_ready_events(uint64_t current_time_us);
static void sort_event_queue(void);

esp_err_t timing_engine_init(void) {
    if (g_timing_engine != NULL) {
        ESP_LOGW(TAG, "Timing engine already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Allocate timing engine structure
    g_timing_engine = calloc(1, sizeof(timing_engine_t));
    if (!g_timing_engine) {
        ESP_LOGE(TAG, "Failed to allocate timing engine");
        return ESP_ERR_NO_MEM;
    }

    // Create mutex for thread safety
    timing_mutex = xSemaphoreCreateMutex();
    if (!timing_mutex) {
        free(g_timing_engine);
        g_timing_engine = NULL;
        ESP_LOGE(TAG, "Failed to create timing mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create high-precision hardware timer
    esp_timer_create_args_t timer_args = {
        .callback = timing_engine_callback,
        .arg = g_timing_engine,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "master_timing",
        .skip_unhandled_events = false
    };

    esp_err_t ret = esp_timer_create(&timer_args, &g_timing_engine->master_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create master timer: %s", esp_err_to_name(ret));
        vSemaphoreDelete(timing_mutex);
        free(g_timing_engine);
        g_timing_engine = NULL;
        return ret;
    }

    // Initialize event queue
    memset(&g_timing_engine->event_queue, 0, sizeof(timing_event_queue_t));
    g_timing_engine->running = false;
    g_timing_engine->total_events_processed = 0;

    ESP_LOGI(TAG, "Timing engine initialized with microsecond precision");
    return ESP_OK;
}

esp_err_t timing_engine_start(void) {
    if (!g_timing_engine) {
        ESP_LOGE(TAG, "Timing engine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire timing mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (g_timing_engine->running) {
        xSemaphoreGive(timing_mutex);
        ESP_LOGW(TAG, "Timing engine already running");
        return ESP_ERR_INVALID_STATE;
    }

    // Get initial timestamp
    g_timing_engine->master_timestamp_us = esp_timer_get_time();
    g_timing_engine->running = true;

    // Start timer with 1ms resolution for responsive event processing
    esp_err_t ret = esp_timer_start_periodic(g_timing_engine->master_timer, 1000); // 1ms
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start master timer: %s", esp_err_to_name(ret));
        g_timing_engine->running = false;
        xSemaphoreGive(timing_mutex);
        return ret;
    }

    xSemaphoreGive(timing_mutex);
    ESP_LOGI(TAG, "Timing engine started with 1ms resolution");
    return ESP_OK;
}

esp_err_t timing_engine_stop(void) {
    if (!g_timing_engine) {
        ESP_LOGE(TAG, "Timing engine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire timing mutex");
        return ESP_ERR_TIMEOUT;
    }

    if (!g_timing_engine->running) {
        xSemaphoreGive(timing_mutex);
        ESP_LOGW(TAG, "Timing engine not running");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop the timer
    esp_err_t ret = esp_timer_stop(g_timing_engine->master_timer);
    g_timing_engine->running = false;

    // Clear event queue
    memset(&g_timing_engine->event_queue, 0, sizeof(timing_event_queue_t));

    xSemaphoreGive(timing_mutex);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Timing engine stopped");
    } else {
        ESP_LOGW(TAG, "Timer stop failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t timing_engine_deinit(void) {
    if (!g_timing_engine) {
        ESP_LOGW(TAG, "Timing engine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop if running
    if (g_timing_engine->running) {
        timing_engine_stop();
    }

    // Delete timer
    esp_err_t ret = esp_timer_delete(g_timing_engine->master_timer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to delete timer: %s", esp_err_to_name(ret));
    }

    // Clean up
    if (timing_mutex) {
        vSemaphoreDelete(timing_mutex);
        timing_mutex = NULL;
    }

    free(g_timing_engine);
    g_timing_engine = NULL;

    ESP_LOGI(TAG, "Timing engine deinitialized");
    return ESP_OK;
}

esp_err_t timing_engine_schedule_event(uint64_t timestamp_us,
                                      timing_event_type_t type,
                                      timing_callback_t callback,
                                      void *user_data) {
    if (!g_timing_engine || !callback) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_timing_engine->running) {
        ESP_LOGE(TAG, "Timing engine not running");
        return ESP_ERR_INVALID_STATE;
    }

    timing_event_t event = {
        .timestamp_us = timestamp_us,
        .type = type,
        .callback = callback,
        .user_data = user_data,
        .active = true
    };

    esp_err_t ret = enqueue_event(&event);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Scheduled %s event for %llu μs",
                 (type == TIMING_EVENT_TIMELINE) ? "TIMELINE" :
                 (type == TIMING_EVENT_AUDIO) ? "AUDIO" :
                 (type == TIMING_EVENT_LED) ? "LED" : "SYNC",
                 timestamp_us);
    }

    return ret;
}

esp_err_t timing_engine_cancel_events_by_type(timing_event_type_t type) {
    if (!g_timing_engine) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    timing_event_queue_t *queue = &g_timing_engine->event_queue;
    size_t cancelled = 0;

    for (size_t i = 0; i < queue->count; i++) {
        size_t index = (queue->head + i) % TIMING_QUEUE_SIZE;
        if (queue->events[index].active && queue->events[index].type == type) {
            queue->events[index].active = false;
            cancelled++;
        }
    }

    xSemaphoreGive(timing_mutex);

    ESP_LOGI(TAG, "Cancelled %zu events of type %d", cancelled, type);
    return ESP_OK;
}

uint64_t timing_engine_get_time_us(void) {
    return esp_timer_get_time();
}

bool timing_engine_is_running(void) {
    return (g_timing_engine != NULL && g_timing_engine->running);
}

uint64_t timing_engine_get_events_processed(void) {
    return g_timing_engine ? g_timing_engine->total_events_processed : 0;
}

size_t timing_engine_get_queue_utilization(void) {
    if (!g_timing_engine) {
        return 0;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return 0; // Can't get lock, assume queue is busy
    }

    size_t utilization = g_timing_engine->event_queue.count;
    xSemaphoreGive(timing_mutex);

    return (utilization * 100) / TIMING_QUEUE_SIZE;
}

// Timer callback - processes ready events with microsecond precision
static void IRAM_ATTR timing_engine_callback(void* arg) {
    timing_engine_t *engine = (timing_engine_t*)arg;
    if (!engine || !engine->running) {
        return;
    }

    // Update master timestamp
    engine->master_timestamp_us = esp_timer_get_time();

    // Process all ready events
    process_ready_events(engine->master_timestamp_us);
}

static void process_ready_events(uint64_t current_time_us) {
    timing_event_t event;
    size_t processed_count = 0;
    const size_t max_events_per_cycle = 8; // Limit processing to avoid blocking

    // Process all events ready for execution
    while (has_pending_events() && processed_count < max_events_per_cycle) {
        if (dequeue_event(&event) != ESP_OK) {
            break;
        }

        // Check if event is ready (with small tolerance for timing precision)
        if (event.active && event.timestamp_us <= current_time_us + 500) { // 500μs tolerance
            // Execute event callback
            event.callback(current_time_us, event.user_data);
            g_timing_engine->total_events_processed++;
            processed_count++;

            ESP_LOGD(TAG, "Executed event at %llu μs (scheduled for %llu μs)",
                     current_time_us, event.timestamp_us);
        } else if (event.active) {
            // Event not ready yet - put back in queue
            enqueue_event(&event);
            break; // Queue is time-sorted, so no more ready events
        }
    }

    if (processed_count == max_events_per_cycle) {
        ESP_LOGD(TAG, "Hit max events per cycle limit (%zu)", max_events_per_cycle);
    }
}

static esp_err_t enqueue_event(const timing_event_t *event) {
    if (!event || !g_timing_engine) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    timing_event_queue_t *queue = &g_timing_engine->event_queue;

    if (queue->count >= TIMING_QUEUE_SIZE) {
        xSemaphoreGive(timing_mutex);
        ESP_LOGW(TAG, "Event queue full, dropping event");
        return ESP_ERR_NO_MEM;
    }

    // Add event to queue
    queue->events[queue->tail] = *event;
    queue->tail = (queue->tail + 1) % TIMING_QUEUE_SIZE;
    queue->count++;

    // Sort queue to maintain time order (simple insertion sort for small queues)
    sort_event_queue();

    xSemaphoreGive(timing_mutex);
    return ESP_OK;
}

static esp_err_t dequeue_event(timing_event_t *event) {
    if (!event || !g_timing_engine) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(timing_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    timing_event_queue_t *queue = &g_timing_engine->event_queue;

    if (queue->count == 0) {
        xSemaphoreGive(timing_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    // Get next event
    *event = queue->events[queue->head];
    queue->head = (queue->head + 1) % TIMING_QUEUE_SIZE;
    queue->count--;

    xSemaphoreGive(timing_mutex);
    return ESP_OK;
}

static bool has_pending_events(void) {
    return (g_timing_engine && g_timing_engine->event_queue.count > 0);
}

static void sort_event_queue(void) {
    // Simple insertion sort for maintaining time order
    // Only called with mutex held, so no additional locking needed
    timing_event_queue_t *queue = &g_timing_engine->event_queue;

    if (queue->count <= 1) {
        return;
    }

    // Create a temporary sorted array
    timing_event_t sorted_events[TIMING_QUEUE_SIZE];
    size_t active_count = 0;

    // Extract active events
    for (size_t i = 0; i < queue->count; i++) {
        size_t index = (queue->head + i) % TIMING_QUEUE_SIZE;
        if (queue->events[index].active) {
            sorted_events[active_count++] = queue->events[index];
        }
    }

    // Sort by timestamp using simple insertion sort
    for (size_t i = 1; i < active_count; i++) {
        timing_event_t key = sorted_events[i];
        int j = i - 1;

        while (j >= 0 && sorted_events[j].timestamp_us > key.timestamp_us) {
            sorted_events[j + 1] = sorted_events[j];
            j--;
        }
        sorted_events[j + 1] = key;
    }

    // Update queue with sorted events
    queue->head = 0;
    queue->tail = active_count;
    queue->count = active_count;

    for (size_t i = 0; i < active_count; i++) {
        queue->events[i] = sorted_events[i];
    }
}