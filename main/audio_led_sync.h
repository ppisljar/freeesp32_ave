#ifndef AUDIO_LED_SYNC_H
#define AUDIO_LED_SYNC_H

#include "esp_err.h"
#include "driver/i2s_std.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct audio_led_sync_state audio_led_sync_state_t;

// Synchronization modes
typedef enum {
    SYNC_MODE_DISABLED,         // No synchronization
    SYNC_MODE_VU_METER,        // LED intensity follows audio amplitude
    SYNC_MODE_BEAT_FREQUENCY,  // LED flicker locked to binaural beat frequency
    SYNC_MODE_PHASE_LOCK,      // LED timing phase-locked to audio samples
    SYNC_MODE_CUSTOM           // User-defined synchronization pattern
} audio_led_sync_mode_t;

// VU meter configuration
typedef struct {
    float attack_time_ms;       // Rise time for VU meter response
    float decay_time_ms;        // Fall time for VU meter response
    float sensitivity;          // Audio amplitude sensitivity (0.0-1.0)
    uint8_t brightness_min;     // Minimum LED brightness (0-100)
    uint8_t brightness_max;     // Maximum LED brightness (0-100)
} vu_meter_config_t;

// Beat frequency synchronization
typedef struct {
    float beat_frequency_hz;    // Target beat frequency to track
    float phase_offset_deg;     // Phase offset for LED relative to audio (0-360°)
    float tracking_bandwidth;   // Frequency tracking bandwidth
    bool auto_detect;          // Automatically detect binaural beat frequency
} beat_sync_config_t;

// Main synchronization state
typedef struct audio_led_sync_state {
    audio_led_sync_mode_t mode;         // Current synchronization mode
    bool active;                        // Synchronization operational
    uint64_t sample_count;             // Current audio sample number
    float current_amplitude;            // Real-time audio amplitude
    float beat_frequency_detected;      // Detected binaural beat frequency
    vu_meter_config_t vu_config;      // VU meter settings
    beat_sync_config_t beat_config;    // Beat synchronization settings
    uint32_t led_update_counter;       // LED updates processed
    uint64_t sync_errors;              // Synchronization error count
    uint32_t queue_errors;             // Queue operation error count (ISR safety)
    i2s_chan_handle_t i2s_handle;     // I2S channel handle for DMA callbacks
} audio_led_sync_state_t;

// Public API functions
esp_err_t audio_led_sync_init(void);
esp_err_t audio_led_sync_start(audio_led_sync_mode_t mode);
esp_err_t audio_led_sync_stop(void);
esp_err_t audio_led_sync_deinit(void);

// Configuration functions
esp_err_t audio_led_sync_set_vu_meter_config(const vu_meter_config_t *config);
esp_err_t audio_led_sync_set_beat_sync_config(const beat_sync_config_t *config);
esp_err_t audio_led_sync_set_mode(audio_led_sync_mode_t mode);

// Real-time status
bool audio_led_sync_is_active(void);
audio_led_sync_mode_t audio_led_sync_get_mode(void);
float audio_led_sync_get_current_amplitude(void);
float audio_led_sync_get_beat_frequency(void);
uint64_t audio_led_sync_get_sync_errors(void);
uint32_t audio_led_sync_get_queue_errors(void);

// Debug/diagnostic functions
bool audio_led_sync_validate_queue_safety(void);

// I2S callback registration (called from audio_manager.c)
esp_err_t audio_led_sync_register_i2s_callback(i2s_chan_handle_t i2s_handle);
bool audio_led_sync_i2s_callback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx);

#endif // AUDIO_LED_SYNC_H