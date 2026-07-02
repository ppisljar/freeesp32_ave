#include "sdkconfig.h"           // for CONFIG_GENERATOR_SERVER_URL
#include "web_server.h"
#include "config_parser.h"
#include "audio_manager.h"
#include "audio_generator.h"     // for NUM_AUDIO_CHANNELS, audio_generator_lock
#include "mod_engine.h"          // POST /api/mod — per-field modulation
#include "cJSON.h"               // POST /api/mod body parsing
#include "led_matrix_example.h"
#include "bg_player.h"
#include "settings.h"            // runtime device settings (GET/POST /api/settings)
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"          // esp_restart (POST /api/reboot)
#include "esp_ota_ops.h"         // OTA boot-slot handoff (POST /api/ota)
#include "esp_app_desc.h"        // esp_app_get_description (GET /api/version)
#include "nvs.h"                 // WiFi cred handoff to the updater (namespace "ota")
#include "esp_spiffs.h"          // web UI assets served from SPIFFS
#include <string.h>
#include <strings.h>           // strcasecmp
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>            // opendir/readdir for /api/configs listing
#include <sys/stat.h>

// Defined in esp32_audioplayer.c — snapshot of the GPIO 5 button press log.
// Returns up to `max` absolute esp_timer_get_time() values, chronological.
extern size_t snapshot_button_get_presses(uint64_t *out, size_t max);

static const char* TAG = "web_server";

// Global web server state
static web_server_state_t g_server_state = {0};

// Set true once the "storage" SPIFFS partition (web UI assets) is mounted.
static bool s_spiffs_ok = false;
#define WEB_SPIFFS_BASE "/spiffs"

// Dedicated SPIFFS partition for user .ledc configs (see partitions.csv).
static bool s_cfgfs_ok = false;
#define WEB_CFG_BASE "/configs"
#define WEB_CFG_MAX_SIZE (32 * 1024)   // max .ledc file size

// HTML pages
// Minimal fallback page, served only if the SPIFFS web image failed to
// mount. The real UI is built from ../web (esbuild → gzipped assets), flashed
// to the "storage" SPIFFS partition and served by static_file_handler below.
static const char* fallback_html =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>ESP32 Audio Player</title></head>"
    "<body style=\"font-family:Arial;margin:40px\"><h1>Web UI not available</h1>"
    "<p>The SPIFFS <code>storage</code> partition could not be mounted, so the UI "
    "asset <code>/spiffs/index.html</code> is unavailable.</p>"
    "<p>Re-flash including the SPIFFS image with <code>idf.py flash</code> "
    "(the image is built from the <code>web/</code> directory).</p>"
    "<p>The JSON control API under <code>/api/*</code> is still functional.</p>"
    "</body></html>";

// HTTP Handler functions
static esp_err_t static_file_handler(httpd_req_t *req);
static esp_err_t appconfig_handler(httpd_req_t *req);
static esp_err_t mod_handler(httpd_req_t *req);
static esp_err_t stop_handler(httpd_req_t *req);
static esp_err_t example_handler(httpd_req_t *req);
static esp_err_t play_config_handler(httpd_req_t *req);
static esp_err_t patch_config_handler(httpd_req_t *req);
static esp_err_t state_handler(httpd_req_t *req);
static esp_err_t report_handler(httpd_req_t *req);
static esp_err_t settings_get_handler(httpd_req_t *req);
static esp_err_t settings_post_handler(httpd_req_t *req);
static esp_err_t settings_reset_handler(httpd_req_t *req);
static esp_err_t reboot_handler(httpd_req_t *req);
static esp_err_t ota_handler(httpd_req_t *req);
static esp_err_t version_handler(httpd_req_t *req);
static esp_err_t configs_list_handler(httpd_req_t *req);
static esp_err_t configs_get_handler(httpd_req_t *req);
static esp_err_t configs_put_handler(httpd_req_t *req);
static esp_err_t configs_delete_handler(httpd_req_t *req);
static esp_err_t reports_list_handler(httpd_req_t *req);
static esp_err_t reports_get_handler(httpd_req_t *req);
static esp_err_t reports_put_handler(httpd_req_t *req);
static esp_err_t reports_delete_handler(httpd_req_t *req);

esp_err_t web_server_init(void)
{
    ESP_LOGI(TAG, "Initializing web server");

    // Mount the SPIFFS "storage" partition that holds the web UI assets.
    // Non-fatal: if it fails, static_file_handler serves a fallback page and
    // the /api/* control endpoints still work.
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = WEB_SPIFFS_BASE,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t sret = esp_vfs_spiffs_register(&spiffs_conf);
    if (sret == ESP_OK) {
        size_t total = 0, used = 0;
        if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
        }
        s_spiffs_ok = true;
    } else {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s) — serving fallback page", esp_err_to_name(sret));
        s_spiffs_ok = false;
    }

    // Mount the dedicated "cfgfs" SPIFFS partition for user .ledc configs at
    // /configs. A full wired flash (flash_all.sh / idf.py flash) seeds this with
    // the built-in session library image (main/CMakeLists.txt
    // spiffs_create_partition_image(cfgfs ...)). On a unit that has never been
    // seeded — or if the image is ever absent — format_if_mount_failed=true
    // formats it empty on first boot. It is separate from "storage" so
    // reflashing the web UI / app (or an OTA app update) does not erase saved
    // configs. Configs are also pushable over the network via PUT /api/configs/*
    // (additive — preserves existing files), so the library can be delivered
    // without a wired flash.
    esp_vfs_spiffs_conf_t cfg_conf = {
        .base_path = WEB_CFG_BASE,
        .partition_label = "cfgfs",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t cret = esp_vfs_spiffs_register(&cfg_conf);
    if (cret == ESP_OK) {
        size_t total = 0, used = 0;
        if (esp_spiffs_info("cfgfs", &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "cfgfs mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
        }
        s_cfgfs_ok = true;
    } else {
        ESP_LOGE(TAG, "cfgfs mount failed (%s) — device config storage disabled", esp_err_to_name(cret));
        s_cfgfs_ok = false;
    }

    // Allocate upload buffer
    g_server_state.upload_buffer = malloc(WEB_SERVER_MAX_UPLOAD_SIZE);
    if (!g_server_state.upload_buffer) {
        ESP_LOGE(TAG, "Failed to allocate upload buffer");
        return ESP_ERR_NO_MEM;
    }

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = 24;
    // Enable wildcard matching so "/*" can serve arbitrary static assets.
    // Exact /api/... handlers are registered first and keep priority.
    config.uri_match_fn = httpd_uri_match_wildcard;

    // Start HTTP server
    esp_err_t ret = httpd_start(&g_server_state.server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        free(g_server_state.upload_buffer);
        return ret;
    }

    // Register URI handlers. Exact /api/... routes are registered first; the
    // wildcard static-asset handler ("/*") is registered LAST (below) so the
    // server matches API routes before falling through to file serving.
    httpd_uri_t stop_uri = {
        .uri = "/api/stop",
        .method = HTTP_POST,
        .handler = stop_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &stop_uri);

    httpd_uri_t example_uri = {
        .uri = "/api/example",
        .method = HTTP_GET,
        .handler = example_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &example_uri);

    httpd_uri_t play_config_uri = {
        .uri = "/api/play-config",
        .method = HTTP_POST,
        .handler = play_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &play_config_uri);

    httpd_uri_t patch_config_uri = {
        .uri = "/api/patch-config",
        .method = HTTP_POST,
        .handler = patch_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &patch_config_uri);

    httpd_uri_t state_uri = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = state_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &state_uri);

    httpd_uri_t report_uri = {
        .uri = "/api/report",
        .method = HTTP_GET,
        .handler = report_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &report_uri);

    httpd_uri_t appconfig_uri = {
        .uri = "/api/appconfig",
        .method = HTTP_GET,
        .handler = appconfig_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &appconfig_uri);

    httpd_uri_t mod_uri = {
        .uri = "/api/mod", .method = HTTP_POST, .handler = mod_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &mod_uri);

    httpd_uri_t settings_get_uri = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = settings_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &settings_get_uri);

    httpd_uri_t settings_post_uri = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = settings_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &settings_post_uri);

    httpd_uri_t settings_reset_uri = {
        .uri = "/api/settings/reset",
        .method = HTTP_POST,
        .handler = settings_reset_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &settings_reset_uri);

    httpd_uri_t reboot_uri = {
        .uri = "/api/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &reboot_uri);

    httpd_uri_t ota_uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &ota_uri);

    httpd_uri_t version_uri = {
        .uri = "/api/version",
        .method = HTTP_GET,
        .handler = version_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &version_uri);

    // Device config storage. Exact /api/configs (list) first, then the
    // /api/configs/* per-file routes (GET/PUT/DELETE), all before the wildcard.
    httpd_uri_t configs_list_uri = {
        .uri = "/api/configs", .method = HTTP_GET, .handler = configs_list_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &configs_list_uri);
    httpd_uri_t configs_get_uri = {
        .uri = "/api/configs/*", .method = HTTP_GET, .handler = configs_get_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &configs_get_uri);
    httpd_uri_t configs_put_uri = {
        .uri = "/api/configs/*", .method = HTTP_PUT, .handler = configs_put_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &configs_put_uri);
    httpd_uri_t configs_delete_uri = {
        .uri = "/api/configs/*", .method = HTTP_DELETE, .handler = configs_delete_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &configs_delete_uri);

    // Device report storage (same cfgfs partition, ".rpt" files).
    httpd_uri_t reports_list_uri = {
        .uri = "/api/reports", .method = HTTP_GET, .handler = reports_list_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &reports_list_uri);
    httpd_uri_t reports_get_uri = {
        .uri = "/api/reports/*", .method = HTTP_GET, .handler = reports_get_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &reports_get_uri);
    httpd_uri_t reports_put_uri = {
        .uri = "/api/reports/*", .method = HTTP_PUT, .handler = reports_put_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &reports_put_uri);
    httpd_uri_t reports_delete_uri = {
        .uri = "/api/reports/*", .method = HTTP_DELETE, .handler = reports_delete_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &reports_delete_uri);

    // Wildcard static-asset handler — MUST be registered last so the exact
    // /api/... routes above take precedence over file serving.
    httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &static_uri);

    ESP_LOGI(TAG, "Web server started on port %d", WEB_SERVER_PORT);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (g_server_state.server) {
        ESP_LOGI(TAG, "Stopping web server");
        httpd_stop(g_server_state.server);
        g_server_state.server = NULL;
    }

    if (g_server_state.upload_buffer) {
        free(g_server_state.upload_buffer);
        g_server_state.upload_buffer = NULL;
    }

    return ESP_OK;
}

bool web_server_is_running(void)
{
    return g_server_state.server != NULL;
}

esp_err_t web_server_get_url(char *url_buffer, size_t buffer_size)
{
    if (!url_buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_server_state.wifi_connected) {
        snprintf(url_buffer, buffer_size, "WiFi not connected");
        return ESP_ERR_INVALID_STATE;
    }

    // Get IP address
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        snprintf(url_buffer, buffer_size, "No WiFi interface");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        snprintf(url_buffer, buffer_size, "Failed to get IP");
        return ret;
    }

    snprintf(url_buffer, buffer_size, "http://" IPSTR ":%d",
             IP2STR(&ip_info.ip), WEB_SERVER_PORT);

    return ESP_OK;
}

esp_err_t web_server_set_wifi_status(bool connected)
{
    g_server_state.wifi_connected = connected;
    return ESP_OK;
}

// HTTP Handler implementations

// Map a file's extension to a Content-Type. A trailing ".gz" is ignored so
// that "app.js.gz" is typed as application/javascript (Content-Encoding is set
// separately). Defaults to text/plain.
static const char *content_type_for(const char *path)
{
    // Work on a copy with any trailing ".gz" stripped.
    char name[64];
    size_t len = strlen(path);
    if (len > 3 && !strcasecmp(path + len - 3, ".gz")) len -= 3;
    if (len >= sizeof(name)) len = sizeof(name) - 1;
    memcpy(name, path, len);
    name[len] = '\0';

    const char *dot = strrchr(name, '.');
    if (!dot) return "text/plain";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html";
    if (!strcasecmp(dot, ".js"))    return "application/javascript";
    if (!strcasecmp(dot, ".css"))   return "text/css";
    if (!strcasecmp(dot, ".json"))  return "application/json";
    if (!strcasecmp(dot, ".png"))   return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".svg"))   return "image/svg+xml";
    if (!strcasecmp(dot, ".ico"))   return "image/x-icon";
    if (!strcasecmp(dot, ".woff2")) return "font/woff2";
    return "text/plain";
}

// Generic static-asset handler: serves files from the SPIFFS "storage"
// partition mounted at /spiffs. "/" maps to /spiffs/index.html. Registered as
// the wildcard "/*" route LAST, so the exact /api/... handlers (matched first)
// keep priority. If SPIFFS isn't mounted, serves the fallback page.
//
// Assets are stored gzipped (e.g. index.html.gz). The handler tries the exact
// path first, then the ".gz" variant; when it serves a ".gz" file it sets
// Content-Encoding: gzip and types it from the pre-".gz" extension. This lets
// the static sources reference plain names (/app.js) while only .gz ships.
static esp_err_t static_file_handler(httpd_req_t *req)
{
    if (!s_spiffs_ok) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, fallback_html, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Copy the path, stopping at any query string.
    char uri[256];
    size_t ulen = 0;
    for (const char *p = req->uri; *p && *p != '?' && ulen < sizeof(uri) - 1; ++p)
        uri[ulen++] = *p;
    uri[ulen] = '\0';

    // Reject path traversal outright.
    if (strstr(uri, "..")) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    const char *rel = (strcmp(uri, "/") == 0) ? "/index.html" : uri;
    char path[320];
    int wrote = snprintf(path, sizeof(path), WEB_SPIFFS_BASE "%s", rel);
    if (wrote <= 0 || wrote >= (int)sizeof(path)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    // Try the exact path, then the gzipped variant.
    bool is_gz = false;
    FILE *f = fopen(path, "r");
    if (!f) {
        char gzpath[324];
        snprintf(gzpath, sizeof(gzpath), "%s.gz", path);
        f = fopen(gzpath, "r");
        if (f) is_gz = true;
    } else {
        // Direct hit; flag gzip if the stored file is itself compressed.
        size_t plen = strlen(path);
        if (plen > 3 && !strcasecmp(path + plen - 3, ".gz")) is_gz = true;
    }
    if (!f) {
        ESP_LOGW(TAG, "static asset not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_for(path));
    if (is_gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char chunk[1024];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, r) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;  // client gone; connection will be closed
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // end of chunked response
    return ESP_OK;
}

// GET /api/appconfig — device-specific runtime config for the web UI. Keeps
// the static assets device-independent (they fetch this at boot instead of
// having values baked in). Currently just the generator base URL.
static esp_err_t appconfig_handler(httpd_req_t *req)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"generator_url\":\"%s\"}", settings_get()->generator_url);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (n > 0 && n < (int)sizeof(buf)) ? n : HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/mod — set or clear a periodic modulation on one audio/LED field.
// Body JSON: {domain:"audio"|"led", ch:N, field:"...", wave:"none"|"triangle"|
//   "sine"|"sawup"|"sawdown"|"square", from:x, to:y, period_ms:p}
//   - audio ch is the generator channel (A1 -> 1 .. A8 -> 8); led ch is 0-based
//     (a single-channel mask 1<<ch is used).
//   - wave "none" stops modulation on that field; from/to/period are in UI units
//     (Hz, %, -100..100), matching the sliders.
static int mod_wave_enum(const char *w)
{
    if (!w) return -1;
    if (!strcmp(w, "triangle")) return MOD_WAVE_TRIANGLE;
    if (!strcmp(w, "sine"))     return MOD_WAVE_SINE;
    if (!strcmp(w, "sawup"))    return MOD_WAVE_SAW_UP;
    if (!strcmp(w, "sawdown"))  return MOD_WAVE_SAW_DOWN;
    if (!strcmp(w, "square"))   return MOD_WAVE_SQUARE;
    return -1;   // "none" or unknown → stop
}
static int mod_audio_field_enum(const char *f)
{
    if (!f) return -1;
    if (!strcmp(f, "freq")) return MOD_AUDIO_FREQ;
    if (!strcmp(f, "pan"))  return MOD_AUDIO_PAN;
    if (!strcmp(f, "vol"))  return MOD_AUDIO_VOLUME;
    if (!strcmp(f, "mod"))  return MOD_AUDIO_MOD;
    return -1;
}
static int mod_led_field_enum(const char *f)
{
    if (!f) return -1;
    if (!strcmp(f, "freq"))   return MOD_LED_FREQ;
    if (!strcmp(f, "duty"))   return MOD_LED_DUTY;
    if (!strcmp(f, "bright")) return MOD_LED_BRIGHT;
    if (!strcmp(f, "r"))      return MOD_LED_R;
    if (!strcmp(f, "g"))      return MOD_LED_G;
    if (!strcmp(f, "b"))      return MOD_LED_B;
    return -1;
}

static esp_err_t mod_handler(httpd_req_t *req)
{
    char body[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL;
    }
    int rec = 0;
    while (rec < total) {
        int r = httpd_req_recv(req, body + rec, total - rec);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_FAIL; }
        rec += r;
    }
    body[rec] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }

    const cJSON *jdom = cJSON_GetObjectItemCaseSensitive(root, "domain");
    const cJSON *jch  = cJSON_GetObjectItemCaseSensitive(root, "ch");
    const cJSON *jfld = cJSON_GetObjectItemCaseSensitive(root, "field");
    const cJSON *jwav = cJSON_GetObjectItemCaseSensitive(root, "wave");
    const cJSON *jfr  = cJSON_GetObjectItemCaseSensitive(root, "from");
    const cJSON *jto  = cJSON_GetObjectItemCaseSensitive(root, "to");
    const cJSON *jper = cJSON_GetObjectItemCaseSensitive(root, "period_ms");

    bool is_audio = cJSON_IsString(jdom) && !strcmp(jdom->valuestring, "audio");
    bool is_led   = cJSON_IsString(jdom) && !strcmp(jdom->valuestring, "led");
    int  ch    = cJSON_IsNumber(jch) ? (int)jch->valuedouble : -1;
    int  wave  = (cJSON_IsString(jwav)) ? mod_wave_enum(jwav->valuestring) : -1;
    float from = cJSON_IsNumber(jfr) ? (float)jfr->valuedouble : 0.0f;
    float to   = cJSON_IsNumber(jto) ? (float)jto->valuedouble : 0.0f;
    uint32_t period = cJSON_IsNumber(jper) ? (uint32_t)jper->valuedouble : 1000u;
    if (period == 0) period = 1000u;

    esp_err_t r = ESP_ERR_INVALID_ARG;
    audio_generator_lock();
    if (is_audio && ch >= 0 && ch < NUM_AUDIO_CHANNELS) {
        int f = mod_audio_field_enum(cJSON_IsString(jfld) ? jfld->valuestring : NULL);
        if (f >= 0) {
            r = (wave < 0) ? mod_engine_stop_audio(ch, (mod_audio_field_t)f)
                           : mod_engine_start_audio(ch, (mod_audio_field_t)f, (mod_wave_t)wave, from, to, period);
        }
    } else if (is_led && ch >= 0 && ch < NUM_LED_CHANNELS) {
        int f = mod_led_field_enum(cJSON_IsString(jfld) ? jfld->valuestring : NULL);
        if (f >= 0) {
            uint8_t mask = (uint8_t)(1u << ch);
            r = (wave < 0) ? mod_engine_stop_led(mask, (mod_led_field_t)f)
                           : mod_engine_start_led(mask, (mod_led_field_t)f, (mod_wave_t)wave, from, to, period);
        }
    }
    audio_generator_unlock();
    cJSON_Delete(root);

    if (r != ESP_OK) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad args"); return ESP_FAIL; }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

// ---- Device config storage (/api/configs, cfgfs SPIFFS at /configs) --------
// Stores user .ledc files on the dedicated "cfgfs" partition so they survive
// web/app reflashes. GET /api/configs lists names; GET/PUT/DELETE
// /api/configs/<name> read/write/remove a single file.

// Validate + extract a bare config filename from the URI after `prefix`.
// Rejects empty names, path separators, "..", and disallowed characters.
// Returns true and fills out (NUL-terminated) on success.
static bool cfg_name_from_uri(httpd_req_t *req, const char *prefix, char *out, size_t cap)
{
    const char *uri = req->uri;
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return false;
    const char *name = uri + plen;
    // stop at any query string
    size_t len = 0;
    while (name[len] && name[len] != '?') len++;
    if (len == 0 || len >= cap) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    if (strstr(name, "..")) return false;
    memcpy(out, name, len);
    out[len] = '\0';
    return true;
}

// Does `name` end with `suffix` (case-insensitive)?
static bool has_suffix_ci(const char *name, const char *suffix)
{
    size_t nl = strlen(name), sl = strlen(suffix);
    return nl >= sl && !strcasecmp(name + nl - sl, suffix);
}

// Shared store on the cfgfs partition (/configs). Configs and reports share the
// partition, separated by extension: ".ledc" vs ".rpt".

// List files in /configs whose name ends with `suffix`: {"files":["a.ledc",...]}
static esp_err_t store_list(httpd_req_t *req, const char *suffix)
{
    httpd_resp_set_type(req, "application/json");
    if (!s_cfgfs_ok) { httpd_resp_sendstr(req, "{\"files\":[]}"); return ESP_OK; }

    httpd_resp_sendstr_chunk(req, "{\"files\":[");
    DIR *d = opendir(WEB_CFG_BASE);
    if (d) {
        struct dirent *e;
        bool first = true;
        char item[160];
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '\0' || e->d_name[0] == '.') continue;
            if (!has_suffix_ci(e->d_name, suffix)) continue;
            int n = snprintf(item, sizeof(item), "%s\"%s\"", first ? "" : ",", e->d_name);
            if (n > 0 && n < (int)sizeof(item)) httpd_resp_sendstr_chunk(req, item);
            first = false;
        }
        closedir(d);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// GET /api/<store>/<name> — return the file's text.
static esp_err_t store_get(httpd_req_t *req, const char *prefix)
{
    char name[128];
    if (!s_cfgfs_ok) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable"); return ESP_FAIL; }
    if (!cfg_name_from_uri(req, prefix, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_FAIL;
    }
    char path[160];
    snprintf(path, sizeof(path), WEB_CFG_BASE "/%s", name);
    FILE *f = fopen(path, "r");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_FAIL; }
    httpd_resp_set_type(req, "text/plain");
    char chunk[512];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// PUT /api/<store>/<name> — write the request body to the file.
static esp_err_t store_put(httpd_req_t *req, const char *prefix)
{
    char name[128];
    if (!s_cfgfs_ok) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable"); return ESP_FAIL; }
    if (!cfg_name_from_uri(req, prefix, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_FAIL;
    }
    int remaining = req->content_len;
    if (remaining <= 0 || remaining > WEB_CFG_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty or too large"); return ESP_FAIL;
    }
    char path[160];
    snprintf(path, sizeof(path), WEB_CFG_BASE "/%s", name);
    FILE *f = fopen(path, "w");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); return ESP_FAIL; }
    char buf[512];
    int received = 0;
    while (received < remaining) {
        int chunk = remaining - received;
        if (chunk > (int)sizeof(buf)) chunk = sizeof(buf);
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            fclose(f); remove(path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed"); return ESP_FAIL;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            fclose(f); remove(path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed (disk full?)"); return ESP_FAIL;
        }
        received += r;
    }
    fclose(f);
    httpd_resp_set_type(req, "application/json");
    char out[160];
    int n = snprintf(out, sizeof(out), "{\"saved\":\"%s\",\"bytes\":%d}", name, received);
    httpd_resp_send(req, out, (n > 0 && n < (int)sizeof(out)) ? n : HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// DELETE /api/<store>/<name> — remove the file.
static esp_err_t store_delete(httpd_req_t *req, const char *prefix)
{
    char name[128];
    if (!s_cfgfs_ok) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable"); return ESP_FAIL; }
    if (!cfg_name_from_uri(req, prefix, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_FAIL;
    }
    char path[160];
    snprintf(path, sizeof(path), WEB_CFG_BASE "/%s", name);
    if (remove(path) != 0) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_FAIL; }
    httpd_resp_sendstr(req, "deleted");
    return ESP_OK;
}

// Configs (.ledc) and reports (.rpt) are thin wrappers over the shared store.
static esp_err_t configs_list_handler(httpd_req_t *req)   { return store_list(req, ".ledc"); }
static esp_err_t configs_get_handler(httpd_req_t *req)    { return store_get(req, "/api/configs/"); }
static esp_err_t configs_put_handler(httpd_req_t *req)    { return store_put(req, "/api/configs/"); }
static esp_err_t configs_delete_handler(httpd_req_t *req) { return store_delete(req, "/api/configs/"); }
static esp_err_t reports_list_handler(httpd_req_t *req)   { return store_list(req, ".rpt"); }
static esp_err_t reports_get_handler(httpd_req_t *req)    { return store_get(req, "/api/reports/"); }
static esp_err_t reports_put_handler(httpd_req_t *req)    { return store_put(req, "/api/reports/"); }
static esp_err_t reports_delete_handler(httpd_req_t *req) { return store_delete(req, "/api/reports/"); }

// GET /api/settings — serialize the runtime device settings as JSON.
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char buf[1024];
    int n = settings_to_json(buf, sizeof(buf));
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "settings serialize failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// POST /api/settings — apply a (possibly partial) JSON settings update and
// persist to NVS. Hardware settings take effect on the next reboot.
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body length");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[total] = '\0';

    esp_err_t err = settings_apply_json(body, total);
    free(body);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "settings apply failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int n = settings_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (n < 0) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send(req, buf, n);
    }
    return ESP_OK;
}

// POST /api/settings/reset — erase the settings namespace, reseed from the
// compile-time CONFIG_* defaults, and return the fresh settings JSON.
static esp_err_t settings_reset_handler(httpd_req_t *req)
{
    esp_err_t err = settings_reset_defaults();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reset failed");
        return ESP_FAIL;
    }
    char buf[1024];
    int n = settings_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (n < 0) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send(req, buf, n);
    }
    return ESP_OK;
}

// Deferred-restart callback: gives the HTTP response time to flush before the
// chip resets. Scheduled by reboot_handler via a one-shot esp_timer.
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Rebooting now (requested via /api/reboot)");
    esp_restart();
}

// POST /api/reboot — acknowledge, then restart after a short delay so the
// response reaches the browser before the connection drops.
static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    static esp_timer_handle_t reboot_timer = NULL;
    if (!reboot_timer) {
        const esp_timer_create_args_t targs = {
            .callback = reboot_timer_cb,
            .name = "reboot",
        };
        if (esp_timer_create(&targs, &reboot_timer) != ESP_OK) {
            // Fall back to an immediate restart if the timer can't be made.
            esp_restart();
        }
    }
    esp_timer_start_once(reboot_timer, 500000 /* 500 ms */);
    return ESP_OK;
}

// POST /api/ota — hand the current WiFi creds to the ota_1 updater (NVS
// namespace "ota", keys "ssid"/"pass"), point the boot slot at the updater,
// acknowledge, then reboot via the same deferred mechanism as /api/reboot so
// the JSON response flushes first. On any NVS or boot-partition failure we
// return 500 and stay in the main app (no reboot).
static esp_err_t ota_handler(httpd_req_t *req)
{
    const device_settings_t *s = settings_get();
    // An empty SSID is valid: we still write it so the updater skips STA and
    // comes up on its SoftAP fallback (ESP32-AVE-Setup @ 192.168.4.1).
    bool ssid_handoff = (s->wifi_ssid[0] != '\0');

    // Hand off the creds to the updater via the shared NVS "ota" namespace.
    nvs_handle_t h;
    esp_err_t err = nvs_open("ota", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: nvs_open failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs open failed");
        return ESP_FAIL;
    }
    err = nvs_set_str(h, "ssid", s->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", s->wifi_password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: cred handoff failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
        return ESP_FAIL;
    }

    // Point the boot slot at the updater. Running = ota_0, so next = ota_1.
    const esp_partition_t *updater = esp_ota_get_next_update_partition(NULL);
    if (!updater) {
        ESP_LOGE(TAG, "OTA: no updater partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no updater partition");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(updater);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_FAIL;
    }

    // Acknowledge before rebooting so the response reaches the browser.
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "target", "ota_1");
        cJSON_AddBoolToObject(root, "ssid_handoff", ssid_handoff);
        char *out = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        if (out) {
            httpd_resp_sendstr(req, out);
            cJSON_free(out);
        } else {
            httpd_resp_sendstr(req, "{\"ok\":true,\"target\":\"ota_1\"}");
        }
        cJSON_Delete(root);
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"target\":\"ota_1\"}");
    }

    // Deferred restart — same one-shot esp_timer pattern as reboot_handler.
    static esp_timer_handle_t ota_reboot_timer = NULL;
    if (!ota_reboot_timer) {
        const esp_timer_create_args_t targs = {
            .callback = reboot_timer_cb,
            .name = "ota_reboot",
        };
        if (esp_timer_create(&targs, &ota_reboot_timer) != ESP_OK) {
            esp_restart();
        }
    }
    esp_timer_start_once(ota_reboot_timer, 500000 /* 500 ms */);
    return ESP_OK;
}

// GET /api/version — report the running firmware's compile-time descriptor
// (ESP-IDF auto-captures version/date/time on every build; the semver comes
// from version.txt → PROJECT_VER) plus which OTA slot we booted from.
static esp_err_t version_handler(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (desc) {
        cJSON_AddStringToObject(root, "version", desc->version);
        cJSON_AddStringToObject(root, "project", desc->project_name);
        cJSON_AddStringToObject(root, "compile_date", desc->date);
        cJSON_AddStringToObject(root, "compile_time", desc->time);
        cJSON_AddStringToObject(root, "idf_ver", desc->idf_ver);
    }
    if (running) {
        cJSON_AddStringToObject(root, "partition", running->label);
        cJSON_AddNumberToObject(root, "address", (double)running->address);
    }

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (out) {
        httpd_resp_sendstr(req, out);
        cJSON_free(out);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json print failed");
    }
    cJSON_Delete(root);
    return out ? ESP_OK : ESP_FAIL;
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Stopping all audio + LED flicker + timeline");

    // ARM all audio fades FIRST, before any blocking call.  Both audio_generator
    // and bg_player respond to stop by arming a 5 ms amp fade and then
    // (in parallel) tearing down their producer state.  If we called
    // config_parser_stop_timeline() first (it calls bg_player_stop which blocks
    // up to 2 s for the HTTP producer task to exit), the generator channels
    // would keep playing for those 2 s — producing a delayed second click.
    // By arming all fades first, BG and generators fade together over ~5 ms.
    // Reference: bug_stop_click_bg_i2s_state_2026-06-17.md (Inv 17 #3).
    for (int i = 0; i < NUM_AUDIO_CHANNELS; i++) {
        audio_manager_stop_generation(i);
    }

    // Now do the heavy stop work (BG producer teardown can take up to 2 s).
    // The generator fades have already started silencing the audio, and the
    // BG fade is armed inside bg_player_stop() which uses a poll loop now
    // (Inv 17 #2 fix) so it won't drain less than needed.
    config_parser_stop_timeline();

    // Wait for every channel's stop-fade to complete before silencing the LEDs.
    // The fade is AUDIO_AMP_RAMP_SAMPLES (220) samples = ~5 ms at 44.1 kHz;
    // a 50 ms ceiling is a safety net in case the synthesis loop is starved.
    // Polling at 2 ms intervals keeps the loop cheap.
    for (int waited = 0; waited < 50; waited += 2) {
        if (!audio_generator_any_stopping()) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Stop LED flicker on all 8 channels (mask 0xFF).
    led_matrix_stop_flicker_masked(0xFF);

    httpd_resp_send(req, "All audio + LED stopped", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t example_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving example config");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, config_parser_get_example());
    return ESP_OK;
}

static esp_err_t play_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Play config from textarea requested");

    // Read config content from request body
    size_t received = 0;
    size_t remaining = req->content_len;

    if (remaining > WEB_SERVER_MAX_UPLOAD_SIZE) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Config too large");
        return ESP_FAIL;
    }

    if (remaining == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty config");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        size_t chunk_size = (remaining > 1024) ? 1024 : remaining;
        int ret = httpd_req_recv(req, g_server_state.upload_buffer + received, chunk_size);

        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            } else {
                httpd_resp_send_500(req);
            }
            return ESP_FAIL;
        }

        received += ret;
        remaining -= ret;
    }

    g_server_state.upload_buffer[received] = '\0';
    ESP_LOGI(TAG, "Received %zu bytes of config data from textarea", received);
    ESP_LOGI(TAG, "---- config content begin ----\n%s---- config content end ----",
             g_server_state.upload_buffer);

    // Implicit stop-before-play sequence — lets the user click PLAY repeatedly
    // without manually clicking STOP first. Mirrors stop_handler() to ensure
    // a clean transition (no audio click, no LED bleed-through from old
    // patterns). Keep these three lines in sync with stop_handler.
    //
    // 1) Arm audio fades (5 ms ramp). Without this, channels cut hard when
    //    config_parser_stop_timeline runs → audible click.
    for (int i = 0; i < NUM_AUDIO_CHANNELS; i++) {
        audio_manager_stop_generation(i);
    }
    // 2) Stop LED flicker on all 8 channels. Without this, old flicker
    //    patterns keep running through the transition and bleed visibly
    //    onto the start of the new timeline.
    led_matrix_stop_flicker_masked(0xFF);
    // 3) Wait up to 50 ms for the audio fades to drain before starting the
    //    new config — prevents overlap-click between old fade-out and new
    //    fade-in. Polling at 2 ms keeps the loop cheap.
    for (int waited = 0; waited < 50; waited += 2) {
        if (!audio_generator_any_stopping()) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Stop the timeline (cancels pending events, tears down BG audio via
    // the fast async variant — see config_parser_stop_timeline comment).
    config_parser_stop_timeline();

    // Parse and execute config
    config_timeline_t timeline = {0};

    esp_err_t parse_ret = config_parser_parse_content(g_server_state.upload_buffer, received, &timeline);

    if (parse_ret != ESP_OK) {
        config_parser_free_timeline(&timeline);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid config format");
        return ESP_FAIL;
    }

    esp_err_t exec_ret = config_parser_execute_timeline(&timeline, false);
    config_parser_free_timeline(&timeline);
    if (exec_ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to execute config");
        return ESP_FAIL;
    }

    httpd_resp_send(req, "Config started successfully! ▶", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// /api/patch-config — additive live-update endpoint
// ---------------------------------------------------------------------------
// Receives a small .ledc snippet (typically 1–2 lines from a UI slider change)
// and dispatches it as a patch on top of whatever is currently running. Unlike
// /api/play-config, this does NOT stop the running timeline and does NOT
// affect channels not mentioned in the patch. See plan 010 + the doc comment
// on config_parser_apply_patch in config_parser.h.
//
// Body: .ledc text, up to WEB_SERVER_MAX_UPLOAD_SIZE bytes (but typically tiny).
// Response: 200 "Patch applied" on success; 4xx with error message on parse failure.
static esp_err_t patch_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Patch config requested");

    size_t received = 0;
    size_t remaining = req->content_len;

    if (remaining > WEB_SERVER_MAX_UPLOAD_SIZE) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Patch too large");
        return ESP_FAIL;
    }
    if (remaining == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty patch");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        size_t chunk_size = (remaining > 1024) ? 1024 : remaining;
        int ret = httpd_req_recv(req, g_server_state.upload_buffer + received, chunk_size);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            } else {
                httpd_resp_send_500(req);
            }
            return ESP_FAIL;
        }
        received += ret;
        remaining -= ret;
    }
    g_server_state.upload_buffer[received] = '\0';

    esp_err_t ret = config_parser_apply_patch(g_server_state.upload_buffer, received);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "patch_config_handler: apply_patch failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid patch");
        return ESP_FAIL;
    }

    httpd_resp_send(req, "Patch applied", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// /api/state — JSON snapshot of all current per-channel state
// ---------------------------------------------------------------------------
// Polled by the live-control UI (~1 Hz when no user interaction) to keep
// slider positions in sync with what the engine is actually producing —
// especially important so the UI reflects timeline progress (e.g. a sweep
// from 12 Hz to 7.83 Hz over 10 min).
//
// Payload shape (see plan 010):
//   {
//     "caps":     { "led_color": bool, "num_led_ch": int, "num_audio_ch": int },
//     "led":      [{...per-channel...}, ...],
//     "audio":    [{...per-channel...}, ...],   // includes noise (index 8, ch9)
//     "bg":       { "active": bool },
//     "timeline": { "running": bool, "position_ms": uint }
//   }
//
// JSON is hand-rolled with snprintf into a stack buffer (~4 KB suffices for
// 8 LED + 16 audio channels). Avoids the cJSON heap-alloc overhead per
// request. Numeric fields are written with low precision (%.1f, %.2f) to
// keep payload small — the UI doesn't need 7-digit precision for sliders.
static esp_err_t state_handler(httpd_req_t *req)
{
    /* Snapshot under each module's lock — short critical sections. */
    led_matrix_channel_snapshot_t led_snap[NUM_LED_CHANNELS] = {0};
    int n_led = led_matrix_get_snapshot(led_snap, NUM_LED_CHANNELS);

    audio_gen_channel_snapshot_t aud_snap[NUM_AUDIO_CHANNELS] = {0};
    int n_aud = audio_generator_get_snapshot(aud_snap, NUM_AUDIO_CHANNELS);

    bool supports_color = led_matrix_supports_pixel_addressing();
    bool bg_active      = bg_player_is_active();
    bool tl_running     = (config_parser_get_timeline_position() > 0);
    uint32_t tl_pos     = config_parser_get_timeline_position();

    /* 4 KB buffer is plenty for 8 LED + 16 audio channels with the chosen
     * precision. If channel counts grow much larger, switch to a streaming
     * write via httpd_resp_send_chunk. */
    char *buf = (char *)malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    char *p   = buf;
    char *end = buf + 4096;

    #define APPEND(...) do { \
        int _n = snprintf(p, end - p, __VA_ARGS__); \
        if (_n < 0 || _n >= end - p) goto truncated; \
        p += _n; \
    } while (0)

    APPEND("{\"caps\":{\"led_color\":%s,\"num_led_ch\":%d,\"num_audio_ch\":%d},",
           supports_color ? "true" : "false", NUM_LED_CHANNELS, NUM_AUDIO_CHANNELS);

    APPEND("\"led\":[");
    for (int i = 0; i < n_led; i++) {
        APPEND("%s{\"ch\":%d,\"active\":%s,\"freq\":%.2f,\"duty\":%u,\"bright\":%u,"
               "\"r\":%u,\"g\":%u,\"b\":%u,"
               "\"mod\":{\"freq\":%s,\"duty\":%s,\"bright\":%s,\"r\":%s,\"g\":%s,\"b\":%s}}",
               (i == 0) ? "" : ",", i + 1,
               led_snap[i].active ? "true" : "false",
               led_snap[i].freq, led_snap[i].duty, led_snap[i].brightness,
               led_snap[i].r, led_snap[i].g, led_snap[i].b,
               led_snap[i].mod_freq_active   ? "true" : "false",
               led_snap[i].mod_duty_active   ? "true" : "false",
               led_snap[i].mod_bright_active ? "true" : "false",
               led_snap[i].mod_r_active      ? "true" : "false",
               led_snap[i].mod_g_active      ? "true" : "false",
               led_snap[i].mod_b_active      ? "true" : "false");
    }
    APPEND("],");

    APPEND("\"audio\":[");
    for (int i = 0; i < n_aud; i++) {
        APPEND("%s{\"ch\":%d,\"active\":%s,\"freq\":%.3f,\"freq_r\":%.3f,"
               "\"pan\":%.1f,\"vol\":%.1f,\"mod\":%.2f,\"wave\":%u,"
               "\"modf\":{\"freq\":%s,\"pan\":%s,\"vol\":%s,\"mod\":%s}}",
               (i == 0) ? "" : ",", i + 1,
               aud_snap[i].active ? "true" : "false",
               aud_snap[i].freq, aud_snap[i].freq_r,
               aud_snap[i].pan, aud_snap[i].volume, aud_snap[i].modulation,
               aud_snap[i].wave_type,
               aud_snap[i].mod_freq_active ? "true" : "false",
               aud_snap[i].mod_pan_active  ? "true" : "false",
               aud_snap[i].mod_vol_active  ? "true" : "false",
               aud_snap[i].mod_mod_active  ? "true" : "false");
    }
    APPEND("],");

    APPEND("\"bg\":{\"active\":%s},", bg_active ? "true" : "false");
    APPEND("\"timeline\":{\"running\":%s,\"position_ms\":%u}}",
           tl_running ? "true" : "false", (unsigned)tl_pos);

    #undef APPEND

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, p - buf);
    free(buf);
    return ESP_OK;

truncated:
    ESP_LOGW(TAG, "state_handler: JSON buffer truncated");
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "State payload too large");
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// /api/report
// ---------------------------------------------------------------------------
// JSON snapshot of the live session:
//   {
//     "config":            "<raw .led source>",
//     "session_origin_us": <esp_timer at timeline start, or 0 if no session>,
//     "now_us":            <esp_timer right now>,
//     "button_presses_ms": [ <ms since session_origin>, ... ]
//   }
// button_presses_ms is filtered to presses within the current session
// (session_origin_us > 0 → only presses >= session_origin_us are returned).
// Used by the web UI to display end-of-session diagnostics 5 s after the
// expected session end.

// JSON-escape one character into the output buffer. Returns bytes written
// (0 if the character would overflow the remaining buffer space).
static size_t json_escape_char(char c, char *out, size_t out_remaining)
{
    if (out_remaining < 2) return 0;
    switch (c) {
        case '"':  if (out_remaining < 3) return 0; out[0]='\\'; out[1]='"';  return 2;
        case '\\': if (out_remaining < 3) return 0; out[0]='\\'; out[1]='\\'; return 2;
        case '\n': if (out_remaining < 3) return 0; out[0]='\\'; out[1]='n';  return 2;
        case '\r': if (out_remaining < 3) return 0; out[0]='\\'; out[1]='r';  return 2;
        case '\t': if (out_remaining < 3) return 0; out[0]='\\'; out[1]='t';  return 2;
        default:
            // Control characters get \uXXXX, everything else passes through.
            if ((unsigned char)c < 0x20) {
                if (out_remaining < 7) return 0;
                snprintf(out, out_remaining, "\\u%04x", (unsigned)c);
                return 6;
            }
            out[0] = c;
            return 1;
    }
}

static esp_err_t report_handler(httpd_req_t *req)
{
    const char *source = config_parser_get_loaded_source();
    uint64_t origin_us = config_parser_get_session_origin_us();
    uint64_t now_us    = (uint64_t)esp_timer_get_time();

    // Snapshot button presses then filter to the current session.
    uint64_t presses[64];
    size_t n_presses = snapshot_button_get_presses(presses, 64);
    size_t n_in_session = 0;
    uint32_t rel_ms[64];
    for (size_t i = 0; i < n_presses; i++) {
        if (origin_us == 0 || presses[i] < origin_us) continue;
        rel_ms[n_in_session++] = (uint32_t)((presses[i] - origin_us) / 1000ULL);
    }

    // Build JSON response. Allocate a generous buffer — config can be up to
    // a few KB, plus JSON overhead. Stack allocation avoids fragmenting
    // heap for short-lived requests.
    const size_t resp_cap = 8192;
    char *resp = malloc(resp_cap);
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    size_t off = 0;

    off += snprintf(resp + off, resp_cap - off,
                    "{\"session_origin_us\":%llu,\"now_us\":%llu,\"config\":\"",
                    (unsigned long long)origin_us, (unsigned long long)now_us);

    if (source) {
        for (const char *p = source; *p && off < resp_cap - 8; p++) {
            off += json_escape_char(*p, resp + off, resp_cap - off);
        }
    }

    off += snprintf(resp + off, resp_cap - off, "\",\"button_presses_ms\":[");
    for (size_t i = 0; i < n_in_session && off < resp_cap - 16; i++) {
        off += snprintf(resp + off, resp_cap - off, "%s%u",
                        i == 0 ? "" : ",", (unsigned)rel_ms[i]);
    }
    off += snprintf(resp + off, resp_cap - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, off);
    free(resp);
    return ESP_OK;
}
