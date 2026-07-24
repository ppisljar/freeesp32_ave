// Diagnostics tab — serial-parity over WiFi. Backed by:
//   GET  /api/state            → the "diag" block (reset reason, uptime, heap, coredump)
//   GET  /api/logs[?clear=1]    → buffered ESP_LOGx text (the serial stream)
//   GET  /api/coredump          → the stored crash dump (octet-stream, via <a download>)
//   POST /api/coredump/erase    → discard the stored dump
//   POST /api/reboot            → restart the device (the "Reboot Device" button)
//
// Auto-refresh runs only while this tab is visible, to avoid polling the device
// in the background when the user is on another page.

import { showMessage } from './util.js';

const POLL_MS = 3000;
let autoTimer = null;

function $(id) { return document.getElementById(id); }
const sleep = ms => new Promise(r => setTimeout(r, ms));

function fmtBytes(n) {
    if (n == null) return '—';
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    return (n / (1024 * 1024)).toFixed(2) + ' MB';
}

function fmtUptime(ms) {
    if (ms == null) return '—';
    let s = Math.floor(ms / 1000);
    const d = Math.floor(s / 86400); s -= d * 86400;
    const h = Math.floor(s / 3600);  s -= h * 3600;
    const m = Math.floor(s / 60);    s -= m * 60;
    const parts = [];
    if (d) parts.push(d + 'd');
    if (h || d) parts.push(h + 'h');
    parts.push(m + 'm', s + 's');
    return parts.join(' ');
}

// Reset reasons that indicate an abnormal (crash) reboot → flag them red.
const CRASH_REASONS = new Set(['PANIC', 'INT_WDT', 'TASK_WDT', 'WDT', 'BROWNOUT']);

async function loadState() {
    let st;
    try {
        st = await fetch('/api/state', { cache: 'no-store' }).then(r => r.json());
    } catch (e) {
        $('diagHealth').textContent = '(device unreachable)';
        return;
    }
    $('diagHealth').textContent = '';
    const d = st.diag || {};

    const reason = d.reset_reason || 'UNKNOWN';
    const crash = CRASH_REASONS.has(reason);
    const rEl = $('diagReset');
    rEl.textContent = reason;
    rEl.className = 'diag-reason ' + (crash ? 'diag-bad' : 'diag-ok');

    $('diagUptime').textContent   = fmtUptime(d.uptime_ms);
    $('diagHeap').textContent     = fmtBytes(d.free_heap);
    $('diagPsram').textContent    = fmtBytes(d.free_psram);
    $('diagLogBytes').textContent = fmtBytes(d.log_bytes);

    // Core-dump card.
    const cd = d.coredump || { present: false };
    const card = $('diagCoredump');
    if (cd.present) {
        card.className = 'diag-card diag-card-alert';
        $('diagCdText').innerHTML =
            '<strong>A crash core dump is stored</strong> (' + fmtBytes(cd.size) +
            '). Download it and symbolize with the ELF of the running build:';
        $('diagCdActions').style.display = '';
        $('diagCdCmd').textContent =
            'espcoredump.py info_corefile -c coredump.bin build/esp32_audioplayer.elf';
    } else {
        card.className = 'diag-card';
        $('diagCdText').textContent =
            'No crash core dump stored. (If core-dump-to-flash is not enabled in this ' +
            'build, dumps are never captured — see the diagnostics plan, Layer 3.)';
        $('diagCdActions').style.display = 'none';
    }

    // Subsystem health. Boot is non-fatal: each subsystem reports ok/failed, so
    // the device always comes up (in recovery mode) even if audio/LED/etc. can't
    // init. Warn prominently about anything degraded; show a chip per subsystem.
    const hEl = $('diagSubsystems');
    if (hEl) {
        const health = d.health || {};
        const keys = Object.keys(health);
        const failed = keys.filter(k => health[k] && health[k].ok === false);
        let html = '';
        if (failed.length) {
            html += '<div class="diag-card diag-card-alert"><strong>⚠ ' + failed.length +
                ' subsystem' + (failed.length > 1 ? 's' : '') + ' degraded</strong> — the device ' +
                'booted in recovery mode so you can fix it from the web UI. Check wiring and the ' +
                'Settings page (codec/pins/LED backend):<ul style="margin:6px 0 0 18px;">';
            failed.forEach(k => { html += '<li><b>' + esc(k) + '</b>: ' + esc(health[k].msg || 'failed') + '</li>'; });
            html += '</ul></div>';
        }
        if (keys.length) {
            html += '<div class="diag-health-grid">';
            keys.forEach(k => {
                const ok = health[k].ok !== false;
                html += '<span class="diag-chip ' + (ok ? 'diag-chip-ok' : 'diag-chip-bad') + '" title="' +
                    esc(health[k].msg || (ok ? 'ok' : 'failed')) + '">' + (ok ? '✓' : '✗') + ' ' + esc(k) + '</span>';
            });
            html += '</div>';
        }
        hEl.innerHTML = html;
    }
}

function esc(x) {
    return String(x == null ? '' : x).replace(/[&<>"]/g, c =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

async function loadLogs() {
    const pre = $('diagLog');
    try {
        const txt = await fetch('/api/logs', { cache: 'no-store' }).then(r => r.text());
        // Preserve the user's scroll position unless they were already at the bottom.
        const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 24;
        pre.textContent = txt || '(log buffer empty)';
        if (atBottom) pre.scrollTop = pre.scrollHeight;
    } catch (e) {
        pre.textContent = '(logs unreachable)';
    }
}

async function refreshAll() { await Promise.all([loadState(), loadLogs()]); }

async function clearLogs() {
    try { await fetch('/api/logs?clear=1', { cache: 'no-store' }); } catch (e) { /* ignore */ }
    await loadLogs();
    showMessage('Log buffer cleared.', 'success');
}

function copyLogs() {
    const txt = $('diagLog').textContent || '';
    if (!navigator.clipboard) { showMessage('Clipboard unavailable.', 'error'); return; }
    navigator.clipboard.writeText(txt).then(
        () => showMessage('Logs copied to clipboard.', 'success'),
        () => showMessage('Copy failed.', 'error'));
}

async function eraseCoredump() {
    if (!confirm('Erase the stored crash core dump? Download it first if you still need it.')) return;
    try {
        const r = await fetch('/api/coredump/erase', { method: 'POST' });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        showMessage('Core dump erased.', 'success');
    } catch (e) {
        showMessage('Erase failed: ' + e.message, 'error');
    }
    await loadState();
}

async function rebootDevice() {
    if (!confirm('Reboot the device now? Audio and LED output will stop for ~10 s.')) return;
    try {
        await fetch('/api/reboot', { method: 'POST' });
    } catch (e) { /* the reboot drops the connection — expected */ }
    showMessage('Rebooting…', 'info');
    $('diagHealth').textContent = 'Rebooting — waiting for the device to return…';
    await sleep(3000);
    const deadline = Date.now() + 60000;
    while (Date.now() < deadline) {
        try {
            const r = await fetch('/api/state', { cache: 'no-store' });
            if (r.ok) {
                showMessage('Device back online.', 'success');
                await refreshAll();
                return;
            }
        } catch (e) { /* still down mid-reboot */ }
        await sleep(2000);
    }
    $('diagHealth').textContent = 'Device did not return within 60 s — check power/network.';
    showMessage('Device did not return within 60 s.', 'error');
}

function setAuto(on) {
    if (on && !autoTimer) autoTimer = setInterval(refreshAll, POLL_MS);
    if (!on && autoTimer) { clearInterval(autoTimer); autoTimer = null; }
}

// --- runtime log levels (GET/POST /api/loglevels) --------------------------
// Loaded once when the tab opens (NOT in the auto-refresh poll, so it never
// clobbers a dropdown mid-selection). Each category maps to a set of ESP_LOG
// tags; changing a level POSTs {category: level} and applies it immediately.
async function loadLogLevels() {
    const box = $('diagLogLevels');
    if (!box) return;
    let data;
    try {
        data = await fetch('/api/loglevels', { cache: 'no-store' }).then(r => r.ok ? r.json() : null);
    } catch (e) { box.textContent = '(log levels unavailable)'; return; }
    if (!data || !Array.isArray(data.categories)) { box.textContent = '(log levels unavailable)'; return; }
    const levels = data.levels || ['none', 'error', 'warn', 'info', 'debug', 'verbose'];
    const optionsFor = (cur) => levels.map(l =>
        '<option value="' + esc(l) + '"' + (l === cur ? ' selected' : '') + '>' + esc(l) + '</option>').join('');
    let html = '<div class="diag-loglevels">';
    for (const c of data.categories) {
        const tags = Array.isArray(c.tags) ? c.tags.join(', ') : '';
        html += '<label class="diag-llrow">' +
                '<span class="diag-llname" title="' + esc(tags) + '">' + esc(c.name) + '</span>' +
                '<select data-cat="' + esc(c.name) + '">' + optionsFor(c.level) + '</select></label>';
    }
    html += '</div>';
    box.innerHTML = html;
    box.querySelectorAll('select[data-cat]').forEach(sel =>
        sel.addEventListener('change', () => setLogLevel(sel.getAttribute('data-cat'), sel.value)));
}

async function setLogLevel(cat, level) {
    try {
        const body = {}; body[cat] = level;
        const r = await fetch('/api/loglevels', {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body),
        });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        showMessage('Log level: ' + cat + ' → ' + level, 'success');
    } catch (e) {
        showMessage('Failed to set log level: ' + (e.message || e), 'error');
    }
}

function onTabVisible() {
    const active = location.hash.replace('#', '') === 'diagnostics';
    if (active) {
        refreshAll();
        loadLogLevels();   // load once on tab open (kept out of the poll)
        setAuto($('diagAuto') && $('diagAuto').checked);
    } else {
        setAuto(false);   // stop polling when the user leaves the tab
    }
}

export function diagnosticsInit() {
    const on = (id, ev, fn) => { const el = $(id); if (el) el.addEventListener(ev, fn); };
    on('diagRefresh', 'click', refreshAll);
    on('diagClear',   'click', clearLogs);
    on('diagCopy',    'click', copyLogs);
    on('diagErase',   'click', eraseCoredump);
    on('diagReboot',  'click', rebootDevice);
    on('diagAuto',    'change', () => setAuto($('diagAuto').checked));

    on('diagRefresh', 'click', loadLogLevels);   // Refresh also re-reads levels
    window.addEventListener('hashchange', onTabVisible);
    // If the page loads directly on #diagnostics, kick off a refresh.
    if (location.hash.replace('#', '') === 'diagnostics') { refreshAll(); loadLogLevels(); }
}
