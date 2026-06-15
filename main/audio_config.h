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
#define AUDIO_DMA_BUFFER_SIZE   1024         // Size of each DMA buffer

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
