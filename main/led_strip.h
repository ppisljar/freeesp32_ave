#ifndef LED_STRIP_H
#define LED_STRIP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <driver/rmt_tx.h>
#include <driver/gpio.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief LED Strip Control Component
 *
 * Provides control for addressable LED strips (WS2812, SK6812, etc.)
 * using the ESP32 RMT peripheral for precise timing.
 */

typedef enum {
    LED_STRIP_WS2812 = 0,
    LED_STRIP_SK6812 = 1,
    LED_STRIP_APA106 = 2,
    LED_STRIP_MAX_TYPES
} led_strip_type_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color_t;

typedef struct {
    led_strip_type_t type;
    uint32_t length;
    gpio_num_t gpio_pin;

    led_color_t *working_buffer;
    led_color_t *display_buffer;

    SemaphoreHandle_t access_mutex;
    bool initialized;

    rmt_channel_handle_t rmt_tx_channel;  // ESP-IDF 5.x RMT handle
    rmt_encoder_handle_t rmt_encoder;     // RMT copy encoder

    // Pre-allocated RMT symbol buffer to avoid malloc/free on each refresh
    rmt_symbol_word_t *symbol_buffer;
    size_t symbol_buffer_size;
} led_strip_handle_t;

/**
 * @brief LED strip configuration structure
 */
typedef struct {
    led_strip_type_t type;     // LED strip type
    uint32_t length;           // Number of LEDs
    gpio_num_t gpio_pin;       // GPIO pin number (must be < GPIO_NUM_33)
} led_strip_config_t;

/**
 * @brief Initialize LED strip
 *
 * @param config LED strip configuration
 * @param handle Output handle for the LED strip
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_init(const led_strip_config_t *config, led_strip_handle_t **handle);

/**
 * @brief Set pixel color by RGB values
 *
 * @param handle LED strip handle
 * @param pixel_num Pixel number (0-based)
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_set_pixel_rgb(led_strip_handle_t *handle, uint32_t pixel_num,
                                  uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Set pixel color by color structure
 *
 * @param handle LED strip handle
 * @param pixel_num Pixel number (0-based)
 * @param color Color to set
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_set_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                    const led_color_t *color);

/**
 * @brief Get pixel color
 *
 * @param handle LED strip handle
 * @param pixel_num Pixel number (0-based)
 * @param color Output color
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_get_pixel_color(led_strip_handle_t *handle, uint32_t pixel_num,
                                    led_color_t *color);

/**
 * @brief Update LED strip display (commit changes)
 *
 * @param handle LED strip handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_refresh(led_strip_handle_t *handle);

/**
 * @brief Clear all pixels (set to black)
 *
 * @param handle LED strip handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_clear(led_strip_handle_t *handle);

/**
 * @brief Set all pixels to the same color
 *
 * @param handle LED strip handle
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_set_all(led_strip_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Create VU meter display on LED strip
 *
 * @param handle LED strip handle
 * @param level_left Left channel level (0-100)
 * @param level_right Right channel level (0-100)
 * @param brightness Overall brightness (0-100)
 * @param color_mode VU meter color mode
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_vu_meter(led_strip_handle_t *handle, uint8_t level_left,
                             uint8_t level_right, uint8_t brightness, uint8_t color_mode);

/**
 * @brief Create spectrum analyzer display on LED strip
 *
 * @param handle LED strip handle
 * @param spectrum_data Frequency spectrum data
 * @param data_length Length of spectrum data
 * @param brightness Overall brightness (0-100)
 * @param style Spectrum display style
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_spectrum(led_strip_handle_t *handle, uint8_t *spectrum_data,
                            size_t data_length, uint8_t brightness, uint8_t style);

/**
 * @brief Free LED strip handle and resources
 *
 * @param handle LED strip handle to free
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_strip_deinit(led_strip_handle_t *handle);

// Convenience color definitions
#define LED_COLOR_RED     {.red = 255, .green = 0,   .blue = 0}
#define LED_COLOR_GREEN   {.red = 0,   .green = 255, .blue = 0}
#define LED_COLOR_BLUE    {.red = 0,   .green = 0,   .blue = 255}
#define LED_COLOR_WHITE   {.red = 255, .green = 255, .blue = 255}
#define LED_COLOR_YELLOW  {.red = 255, .green = 255, .blue = 0}
#define LED_COLOR_CYAN    {.red = 0,   .green = 255, .blue = 255}
#define LED_COLOR_MAGENTA {.red = 255, .green = 0,   .blue = 255}
#define LED_COLOR_BLACK   {.red = 0,   .green = 0,   .blue = 0}

#ifdef __cplusplus
}
#endif

#endif // LED_STRIP_H
