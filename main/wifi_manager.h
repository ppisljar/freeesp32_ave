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

/** Whether the radio is acting as a station (joined a network) or a SoftAP. */
typedef enum {
    WIFI_RUN_MODE_STA = 0,
    WIFI_RUN_MODE_AP
} wifi_run_mode_t;

/* SoftAP fallback parameters (advertised in logs / docs). Clients reach the
 * web UI at http://192.168.4.1 after joining this network. */
#define WIFI_AP_SSID     "ESP32-AVE-Setup"
#define WIFI_AP_PASSWORD "entrain123"   /* WPA2, >= 8 chars */
#define WIFI_AP_IP_STR   "192.168.4.1"

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
 * @brief Start a SoftAP fallback so the web UI stays reachable when STA can't
 *        join (wrong/empty credentials or association timeout). Brings up
 *        SSID WIFI_AP_SSID with WPA2 password WIFI_AP_PASSWORD on channel 1;
 *        clients reach the device at WIFI_AP_IP_STR (192.168.4.1).
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_manager_start_ap(void);

/**
 * @brief Current run mode (STA joined a network, or SoftAP fallback active).
 */
wifi_run_mode_t wifi_manager_get_run_mode(void);

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
