#include "lock_free_comm.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "lock_free_comm";

// Global lock-free communication system
static lock_free_comm_system_t *g_comm_system = NULL;

// Ring buffer operations
static esp_err_t ring_buffer_init(lock_free_ring_buffer_t *buffer);

esp_err_t lock_free_comm_init(void) {
    if (g_comm_system != NULL) {
        ESP_LOGW(TAG, "Lock-free communication already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Allocate communication system structure
    g_comm_system = malloc(sizeof(lock_free_comm_system_t));
    if (!g_comm_system) {
        ESP_LOGE(TAG, "Failed to allocate lock-free communication system");
        return ESP_ERR_NO_MEM;
    }

    // Initialize all ring buffers
    ring_buffer_init(&g_comm_system->audio_to_led_queue);
    ring_buffer_init(&g_comm_system->led_to_audio_queue);
    ring_buffer_init(&g_comm_system->timeline_queue);
    ring_buffer_init(&g_comm_system->monitoring_queue);

    // Initialize state variables
    g_comm_system->audio_state.audio_active = false;
    g_comm_system->audio_state.audio_channels_mask = 0;
    g_comm_system->audio_state.audio_sample_count = 0;
    g_comm_system->audio_state.audio_amplitude_scaled = 0;

    g_comm_system->led_state.led_active = false;
    g_comm_system->led_state.led_frequency_hz_scaled = 0;
    g_comm_system->led_state.led_brightness = 0;
    g_comm_system->led_state.led_update_counter = 0;

    g_comm_system->sync_state.sync_active = false;
    g_comm_system->sync_state.sync_mode = 0;
    g_comm_system->sync_state.sync_events_processed = 0;
    g_comm_system->sync_state.sync_errors = 0;

    g_comm_system->total_communications = 0;

    // Memory barrier
    __sync_synchronize();
    g_comm_system->system_initialized = true;

    ESP_LOGI(TAG, "Lock-free communication system initialized");
    return ESP_OK;
}

esp_err_t lock_free_comm_deinit(void) {
    if (!g_comm_system) {
        ESP_LOGW(TAG, "Lock-free communication not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    g_comm_system->system_initialized = false;
    __sync_synchronize();

    free(g_comm_system);
    g_comm_system = NULL;

    ESP_LOGI(TAG, "Lock-free communication system deinitialized");
    return ESP_OK;
}

static esp_err_t ring_buffer_init(lock_free_ring_buffer_t *buffer) {
    buffer->write_index = 0;
    buffer->read_index = 0;
    buffer->total_messages = 0;
    buffer->dropped_messages = 0;

    memset(buffer->messages, 0, sizeof(buffer->messages));
    return ESP_OK;
}

// task-context only — memcpy and index update are not atomic under concurrent ISR writes
esp_err_t lock_free_send_message(lock_free_ring_buffer_t *buffer,
                                const lock_free_message_t *message) {
    if (!buffer || !message) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t write_index = buffer->write_index;
    uint32_t next_write_index = (write_index + 1) & RING_BUFFER_MASK;
    uint32_t read_index = buffer->read_index;

    // Check if buffer is full
    if (next_write_index == read_index) {
        buffer->dropped_messages++;
        return ESP_ERR_NO_MEM;
    }

    // Copy message
    memcpy(&buffer->messages[write_index], message, sizeof(lock_free_message_t));

    // Memory barrier before updating write index
    __sync_synchronize();

    buffer->write_index = next_write_index;
    buffer->total_messages++;

    return ESP_OK;
}

// task-context only — same atomicity constraint as lock_free_send_message
esp_err_t lock_free_receive_message(lock_free_ring_buffer_t *buffer,
                                   lock_free_message_t *message) {
    if (!buffer || !message) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t read_index = buffer->read_index;
    uint32_t write_index = buffer->write_index;

    if (read_index == write_index) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(message, &buffer->messages[read_index], sizeof(lock_free_message_t));

    __sync_synchronize();

    uint32_t next_read_index = (read_index + 1) & RING_BUFFER_MASK;
    buffer->read_index = next_read_index;

    return ESP_OK;
}

bool lock_free_has_messages(lock_free_ring_buffer_t *buffer) {
    if (!buffer) return false;
    return (buffer->read_index != buffer->write_index);
}

size_t lock_free_get_message_count(lock_free_ring_buffer_t *buffer) {
    if (!buffer) return 0;
    return (buffer->write_index - buffer->read_index) & RING_BUFFER_MASK;
}

// State management functions
void IRAM_ATTR lock_free_set_audio_state(bool active, uint32_t channels_mask,
                                         uint64_t sample_count, uint32_t amplitude_milli) {
    if (!g_comm_system) return;

    g_comm_system->audio_state.audio_active = active;
    g_comm_system->audio_state.audio_channels_mask = channels_mask;
    g_comm_system->audio_state.audio_sample_count = sample_count;
    g_comm_system->audio_state.audio_amplitude_scaled = amplitude_milli;

    g_comm_system->total_communications++;
}

void lock_free_get_audio_state(bool *active, uint32_t *channels_mask,
                               uint64_t *sample_count, float *amplitude) {
    if (!g_comm_system || !active || !channels_mask || !sample_count || !amplitude) return;

    *active = g_comm_system->audio_state.audio_active;
    *channels_mask = g_comm_system->audio_state.audio_channels_mask;
    *sample_count = g_comm_system->audio_state.audio_sample_count;
    *amplitude = (float)g_comm_system->audio_state.audio_amplitude_scaled / 1000.0f;
}

void IRAM_ATTR lock_free_set_led_state(bool active, uint32_t frequency_milliHz,
                                       uint32_t brightness, uint32_t update_count) {
    if (!g_comm_system) return;

    g_comm_system->led_state.led_active = active;
    g_comm_system->led_state.led_brightness = brightness;
    g_comm_system->led_state.led_update_counter = update_count;
    g_comm_system->led_state.led_frequency_hz_scaled = frequency_milliHz;

    g_comm_system->total_communications++;
}

void lock_free_get_led_state(bool *active, float *frequency_hz,
                             uint32_t *brightness, uint32_t *update_count) {
    if (!g_comm_system || !active || !frequency_hz || !brightness || !update_count) return;

    *active = g_comm_system->led_state.led_active;
    *brightness = g_comm_system->led_state.led_brightness;
    *update_count = g_comm_system->led_state.led_update_counter;
    *frequency_hz = (float)g_comm_system->led_state.led_frequency_hz_scaled / 1000.0f;
}

void IRAM_ATTR lock_free_set_sync_state(bool active, uint32_t mode,
                                        uint64_t events_processed, uint32_t errors) {
    if (!g_comm_system) return;

    g_comm_system->sync_state.sync_active = active;
    g_comm_system->sync_state.sync_mode = mode;
    g_comm_system->sync_state.sync_events_processed = events_processed;
    g_comm_system->sync_state.sync_errors = errors;

    g_comm_system->total_communications++;
}

void lock_free_get_sync_state(bool *active, uint32_t *mode,
                              uint64_t *events_processed, uint32_t *errors) {
    if (!g_comm_system || !active || !mode || !events_processed || !errors) return;

    *active = g_comm_system->sync_state.sync_active;
    *mode = g_comm_system->sync_state.sync_mode;
    *events_processed = g_comm_system->sync_state.sync_events_processed;
    *errors = g_comm_system->sync_state.sync_errors;
}

uint64_t lock_free_get_total_communications(void) {
    return g_comm_system ? g_comm_system->total_communications : 0;
}

esp_err_t lock_free_get_queue_stats(lock_free_ring_buffer_t *buffer,
                                   size_t *total_messages,
                                   size_t *dropped_messages,
                                   size_t *current_utilization) {
    if (!buffer || !total_messages || !dropped_messages || !current_utilization) {
        return ESP_ERR_INVALID_ARG;
    }

    *total_messages = buffer->total_messages;
    *dropped_messages = buffer->dropped_messages;
    *current_utilization = lock_free_get_message_count(buffer);

    return ESP_OK;
}

// Queue access helpers
lock_free_ring_buffer_t* lock_free_get_audio_to_led_queue(void) {
    return g_comm_system ? &g_comm_system->audio_to_led_queue : NULL;
}

lock_free_ring_buffer_t* lock_free_get_led_to_audio_queue(void) {
    return g_comm_system ? &g_comm_system->led_to_audio_queue : NULL;
}

lock_free_ring_buffer_t* lock_free_get_timeline_queue(void) {
    return g_comm_system ? &g_comm_system->timeline_queue : NULL;
}

lock_free_ring_buffer_t* lock_free_get_monitoring_queue(void) {
    return g_comm_system ? &g_comm_system->monitoring_queue : NULL;
}