#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include "driver/gpio.h"

/**
 * @brief Audio Configuration
 *
 * Centralized configuration for the ESP32 Audio Player
 */

// I2S Pin Configuration
#define AUDIO_I2S_BCK_GPIO      GPIO_NUM_26  // Bit Clock
#define AUDIO_I2S_WS_GPIO       GPIO_NUM_25  // Word Select (LRC)
#define AUDIO_I2S_DATA_GPIO     GPIO_NUM_22  // Data Out

// Audio Settings
#define AUDIO_SAMPLE_RATE       44100        // 44.1kHz
#define AUDIO_BITS_PER_SAMPLE   16           // 16-bit
#define AUDIO_CHANNELS          2            // Stereo

// LED Matrix Configuration
#define LED_STRIP_GPIO          GPIO_NUM_12  // LED data pin
#define LED_STRIP_COUNT         48           // 12x4 matrix = 48 LEDs
#define LED_MATRIX_WIDTH        12           // 12 columns
#define LED_MATRIX_HEIGHT       4            // 4 rows

// Buffer Settings
#define AUDIO_DMA_BUFFER_COUNT  8            // Number of DMA buffers
#define AUDIO_DMA_BUFFER_SIZE   1024         // Size of each DMA buffer in bytes (stereo 16-bit)

/**
 * DMA frames per buffer: bytes / (channels * bytes_per_sample).
 * 1024 / (2 channels × 2 bytes) = 256 frames per buffer.
 */
#define AUDIO_DMA_FRAMES_PER_BUF \
    ((uint32_t)(AUDIO_DMA_BUFFER_SIZE / (AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8))))

/**
 * Total I2S DMA pipeline depth in sample frames.
 *
 * Samples written to the I2S DMA ring buffer take this many sample frames to
 * traverse the pipeline before emerging from the DAC.  Used by:
 *   - Audio phase pre-advance (audio_generator.c): start each channel's Q32
 *     phase so that phase=0 coincides with the first sample's DAC emission.
 *
 * At 8 × 256 frames: 2048 total pipeline frames.
 */
#define AUDIO_DMA_PIPELINE_SAMPLES \
    ((uint32_t)(AUDIO_DMA_FRAMES_PER_BUF * AUDIO_DMA_BUFFER_COUNT))

/**
 * Total I2S DMA pipeline depth in microseconds.
 *
 * Samples written to the I2S DMA ring buffer take this long to traverse the
 * pipeline before emerging from the DAC.  Used by:
 *   - LED dispatch (config_parser.c): offset cycle_hint_us so LED transitions
 *     align with the DAC's actual emission time, not the dispatcher's write time.
 *   - Audio phase pre-advance (audio_generator.c): start each channel's Q32
 *     phase so that phase=0 lands at the moment the first sample emerges from DAC.
 *
 * Formula: (pipeline_frames × 1_000_000) / sample_rate.
 * At 8 × 256 frames @ 44100 Hz: (2048 × 1_000_000) / 44100 = 46_439 µs ≈ 46 ms.
 * Reference: bug_led_audio_proof_of_sync_2026-06-17.md (Inv 14).
 */
#define AUDIO_DMA_PIPELINE_LAG_US \
    ((uint64_t)(AUDIO_DMA_PIPELINE_SAMPLES) * 1000000ULL / (uint64_t)(AUDIO_SAMPLE_RATE))

// Volume Settings
#define AUDIO_DEFAULT_VOLUME    0.5f         // Default volume (50%)
#define AUDIO_MAX_VOLUME        1.0f         // Maximum volume
#define AUDIO_MIN_VOLUME        0.0f         // Minimum volume (mute)

// SD Card Configuration (when implemented)
#define SDCARD_SPI_HOST         SPI2_HOST
#define SDCARD_CS_GPIO          GPIO_NUM_5
#define SDCARD_MOSI_GPIO        GPIO_NUM_23
#define SDCARD_MISO_GPIO        GPIO_NUM_19
#define SDCARD_CLK_GPIO         GPIO_NUM_18

// Network Configuration (when implemented)
#define WIFI_SSID_MAX_LEN       32
#define WIFI_PASS_MAX_LEN       64
#define RTP_DEFAULT_PORT        5004
#define VBAN_DEFAULT_PORT       6980

// Web Interface (when implemented)
#define WEB_SERVER_PORT         80

#endif // AUDIO_CONFIG_H
