// Test-config panel: load/edit/play .led configs, parse them client-side to
// resolve per-press parameter snapshots, and display the session report.
import { appConfig, showMessage } from './util.js';
import { getCurrentConfigName } from './configstore.js';
import { presentGeneratedReport, getReportMeta, getCurrentReportName, refreshLogSection } from './reportstore.js';
import { startLogCapture, finalizeLogCapture, cancelLogCapture } from './logcapture.js';
import { clogI } from './clientlog.js';
// Interpolation evaluators are shared with the Generator (gen/interp.js) so the
// report path and the Generator agree on the math. Behavior is identical to the
// previous in-file definitions.
import {
    parseValueInterp, audioStateAtTime, ledStateAtTime,
} from './gen/interp.js';
// Play through the SAME path as the Generator (parse → playSession) so the Home
// Play button handles speech (S) rows and push:// BG, not just a raw device POST.
import { parse } from './gen/parse.js';
import { playSession, cancelPendingPlay } from './gen/play.js';
import { stopBg } from './gen/transport.js';

import { deviceFetch } from './devicefetch.js';
// Report-on-session-end. Rather than guessing when a session finishes, we
// fetch the report when the live-control poll detects the timeline's
// running→stopped edge (fired as a 'sessionended' window event). A generous
// fallback timer covers the rare session shorter than one poll interval.
let awaitingReport = false;
let reportFallbackTimer = null;

function armReport(fallbackMs) {
    awaitingReport = true;
    if (reportFallbackTimer) clearTimeout(reportFallbackTimer);
    reportFallbackTimer = setTimeout(triggerReport, fallbackMs);
    // Hide the previous report + its editor until the new one is generated, so
    // stale title/comments don't linger over a fresh session.
    document.getElementById('reportBox').style.display = 'none';
    const ed = document.getElementById('reportEditor');
    if (ed) ed.style.display = 'none';
}

function triggerReport() {
    if (!awaitingReport) return;
    awaitingReport = false;
    if (reportFallbackTimer) { clearTimeout(reportFallbackTimer); reportFallbackTimer = null; }
    fetchReport();
}

// The timeline just stopped (finished or terminated early). If we're expecting
// a report, fetch it now — small settle delay so the device finalizes state.
window.addEventListener('sessionended', () => {
    if (awaitingReport) setTimeout(triggerReport, 500);
});

export function loadExample() {
    deviceFetch('/api/example')
        .then(response => response.text())
        .then(data => {
            document.getElementById('exampleConfig').value = data;
        })
        .catch(error => showMessage('Error loading example: ' + error, 'error'));
}

export function stopConfig() {
    clogI('session', 'STOP pressed');
    // Cancel a pending pre-roll countdown so STOP works even before playback
    // has actually begun.
    cancelPendingPlay();
    // Abort any in-flight browser BG push FIRST, else it keeps uploading and the
    // device keeps playing the background track after the timeline is stopped.
    stopBg();
    deviceFetch('/api/stop', { method: 'POST' })
        .then(response => response.text())
        .then(result => {
            showMessage(result + ' — report when the session ends', 'success');
            // The stop drops the timeline; the poll's running→stopped edge will
            // trigger the report. Fallback in case the edge is missed.
            armReport(8000);
        })
        .catch(error => showMessage('Error: ' + error, 'error'));
}

// Parse the .led config text and return the highest entry timestamp in ms.
// Format: LED entries start with a number (time), audio entries start with 'A'.
// Lines starting with '#' are comments and skipped. Returns 0 if none.
function parseConfigDurationMs(text) {
    let maxMs = 0;
    for (const raw of text.split('\n')) {
        const line = raw.replace(/#.*$/, '').trim();
        if (!line) continue;
        const tok = line.split(/\s+/);
        let t = NaN;
        if (tok[0] === 'A' && tok.length > 1) t = parseInt(tok[1], 10);
        else if (tok[0] === 'BG') continue; // BG has no time field
        else t = parseInt(tok[0], 10);
        if (!isNaN(t) && t > maxMs) maxMs = t;
    }
    return maxMs;
}

// ---- Config parsing + per-press state resolution -----------------
// Mirrors the device-side interpolation: for each parameter on each
// entry, find the active entry at time T and (if the next entry has
// a '>' or '*' marker on that parameter) interpolate live value.
// (parseValueInterp / lerp / quad / interpField / audioStateAtTime /
//  ledStateAtTime now live in gen/interp.js and are imported above.)
function parseConfigStructured(text) {
    const led = [], audio = [];
    for (const raw of text.split('\n')) {
        const line = raw.replace(/#.*$/, '').trim();
        if (!line) continue;
        const t = line.split(/\s+/);
        if (t[0] === 'A') {
            if (t.length < 7) continue;
            audio.push({
                time: parseInt(t[1], 10),
                freq: parseValueInterp(t[2]),
                pan:  parseValueInterp(t[3]),
                vol:  parseValueInterp(t[4]),
                mod:  parseValueInterp(t[5]),
                channel: parseInt(t[6], 10),
            });
        } else if (t[0] === 'BG') {
            continue;
        } else if (t.length === 8) {
            // 8-field LED (canonical): time freq duty bright R G B mask
            led.push({
                time: parseInt(t[0], 10),
                freq: parseValueInterp(t[1]),
                duty: parseValueInterp(t[2]),
                brightness: parseValueInterp(t[3]),
                r:    parseValueInterp(t[4]),
                g:    parseValueInterp(t[5]),
                b:    parseValueInterp(t[6]),
                mask: parseInt(t[7], 10),
            });
        } else if (t.length === 5) {
            // 5-field legacy: time freq duty brightness channel
            const ch = parseInt(t[4], 10);
            led.push({
                time: parseInt(t[0], 10),
                mask: ch === 0 ? 0xFF : (1 << (ch - 1)),
                freq: parseValueInterp(t[1]),
                duty: parseValueInterp(t[2]),
                brightness: parseValueInterp(t[3]),
                r: { v: 255, interp: 'none' },
                g: { v: 255, interp: 'none' },
                b: { v: 255, interp: 'none' },
            });
        }
    }
    return { led: led, audio: audio };
}
function formatPressSnapshot(parsed, tMs) {
    const lines = [];
    for (let ch = 1; ch <= 16; ch++) {
        const s = audioStateAtTime(parsed.audio, tMs, ch);
        if (!s) continue;
        lines.push('  AUDIO[ch=' + ch + '] freq=' + s.freq.toFixed(2) +
                   'Hz pan=' + s.pan.toFixed(0) +
                   ' vol=' + s.vol.toFixed(0) +
                   ' mod=' + s.mod.toFixed(1));
    }
    for (let ch = 0; ch < 8; ch++) {
        const s = ledStateAtTime(parsed.led, tMs, ch);
        if (!s) continue;
        lines.push('  LED[ch=' + ch + '] freq=' + s.freq.toFixed(2) +
                   'Hz duty=' + s.duty.toFixed(0) +
                   '% bri=' + s.bri.toFixed(0) +
                   '% RGB=(' + s.r.toFixed(0) + ',' + s.g.toFixed(0) + ',' + s.b.toFixed(0) + ')');
    }
    return lines.length ? lines.join('\n') : '  (no active channels at this time)';
}

function fetchReport() {
    deviceFetch('/api/report')
        .then(r => r.json())
        .then(rep => {
            const sessSec = rep.session_origin_us > 0
                ? ((rep.now_us - rep.session_origin_us) / 1e6).toFixed(1)
                : '—';
            const presses = rep.button_presses_ms || [];
            const cfg = rep.config || '';
            const parsed = cfg ? parseConfigStructured(cfg) : null;

            let out = '=== SESSION REPORT ===\n';
            out += 'Session length so far: ' + sessSec + ' s\n\n';
            out += '--- Button press snapshots ---\n';
            if (!presses.length) {
                out += '(no button presses recorded)\n';
            } else if (!parsed) {
                out += '(' + presses.length + ' press(es), but no config to resolve params): ' +
                       presses.join(', ') + '\n';
            } else {
                for (let i = 0; i < presses.length; i++) {
                    out += '\n@ +' + presses[i] + 'ms (press ' + (i+1) + '):\n';
                    out += formatPressSnapshot(parsed, presses[i]) + '\n';
                }
            }
            out += '\n--- Last loaded config ---\n';
            out += cfg || '(no config in memory)';

            // Hand the machine body to the report editor: it shows the Title +
            // Comments editor (title defaulted to the session name), renders the
            // composed report in the box, and auto-saves under a
            // session-name+timestamp filename per the Settings destination.
            const box = document.getElementById('reportBox');
            presentGeneratedReport(out, getCurrentConfigName());
            const meta = getReportMeta();

            // Stop the background log capture and store the accumulated device
            // log under this report's filename, then refresh the editor's log
            // row so the View/Download buttons light up.
            finalizeLogCapture(getCurrentReportName())
                .then(() => refreshLogSection())
                .catch(() => {});

            // Best-effort upload to the generator. Failure is
            // non-fatal — the local display always succeeds first.
            const upload = {
                config_name: getCurrentConfigName(),
                title: meta.title,
                comments: meta.comments,
                session_origin_us: rep.session_origin_us,
                session_length_s: rep.session_origin_us > 0
                    ? (rep.now_us - rep.session_origin_us) / 1e6 : null,
                button_presses_ms: presses,
                config: cfg,
            };
            fetch(appConfig.generatorUrl + '/reports', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(upload),
            })
            .then(r => r.ok ? r.json() : null)
            .then(result => {
                if (result && result.saved) {
                    box.textContent += '\n\n[uploaded to generator as ' + result.saved + ']';
                }
            })
            .catch(err => {
                box.textContent += '\n\n[generator upload failed: ' + err + ']';
            });
        })
        .catch(err => {
            // The session ended but we couldn't fetch the report — stop the
            // background log poller so it doesn't run forever (no report name to
            // link the captured log to in this failure path).
            cancelLogCapture();
            showMessage('Report fetch failed: ' + err, 'error');
        });
}

export function playConfig() {
    const config = document.getElementById('exampleConfig').value.trim();
    if (!config) {
        showMessage('Please enter a config to play', 'error');
        return;
    }

    // Optional pre-roll countdown (seconds) so the user can put on the glasses
    // and headphones before playback starts. Defaults to 60 s via the input.
    const delayEl = document.getElementById('playDelay');
    const delaySec = Math.max(0, parseInt(delayEl && delayEl.value, 10) || 0);

    // Parse to the shared model and play via the same code path as the Generator
    // (device timeline + speech merge + push:// BG). playSession handles its own
    // success/error messaging (including the live countdown).
    const { doc } = parse(config);
    // Begin capturing the device log in the background (runs on any tab) so the
    // full session log — not just the last 32 KB the device ring holds — is
    // preserved and linked to the report generated at session end.
    startLogCapture();
    clogI('session', 'Home Play: config="' + (getCurrentConfigName() || '(unsaved)') + '" ' + config.length + ' bytes, delay=' + delaySec + 's');
    playSession(doc, delaySec);

    // Report-on-session-end stays a Home feature: the report fires when the poll
    // detects the timeline stop; the fallback (parsed duration + 10 s) only
    // covers a session too short for the 1 s poll to catch the edge. Add the
    // pre-roll so the fallback timer doesn't fire mid-countdown.
    const durMs = parseConfigDurationMs(config);
    armReport(delaySec * 1000 + durMs + 10000);
    if (delaySec > 0) return;                 // playSession shows the live countdown message
    showMessage('Playing — report when the session ends', 'info');
}

export function clearConfig() {
    document.getElementById('exampleConfig').value = '';
    showMessage('Config cleared', 'info');
}

// File-picker → textarea. Reads the chosen file CLIENT-SIDE via FileReader
// (no backend round-trip) and drops the contents into the exampleConfig
// textarea. User then clicks PLAY Config to execute. Uploading and executing
// are two distinct steps so the user can review / edit the loaded text first.
export function bindFileInput() {
    document.getElementById('configFile').addEventListener('change', function(e) {
        const f = e.target.files && e.target.files[0];
        if (!f) return;
        const reader = new FileReader();
        reader.onload = (ev) => {
            document.getElementById('exampleConfig').value = ev.target.result;
            document.getElementById('loadedFilename').textContent = '(loaded: ' + f.name + ')';
            showMessage('Loaded ' + f.name + ' (' + ev.target.result.length + ' bytes) — click PLAY to execute', 'success');
        };
        reader.onerror = () => showMessage('Failed to read ' + f.name, 'error');
        reader.readAsText(f);
        // Reset so picking the same file twice re-fires `change`.
        e.target.value = '';
    });
}
