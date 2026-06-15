#ifndef LED_MATRIX_EXAMPLE_H
#define LED_MATRIX_EXAMPLE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize 12x4 LED matrix on GPIO 12
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_init(void);

/**
 * @brief Set LED at matrix coordinates (x,y)
 *
 * @param x Column (0-11)
 * @param y Row (0-3)
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_set_pixel(uint8_t x, uint8_t y, uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Update matrix display
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_refresh(void);

/**
 * @brief Clear entire matrix
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_clear(void);

/**
 * @brief Display VU meter on matrix
 *
 * @param left_level Left channel level (0.0-1.0)
 * @param right_level Right channel level (0.0-1.0)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_vu_meter(float left_level, float right_level);

/**
 * @brief Demo pattern - test all LEDs in sequence
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_test_pattern(void);

/**
 * @brief Start LED flicker for meditation/brainwave entrainment
 *
 * @param frequency Flicker frequency in Hz (0.1-100.0)
 * @param duty_cycle Duty cycle percentage (0-100)
 * @param brightness Maximum brightness (0-100)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_start_flicker(float frequency, uint8_t duty_cycle, uint8_t brightness);

/**
 * @brief Stop LED flicker
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_stop_flicker(void);

/**
 * @brief Update flicker parameters while running
 *
 * @param frequency New flicker frequency in Hz
 * @param duty_cycle New duty cycle percentage (0-100)
 * @param brightness New maximum brightness (0-100)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_update_flicker_params(float frequency, uint8_t duty_cycle, uint8_t brightness);

/**
 * @brief Set flicker color (default: white)
 *
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t led_matrix_set_flicker_color(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Check if LED flicker is currently active
 *
 * @return bool true if flickering, false otherwise
 */
bool led_matrix_is_flickering(void);

/**
 * @brief Get current flicker frequency
 *
 * @return float Current frequency in Hz (0.0 if not flickering)
 */
float led_matrix_get_current_frequency(void);

#ifdef __cplusplus
}
#endif

#endif // LED_MATRIX_EXAMPLE_H
