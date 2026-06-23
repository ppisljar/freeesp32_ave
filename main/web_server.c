#include "sdkconfig.h"           // for CONFIG_GENERATOR_SERVER_URL
#include "web_server.h"
#include "config_parser.h"
#include "audio_manager.h"
#include "audio_generator.h"     // for NUM_AUDIO_CHANNELS
#include "led_matrix_example.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Defined in esp32_audioplayer.c — snapshot of the GPIO 5 button press log.
// Returns up to `max` absolute esp_timer_get_time() values, chronological.
extern size_t snapshot_button_get_presses(uint64_t *out, size_t max);

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
"\n"
"        <div id=\"status\" class=\"status info\" style=\"display:none\"></div>\n"
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
"            <h2>Test Config</h2>\n"
"            <div class=\"controls\">\n"
"                <button onclick=\"loadExample()\">Load Fresh Example</button>\n"
"                <button onclick=\"playConfig()\" style=\"background: #28a745;\">▶ PLAY Config</button>\n"
"                <button onclick=\"stopConfig()\" style=\"background: #dc3545;\">■ STOP</button>\n"
"                <button onclick=\"clearConfig()\">Clear</button>\n"
"            </div>\n"
"            <div style=\"margin:10px 0;display:flex;gap:8px;align-items:center;flex-wrap:wrap;padding:10px;background:#fafafa;border:1px solid #e0e0e0;border-radius:4px;\">\n"
"                <strong style=\"font-size:13px;\">Generator:</strong>\n"
"                <select id=\"ledcDropdown\" style=\"min-width:220px;padding:4px;\"></select>\n"
"                <button onclick=\"refreshLedcDropdown()\" title=\"Reload list from generator\">⟳</button>\n"
"                <button onclick=\"loadFromGenerator()\">Load</button>\n"
"                <button onclick=\"saveToGenerator()\">Save</button>\n"
"                <button onclick=\"saveAsToGenerator()\">Save As…</button>\n"
"                <span id=\"loadedFilename\" style=\"color:#666;font-style:italic;font-size:13px;margin-left:8px;\"></span>\n"
"            </div>\n"
"            <textarea id=\"exampleConfig\" placeholder=\"Enter .led config here...\"></textarea>\n"
"            <div id=\"reportBox\" style=\"margin-top:15px;display:none;background:#f0f4f8;border:1px solid #c3d0e0;border-radius:4px;padding:12px;font-family:monospace;font-size:13px;white-space:pre-wrap;\"></div>\n"
"        </div>\n"
"    </div>\n"
"\n"
"    <script>\n"
"        // Generator base URL injected at flash time from CONFIG_GENERATOR_SERVER_URL.\n"
"        // Override via `idf.py menuconfig` → ESP32 Audio Player Configuration.\n"
"        const GENERATOR_URL = \"" CONFIG_GENERATOR_SERVER_URL "\";\n"
"        let currentLedcName = null;\n"
"\n"
"        function refreshLedcDropdown() {\n"
"            return fetch(GENERATOR_URL + '/ledc')\n"
"                .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })\n"
"                .then(data => {\n"
"                    const dd = document.getElementById('ledcDropdown');\n"
"                    dd.innerHTML = '';\n"
"                    const files = data.files || [];\n"
"                    if (files.length === 0) {\n"
"                        const opt = document.createElement('option');\n"
"                        opt.textContent = '(no configs on generator)';\n"
"                        opt.disabled = true;\n"
"                        dd.appendChild(opt);\n"
"                        return;\n"
"                    }\n"
"                    files.forEach(f => {\n"
"                        const opt = document.createElement('option');\n"
"                        opt.value = f; opt.textContent = f;\n"
"                        if (f === currentLedcName) opt.selected = true;\n"
"                        dd.appendChild(opt);\n"
"                    });\n"
"                })\n"
"                .catch(err => {\n"
"                    const dd = document.getElementById('ledcDropdown');\n"
"                    dd.innerHTML = '';\n"
"                    const opt = document.createElement('option');\n"
"                    opt.textContent = '(generator unreachable @ ' + GENERATOR_URL + ')';\n"
"                    opt.disabled = true;\n"
"                    dd.appendChild(opt);\n"
"                });\n"
"        }\n"
"\n"
"        function loadFromGenerator() {\n"
"            const name = document.getElementById('ledcDropdown').value;\n"
"            if (!name) { showMessage('Pick a config first', 'error'); return; }\n"
"            fetch(GENERATOR_URL + '/ledc/' + encodeURIComponent(name))\n"
"                .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); })\n"
"                .then(text => {\n"
"                    document.getElementById('exampleConfig').value = text;\n"
"                    currentLedcName = name;\n"
"                    document.getElementById('loadedFilename').textContent = '(loaded: ' + name + ')';\n"
"                    showMessage('Loaded ' + name + ' from generator', 'success');\n"
"                })\n"
"                .catch(err => showMessage('Load failed: ' + err, 'error'));\n"
"        }\n"
"\n"
"        function putLedc(name, body) {\n"
"            return fetch(GENERATOR_URL + '/ledc/' + encodeURIComponent(name), {\n"
"                method: 'PUT',\n"
"                headers: { 'Content-Type': 'text/plain' },\n"
"                body: body,\n"
"            })\n"
"            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })\n"
"            .then(result => {\n"
"                const verb = result.overwritten ? 'Overwrote' : 'Created';\n"
"                showMessage(verb + ' ' + result.saved + ' on generator (' + result.bytes + ' bytes)', 'success');\n"
"                currentLedcName = result.saved;\n"
"                document.getElementById('loadedFilename').textContent = '(loaded: ' + result.saved + ')';\n"
"                return refreshLedcDropdown();\n"
"            })\n"
"            .catch(err => showMessage('Save failed: ' + err, 'error'));\n"
"        }\n"
"\n"
"        function saveToGenerator() {\n"
"            const body = document.getElementById('exampleConfig').value;\n"
"            if (!body.trim()) { showMessage('Config is empty', 'error'); return; }\n"
"            if (!currentLedcName) {\n"
"                // No file loaded — fall through to Save As so user names it.\n"
"                return saveAsToGenerator();\n"
"            }\n"
"            return putLedc(currentLedcName, body);\n"
"        }\n"
"\n"
"        function saveAsToGenerator() {\n"
"            const body = document.getElementById('exampleConfig').value;\n"
"            if (!body.trim()) { showMessage('Config is empty', 'error'); return; }\n"
"            const suggested = currentLedcName || 'untitled.ledc';\n"
"            let name = prompt('Save as (must end in .ledc):', suggested);\n"
"            if (!name) return;\n"
"            name = name.trim();\n"
"            if (!/^[A-Za-z0-9._-]+\\.ledc$/.test(name)) {\n"
"                showMessage('Invalid filename. Allowed: [A-Za-z0-9._-]+.ledc', 'error');\n"
"                return;\n"
"            }\n"
"            // If filename exists in the dropdown AND it's not the currently-loaded one,\n"
"            // confirm overwrite. (Saving back to the current file is the normal Save\n"
"            // path; explicit Save-As to a different existing name should warn.)\n"
"            const existing = Array.from(document.getElementById('ledcDropdown').options)\n"
"                                   .map(o => o.value).filter(Boolean);\n"
"            if (existing.includes(name) && name !== currentLedcName) {\n"
"                if (!confirm('\"' + name + '\" exists on the generator. Overwrite?')) return;\n"
"            }\n"
"            return putLedc(name, body);\n"
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
"                .then(result => {\n"
"                    showMessage(result + ' — report in 5 s', 'success');\n"
"                    // Replace any pending end-of-session timer with a short\n"
"                    // post-stop one so the report reflects the truncated run.\n"
"                    if (reportTimer) { clearTimeout(reportTimer); reportTimer = null; }\n"
"                    document.getElementById('reportBox').style.display = 'none';\n"
"                    reportTimer = setTimeout(fetchReport, 5000);\n"
"                })\n"
"                .catch(error => showMessage('Error: ' + error, 'error'));\n"
"        }\n"
"\n"
"        let reportTimer = null;\n"
"\n"
"        // Parse the .led config text and return the highest entry timestamp in ms.\n"
"        // Format: LED entries start with a number (time), audio entries start with 'A'.\n"
"        // Lines starting with '#' are comments and skipped. Returns 0 if none.\n"
"        function parseConfigDurationMs(text) {\n"
"            let maxMs = 0;\n"
"            for (const raw of text.split('\\n')) {\n"
"                const line = raw.replace(/#.*$/, '').trim();\n"
"                if (!line) continue;\n"
"                const tok = line.split(/\\s+/);\n"
"                let t = NaN;\n"
"                if (tok[0] === 'A' && tok.length > 1) t = parseInt(tok[1], 10);\n"
"                else if (tok[0] === 'BG') continue; // BG has no time field\n"
"                else t = parseInt(tok[0], 10);\n"
"                if (!isNaN(t) && t > maxMs) maxMs = t;\n"
"            }\n"
"            return maxMs;\n"
"        }\n"
"\n"
"        // ---- Config parsing + per-press state resolution -----------------\n"
"        // Mirrors the device-side interpolation: for each parameter on each\n"
"        // entry, find the active entry at time T and (if the next entry has\n"
"        // a '>' or '*' marker on that parameter) interpolate live value.\n"
"        function parseValueInterp(s) {\n"
"            let interp = 'none';\n"
"            if (s[0] === '>') { interp = 'linear'; s = s.slice(1); }\n"
"            else if (s[0] === '*') { interp = 'quadratic'; s = s.slice(1); }\n"
"            return { v: parseFloat(s), interp: interp };\n"
"        }\n"
"        function lerp(a, b, t) { return a + (b - a) * t; }\n"
"        function quad(a, b, t) {\n"
"            const tt = t < 0.5 ? 2*t*t : 1 - 2*(1-t)*(1-t);\n"
"            return a + (b - a) * tt;\n"
"        }\n"
"        function interpField(active, next, tMs, fname) {\n"
"            if (!active[fname]) return null;\n"
"            if (next && next[fname] && next[fname].interp !== 'none') {\n"
"                const win = next.time - active.time;\n"
"                if (win <= 0) return active[fname].v;\n"
"                const p = Math.max(0, Math.min(1, (tMs - active.time) / win));\n"
"                const fn = next[fname].interp === 'linear' ? lerp : quad;\n"
"                return fn(active[fname].v, next[fname].v, p);\n"
"            }\n"
"            return active[fname].v;\n"
"        }\n"
"        function parseConfigStructured(text) {\n"
"            const led = [], audio = [];\n"
"            for (const raw of text.split('\\n')) {\n"
"                const line = raw.replace(/#.*$/, '').trim();\n"
"                if (!line) continue;\n"
"                const t = line.split(/\\s+/);\n"
"                if (t[0] === 'A') {\n"
"                    if (t.length < 7) continue;\n"
"                    audio.push({\n"
"                        time: parseInt(t[1], 10),\n"
"                        freq: parseValueInterp(t[2]),\n"
"                        pan:  parseValueInterp(t[3]),\n"
"                        vol:  parseValueInterp(t[4]),\n"
"                        mod:  parseValueInterp(t[5]),\n"
"                        channel: parseInt(t[6], 10),\n"
"                    });\n"
"                } else if (t[0] === 'BG') {\n"
"                    continue;\n"
"                } else if (t.length === 8) {\n"
"                    // 8-field LED (canonical): time freq duty bright R G B mask\n"
"                    led.push({\n"
"                        time: parseInt(t[0], 10),\n"
"                        freq: parseValueInterp(t[1]),\n"
"                        duty: parseValueInterp(t[2]),\n"
"                        brightness: parseValueInterp(t[3]),\n"
"                        r:    parseValueInterp(t[4]),\n"
"                        g:    parseValueInterp(t[5]),\n"
"                        b:    parseValueInterp(t[6]),\n"
"                        mask: parseInt(t[7], 10),\n"
"                    });\n"
"                } else if (t.length === 5) {\n"
"                    // 5-field legacy: time freq duty brightness channel\n"
"                    const ch = parseInt(t[4], 10);\n"
"                    led.push({\n"
"                        time: parseInt(t[0], 10),\n"
"                        mask: ch === 0 ? 0xFF : (1 << (ch - 1)),\n"
"                        freq: parseValueInterp(t[1]),\n"
"                        duty: parseValueInterp(t[2]),\n"
"                        brightness: parseValueInterp(t[3]),\n"
"                        r: { v: 255, interp: 'none' },\n"
"                        g: { v: 255, interp: 'none' },\n"
"                        b: { v: 255, interp: 'none' },\n"
"                    });\n"
"                }\n"
"            }\n"
"            return { led: led, audio: audio };\n"
"        }\n"
"        function audioStateAtTime(audioEntries, tMs, channel) {\n"
"            const seq = audioEntries.filter(e => e.channel === channel)\n"
"                                    .sort((a,b) => a.time - b.time);\n"
"            let active = null, next = null;\n"
"            for (let i = 0; i < seq.length; i++) {\n"
"                if (seq[i].time <= tMs) { active = seq[i]; next = seq[i+1] || null; }\n"
"                else break;\n"
"            }\n"
"            if (!active) return null;\n"
"            return {\n"
"                freq: interpField(active, next, tMs, 'freq'),\n"
"                pan:  interpField(active, next, tMs, 'pan'),\n"
"                vol:  interpField(active, next, tMs, 'vol'),\n"
"                mod:  interpField(active, next, tMs, 'mod'),\n"
"            };\n"
"        }\n"
"        function ledStateAtTime(ledEntries, tMs, ledCh) {\n"
"            const bit = 1 << ledCh;\n"
"            const seq = ledEntries.filter(e => e.mask & bit)\n"
"                                  .sort((a,b) => a.time - b.time);\n"
"            let active = null, next = null;\n"
"            for (let i = 0; i < seq.length; i++) {\n"
"                if (seq[i].time <= tMs) { active = seq[i]; next = seq[i+1] || null; }\n"
"                else break;\n"
"            }\n"
"            if (!active) return null;\n"
"            return {\n"
"                freq: interpField(active, next, tMs, 'freq'),\n"
"                duty: interpField(active, next, tMs, 'duty'),\n"
"                bri:  interpField(active, next, tMs, 'brightness'),\n"
"                r:    interpField(active, next, tMs, 'r'),\n"
"                g:    interpField(active, next, tMs, 'g'),\n"
"                b:    interpField(active, next, tMs, 'b'),\n"
"            };\n"
"        }\n"
"        function formatPressSnapshot(parsed, tMs) {\n"
"            const lines = [];\n"
"            for (let ch = 1; ch <= 16; ch++) {\n"
"                const s = audioStateAtTime(parsed.audio, tMs, ch);\n"
"                if (!s) continue;\n"
"                lines.push('  AUDIO[ch=' + ch + '] freq=' + s.freq.toFixed(2) +\n"
"                           'Hz pan=' + s.pan.toFixed(0) +\n"
"                           ' vol=' + s.vol.toFixed(0) +\n"
"                           ' mod=' + s.mod.toFixed(1));\n"
"            }\n"
"            for (let ch = 0; ch < 8; ch++) {\n"
"                const s = ledStateAtTime(parsed.led, tMs, ch);\n"
"                if (!s) continue;\n"
"                lines.push('  LED[ch=' + ch + '] freq=' + s.freq.toFixed(2) +\n"
"                           'Hz duty=' + s.duty.toFixed(0) +\n"
"                           '% bri=' + s.bri.toFixed(0) +\n"
"                           '% RGB=(' + s.r.toFixed(0) + ',' + s.g.toFixed(0) + ',' + s.b.toFixed(0) + ')');\n"
"            }\n"
"            return lines.length ? lines.join('\\n') : '  (no active channels at this time)';\n"
"        }\n"
"\n"
"        function fetchReport() {\n"
"            fetch('/api/report')\n"
"                .then(r => r.json())\n"
"                .then(rep => {\n"
"                    const sessSec = rep.session_origin_us > 0\n"
"                        ? ((rep.now_us - rep.session_origin_us) / 1e6).toFixed(1)\n"
"                        : '—';\n"
"                    const presses = rep.button_presses_ms || [];\n"
"                    const cfg = rep.config || '';\n"
"                    const parsed = cfg ? parseConfigStructured(cfg) : null;\n"
"\n"
"                    let out = '=== SESSION REPORT ===\\n';\n"
"                    out += 'Session length so far: ' + sessSec + ' s\\n\\n';\n"
"                    out += '--- Button press snapshots ---\\n';\n"
"                    if (!presses.length) {\n"
"                        out += '(no button presses recorded)\\n';\n"
"                    } else if (!parsed) {\n"
"                        out += '(' + presses.length + ' press(es), but no config to resolve params): ' +\n"
"                               presses.join(', ') + '\\n';\n"
"                    } else {\n"
"                        for (let i = 0; i < presses.length; i++) {\n"
"                            out += '\\n@ +' + presses[i] + 'ms (press ' + (i+1) + '):\\n';\n"
"                            out += formatPressSnapshot(parsed, presses[i]) + '\\n';\n"
"                        }\n"
"                    }\n"
"                    out += '\\n--- Last loaded config ---\\n';\n"
"                    out += cfg || '(no config in memory)';\n"
"                    const box = document.getElementById('reportBox');\n"
"                    box.textContent = out;\n"
"                    box.style.display = 'block';\n"
"\n"
"                    // Best-effort upload to the generator. Failure is\n"
"                    // non-fatal — the local display always succeeds first.\n"
"                    const upload = {\n"
"                        config_name: currentLedcName,\n"
"                        session_origin_us: rep.session_origin_us,\n"
"                        session_length_s: rep.session_origin_us > 0\n"
"                            ? (rep.now_us - rep.session_origin_us) / 1e6 : null,\n"
"                        button_presses_ms: presses,\n"
"                        config: cfg,\n"
"                    };\n"
"                    fetch(GENERATOR_URL + '/reports', {\n"
"                        method: 'POST',\n"
"                        headers: { 'Content-Type': 'application/json' },\n"
"                        body: JSON.stringify(upload),\n"
"                    })\n"
"                    .then(r => r.ok ? r.json() : null)\n"
"                    .then(result => {\n"
"                        if (result && result.saved) {\n"
"                            box.textContent += '\\n\\n[uploaded to generator as ' + result.saved + ']';\n"
"                        }\n"
"                    })\n"
"                    .catch(err => {\n"
"                        box.textContent += '\\n\\n[generator upload failed: ' + err + ']';\n"
"                    });\n"
"                })\n"
"                .catch(err => showMessage('Report fetch failed: ' + err, 'error'));\n"
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
"            .then(result => {\n"
"                showMessage(result, 'success');\n"
"                // Schedule auto-fetch of /api/report 5 s after the parsed\n"
"                // session end. Cancels any previously-armed timer.\n"
"                if (reportTimer) { clearTimeout(reportTimer); reportTimer = null; }\n"
"                document.getElementById('reportBox').style.display = 'none';\n"
"                const durMs = parseConfigDurationMs(config);\n"
"                const waitMs = durMs + 5000;\n"
"                showMessage('Playing — report due in ' + Math.round(waitMs/1000) + ' s', 'info');\n"
"                reportTimer = setTimeout(fetchReport, waitMs);\n"
"            })\n"
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
"            statusDiv.style.display = 'block';\n"
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
"        loadExample(); // Load example config on page load\n"
"        refreshLedcDropdown(); // Fetch the generator's config list on page load\n"
"    </script>\n"
"</body>\n"
"</html>\n";

// HTTP Handler functions
static esp_err_t index_handler(httpd_req_t *req);
static esp_err_t upload_handler(httpd_req_t *req);
static esp_err_t stop_handler(httpd_req_t *req);
static esp_err_t example_handler(httpd_req_t *req);
static esp_err_t play_config_handler(httpd_req_t *req);
static esp_err_t report_handler(httpd_req_t *req);

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

    httpd_uri_t upload_uri = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = upload_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &upload_uri);

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

    httpd_uri_t report_uri = {
        .uri = "/api/report",
        .method = HTTP_GET,
        .handler = report_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(g_server_state.server, &report_uri);

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
