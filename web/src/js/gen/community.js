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

const REPO   = 'ppisljar/freeesp32_ave';
const BRANCH = 'main';
const INDEX_URL = `https://raw.githubusercontent.com/${REPO}/${BRANCH}/sessions/community.json`;
const rawUrl = (path, file) => `https://raw.githubusercontent.com/${REPO}/${BRANCH}/${path}/${file}`;

let loaded = false;

function esc(x) {
    return String(x == null ? '' : x).replace(/[&<>"]/g, c =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
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
        card.innerHTML =
            `<div class="comm-title">${esc(s.title || s.file)} <span class="comm-file">${esc(s.file)}</span></div>` +
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
        card.querySelector('[data-act="load"]').addEventListener('click', () => loadInto(path, s));
        card.querySelector('[data-act="save"]').addEventListener('click', () => saveToDevice(path, s));
        root.appendChild(card);
    });
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
    fetchLedc(path, s).then(txt =>
        saveConfigText('spiffs', s.file, txt)   // 'spiffs' = device PUT /api/configs/<name>
            .then(() => showMessage('Saved "' + s.file + '" to device', 'success'))
    ).catch(err => showMessage('Save failed: ' + err.message, 'error'));
}
