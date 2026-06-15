#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

/**
 * @brief Simple WiFi Manager
 *
 * Provides basic WiFi connectivity for the web interface
 */

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASSWORD_MAX_LEN 64

typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_ERROR
} wifi_state_t;

/**
 * @brief Initialize WiFi manager
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to WiFi network
 *
 * @param ssid WiFi network name
 * @param password WiFi password
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_connect(const char* ssid, const char* password);

/**
 * @brief Disconnect from WiFi
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Get WiFi connection state
 *
 * @return wifi_state_t Current WiFi state
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief Get IP address as string
 *
 * @param ip_str Buffer to store IP string
 * @param max_len Maximum buffer length
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_get_ip_string(char* ip_str, size_t max_len);

#endif // WIFI_MANAGER_H
