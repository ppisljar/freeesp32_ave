// Community tab — browse .ledc sessions published in the GitHub repo and load
// them into the editor or save them to the device.
//
// BROWSER-ONLY, no ESP32 proxy: raw.githubusercontent.com and api.github.com
// both send `Access-Control-Allow-Origin: *`, and the repo is public, so the
// browser fetches the index + each .ledc cross-origin directly. (The app is
// served over HTTP from the device and fetches HTTPS from GitHub — allowed;
// only HTTPS→HTTP is blocked as mixed content.) It needs internet IN THE
// BROWSER — works on a normal WiFi (station mode), not on the device SoftAP.
import { setText } from './generator.js';
import { saveConfigText } from '../configstore.js';
import { showMessage } from '../util.js';

import { deviceFetch } from '../devicefetch.js';
const REPO   = 'ppisljar/freeesp32_ave';
const BRANCH = 'main';
// Fully-qualified `refs/heads/<branch>` form (what GitHub's "Raw" button emits).
const RAW_BASE = `https://raw.githubusercontent.com/${REPO}/refs/heads/${BRANCH}`;
const INDEX_URL = `${RAW_BASE}/sessions/community.json`;
const rawUrl = (path, file) => `${RAW_BASE}/${path}/${file}`;

let loaded = false;

function esc(x) {
    return String(x == null ? '' : x).replace(/[&<>"]/g, c =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

// FNV-1a (32-bit) over the UTF-8 bytes of the whitespace-trailing-normalized text.
// crypto.subtle is unavailable on this HTTP origin, so this pure-JS hash is used to
// tell whether the device's copy of a session matches the online one. Must stay
// byte-for-byte identical to the Python generator in sessions/community.json.
function contentHash(text) {
    const norm = String(text).replace(/\r\n/g, '\n').replace(/\s+$/, '');
    const bytes = new TextEncoder().encode(norm);   // UTF-8, matches Python .encode('utf-8')
    let h = 0x811c9dc5;
    for (let i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193); }
    return (h >>> 0).toString(16).padStart(8, '0');
}

function setBadge(root, file, cls, text) {
    const el = root.querySelector('.comm-card[data-file="' + (window.CSS ? CSS.escape(file) : file) + '"] [data-badge]');
    if (el) { el.className = 'comm-badge ' + cls; el.textContent = text; }
}

// Presence (GET /api/configs) + per-present-file freshness (device content hash vs
// the index hash). Runs after the list renders so badges fill in progressively.
async function refreshStatus(sessions, path, root) {
    let devSet = new Set();
    try {
        const j = await deviceFetch('/api/configs').then(r => r.json());
        devSet = new Set(j.files || []);
    } catch (e) { return; }   // device unreachable → leave badges neutral
    for (const s of sessions) {
        if (!devSet.has(s.file)) { setBadge(root, s.file, 'comm-badge-avail', 'available'); continue; }
        // Present on device — compare content unless the index has no hash.
        if (!s.hash) { setBadge(root, s.file, 'comm-badge-ondev', 'on device'); continue; }
        setBadge(root, s.file, 'comm-badge-check', 'checking…');
        try {
            const devTxt = await deviceFetch('/api/configs/' + encodeURIComponent(s.file)).then(r => r.text());
            const same = contentHash(devTxt) === s.hash;
            setBadge(root, s.file, same ? 'comm-badge-ok' : 'comm-badge-update',
                     same ? '✓ on device' : '⬆ update available');
        } catch (e) {
            setBadge(root, s.file, 'comm-badge-ondev', 'on device');
        }
    }
}

// Called by the generator when the Community subtab is shown. Fetches once.
export function communityShow() {
    const root = document.getElementById('genCommunityView');
    if (!root || loaded) return;
    root.innerHTML = '<div class="comm-msg">Loading community sessions from GitHub…</div>';
    fetch(INDEX_URL, { cache: 'no-cache' })
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(idx => { renderList(root, idx); loaded = true; })
        .catch(err => {
            root.innerHTML =
                '<div class="comm-msg comm-err">Could not load community sessions: ' + esc(err.message) +
                '.<br>This needs internet access <b>in your browser</b> — it works on your normal WiFi ' +
                '(station mode), but not when your browser is joined to the device\'s SoftAP (no internet). ' +
                '<button id="commRetry">Retry</button></div>';
            const b = document.getElementById('commRetry');
            if (b) b.addEventListener('click', () => { loaded = false; communityShow(); });
        });
}

function renderList(root, idx) {
    const sessions = (idx && idx.sessions) || [];
    const path = (idx && idx.path) || 'sessions/library';
    root.innerHTML = '';
    const bar = document.createElement('div');
    bar.className = 'comm-bar';
    bar.innerHTML = `<span>${sessions.length} community sessions · ${esc(REPO)}@${esc(BRANCH)}</span>` +
                    `<button id="commRefresh">↻ Refresh</button>`;
    root.appendChild(bar);
    bar.querySelector('#commRefresh').addEventListener('click', () => { loaded = false; communityShow(); });

    sessions.forEach(s => {
        const card = document.createElement('div');
        card.className = 'comm-card';
        card.dataset.file = s.file;
        card.innerHTML =
            `<div class="comm-title">${esc(s.title || s.file)}` +
              ` <span class="comm-badge" data-badge></span>` +
              ` <span class="comm-file">${esc(s.file)}</span></div>` +
            `<div class="comm-short">${esc(s.short || '')}</div>` +
            `<div class="comm-long" style="display:none">${esc(s.long || '')}</div>` +
            `<div class="comm-actions">` +
              `<button data-act="details">Details ▾</button>` +
              `<button data-act="load" class="primary">Load into editor</button>` +
              `<button data-act="save">Save to device</button>` +
            `</div>`;
        card.querySelector('[data-act="details"]').addEventListener('click', (e) => {
            const l = card.querySelector('.comm-long');
            const open = l.style.display === 'none';
            l.style.display = open ? '' : 'none';
            e.target.textContent = open ? 'Details ▴' : 'Details ▾';
        });
        // After a Save, re-check this one card's status so the badge updates.
        card.querySelector('[data-act="load"]').addEventListener('click', () => loadInto(path, s));
        card.querySelector('[data-act="save"]').addEventListener('click', () =>
            saveToDevice(path, s).then(() => refreshStatus([s], path, root)));
        root.appendChild(card);
    });

    // Presence + freshness badges fill in progressively (device may be unreachable).
    refreshStatus(sessions, path, root);
}

function fetchLedc(path, s) {
    return fetch(rawUrl(path, s.file), { cache: 'no-cache' })
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); });
}

function loadInto(path, s) {
    fetchLedc(path, s).then(txt => {
        setText(txt);
        // Jump to the Text view so the loaded session is visible/editable.
        const t = document.querySelector('.gen-subtab[data-view="text"]');
        if (t) t.click();
        showMessage('Loaded "' + (s.title || s.file) + '" into the editor', 'success');
    }).catch(err => showMessage('Load failed: ' + err.message, 'error'));
}

function saveToDevice(path, s) {
    return fetchLedc(path, s).then(txt =>
        saveConfigText('spiffs', s.file, txt)   // 'spiffs' = device PUT /api/configs/<name>
            .then(() => showMessage('Saved "' + s.file + '" to device', 'success'))
    ).catch(err => showMessage('Save failed: ' + err.message, 'error'));
}
