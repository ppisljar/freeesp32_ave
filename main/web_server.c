#include "web_server.h"
#include "config_parser.h"
#include "audio_manager.h"
#include "audio_test.h"
#include "led_matrix_example.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "web_server";

// Global web server state
static web_server_state_t g_server_state = {0};

// HTML pages
static const char* index_html =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"    <title>ESP32 Audio Player</title>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <style>\n"
"        body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }\n"
"        .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n"
"        h1 { color: #333; text-align: center; }\n"
"        .section { margin: 20px 0; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }\n"
"        .section h2 { margin-top: 0; color: #555; }\n"
"        button { background: #007bff; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; margin: 5px; }\n"
"        button:hover { background: #0056b3; }\n"
"        .stop-btn { background: #dc3545; }\n"
"        .stop-btn:hover { background: #c82333; }\n"
"        input[type=\"file\"] { margin: 10px 0; }\n"
"        .status { padding: 10px; margin: 10px 0; border-radius: 4px; }\n"
"        .status.success { background: #d4edda; border: 1px solid #c3e6cb; color: #155724; }\n"
"        .status.error { background: #f8d7da; border: 1px solid #f5c6cb; color: #721c24; }\n"
"        .status.info { background: #d1ecf1; border: 1px solid #bee5eb; color: #0c5460; }\n"
"        textarea { width: 100%; height: 200px; font-family: monospace; }\n"
"        .controls { display: flex; flex-wrap: wrap; gap: 10px; }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"container\">\n"
"        <h1>ESP32 Audio Player Control Panel</h1>\n"
"        \n"
"        <div class=\"section\">\n"
"            <h2>System Status</h2>\n"
"            <div id=\"status\" class=\"status info\">System Ready</div>\n"
"            <button onclick=\"updateStatus()\">Refresh Status</button>\n"
"        </div>\n"
"\n"
"        <div class=\"section\">\n"
"            <h2>Audio Tests</h2>\n"
"            <div class=\"controls\">\n"
"                <button onclick=\"testTone()\">Test 440Hz Tone</button>\n"
"                <button onclick=\"testBinaural()\">Test Binaural Beats</button>\n"
"                <button onclick=\"testSweep()\">Test Frequency Sweep</button>\n"
"                <button onclick=\"stopAudio()\" class=\"stop-btn\">Stop All Audio</button>\n"
"            </div>\n"
"        </div>\n"
"\n"
"        <div class=\"section\">\n"
"            <h2>Config File Upload</h2>\n"
"            <form id=\"uploadForm\" enctype=\"multipart/form-data\">\n"
"                <input type=\"file\" id=\"configFile\" name=\"config\" accept=\".led,.txt\" required>\n"
"                <br>\n"
"                <button type=\"submit\">Upload & Execute Config</button>\n"
"            </form>\n"
"        </div>\n"
"\n"
"        <div class=\"section\">\n"
"            <h2>LED Matrix Tests</h2>\n"
"            <div class=\"controls\">\n"
"                <button onclick=\"testLedPattern()\">Test Pattern</button>\n"
"                <button onclick=\"testLedColors()\">Test Colors</button>\n"
"                <button onclick=\"testLedRed()\">All Red</button>\n"
"                <button onclick=\"testLedGreen()\">All Green</button>\n"
"                <button onclick=\"testLedBlue()\">All Blue</button>\n"
"                <button onclick=\"testLedWhite()\">All White</button>\n"
"                <button onclick=\"clearLeds()\">Clear LEDs</button>\n"
"            </div>\n"
"        </div>\n"
"\n"
"        <div class=\"section\">\n"
"            <h2>Test Config</h2>\n"
"            <div class=\"controls\">\n"
"                <button onclick=\"loadExample()\">Load Fresh Example</button>\n"
"                <button onclick=\"playConfig()\" style=\"background: #28a745;\">▶ PLAY Config</button>\n"
"                <button onclick=\"stopConfig()\" style=\"background: #dc3545;\">■ STOP</button>\n"
"                <button onclick=\"clearConfig()\">Clear</button>\n"
"            </div>\n"
"            <textarea id=\"exampleConfig\" placeholder=\"Enter .led config here...\"></textarea>\n"
"        </div>\n"
"    </div>\n"
"\n"
"    <script>\n"
"        function updateStatus() {\n"
"            fetch('/api/status')\n"
"                .then(response => response.json())\n"
"                .then(data => {\n"
"                    const statusDiv = document.getElementById('status');\n"
"                    statusDiv.textContent = `Audio: ${data.audio_state}, Source: ${data.audio_source}, Volume: ${data.volume}%`;\n"
"                    statusDiv.className = 'status info';\n"
"                })\n"
"                .catch(error => {\n"
"                    document.getElementById('status').textContent = 'Error: ' + error;\n"
"                    document.getElementById('status').className = 'status error';\n"
"                });\n"
"        }\n"
"\n"
"        function testTone() {\n"
"            fetch('/api/test/tone', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testBinaural() {\n"
"            fetch('/api/test/binaural', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testSweep() {\n"
"            fetch('/api/test/sweep', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function stopAudio() {\n"
"            fetch('/api/stop', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testLedPattern() {\n"
"            fetch('/api/led/pattern', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('LED Pattern Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testLedColors() {\n"
"            fetch('/api/led/colors', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('LED Colors Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testLedRed() {\n"
"            fetch('/api/led/red', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('LED Red Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testLedGreen() {\n"
"            fetch('/api/led/green', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('LED Green Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testLedBlue() {\n"
"            fetch('/api/led/blue', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('LED Blue Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function testLedWhite() {\n"
"            fetch('/api/led/white', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('LED White Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function clearLeds() {\n"
"            fetch('/api/led/clear', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('LED Clear Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function loadExample() {\n"
"            fetch('/api/example')\n"
"                .then(response => response.text())\n"
"                .then(data => {\n"
"                    document.getElementById('exampleConfig').value = data;\n"
"                })\n"
"                .catch(error => showMessage('Error loading example: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function stopConfig() {\n"
"            fetch('/api/stop', { method: 'POST' })\n"
"                .then(response => response.text())\n"
"                .then(result => showMessage(result, 'success'))\n"
"                .catch(error => showMessage('Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function playConfig() {\n"
"            const config = document.getElementById('exampleConfig').value.trim();\n"
"            if (!config) {\n"
"                showMessage('Please enter a config to play', 'error');\n"
"                return;\n"
"            }\n"
"\n"
"            fetch('/api/play-config', {\n"
"                method: 'POST',\n"
"                headers: {\n"
"                    'Content-Type': 'text/plain'\n"
"                },\n"
"                body: config\n"
"            })\n"
"            .then(response => response.text())\n"
"            .then(result => showMessage(result, 'success'))\n"
"            .catch(error => showMessage('Play error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        function clearConfig() {\n"
"            document.getElementById('exampleConfig').value = '';\n"
"            showMessage('Config cleared', 'info');\n"
"        }\n"
"\n"
"        function showMessage(message, type) {\n"
"            const statusDiv = document.getElementById('status');\n"
"            statusDiv.textContent = message;\n"
"            statusDiv.className = 'status ' + type;\n"
"        }\n"
"\n"
"        document.getElementById('uploadForm').addEventListener('submit', function(e) {\n"
"            e.preventDefault();\n"
"            const formData = new FormData();\n"
"            const fileInput = document.getElementById('configFile');\n"
"            formData.append('config', fileInput.files[0]);\n"
"\n"
"            fetch('/api/upload', {\n"
"                method: 'POST',\n"
"                body: formData\n"
"            })\n"
"            .then(response => response.text())\n"
"            .then(result => showMessage(result, 'success'))\n"
"            .catch(error => showMessage('Upload error: ' + error, 'error'));\n"
"        });\n"
"\n"
"        // Auto-update status every 5 seconds\n"
"        setInterval(updateStatus, 5000);\n"
"        updateStatus(); // Initial load\n"
"        loadExample(); // Load example config on page load\n"
"    </script>\n"
"</body>\n"
"</html>\n";

// HTTP Handler functions
static esp_err_t index_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);
static esp_err_t upload_handler(httpd_req_t *req);
static esp_err_t test_tone_handler(httpd_req_t *req);
static esp_err_t test_binaural_handler(httpd_req_t *req);
static esp_err_t test_sweep_handler(httpd_req_t *req);
static esp_err_t stop_handler(httpd_req_t *req);
static esp_err_t example_handler(httpd_req_t *req);
static esp_err_t led_pattern_handler(httpd_req_t *req);
static esp_err_t led_colors_handler(httpd_req_t *req);
static esp_err_t led_red_handler(httpd_req_t *req);
static esp_err_t led_green_handler(httpd_req_t *req);
static esp_err_t led_blue_handler(httpd_req_t *req);
static esp_err_t led_white_handler(httpd_req_t *req);
static esp_err_t led_clear_handler(httpd_req_t *req);
static esp_err_t play_config_handler(httpd_req_t *req);

esp_err_t web_server_init(void)
{
    ESP_LOGI(TAG, "Initializing web server");

    // Allocate upload buffer
    g_server_state.upload_buffer = malloc(WEB_SERVER_MAX_UPLOAD_SIZE);
    if (!g_server_state.upload_buffer) {
        ESP_LOGE(TAG, "Failed to allocate upload buffer");
        return ESP_ERR_NO_MEM;
    }

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = 16;

    // Start HTTP server
    esp_err_t ret = httpd_start(&g_server_state.server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        free(g_server_state.upload_buffer);
        return ret;
    }

    // Register URI handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &index_uri);

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &status_uri);

    httpd_uri_t upload_uri = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &upload_uri);

    httpd_uri_t test_tone_uri = {
        .uri = "/api/test/tone",
        .method = HTTP_POST,
        .handler = test_tone_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &test_tone_uri);

    httpd_uri_t test_binaural_uri = {
        .uri = "/api/test/binaural",
        .method = HTTP_POST,
        .handler = test_binaural_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &test_binaural_uri);

    httpd_uri_t test_sweep_uri = {
        .uri = "/api/test/sweep",
        .method = HTTP_POST,
        .handler = test_sweep_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &test_sweep_uri);

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

    // LED Matrix handlers
    httpd_uri_t led_pattern_uri = {
        .uri = "/api/led/pattern",
        .method = HTTP_POST,
        .handler = led_pattern_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &led_pattern_uri);

    httpd_uri_t led_colors_uri = {
        .uri = "/api/led/colors",
        .method = HTTP_POST,
        .handler = led_colors_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &led_colors_uri);

    httpd_uri_t led_red_uri = {
        .uri = "/api/led/red",
        .method = HTTP_POST,
        .handler = led_red_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &led_red_uri);

    httpd_uri_t led_green_uri = {
        .uri = "/api/led/green",
        .method = HTTP_POST,
        .handler = led_green_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &led_green_uri);

    httpd_uri_t led_blue_uri = {
        .uri = "/api/led/blue",
        .method = HTTP_POST,
        .handler = led_blue_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &led_blue_uri);

    httpd_uri_t led_white_uri = {
        .uri = "/api/led/white",
        .method = HTTP_POST,
        .handler = led_white_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &led_white_uri);

    httpd_uri_t led_clear_uri = {
        .uri = "/api/led/clear",
        .method = HTTP_POST,
        .handler = led_clear_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &led_clear_uri);

    httpd_uri_t play_config_uri = {
        .uri = "/api/play-config",
        .method = HTTP_POST,
        .handler = play_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &play_config_uri);

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

static esp_err_t index_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving index page");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    // Status API is polled by the web UI every ~1 s — demoted from I to D to keep UART quiet.
    ESP_LOGD(TAG, "Serving status API");

    audio_manager_state_t *state = audio_manager_get_state();

    char json_response[256];
    snprintf(json_response, sizeof(json_response),
             "{\n"
             "  \"audio_state\": %d,\n"
             "  \"audio_source\": %d,\n"
             "  \"volume\": %.0f,\n"
             "  \"muted\": %s\n"
             "}",
             state->state,
             state->current_source,
             state->volume * 100,
             state->muted ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Handling config file upload");

    // Read uploaded file content
    size_t received = 0;
    size_t remaining = req->content_len;

    if (remaining > WEB_SERVER_MAX_UPLOAD_SIZE) {
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "File too large");
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

    g_server_state.upload_size = received;
    g_server_state.upload_buffer[received] = '\0';

    ESP_LOGI(TAG, "Received %zu bytes of config data", received);

    // Parse and execute config
    config_timeline_t timeline = {0};

    esp_err_t ret = config_parser_parse_content(g_server_state.upload_buffer, received, &timeline);

    if (ret != ESP_OK) {
        config_parser_free_timeline(&timeline);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid config file format");
        return ESP_FAIL;
    }

    // Execute timeline (deep-copies entries into the pool-backed persistent slot)
    ret = config_parser_execute_timeline(&timeline, false);
    config_parser_free_timeline(&timeline);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to execute config");
        return ESP_FAIL;
    }

    httpd_resp_send(req, "Config uploaded and executed successfully", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t test_tone_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting tone test");

    esp_err_t ret = audio_test_basic_generation();
    if (ret == ESP_OK) {
        httpd_resp_send(req, "440Hz tone test started", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start tone test");
    }

    return ESP_OK;
}

static esp_err_t test_binaural_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting binaural beats test");

    esp_err_t ret = audio_test_binaural_beats(440.0f, 10.0f, 5000);
    if (ret == ESP_OK) {
        httpd_resp_send(req, "Binaural beats test started (440Hz + 10Hz)", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start binaural test");
    }

    return ESP_OK;
}

static esp_err_t test_sweep_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting frequency sweep test");

    esp_err_t ret = audio_test_frequency_sweep(200.0f, 800.0f, 4000, AUDIO_GEN_SWEEP_LINEAR);
    if (ret == ESP_OK) {
        httpd_resp_send(req, "Frequency sweep test started (200-800Hz)", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start sweep test");
    }

    return ESP_OK;
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
    for (int i = 0; i < 8; i++) {
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

    char example_buffer[2048];
    esp_err_t ret = config_parser_create_example(example_buffer, sizeof(example_buffer));

    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, example_buffer, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create example");
    }

    return ESP_OK;
}

// LED Matrix Handler Functions

static esp_err_t led_pattern_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "LED pattern test requested");

    if (!led_matrix_supports_pixel_addressing()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "LED test patterns are not supported in direct mode (no addressable pixels).");
        return ESP_OK;
    }

    esp_err_t ret = led_matrix_test_pattern();
    if (ret == ESP_OK) {
        httpd_resp_send(req, "LED pattern test started", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start LED pattern test");
    }

    return ESP_OK;
}

static esp_err_t led_colors_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "LED color test requested");

    if (!led_matrix_supports_pixel_addressing()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "LED test patterns are not supported in direct mode (no addressable pixels).");
        return ESP_OK;
    }

    // Test each color for 1 second each
    for (uint8_t x = 0; x < 12; x++) {
        for (uint8_t y = 0; y < 4; y++) {
            if (x < 4) {
                led_matrix_set_pixel(x, y, 255, 0, 0); // Red
            } else if (x < 8) {
                led_matrix_set_pixel(x, y, 0, 255, 0); // Green
            } else {
                led_matrix_set_pixel(x, y, 0, 0, 255); // Blue
            }
        }
    }
    led_matrix_refresh();

    httpd_resp_send(req, "LED color test completed (Red/Green/Blue columns)", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t led_red_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "LED all red requested");

    if (!led_matrix_supports_pixel_addressing()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "LED test patterns are not supported in direct mode (no addressable pixels).");
        return ESP_OK;
    }

    // Clear all LEDs first
    led_matrix_clear();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Set all LEDs to red with logging
    ESP_LOGI(TAG, "Setting %d LEDs to RED (255,0,0)", 12*4);
    for (uint8_t x = 0; x < 12; x++) {
        for (uint8_t y = 0; y < 4; y++) {
            esp_err_t ret = led_matrix_set_pixel(x, y, 255, 0, 0);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set LED[%d,%d]: %s", x, y, esp_err_to_name(ret));
            }
        }
    }

    ESP_LOGI(TAG, "Calling led_matrix_refresh()");
    esp_err_t refresh_ret = led_matrix_refresh();
    ESP_LOGI(TAG, "Refresh result: %s", esp_err_to_name(refresh_ret));

    httpd_resp_send(req, "All LEDs set to red", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t led_green_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "LED all green requested");

    if (!led_matrix_supports_pixel_addressing()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "LED test patterns are not supported in direct mode (no addressable pixels).");
        return ESP_OK;
    }

    for (uint8_t x = 0; x < 12; x++) {
        for (uint8_t y = 0; y < 4; y++) {
            led_matrix_set_pixel(x, y, 0, 255, 0);
        }
    }
    led_matrix_refresh();

    httpd_resp_send(req, "All LEDs set to green", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t led_blue_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "LED all blue requested");

    if (!led_matrix_supports_pixel_addressing()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "LED test patterns are not supported in direct mode (no addressable pixels).");
        return ESP_OK;
    }

    for (uint8_t x = 0; x < 12; x++) {
        for (uint8_t y = 0; y < 4; y++) {
            led_matrix_set_pixel(x, y, 0, 0, 255);
        }
    }
    led_matrix_refresh();

    httpd_resp_send(req, "All LEDs set to blue", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t led_white_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "LED all white requested");

    if (!led_matrix_supports_pixel_addressing()) {
        httpd_resp_set_status(req, "501 Not Implemented");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "LED test patterns are not supported in direct mode (no addressable pixels).");
        return ESP_OK;
    }

    for (uint8_t x = 0; x < 12; x++) {
        for (uint8_t y = 0; y < 4; y++) {
            led_matrix_set_pixel(x, y, 255, 255, 255);
        }
    }
    led_matrix_refresh();

    httpd_resp_send(req, "All LEDs set to white", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t led_clear_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "LED clear requested");

    esp_err_t ret = led_matrix_clear();
    if (ret == ESP_OK) {
        httpd_resp_send(req, "All LEDs cleared", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to clear LEDs");
    }

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

    // Stop any currently running timeline
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
