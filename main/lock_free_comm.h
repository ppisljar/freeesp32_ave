#ifndef LOCK_FREE_COMM_H
#define LOCK_FREE_COMM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Lock-free ring buffer for message passing
#define RING_BUFFER_SIZE 64
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

// Message types for inter-subsystem communication
typedef enum {
    MSG_TYPE_TIMELINE_EVENT,    // Timeline execution event
    MSG_TYPE_AUDIO_PARAM,       // Audio parameter update
    MSG_TYPE_LED_CONTROL,       // LED control command
    MSG_TYPE_SYNC_CONFIG,       // Sync configuration change
    MSG_TYPE_PERFORMANCE_DATA,  // Performance monitoring data
    MSG_TYPE_MAX
} message_type_t;

// Generic message structure for all subsystem communication
typedef struct {
    message_type_t type;        // Message type identifier
    uint64_t timestamp_us;      // Microsecond timestamp
    uint32_t data_size;         // Size of data payload
    uint8_t data[32];          // Inline data payload (small messages)
} lock_free_message_t;

// Simple lock-free ring buffer structure using volatile variables
typedef struct {
    volatile uint32_t write_index;                // Producer index
    uint8_t _pad1[60];                           // Padding for cache line alignment
    volatile uint32_t read_index;                // Consumer index
    uint8_t _pad2[60];                           // Padding for cache line alignment
    lock_free_message_t messages[RING_BUFFER_SIZE]; // Message storage
    volatile uint64_t total_messages;            // Statistics: total messages
    volatile uint32_t dropped_messages;          // Statistics: dropped messages
} lock_free_ring_buffer_t;

// Volatile shared state for audio system
typedef struct {
    volatile bool audio_active;                   // Audio system active
    volatile uint32_t audio_channels_mask;       // Active channel bitmask
    volatile uint64_t audio_sample_count;        // Current sample count
    volatile uint32_t audio_amplitude_scaled;    // Current amplitude (0-1000)
} atomic_audio_state_t;

// Volatile shared state for LED system
typedef struct {
    volatile bool led_active;                     // LED system active
    volatile uint32_t led_frequency_hz_scaled;   // Frequency in Hz * 1000
    volatile uint32_t led_brightness;            // Brightness (0-100)
    volatile uint32_t led_update_counter;        // LED updates processed
} atomic_led_state_t;

// Volatile shared state for synchronization
typedef struct {
    volatile bool sync_active;                    // Sync system active
    volatile uint32_t sync_mode;                 // Current sync mode
    volatile uint64_t sync_events_processed;     // Sync events processed
    volatile uint32_t sync_errors;               // Sync error count
} atomic_sync_state_t;

// Main lock-free communication system
typedef struct {
    lock_free_ring_buffer_t audio_to_led_queue;   // Audio → LED messages
    lock_free_ring_buffer_t led_to_audio_queue;   // LED → Audio messages
    lock_free_ring_buffer_t timeline_queue;       // Timeline → All messages
    lock_free_ring_buffer_t monitoring_queue;     // Performance monitoring

    atomic_audio_state_t audio_state;             // Audio system state
    atomic_led_state_t led_state;                 // LED system state
    atomic_sync_state_t sync_state;               // Sync system state

    volatile bool system_initialized;             // System ready flag
    volatile uint64_t total_communications;      // Total messages passed
} lock_free_comm_system_t;

// Public API functions
esp_err_t lock_free_comm_init(void);
esp_err_t lock_free_comm_deinit(void);

// Message passing functions (task-context only — not ISR-safe)
esp_err_t lock_free_send_message(lock_free_ring_buffer_t *buffer,
                                const lock_free_message_t *message);
esp_err_t lock_free_receive_message(lock_free_ring_buffer_t *buffer,
                                   lock_free_message_t *message);
bool lock_free_has_messages(lock_free_ring_buffer_t *buffer);
size_t lock_free_get_message_count(lock_free_ring_buffer_t *buffer);

// Atomic state access functions
void lock_free_set_audio_state(bool active, uint32_t channels_mask,
                               uint64_t sample_count, uint32_t amplitude_milli);
void lock_free_get_audio_state(bool *active, uint32_t *channels_mask,
                               uint64_t *sample_count, float *amplitude);

void lock_free_set_led_state(bool active, uint32_t frequency_milliHz,
                             uint32_t brightness, uint32_t update_count);
void lock_free_get_led_state(bool *active, float *frequency_hz,
                             uint32_t *brightness, uint32_t *update_count);

void lock_free_set_sync_state(bool active, uint32_t mode,
                              uint64_t events_processed, uint32_t errors);
void lock_free_get_sync_state(bool *active, uint32_t *mode,
                              uint64_t *events_processed, uint32_t *errors);

// Performance monitoring
uint64_t lock_free_get_total_communications(void);
esp_err_t lock_free_get_queue_stats(lock_free_ring_buffer_t *buffer,
                                   size_t *total_messages,
                                   size_t *dropped_messages,
                                   size_t *current_utilization);

// Queue access helpers for subsystems
lock_free_ring_buffer_t* lock_free_get_audio_to_led_queue(void);
lock_free_ring_buffer_t* lock_free_get_led_to_audio_queue(void);
lock_free_ring_buffer_t* lock_free_get_timeline_queue(void);
lock_free_ring_buffer_t* lock_free_get_monitoring_queue(void);

#endif // LOCK_FREE_COMM_H