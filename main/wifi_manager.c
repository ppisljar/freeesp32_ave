#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "mdns.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_STARTED_BIT   BIT2

// Initial association attempts before we give up and fall back to SoftAP. Once
// associated, reconnects after a drop are unlimited (handled separately).
#define WIFI_MAX_RETRY     8

// Associating is not the same as being on the network: an AP whose uplink is
// down will happily accept us and then never answer DHCP. That state raises no
// event at all, so a bare wait for GOT_IP blocks forever and every init step
// below wifi_manager_connect() -- web server, mDNS, and the SoftAP fallback
// itself -- never runs, leaving the device unreachable except over USB. Bound
// the wait: give each association this long to produce a lease, then drop it
// and try again, and after WIFI_IP_MAX_ROUNDS give up so the caller can fall
// back to SoftAP.
#define WIFI_IP_TIMEOUT_MS   15000
#define WIFI_IP_MAX_ROUNDS   3

static EventGroupHandle_t s_wifi_event_group;
static wifi_state_t current_state = WIFI_STATE_DISCONNECTED;
static esp_netif_ip_info_t ip_info = {0};
static wifi_run_mode_t s_run_mode = WIFI_RUN_MODE_STA;
static esp_netif_t *s_ap_netif = NULL;
static int s_retry_num = 0;
static bool s_ever_connected = false;  // got an IP at least once → keep retrying
static bool s_defer_connect = false;   // hold off auto-connect during the boot scan

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    // In SoftAP fallback mode the STA reconnect logic must stay quiet.
    if (s_run_mode == WIFI_RUN_MODE_AP) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
        // During the boot scan we drive the association by hand afterwards;
        // connecting here would race the scan and make it fail.
        if (!s_defer_connect) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        current_state = WIFI_STATE_DISCONNECTED;
        // Reason code is invaluable for diagnosing flaky links (e.g. reason 8 =
        // ASSOC_LEAVE, 200/206 = 4-way handshake, 39 = DELBA/timeout). Task/event
        // context — ESP_LOG is safe.
        {
            const wifi_event_sta_disconnected_t *d =
                (const wifi_event_sta_disconnected_t *)event_data;
            if (d) {
                ESP_LOGD(TAG, "WIFIDBG disconnect: reason=%d rssi=%d ssid=%.*s",
                         (int)d->reason, (int)d->rssi, (int)d->ssid_len, (const char *)d->ssid);
            }
        }
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
        {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                ESP_LOGD(TAG, "WIFIDBG connected: ssid=%s rssi=%d ch=%d",
                         (const char *)ap.ssid, (int)ap.rssi, (int)ap.primary);
            }
        }
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

/* Log every AP this antenna can hear, including hidden ones.
 *
 * A laptop sitting next to the device is not a reliable second opinion: its
 * scan lists only APs that beacon their SSID, while our STA finds hidden ones
 * too because it probes for the configured SSID by name. When several APs
 * share one SSID and only some of them have a working uplink, that difference
 * is exactly what makes the failure look impossible from the desk. Dump the
 * radio's own view so BSSID, channel and RSSI are on the record. */
static void wifi_scan_dump(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* blocking */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "boot scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK || num == 0) {
        ESP_LOGW(TAG, "boot scan: no APs visible");
        return;
    }
    if (num > 32) num = 32;

    wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
    if (!recs) {
        ESP_LOGW(TAG, "boot scan: out of memory for %u records", (unsigned)num);
        return;
    }
    if (esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        ESP_LOGI(TAG, "boot scan: %u AP(s) visible from this antenna", (unsigned)num);
        for (uint16_t i = 0; i < num; i++) {
            const wifi_ap_record_t *r = &recs[i];
            ESP_LOGI(TAG, "  ch%-3d %4d dBm  " MACSTR "  \"%s\"%s",
                     (int)r->primary, (int)r->rssi, MAC2STR(r->bssid),
                     (const char *)r->ssid,
                     r->ssid[0] == '\0' ? "  <hidden>" : "");
        }
    }
    free(recs);
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

    // The default WIFI_FAST_SCAN associates with the first AP matching the SSID
    // and sweeps channels in ascending order, so on a multi-AP network the
    // low-channel AP always wins regardless of how weak or how broken it is.
    // Sweep every channel and take the strongest instead.
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    s_run_mode = WIFI_RUN_MODE_STA;
    s_retry_num = 0;
    s_ever_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    current_state = WIFI_STATE_CONNECTING;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Scan before associating. esp_wifi_start() makes the driver emit
    // WIFI_EVENT_STA_START, whose handler would normally kick off the
    // association immediately; defer that so the scan has the radio to itself.
    s_defer_connect = true;
    xEventGroupClearBits(s_wifi_event_group, WIFI_STARTED_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());
    // esp_wifi_start() is asynchronous and scanning before the driver reports
    // STA_START fails with ESP_ERR_WIFI_NOT_STARTED.
    xEventGroupWaitBits(s_wifi_event_group, WIFI_STARTED_BIT,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    wifi_scan_dump();
    s_defer_connect = false;
    esp_wifi_connect();

    // Wait for an IP, for outright association failure, or for the DHCP budget
    // to run out. A silent association to an AP with no working uplink lands in
    // the third case; each round drops that association so the driver gets
    // another chance to land somewhere useful.
    EventBits_t bits = 0;
    for (int round = 1; round <= WIFI_IP_MAX_ROUNDS; round++) {
        bits = xEventGroupWaitBits(s_wifi_event_group,
                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(WIFI_IP_TIMEOUT_MS));
        if (bits & (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)) {
            break;
        }

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGW(TAG, "associated to " MACSTR " (ch%d, %d dBm) but no DHCP lease "
                          "after %d ms - dropping it (round %d/%d)",
                     MAC2STR(ap.bssid), (int)ap.primary, (int)ap.rssi,
                     WIFI_IP_TIMEOUT_MS, round, WIFI_IP_MAX_ROUNDS);
        } else {
            ESP_LOGW(TAG, "no IP after %d ms and not associated - retrying (round %d/%d)",
                     WIFI_IP_TIMEOUT_MS, round, WIFI_IP_MAX_ROUNDS);
        }
        // Triggers STA_DISCONNECTED, whose handler re-issues esp_wifi_connect().
        esp_wifi_disconnect();
    }

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
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi (association rejected)");
        current_state = WIFI_STATE_ERROR;
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Gave up: associated but never got a DHCP lease in %d rounds "
                      "- check the AP's uplink. Falling back to SoftAP.",
                 WIFI_IP_MAX_ROUNDS);
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
