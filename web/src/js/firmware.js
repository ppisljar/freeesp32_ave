// Firmware Update tab — the browser side of the dual-boot OTA flow.
//
// State machine:
//   pick → hashing → ready → triggering → waiting-updater → uploading
//        → verifying/booting → done           (with error/retry branches)
//
// 1. Pick a .bin and a target (app / storage / cfgfs). On selection the file is
//    read and SHA-256'd in-browser (crypto.subtle) so the hash can be shown and
//    sent with the upload — the updater requires it for the app slot and uses it
//    to verify every byte before committing.
// 2. POST /api/ota → the main app hands WiFi creds to the updater (NVS) and
//    reboots into ota_1. Response: {ok, target, ssid_handoff}.
// 3. Poll GET / until the body looks like the updater's status line
//    ("OTA updater ready …") rather than the main app's HTML. If the updater
//    never appears on this origin it may have fallen back to SoftAP
//    (ESP32-AVE-Setup @ 192.168.4.1) on a different IP — which is cross-origin,
//    so we cannot poll it silently; we show manual recovery instructions.
// 4. Upload the file with XMLHttpRequest (for upload.onprogress) to
//    POST /update?sha256=<hex> (+ &part=storage|cfgfs for data targets).
//    Status codes are surfaced distinctly: 200 ok, 400 bad/missing hash,
//    413 too large, 422 hash mismatch, 500 write error.
// 5. App target: the updater auto-reboots into the new ota_0 — poll /api/state
//    until the main app answers. Data target: call GET /reboot, then poll back.

import { showMessage } from './util.js';

import { deviceFetch } from './devicefetch.js';
// ---- pure helpers (exported for unit testing) ------------------------------

// Lowercase hex string of an ArrayBuffer / TypedArray of bytes.
export function bufToHex(buf) {
    const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, '0');
    return s;
}

// Distinguish the minimal updater from the full main app by response body.
// The updater's GET / returns a one-line text status starting with
// "OTA updater ready (running: …)"; the main app's GET / returns the HTML page.
export function looksLikeUpdater(text) {
    return typeof text === 'string' && /OTA updater ready/i.test(text);
}

// Build the updater's /update URL for a target. App target → no `part`
// (resolves to the next app slot, ota_0). Data targets pass `part`. The
// sha256 is always appended. `base` is '' for same-origin (relative) or a full
// origin like 'http://192.168.4.1' for the SoftAP recovery copy.
export function buildUpdateUrl(base, target, hex) {
    let q = '';
    if (target === 'storage') q = 'part=storage&';
    else if (target === 'cfgfs') q = 'part=cfgfs&';
    q += 'sha256=' + hex;
    return base + '/update?' + q;
}

// ---- module state ----------------------------------------------------------

let selectedFile = null;
let fileHex = '';
let pendingRetry = null;   // function to re-run the failed phase, or null

const POLL_INTERVAL_MS = 2000;
const UPDATER_TIMEOUT_MS = 90000;   // reboot into updater + WiFi (re)join
const APP_TIMEOUT_MS = 90000;       // flash + reboot into new app

function $(id) { return document.getElementById(id); }
const sleep = ms => new Promise(r => setTimeout(r, ms));

// ---- banner / progress UI --------------------------------------------------

// kind: 'info' | 'working' | 'success' | 'error'
function banner(text, kind) {
    const el = $('fwBanner');
    if (!el) return;
    el.textContent = text;
    el.className = 'fw-banner fw-' + (kind || 'info');
    el.style.display = 'block';
}

function setProgress(frac) {
    const wrap = $('fwProgressWrap'), bar = $('fwProgressBar'), txt = $('fwProgressText');
    if (!wrap) return;
    wrap.style.display = 'block';
    const pct = Math.max(0, Math.min(100, Math.round(frac * 100)));
    if (bar) bar.style.width = pct + '%';
    if (txt) txt.textContent = pct + '%';
}

function hideProgress() {
    const wrap = $('fwProgressWrap');
    if (wrap) wrap.style.display = 'none';
}

function showSoftapHelp() {
    const el = $('fwSoftap');
    if (!el) return;
    const cmd = buildUpdateUrl('http://192.168.4.1', $('fwTarget').value, fileHex || '<sha256>');
    const isData = $('fwTarget').value !== 'app';
    el.innerHTML =
        '<strong>Updater not reachable on this network.</strong>' +
        '<p>The updater may have fallen back to its own access point. Join WiFi ' +
        '<code>ESP32-AVE-Setup</code> (password <code>entrain123</code>), then upload the ' +
        'image to <code>http://192.168.4.1</code> manually — for example:</p>' +
        '<pre class="fw-cmd">curl -X POST --data-binary @' +
        (selectedFile ? esc(selectedFile.name) : 'firmware.bin') +
        ' "' + esc(cmd) + '"' +
        (isData ? '\ncurl http://192.168.4.1/reboot' : '') + '</pre>' +
        '<p>The minimal updater serves only this endpoint over SoftAP; the rich UI ' +
        'is part of the main app and returns once the new firmware boots.</p>';
    el.style.display = 'block';
}

function hideSoftapHelp() {
    const el = $('fwSoftap');
    if (el) el.style.display = 'none';
}

function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// Show/hide the retry + reset buttons.
function setButtons({ start, retry, reset }) {
    const sEl = $('fwStart'), rEl = $('fwRetry'), zEl = $('fwReset');
    if (sEl) sEl.style.display = start ? '' : 'none';
    if (rEl) rEl.style.display = retry ? '' : 'none';
    if (zEl) zEl.style.display = reset ? '' : 'none';
}

// ---- network helpers -------------------------------------------------------

function fetchWithTimeout(url, ms) {
    const ctl = new AbortController();
    const t = setTimeout(() => ctl.abort(), ms);
    return fetch(url, { signal: ctl.signal, cache: 'no-store' })
        .finally(() => clearTimeout(t));
}

// Poll GET / until it answers with the updater's status line.
async function waitForUpdater(timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        try {
            const r = await fetchWithTimeout('/', 4000);
            if (r.ok) {
                const text = await r.text();
                if (looksLikeUpdater(text)) return true;
                // Still the main app (deferred reboot hasn't fired yet) — keep polling.
            }
        } catch (e) { /* device offline mid-reboot — expected */ }
        await sleep(POLL_INTERVAL_MS);
    }
    return false;
}

// Poll an app-only endpoint until the main app answers (updater 404s on it).
async function waitForMainApp(timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        try {
            const r = await fetchWithTimeout('/api/state', 4000);
            if (r.ok) return true;
        } catch (e) { /* offline mid-reboot — expected */ }
        await sleep(POLL_INTERVAL_MS);
    }
    return false;
}

// Upload the file via XHR so we get upload progress. Resolves with
// { status, body }; rejects only on transport error (network/abort/timeout).
function uploadFile(url, file, onProgress) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('POST', url);
        xhr.upload.onprogress = e => {
            if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total);
        };
        xhr.onload = () => resolve({ status: xhr.status, body: xhr.responseText || '' });
        xhr.onerror = () => reject(new Error('network'));
        xhr.ontimeout = () => reject(new Error('timeout'));
        xhr.onabort = () => reject(new Error('abort'));
        xhr.send(file);
    });
}

// ---- SHA-256 of the selected file ------------------------------------------

async function hashFile(file) {
    const buf = await file.arrayBuffer();
    const digest = await crypto.subtle.digest('SHA-256', buf);
    return bufToHex(new Uint8Array(digest));
}

async function onFilePicked() {
    hideSoftapHelp();
    pendingRetry = null;
    setButtons({ start: true, retry: false, reset: false });
    const f = $('fwFile').files[0];
    selectedFile = f || null;
    fileHex = '';
    const hashEl = $('fwHash');
    const startEl = $('fwStart');
    if (!f) {
        if (hashEl) hashEl.textContent = '';
        if (startEl) startEl.disabled = true;
        return;
    }
    if (startEl) startEl.disabled = true;
    if (hashEl) hashEl.textContent = 'Computing SHA-256…';
    banner('Hashing image…', 'working');
    try {
        fileHex = await hashFile(f);
        if (hashEl) {
            hashEl.innerHTML = '<span class="fw-hash-label">SHA-256:</span> ' +
                '<code class="fw-hash-val">' + esc(fileHex) + '</code>' +
                '<span class="fw-hash-size">(' + f.size.toLocaleString() + ' bytes)</span>';
        }
        banner('Ready to update. Click "Start Update" to begin.', 'info');
        if (startEl) startEl.disabled = false;
    } catch (e) {
        if (hashEl) hashEl.textContent = '';
        banner('Could not hash the file: ' + e.message, 'error');
    }
}

// ---- phase: distinct failure messages per /update status -------------------

function uploadErrorMessage(status, body) {
    const detail = body ? (' — ' + body.trim().slice(0, 200)) : '';
    switch (status) {
        case 400: return 'Update rejected (400): missing or malformed SHA-256 hash' + detail +
            '. The app slot requires a valid hash.';
        case 413: return 'Update rejected (413): the image is larger than the target partition' + detail + '.';
        case 422: return 'Update rejected (422): SHA-256 mismatch — the uploaded bytes did not match the ' +
            'expected hash' + detail + '. The image was NOT flashed; try re-selecting the file.';
        case 500: return 'Update failed (500): the updater could not write the partition' + detail +
            '. Nothing was committed; you can retry.';
        default:  return 'Update failed (HTTP ' + status + ')' + detail + '.';
    }
}

// ---- orchestration ---------------------------------------------------------

function lockForm(locked) {
    const ids = ['fwFile', 'fwTarget'];
    for (const id of ids) { const el = $(id); if (el) el.disabled = locked; }
}

async function start() {
    if (!selectedFile || !fileHex) {
        banner('Pick a .bin file first.', 'error');
        return;
    }
    hideSoftapHelp();
    lockForm(true);
    setButtons({ start: false, retry: false, reset: false });
    pendingRetry = null;
    await runFromTrigger();
}

// Phase 1: trigger the reboot into the updater.
async function runFromTrigger() {
    banner('Requesting reboot into the updater…', 'working');
    let ack;
    try {
        // The device writes flash before replying; well past the default deadline.
        const r = await deviceFetch('/api/ota', { method: 'POST', timeoutMs: 30000 });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        ack = await r.json();
    } catch (e) {
        banner('Could not start the updater: ' + e.message +
            '. The device may not support OTA, or the request failed.', 'error');
        failTo(runFromTrigger);
        return;
    }
    if (!ack || !ack.ok) {
        banner('The device declined the OTA request.', 'error');
        failTo(runFromTrigger);
        return;
    }
    if (ack.ssid_handoff === false) {
        banner('Rebooting into the updater… (no WiFi creds handed off — it will come up on ' +
            'its own access point ESP32-AVE-Setup @ 192.168.4.1)', 'working');
    } else {
        banner('Rebooting into the updater…', 'working');
    }
    await runFromWaitUpdater();
}

// Phase 2: wait for the updater to answer.
async function runFromWaitUpdater() {
    banner('Waiting for the updater to come online…', 'working');
    const ok = await waitForUpdater(UPDATER_TIMEOUT_MS);
    if (!ok) {
        banner('The updater did not respond on this network within ' +
            Math.round(UPDATER_TIMEOUT_MS / 1000) + 's.', 'error');
        showSoftapHelp();
        failTo(runFromWaitUpdater);
        return;
    }
    banner('Updater online. Uploading image…', 'working');
    await runFromUpload();
}

// Phase 3: upload + verify/flash.
async function runFromUpload() {
    const isApp = $('fwTarget').value === 'app';
    const url = buildUpdateUrl('', $('fwTarget').value, fileHex);
    setProgress(0);
    let res;
    let reached100 = false;
    try {
        res = await uploadFile(url, selectedFile, frac => {
            setProgress(frac);
            banner('Uploading… ' + Math.round(frac * 100) + '%', 'working');
            if (frac >= 0.999 && !reached100) {
                reached100 = true;
                banner('Upload complete — verifying & flashing…', 'working');
            }
        });
    } catch (e) {
        // Transport error. For an app target the updater reboots the instant the
        // image verifies, which can drop the connection right at the end — if we
        // already sent the whole file, treat that as "probably flashing" and go
        // wait for the main app. A data target never reboots, so a drop is a
        // genuine failure.
        if (isApp && reached100) {
            banner('Connection closed after upload — the updater is likely flashing & rebooting…', 'working');
            await runFromBootApp(true);
            return;
        }
        hideProgress();
        banner('Upload connection failed (' + e.message + '). The updater stayed put; you can retry.', 'error');
        failTo(runFromUpload);
        return;
    }

    if (res.status !== 200) {
        hideProgress();
        banner(uploadErrorMessage(res.status, res.body), 'error');
        // 400/413/422/500: the updater is still running → retry re-uploads.
        failTo(runFromUpload);
        return;
    }

    setProgress(1);
    if (isApp) {
        // Updater auto-reboots into the freshly flashed ota_0.
        await runFromBootApp(false);
    } else {
        // Data partition flashed; updater stays put → tell it to boot the app.
        banner('Image flashed. Rebooting into the main app…', 'working');
        try {
            await fetchWithTimeout('/reboot', 5000);
        } catch (e) { /* the reboot itself drops the connection — expected */ }
        await runFromBootApp(false);
    }
}

// Phase 4: wait for the main app to return.
async function runFromBootApp(inferred) {
    banner(inferred
        ? 'Booting new firmware (inferred from the dropped connection)…'
        : 'Booting new firmware…', 'working');
    const ok = await waitForMainApp(APP_TIMEOUT_MS);
    if (!ok) {
        banner('The main app did not come back within ' + Math.round(APP_TIMEOUT_MS / 1000) +
            's. The new image may be bad — re-upload a known-good image via the updater, ' +
            'or recover over SoftAP / USB.', 'error');
        showSoftapHelp();
        failTo(runFromBootApp.bind(null, inferred));
        return;
    }
    hideProgress();
    banner('Update complete — the device is running the new firmware.', 'success');
    lockForm(false);
    setButtons({ start: false, retry: false, reset: true });
    // Offer a reload so the page picks up any new assets.
    const z = $('fwReset');
    if (z) { z.textContent = 'Reload page'; z.onclick = () => location.reload(); }
    showMessage('Firmware update complete.', 'success');
}

// Mark a phase as failed: stash the resume fn and show Retry + Start Over.
function failTo(resumeFn) {
    pendingRetry = resumeFn;
    lockForm(false);
    setButtons({ start: false, retry: true, reset: true });
    const z = $('fwReset');
    if (z) { z.textContent = 'Start Over'; z.onclick = resetAll; }
}

function onRetry() {
    if (!pendingRetry) return;
    const fn = pendingRetry;
    pendingRetry = null;
    hideSoftapHelp();
    lockForm(true);
    setButtons({ start: false, retry: false, reset: false });
    fn();
}

function resetAll() {
    selectedFile = null;
    fileHex = '';
    pendingRetry = null;
    const fileEl = $('fwFile');
    if (fileEl) fileEl.value = '';
    const hashEl = $('fwHash');
    if (hashEl) hashEl.textContent = '';
    hideProgress();
    hideSoftapHelp();
    const b = $('fwBanner');
    if (b) b.style.display = 'none';
    lockForm(false);
    setButtons({ start: true, retry: false, reset: false });
    const s = $('fwStart');
    if (s) s.disabled = true;
}

// ---- running firmware version ----------------------------------------------

// Fetch GET /api/version and render it into #fwVersion. Fails gracefully:
// any network/parse error shows "version unavailable" rather than throwing.
async function loadVersion() {
    const el = $('fwVersion');
    if (!el) return;
    try {
        const r = await deviceFetch('/api/version', { cache: 'no-store' });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        const v = await r.json();
        const ver = v.version || 'unknown';
        const built = [v.compile_date, v.compile_time].filter(Boolean).join(' ');
        const part = v.partition || '?';
        const idf = v.idf_ver || '?';
        el.innerHTML =
            '<span class="fw-version-main">Current firmware: ' + esc(ver) +
            (built ? ' (built ' + esc(built) + ')' : '') + '</span>' +
            '<span class="fw-version-sub">running: ' + esc(part) + ' · IDF ' + esc(idf) + '</span>';
    } catch (e) {
        el.textContent = 'Current firmware: version unavailable';
    }
}

export function firmwareInit() {
    loadVersion();
    const fileEl = $('fwFile');
    if (fileEl) fileEl.addEventListener('change', onFilePicked);
    const startEl = $('fwStart');
    if (startEl) startEl.addEventListener('click', start);
    const retryEl = $('fwRetry');
    if (retryEl) retryEl.addEventListener('click', onRetry);
    const resetEl = $('fwReset');
    if (resetEl) resetEl.addEventListener('click', resetAll);
}
