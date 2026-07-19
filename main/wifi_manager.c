#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char* TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Initial association attempts before we give up and fall back to SoftAP. Once
// associated, reconnects after a drop are unlimited (handled separately).
#define WIFI_MAX_RETRY     8

static EventGroupHandle_t s_wifi_event_group;
static wifi_state_t current_state = WIFI_STATE_DISCONNECTED;
static esp_netif_ip_info_t ip_info = {0};
static wifi_run_mode_t s_run_mode = WIFI_RUN_MODE_STA;
static esp_netif_t *s_ap_netif = NULL;
static int s_retry_num = 0;
static bool s_ever_connected = false;  // got an IP at least once → keep retrying

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    // In SoftAP fallback mode the STA reconnect logic must stay quiet.
    if (s_run_mode == WIFI_RUN_MODE_AP) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        current_state = WIFI_STATE_DISCONNECTED;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_ever_connected || s_retry_num < WIFI_MAX_RETRY) {
            // Either a post-association drop (retry forever) or still within the
            // initial attempt budget.
            s_retry_num++;
            esp_wifi_connect();
            ESP_LOGI(TAG, "WiFi disconnected, retry connecting (%d)", s_retry_num);
        } else {
            // Initial association failed within budget → signal failure so the
            // caller can bring up the SoftAP fallback.
            ESP_LOGW(TAG, "WiFi association failed after %d attempts", s_retry_num);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        ip_info = event->ip_info;
        current_state = WIFI_STATE_CONNECTED;
        s_ever_connected = true;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        /* Re-apply WIFI_PS_NONE on every (re)connect. ESP-IDF restores
         * WIFI_PS_MIN_MODEM on silent reconnect (WIFI_EVENT_STA_DISCONNECTED
         * → esp_wifi_connect()), and MIN_MODEM adds ~100 ms latency per
         * TCP segment which degrades streaming. The one-shot call in
         * wifi_manager_connect() only fires for the *initial* connect;
         * this handler covers every subsequent reconnect. */
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) on got-IP failed: %s",
                     esp_err_to_name(ps_err));
        }
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
    if (!ssid || ssid[0] == '\0') {
        // No network configured — caller should fall back to SoftAP.
        ESP_LOGW(TAG, "No WiFi SSID configured");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }

    s_run_mode = WIFI_RUN_MODE_STA;
    s_retry_num = 0;
    s_ever_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
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

esp_err_t wifi_manager_start_ap(void)
{
    ESP_LOGW(TAG, "Starting SoftAP fallback (SSID: %s, IP: %s)",
             WIFI_AP_SSID, WIFI_AP_IP_STR);

    // Switch out of STA mode; the disconnect handler is gated on s_run_mode so
    // it won't fight us once this flag flips.
    s_run_mode = WIFI_RUN_MODE_AP;

    esp_wifi_stop();

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap_config = {0};
    strncpy((char*)ap_config.ap.ssid, WIFI_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    strncpy((char*)ap_config.ap.password, WIFI_AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = (strlen(WIFI_AP_PASSWORD) >= 8) ? WIFI_AUTH_WPA2_PSK
                                                            : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
        current_state = WIFI_STATE_ERROR;
        return err;
    }

    current_state = WIFI_STATE_CONNECTED;  // AP is up and reachable
    ESP_LOGI(TAG, "SoftAP up — join '%s' (pw '%s') and browse to http://%s",
             WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_IP_STR);
    return ESP_OK;
}

wifi_run_mode_t wifi_manager_get_run_mode(void)
{
    return s_run_mode;
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

    if (s_run_mode == WIFI_RUN_MODE_AP) {
        strncpy(ip_str, WIFI_AP_IP_STR, max_len - 1);
        ip_str[max_len - 1] = '\0';
        return ESP_OK;
    }

    if (current_state != WIFI_STATE_CONNECTED) {
        strncpy(ip_str, "Not connected", max_len - 1);
        ip_str[max_len - 1] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_manager_start_mdns(void)
{
    // Hostname comes from runtime settings (sanitized to a valid DNS label on
    // save); fall back to the compile-time default if unset/empty.
    const device_settings_t *cfg = settings_get();
    const char *host = (cfg && cfg->mdns_hostname[0]) ? cfg->mdns_hostname
                                                      : WIFI_MDNS_HOSTNAME;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    if ((err = mdns_hostname_set(host)) != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set('%s') failed: %s", host, esp_err_to_name(err));
        return err;
    }
    mdns_instance_name_set(WIFI_MDNS_INSTANCE);
    // Advertise the web UI so browsers/discovery tools can find it.
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS started — reachable at http://%s.local", host);
    return ESP_OK;
}
