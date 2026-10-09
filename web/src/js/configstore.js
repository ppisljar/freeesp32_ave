// Unified .ledc config store across two sources:
//   - spiffs:    device, dedicated cfgfs partition (/api/configs...)
//   - local:     browser localStorage (key prefix "ledc:")
// The dropdown lists configs from both sources (grouped by source);
// Save / Save As route to the chosen source.
// (The old external "generator (server)" source was removed — it blocked the
//  dropdown while an unreachable generatorUrl timed out.)
import { showMessage } from './util.js';

import { deviceFetch } from './devicefetch.js';
const LOCAL_PREFIX = 'ledc:';
const NAME_RE = /^[A-Za-z0-9._-]+\.ledc$/;

// Currently-loaded config (so plain Save knows where to write).
let currentSrc = null;     // 'spiffs' | 'local'
let currentName = null;
// Last-fetched name lists per source (for overwrite checks).
const lastLists = { spiffs: [], local: [] };

const SRC_LABEL = { spiffs: 'Device (SPIFFS)', local: 'Browser (local)' };

// Exposed for config.js report upload (tags the report with the config name).
export function getCurrentConfigName() { return currentName; }

// ---- per-source list/load/save -------------------------------------------
function listSpiffs() {
    return deviceFetch('/api/configs')
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(d => d.files || []);
}
function listLocal() {
    const names = [];
    for (let i = 0; i < localStorage.length; i++) {
        const k = localStorage.key(i);
        if (k && k.startsWith(LOCAL_PREFIX)) names.push(k.slice(LOCAL_PREFIX.length));
    }
    return Promise.resolve(names.sort());
}

function loadFromSource(src, name) {
    if (src === 'spiffs') {
        return deviceFetch('/api/configs/' + encodeURIComponent(name))
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); });
    }
    // local
    const v = localStorage.getItem(LOCAL_PREFIX + name);
    return v == null ? Promise.reject(new Error('not in local storage')) : Promise.resolve(v);
}

function saveToSource(src, name, body) {
    if (src === 'spiffs') {
        return deviceFetch('/api/configs/' + encodeURIComponent(name),
            { method: 'PUT', headers: { 'Content-Type': 'text/plain' }, body })
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json().catch(() => ({ saved: name })); });
    }
    // local
    try { localStorage.setItem(LOCAL_PREFIX + name, body); return Promise.resolve({ saved: name }); }
    catch (e) { return Promise.reject(new Error('localStorage full: ' + e)); }
}

// Fetch the text of EVERY known config (device SPIFFS + browser-local), for
// bulk operations like the TTS speech preload. Best-effort: a source that won't
// list, or an individual config that won't load, is skipped rather than failing
// the whole batch. Returns [{ src, name, text }].
export async function allConfigTexts() {
    const out = [];
    const sources = [['spiffs', listSpiffs], ['local', listLocal]];
    for (const [src, lister] of sources) {
        let names = [];
        try { names = await lister(); } catch (e) { names = []; }
        for (const name of names) {
            try { out.push({ src, name, text: await loadFromSource(src, name) }); }
            catch (e) { /* skip unreadable config */ }
        }
    }
    return out;
}

// ---- dropdown ------------------------------------------------------------
export function refreshConfigList() {
    const dd = document.getElementById('ledcDropdown');
    const sources = [
        ['spiffs', listSpiffs()],
        ['local', listLocal()],
    ];
    return Promise.allSettled(sources.map(s => s[1])).then(results => {
        dd.innerHTML = '';
        results.forEach((res, i) => {
            const src = sources[i][0];
            const group = document.createElement('optgroup');
            group.label = SRC_LABEL[src];
            if (res.status === 'fulfilled' && res.value) {
                // Sort by name within each source group (natural + case-insensitive)
                // — the device (spiffs) list arrives in filesystem order otherwise,
                // so base + _RGB variants would appear scrambled. Same ordering the
                // Generator picker uses (fetchConfigGroups).
                const names = res.value.slice().sort(byConfigName);
                lastLists[src] = names;
                if (names.length === 0) {
                    const o = document.createElement('option'); o.textContent = '(none)'; o.disabled = true; group.appendChild(o);
                } else {
                    names.forEach(name => {
                        const o = document.createElement('option');
                        o.value = src + ':' + name;
                        o.textContent = name;
                        if (src === currentSrc && name === currentName) o.selected = true;
                        group.appendChild(o);
                    });
                }
            } else {
                lastLists[src] = [];
                const o = document.createElement('option');
                o.textContent = '(unavailable)';
                o.disabled = true; group.appendChild(o);
            }
            dd.appendChild(group);
        });
    });
}

export function loadSelected() {
    const dd = document.getElementById('ledcDropdown');
    const val = dd.value;
    if (!val || val.indexOf(':') < 0) { showMessage('Pick a config first', 'error'); return; }
    const src = val.slice(0, val.indexOf(':'));
    const name = val.slice(val.indexOf(':') + 1);
    loadFromSource(src, name)
        .then(text => {
            document.getElementById('exampleConfig').value = text;
            currentSrc = src; currentName = name;
            document.getElementById('loadedFilename').textContent = '(loaded: ' + name + ' — ' + SRC_LABEL[src] + ')';
            showMessage('Loaded ' + name + ' from ' + SRC_LABEL[src], 'success');
        })
        .catch(err => showMessage('Load failed: ' + err, 'error'));
}

// Plain Save → write back to the currently-loaded source/name; if nothing is
// loaded, fall through to Save As.
export function saveCurrent() {
    const body = document.getElementById('exampleConfig').value;
    if (!body.trim()) { showMessage('Config is empty', 'error'); return; }
    if (!currentSrc || !currentName) { saveAsDialog(); return; }
    saveToSource(currentSrc, currentName, body)
        .then(res => {
            showMessage('Saved ' + (res.saved || currentName) + ' to ' + SRC_LABEL[currentSrc], 'success');
            refreshConfigList();
        })
        .catch(err => showMessage('Save failed: ' + err, 'error'));
}

// ---- Save As modal -------------------------------------------------------
function commitSaveAs(src, name, body, closeModal) {
    if (!NAME_RE.test(name)) { showMessage('Invalid filename. Use [A-Za-z0-9._-] and end in .ledc', 'error'); return; }
    if (lastLists[src] && lastLists[src].includes(name) && !(src === currentSrc && name === currentName)) {
        if (!confirm('"' + name + '" already exists in ' + SRC_LABEL[src] + '. Overwrite?')) return;
    }
    saveToSource(src, name, body)
        .then(res => {
            currentSrc = src; currentName = res.saved || name;
            document.getElementById('loadedFilename').textContent = '(loaded: ' + currentName + ' — ' + SRC_LABEL[src] + ')';
            showMessage('Saved ' + currentName + ' to ' + SRC_LABEL[src], 'success');
            closeModal();
            refreshConfigList();
        })
        .catch(err => showMessage('Save failed: ' + err, 'error'));
}

export function saveAsDialog() {
    const body = document.getElementById('exampleConfig').value;
    if (!body.trim()) { showMessage('Config is empty', 'error'); return; }

    // Backdrop + dialog (built in JS to avoid extra markup).
    const back = document.createElement('div');
    back.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.4);display:flex;align-items:center;justify-content:center;z-index:1000;';
    const box = document.createElement('div');
    box.style.cssText = 'background:#fff;padding:20px;border-radius:8px;min-width:300px;box-shadow:0 4px 20px rgba(0,0,0,0.3);font-size:14px;';
    const dests = ['spiffs', 'local'];
    const defaultDest = (currentSrc && dests.includes(currentSrc)) ? currentSrc : dests[0];
    box.innerHTML =
        '<h3 style="margin:0 0 12px 0;">Save Config As</h3>'
      + '<div style="margin-bottom:10px;"><label>Filename<br><input id="saveAsName" type="text" style="width:100%;padding:6px;" value="' + (currentName || 'untitled.ledc') + '"></label></div>'
      + '<div style="margin-bottom:14px;"><label>Destination<br><select id="saveAsDest" style="width:100%;padding:6px;">'
      + dests.map(d => '<option value="' + d + '"' + (d === defaultDest ? ' selected' : '') + '>' + SRC_LABEL[d] + '</option>').join('')
      + '</select></label></div>'
      + '<div style="text-align:right;"><button id="saveAsCancel">Cancel</button> <button id="saveAsOk" style="background:#28a745;">Save</button></div>';
    back.appendChild(box);
    document.body.appendChild(back);

    const close = () => document.body.removeChild(back);
    box.querySelector('#saveAsCancel').addEventListener('click', close);
    back.addEventListener('click', (e) => { if (e.target === back) close(); });
    box.querySelector('#saveAsOk').addEventListener('click', () => {
        const name = box.querySelector('#saveAsName').value.trim();
        const dest = box.querySelector('#saveAsDest').value;
        commitSaveAs(dest, name, body, close);
    });
    box.querySelector('#saveAsName').focus();
}

// ---- Generator-tab helpers (additive; do NOT touch the Home wiring) --------
// The existing exports above are bound to the Home page's DOM ids
// (exampleConfig / ledcDropdown / loadedFilename). The Generator tab owns its
// own model + controls, so it needs DOM-agnostic access to the same per-source
// list/load/save plumbing. These wrappers reuse the internal functions without
// changing any existing behavior, keeping Option A (Home stays independent).

// Natural, case-insensitive name ordering for the config dropdowns.
export function byConfigName(a, b) {
    return String(a).localeCompare(String(b), undefined, { numeric: true, sensitivity: 'base' });
}

// Returns a Promise of [{ src, label, names[] }] across all available sources.
// A source that fails to list resolves to names:[] (with an `error` flag).
export function fetchConfigGroups() {
    const sources = [
        ['spiffs', listSpiffs()],
        ['local', listLocal()],
    ];
    return Promise.allSettled(sources.map(s => s[1])).then(results => {
        const groups = [];
        results.forEach((res, i) => {
            const src = sources[i][0];
            if (res.status === 'fulfilled' && res.value) {
                // Sort by name within the category (natural + case-insensitive, so
                // "09_" precedes "10_" and unpadded names order sensibly). The
                // device (spiffs) list arrives in filesystem order otherwise.
                const names = res.value.slice().sort(byConfigName);
                lastLists[src] = names;
                groups.push({ src, label: SRC_LABEL[src], names });
            } else {
                lastLists[src] = [];
                groups.push({ src, label: SRC_LABEL[src], names: [], error: true });
            }
        });
        return groups;
    });
}

// Load raw text for a given source/name (no DOM side effects).
export function loadConfigText(src, name) {
    return loadFromSource(src, name);
}

// Save text to a source/name; validates the filename against NAME_RE.
export function saveConfigText(src, name, body) {
    if (!NAME_RE.test(name)) {
        return Promise.reject(new Error('Invalid filename. Use [A-Za-z0-9._-] and end in .ledc'));
    }
    return saveToSource(src, name, body);
}

export function isValidConfigName(name) { return NAME_RE.test(name); }
