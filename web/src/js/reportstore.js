// Session report storage + viewer. Reports are always uploaded to the generator
// by config.js (when a generator URL is set); this module additionally persists
// them to the destination chosen in Settings (report_storage: none|local|spiffs)
// and provides a viewer/editor over the locally/device-stored reports.
//   - spiffs: device cfgfs partition, ".rpt" files (/api/reports...)
//   - local:  browser localStorage (key prefix "report:")
//
// A saved report has a small human-editable header (Title / Session / Date /
// Comments) followed by the machine-generated body. Both the freshly-generated
// report (from config.js) and reports re-opened from the dropdown flow through
// the SAME editor, so a title/comment can be set at generation time or amended
// later and re-saved.
import { showMessage } from './util.js';
import { getStoredLog, deleteStoredLog } from './logcapture.js';

const LOCAL_PREFIX = 'report:';
const SRC_LABEL = { spiffs: 'Device (SPIFFS)', local: 'Browser (local)' };
const lastLists = { spiffs: [], local: [] };

// Marks where the machine-generated body starts; everything above it is the
// editable header. Kept in sync with config.js's report body.
const BODY_MARKER = '=== SESSION REPORT ===';

// The report currently shown in the editor/box, so the Save button knows which
// file to (over)write and which body to re-wrap with the edited header.
let current = { name: null, src: null, session: null, body: '' };

// ---- filename ------------------------------------------------------------
// Build a filesystem-safe report filename from the session name + timestamp,
// e.g. "04_meditation_theta-20260723-134501.rpt". The session name is what
// makes reports self-identifying in the dropdown.
export function makeReportName(sessionName) {
    const d = new Date();
    const p = n => String(n).padStart(2, '0');
    const stamp = d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate())
                + '-' + p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds());
    return sanitizeBase(sessionName) + '-' + stamp + '.rpt';
}

function sanitizeBase(name) {
    if (!name) return 'session';
    // Drop a trailing ".ledc", collapse anything outside the safe charset to
    // '-', and trim stray separators. Matches configstore's NAME_RE charset.
    const b = String(name).replace(/\.ledc$/i, '')
        .replace(/[^A-Za-z0-9._-]+/g, '-').replace(/^[-.]+|[-.]+$/g, '');
    return b || 'session';
}

// ---- compose / parse -----------------------------------------------------
// Wrap a machine body with the editable header. Called every time we save so
// the on-disk report always reflects the current Title/Comments inputs.
export function composeReport(body, meta) {
    const title = (meta.title || '').trim() || (meta.session || 'session');
    const comments = (meta.comments || '').trim();
    const date = meta.date || new Date().toLocaleString();
    let head = 'Title: ' + title + '\n';
    head += 'Session: ' + (meta.session || '(unknown)') + '\n';
    head += 'Date: ' + date + '\n\n';
    head += 'Comments:\n' + (comments || '(none)') + '\n\n';
    return head + (body || '');
}

// Split a saved report back into {title, session, comments, body}. Tolerant of
// old header-less reports (whole text becomes the body; title/comments empty).
export function parseReport(text) {
    const t = text || '';
    const idx = t.indexOf(BODY_MARKER);
    if (idx < 0 || !/^Title:/.test(t)) {
        return { title: '', session: '', comments: '', body: t };
    }
    const header = t.slice(0, idx);
    const body = t.slice(idx);
    const grab = re => { const m = header.match(re); return m ? m[1].trim() : ''; };
    const title = grab(/^Title:\s*(.*)$/m);
    const session = grab(/^Session:\s*(.*)$/m);
    // Comments run from the "Comments:" line to the end of the header block.
    let comments = '';
    const cm = header.match(/^Comments:\s*\n([\s\S]*)$/m);
    if (cm) comments = cm[1].replace(/\s+$/, '');
    if (comments === '(none)') comments = '';
    return { title, session, comments, body };
}

// ---- editor UI -----------------------------------------------------------
function editorEls() {
    return {
        wrap: document.getElementById('reportEditor'),
        title: document.getElementById('reportTitle'),
        comments: document.getElementById('reportComments'),
        savedName: document.getElementById('reportSavedName'),
        box: document.getElementById('reportBox'),
    };
}

function readMeta() {
    const e = editorEls();
    return {
        title: (e.title && e.title.value) || '',
        comments: (e.comments && e.comments.value) || '',
        session: current.session,
    };
}

// Render the editor + composed report box for `current`, filling the inputs
// from the supplied meta (title/comments). Used by both generation and view.
function showEditor(meta) {
    const e = editorEls();
    if (e.title) e.title.value = (meta.title || '').trim() || (current.session || '');
    if (e.comments) e.comments.value = meta.comments || '';
    if (e.savedName) e.savedName.textContent = current.name ? '(' + current.name + ')' : '';
    if (e.wrap) e.wrap.style.display = 'block';
    if (e.box) {
        e.box.textContent = composeReport(current.body, readMeta());
        e.box.style.display = 'block';
    }
    refreshLogSection();   // reflect any captured device log for this report
}

// ---- persistence ---------------------------------------------------------
// Low-level save of a fully-composed report to the destination configured in
// Settings, under `name`. Returns the name on success, null otherwise.
function persist(name, text) {
    return fetch('/api/settings')
        .then(r => r.ok ? r.json() : {})
        .then(s => {
            const dest = (s && s.report_storage) || 'none';
            if (dest === 'none') { showMessage('Report storage is off (set it in Settings)', 'info'); return null; }
            if (dest === 'local') {
                try { localStorage.setItem(LOCAL_PREFIX + name, text); }
                catch (e) { showMessage('Report not saved locally: ' + e, 'error'); return null; }
                showMessage('Report saved to browser as ' + name, 'info');
                refreshReportList();
                return name;
            }
            // spiffs
            return fetch('/api/reports/' + encodeURIComponent(name),
                { method: 'PUT', headers: { 'Content-Type': 'text/plain' }, body: text })
                .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); })
                .then(() => { showMessage('Report saved to device as ' + name, 'info'); refreshReportList(); return name; })
                .catch(err => { showMessage('Report not saved to device: ' + err, 'error'); return null; });
        })
        .catch(() => null);   // never let report storage break the report flow
}

// Called by config.js when a session ends: takes the machine body + session
// name, shows the editor (title defaulted to the session name, comments empty),
// and auto-saves once under a session+timestamp filename. Returns the composed
// text so config.js can forward title/comments to the generator upload.
export function presentGeneratedReport(body, sessionName) {
    const session = sessionName || 'session';
    current = { name: makeReportName(session), src: null, session, body };
    showEditor({ title: session, comments: '' });
    const composed = composeReport(current.body, readMeta());
    persist(current.name, composed);
    return composed;
}

// Read the current editor meta (for the generator upload in config.js).
export function getReportMeta() {
    const m = readMeta();
    return { title: (m.title || '').trim() || (current.session || 'session'), comments: (m.comments || '').trim() };
}

// The filename of the report currently in the editor, so config.js can link the
// captured device log to it after finalizing the capture.
export function getCurrentReportName() { return current.name; }

// ---- captured logs (device + browser) -------------------------------------
// Two logs travel with a report: the ESP32 device log (logcapture polling) and
// the browser-side session log (clientlog). Each renders an identical
// View/Download row. `s_logs` caches the text currently loaded per kind.
let s_logs = { device: null, browser: null };

const LOG_UI = {
    device:  { row: 'reportLogRow',  info: 'reportLogInfo',  box: 'reportLogBox',  view: 'btnReportLogView',  dl: 'btnReportLogDownload',  suffix: '.device.log' },
    browser: { row: 'reportBLogRow', info: 'reportBLogInfo', box: 'reportBLogBox', view: 'btnReportBLogView', dl: 'btnReportBLogDownload', suffix: '.browser.log' },
};

function fmtKB(n) {
    if (n == null) return '—';
    return n < 1024 ? n + ' B' : (n / 1024).toFixed(1) + ' KB';
}

function resetLogRow(kind) {
    const u = LOG_UI[kind];
    const box = document.getElementById(u.box);
    if (box) { box.style.display = 'none'; box.textContent = ''; }
    s_logs[kind] = null;
    const row = document.getElementById(u.row);
    if (!row) return;
    row.style.display = current.name ? 'flex' : 'none';
    const info = document.getElementById(u.info);
    if (info) info.textContent = current.name ? 'checking…' : '';
    setLogBtns(kind, false);
}

function setLogBtns(kind, on) {
    const u = LOG_UI[kind];
    const view = document.getElementById(u.view), dl = document.getElementById(u.dl);
    if (view) view.disabled = !on;
    if (dl) dl.disabled = !on;
}

function applyLog(kind, text, bytes) {
    const u = LOG_UI[kind];
    const info = document.getElementById(u.info);
    if (text == null) { if (info) info.textContent = '(none captured for this report)'; setLogBtns(kind, false); return; }
    s_logs[kind] = text;
    if (info) info.textContent = fmtKB(bytes == null ? text.length : bytes) + ' captured';
    setLogBtns(kind, true);
}

// Re-check IndexedDB for logs linked to the open report and update both rows.
// Called whenever the editor (re)opens and again after a fresh session's
// capture is finalized.
export function refreshLogSection() {
    s_logs = { device: null, browser: null };
    resetLogRow('device');
    resetLogRow('browser');
    if (!current.name) return;
    getStoredLog(current.name).then(rec => {
        applyLog('device', rec ? rec.deviceText : null, rec ? rec.deviceBytes : null);
        applyLog('browser', rec ? rec.browserText : null, rec ? rec.browserBytes : null);
    });
}

function toggleLog(kind) {
    const box = document.getElementById(LOG_UI[kind].box);
    if (!box) return;
    if (s_logs[kind] == null) { showMessage('No ' + kind + ' log for this report', 'info'); return; }
    const hidden = box.style.display === 'none' || !box.style.display;
    if (hidden) { box.textContent = s_logs[kind] || '(empty)'; box.style.display = 'block'; box.scrollTop = box.scrollHeight; }
    else { box.style.display = 'none'; }
}

function downloadLog(kind) {
    if (s_logs[kind] == null) { showMessage('No ' + kind + ' log for this report', 'info'); return; }
    const base = (current.name || 'session').replace(/\.rpt$/, '');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([s_logs[kind]], { type: 'text/plain' }));
    a.download = base + LOG_UI[kind].suffix;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

export function toggleDeviceLog()   { toggleLog('device'); }
export function downloadDeviceLog() { downloadLog('device'); }
export function toggleBrowserLog()  { toggleLog('browser'); }
export function downloadBrowserLog(){ downloadLog('browser'); }

// Save button: re-wrap the current body with the edited Title/Comments and
// overwrite the same file (or create it if generation-time save was skipped).
export function saveReportEdits() {
    if (!current.body) { showMessage('No report to save yet', 'error'); return; }
    if (!current.name) current.name = makeReportName(current.session);
    const composed = composeReport(current.body, readMeta());
    const e = editorEls();
    if (e.box) e.box.textContent = composed;
    persist(current.name, composed).then(name => { if (name && e.savedName) e.savedName.textContent = '(' + name + ')'; });
}

// ---- listing / viewing ---------------------------------------------------
function listSpiffs() {
    return fetch('/api/reports')
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(d => d.files || []);
}
function listLocal() {
    const names = [];
    for (let i = 0; i < localStorage.length; i++) {
        const k = localStorage.key(i);
        if (k && k.startsWith(LOCAL_PREFIX)) names.push(k.slice(LOCAL_PREFIX.length));
    }
    return Promise.resolve(names.sort().reverse());   // newest-named first
}

export function refreshReportList() {
    const dd = document.getElementById('reportDropdown');
    if (!dd) return Promise.resolve();
    const sources = [['spiffs', listSpiffs()], ['local', listLocal()]];
    return Promise.allSettled(sources.map(s => s[1])).then(results => {
        dd.innerHTML = '';
        results.forEach((res, i) => {
            const src = sources[i][0];
            const group = document.createElement('optgroup');
            group.label = SRC_LABEL[src];
            if (res.status === 'fulfilled') {
                lastLists[src] = res.value;
                if (res.value.length === 0) {
                    const o = document.createElement('option'); o.textContent = '(none)'; o.disabled = true; group.appendChild(o);
                } else {
                    res.value.forEach(name => {
                        const o = document.createElement('option');
                        o.value = src + ':' + name; o.textContent = name;
                        group.appendChild(o);
                    });
                }
            } else {
                lastLists[src] = [];
                const o = document.createElement('option'); o.textContent = '(unavailable)'; o.disabled = true; group.appendChild(o);
            }
            dd.appendChild(group);
        });
    });
}

function loadReport(src, name) {
    if (src === 'spiffs') {
        return fetch('/api/reports/' + encodeURIComponent(name))
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); });
    }
    const v = localStorage.getItem(LOCAL_PREFIX + name);
    return v == null ? Promise.reject(new Error('not in local storage')) : Promise.resolve(v);
}

function parseSel() {
    const dd = document.getElementById('reportDropdown');
    const val = dd && dd.value;
    if (!val || val.indexOf(':') < 0) return null;
    return { src: val.slice(0, val.indexOf(':')), name: val.slice(val.indexOf(':') + 1) };
}

export function viewReport() {
    const sel = parseSel();
    if (!sel) { showMessage('Pick a report first', 'error'); return; }
    loadReport(sel.src, sel.name)
        .then(text => {
            const parsed = parseReport(text);
            current = {
                name: sel.name, src: sel.src,
                session: parsed.session || sanitizeBase(sel.name),
                body: parsed.body,
            };
            showEditor({ title: parsed.title, comments: parsed.comments });
            showMessage('Viewing ' + sel.name + ' (' + SRC_LABEL[sel.src] + ')', 'success');
        })
        .catch(err => showMessage('Load failed: ' + err, 'error'));
}

export function deleteReport() {
    const sel = parseSel();
    if (!sel) { showMessage('Pick a report first', 'error'); return; }
    if (!confirm('Delete report "' + sel.name + '" from ' + SRC_LABEL[sel.src] + '?')) return;
    const done = () => {
        showMessage('Deleted ' + sel.name, 'success');
        deleteStoredLog(sel.name);         // drop the linked device log too
        if (current.name === sel.name) {   // clear editor if we just deleted the open one
            current = { name: null, src: null, session: null, body: '' };
            const e = editorEls();
            if (e.wrap) e.wrap.style.display = 'none';
            if (e.box) e.box.style.display = 'none';
        }
        refreshReportList();
    };
    if (sel.src === 'spiffs') {
        fetch('/api/reports/' + encodeURIComponent(sel.name), { method: 'DELETE' })
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); done(); })
            .catch(err => showMessage('Delete failed: ' + err, 'error'));
    } else {
        localStorage.removeItem(LOCAL_PREFIX + sel.name);
        done();
    }
}
