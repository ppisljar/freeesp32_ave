#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char* TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static wifi_state_t current_state = WIFI_STATE_DISCONNECTED;
static esp_netif_ip_info_t ip_info = {0};

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        current_state = WIFI_STATE_DISCONNECTED;
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WiFi disconnected, retry connecting");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        ip_info = event->ip_info;
        current_state = WIFI_STATE_CONNECTED;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi manager");

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char* ssid, const char* password)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }

    current_state = WIFI_STATE_CONNECTING;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
        // Disable WiFi modem sleep — the default WIFI_PS_MIN_MODEM lets the
        // radio sleep between DTIM beacons (~100ms), adding latency to every
        // TCP segment delivery.  For real-time HTTP audio streaming we must
        // keep the modem awake.  This is what squeezelite-esp32 does.
        // (Cost: higher idle current draw.  Worth it for streaming.)
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(ps_err));
        } else {
            ESP_LOGI(TAG, "WiFi modem sleep disabled (WIFI_PS_NONE)");
        }
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        current_state = WIFI_STATE_ERROR;
        return ESP_FAIL;
    }
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from WiFi");
    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK) {
        current_state = WIFI_STATE_DISCONNECTED;
    }
    return ret;
}

wifi_state_t wifi_manager_get_state(void)
{
    return current_state;
}

esp_err_t wifi_manager_get_ip_string(char* ip_str, size_t max_len)
{
    if (!ip_str || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (current_state != WIFI_STATE_CONNECTED) {
        strncpy(ip_str, "Not connected", max_len - 1);
        ip_str[max_len - 1] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}
