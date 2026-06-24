#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include "driver/gpio.h"
#include "sdkconfig.h"

/**
 * @brief Audio Configuration
 *
 * Centralized configuration for the ESP32 Audio Player
 */

// I2S Pin Configuration — driven by menuconfig (see Kconfig.projbuild).
// Defaults reproduce the original glasses board layout (26/25/22).
// For boards with on-board codecs (AC101 etc.) override via menuconfig.
#define AUDIO_I2S_BCK_GPIO      ((gpio_num_t)CONFIG_AUDIO_I2S_BCK_GPIO)
#define AUDIO_I2S_WS_GPIO       ((gpio_num_t)CONFIG_AUDIO_I2S_WS_GPIO)
#define AUDIO_I2S_DATA_GPIO     ((gpio_num_t)CONFIG_AUDIO_I2S_DATA_GPIO)
// MCLK / DIN may be -1 (unused); cast preserved for API symmetry. Code that
// uses these pins must check for GPIO_NUM_NC before configuring the pin.
#define AUDIO_I2S_MCLK_GPIO     ((gpio_num_t)CONFIG_AUDIO_I2S_MCLK_GPIO)
#define AUDIO_I2S_DIN_GPIO      ((gpio_num_t)CONFIG_AUDIO_I2S_DIN_GPIO)
#define AUDIO_AMP_ENABLE_GPIO   ((gpio_num_t)CONFIG_AUDIO_AMP_ENABLE_GPIO)

// Audio Settings
#define AUDIO_SAMPLE_RATE       44100        // 44.1kHz
#define AUDIO_BITS_PER_SAMPLE   16           // 16-bit
#define AUDIO_CHANNELS          2            // Stereo

// LED Matrix Configuration — driven by menuconfig (see Kconfig.projbuild).
// For neopixel and dotstar builds the values come from CONFIG_LED_DATA_PIN,
// CONFIG_LED_COUNT, CONFIG_LED_GRID_WIDTH, CONFIG_LED_GRID_HEIGHT.
// For direct mode there is no addressable strip; the fallback constants below
// are valid compile-time values for any code that references these macros as
// array sizes, but they are never used at runtime because led_strip_supports_pixel_addressing()
// returns false and the matrix layer gates all grid-dependent paths accordingly.
#if defined(CONFIG_LED_TYPE_NEOPIXEL) || defined(CONFIG_LED_TYPE_DOTSTAR)
#define LED_STRIP_GPIO          ((gpio_num_t)CONFIG_LED_DATA_PIN)
#define LED_STRIP_COUNT         CONFIG_LED_COUNT
#define LED_MATRIX_WIDTH        CONFIG_LED_GRID_WIDTH
#define LED_MATRIX_HEIGHT       CONFIG_LED_GRID_HEIGHT
#else  // LED_TYPE_DIRECT — no addressable strip
#define LED_STRIP_GPIO          GPIO_NUM_NC
#define LED_STRIP_COUNT         4            // 4 logical channels (one per LEDC output)
#define LED_MATRIX_WIDTH        2            // compile-time fallback; gated at runtime
#define LED_MATRIX_HEIGHT       2            // compile-time fallback; gated at runtime
#endif

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

// SD Card Configuration — driven by menuconfig (CONFIG_BG_SDCARD_*).
// Macros expand to GPIO_NUM_NC (-1) when a pin is unset, so callers should
// check for !=GPIO_NUM_NC before configuring the pin. Only meaningful when
// CONFIG_BG_SDCARD_ENABLED=y; for builds without SD-card support the
// CONFIG_BG_SDCARD_*_GPIO symbols don't exist (gated by Kconfig depends).
#define SDCARD_SPI_HOST         SPI2_HOST
#ifdef CONFIG_BG_SDCARD_ENABLED
#define SDCARD_CS_GPIO          ((gpio_num_t)CONFIG_BG_SDCARD_CS_GPIO)
#define SDCARD_MOSI_GPIO        ((gpio_num_t)CONFIG_BG_SDCARD_MOSI_GPIO)
#define SDCARD_MISO_GPIO        ((gpio_num_t)CONFIG_BG_SDCARD_MISO_GPIO)
#define SDCARD_CLK_GPIO         ((gpio_num_t)CONFIG_BG_SDCARD_CLK_GPIO)
#else
#define SDCARD_CS_GPIO          GPIO_NUM_NC
#define SDCARD_MOSI_GPIO        GPIO_NUM_NC
#define SDCARD_MISO_GPIO        GPIO_NUM_NC
#define SDCARD_CLK_GPIO         GPIO_NUM_NC
#endif

// Network Configuration (when implemented)
#define WIFI_SSID_MAX_LEN       32
#define WIFI_PASS_MAX_LEN       64
#define RTP_DEFAULT_PORT        5004
#define VBAN_DEFAULT_PORT       6980

// Web Interface (when implemented)
#define WEB_SERVER_PORT         80

#endif // AUDIO_CONFIG_H
