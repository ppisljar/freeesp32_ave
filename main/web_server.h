#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief Web Server Component
 *
 * Provides HTTP server functionality for:
 * - Config file upload (.led files)
 * - Real-time status display
 * - Audio control interface
 * - System configuration
 */

#define WEB_SERVER_MAX_UPLOAD_SIZE  (32 * 1024)  // 32KB max .led file size
// 32KB gives ~6x headroom over the worst-case 100-entry timeline (~5KB raw text).
// Was 64KB; downsized to free DRAM for the BG audio ring buffer (Plan 006).
#define WEB_SERVER_PORT            80

typedef struct {
    httpd_handle_t server;
    char *upload_buffer;
    size_t upload_size;
    bool wifi_connected;
} web_server_state_t;

/**
 * @brief Initialize and start web server
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_init(void);

/**
 * @brief Stop web server
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_stop(void);

/**
 * @brief Check if web server is running
 *
 * @return true if running
 */
bool web_server_is_running(void);

/**
 * @brief Get server URL
 *
 * @param url_buffer Buffer to store URL
 * @param buffer_size Size of buffer
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_get_url(char *url_buffer, size_t buffer_size);

/**
 * @brief Set WiFi connection status
 *
 * @param connected WiFi connection status
 * @return esp_err_t ESP_OK on success
 */
esp_err_t web_server_set_wifi_status(bool connected);

#endif // WEB_SERVER_H
