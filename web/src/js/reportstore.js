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
//
// The dropdown lists reports newest-first and labels each one
// "<date> - <session>.ledc - <title>"; the open report is reflected in the URL
// (#reports/<src>:<file>.rpt) so a specific report can be linked to and
// re-opened on load.
import { showMessage } from './util.js';
import { getStoredLog, deleteStoredLog } from './logcapture.js';

import { deviceFetch } from './devicefetch.js';
const LOCAL_PREFIX = 'report:';
const SRC_LABEL = { spiffs: 'Device (SPIFFS)', local: 'Browser (local)' };
const lastLists = { spiffs: [], local: [] };

// Marks where the machine-generated body starts; everything above it is the
// editable header. Kept in sync with config.js's report body.
const BODY_MARKER = '=== SESSION REPORT ===';

// The report currently shown in the editor/box, so the Save button knows which
// file to (over)write and which body to re-wrap with the edited header.
let current = { name: null, src: null, session: null, date: '', body: '' };

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

// ---- report identity (date / session / title) ----------------------------
// Reports are named "<session-base>-YYYYMMDD-HHMMSS.rpt", so the session and the
// recording time are recoverable from the filename alone — no fetch needed to
// sort the list or label an entry. Legacy/hand-named files degrade gracefully
// (base = whole name, date = null).
export function parseReportName(name) {
    const m = /^(.*)-(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})\.rpt$/i.exec(name || '');
    if (!m) return { base: String(name || '').replace(/\.rpt$/i, ''), date: null };
    const d = new Date(+m[2], +m[3] - 1, +m[4], +m[5], +m[6], +m[7]);
    return { base: m[1], date: isNaN(d.getTime()) ? null : d };
}

function fmtWhen(d) {
    if (!d) return '(no date)';
    const p = n => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate())
         + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
}

// Dropdown text for one report: a "*" when the report carries comments, then
// date/time, the .ledc it came from, and the custom title when the user
// actually set one (a title equal to the session name is the auto-default and
// adds nothing). The marker leads so the annotated reports line up down the
// left edge of the dropdown and can be picked out without reading each row.
export function reportLabel(e) {
    const ledc = e.session || (e.base ? e.base + '.ledc' : e.name);
    let s = (e.hasComments ? '* ' : '') + fmtWhen(e.date) + '  ·  ' + ledc;
    const title = (e.title || '').trim();
    if (title && title !== e.session && title !== e.base) s += '  ·  ' + title;
    return s;
}

// Newest first; undated entries sink to the bottom, ties broken by name.
export function compareReports(a, b) {
    const ta = a.date ? a.date.getTime() : null, tb = b.date ? b.date.getTime() : null;
    if (ta !== tb) {
        if (ta == null) return 1;
        if (tb == null) return -1;
        return tb - ta;
    }
    return a.name < b.name ? 1 : (a.name > b.name ? -1 : 0);
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
        return { title: '', session: '', date: '', comments: '', body: t };
    }
    const header = t.slice(0, idx);
    const body = t.slice(idx);
    const grab = re => { const m = header.match(re); return m ? m[1].trim() : ''; };
    const title = grab(/^Title:\s*(.*)$/m);
    const session = grab(/^Session:\s*(.*)$/m);
    const date = grab(/^Date:\s*(.*)$/m);
    // Comments run from the "Comments:" line to the end of the header block.
    let comments = '';
    const cm = header.match(/^Comments:\s*\n([\s\S]*)$/m);
    if (cm) comments = cm[1].replace(/\s+$/, '');
    if (comments === '(none)') comments = '';
    return { title, session, date, comments, body };
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
        // Re-saving an edited title must not restamp the report: keep the date
        // it was recorded with (composeReport falls back to "now" when empty).
        date: current.date,
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
    return deviceFetch('/api/settings')
        .then(r => r.ok ? r.json() : {})
        .then(s => {
            const dest = (s && s.report_storage) || 'none';
            if (dest === 'none') { showMessage('Report storage is off (set it in Settings)', 'info'); return null; }
            if (dest === 'local') {
                try { localStorage.setItem(LOCAL_PREFIX + name, text); }
                catch (e) { showMessage('Report not saved locally: ' + e, 'error'); return null; }
                showMessage('Report saved to browser as ' + name, 'info');
                noteSaved('local', name, text);
                refreshReportList();
                return name;
            }
            // spiffs
            return deviceFetch('/api/reports/' + encodeURIComponent(name),
                { method: 'PUT', headers: { 'Content-Type': 'text/plain' }, body: text })
                .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); })
                .then(() => {
                    showMessage('Report saved to device as ' + name, 'info');
                    noteSaved('spiffs', name, text);
                    refreshReportList();
                    return name;
                })
                .catch(err => { showMessage('Report not saved to device: ' + err, 'error'); return null; });
        })
        .catch(() => null);   // never let report storage break the report flow
}

// After a successful write: the report now has a home, so the editor knows
// which source it belongs to, the URL can point at it, and the dropdown label
// picks up an edited title without re-reading the file from the device.
function noteSaved(src, name, text) {
    const p = parseReport(text);
    // Only device reports need the header cache; browser ones are read locally.
    if (src === 'spiffs') writeMetaCache(src, name, { title: p.title, session: p.session, date: p.date,
                                                     hasComments: !!(p.comments || '').trim() });
    if (current.name === name) {
        current.src = src;
        setHash(src, name);
    }
}

// Called by config.js when a session ends: takes the machine body + session
// name, shows the editor (title defaulted to the session name, comments empty),
// and auto-saves once under a session+timestamp filename. Returns the composed
// text so config.js can forward title/comments to the generator upload.
export function presentGeneratedReport(body, sessionName) {
    const session = sessionName || 'session';
    current = { name: makeReportName(session), src: null, session, date: new Date().toLocaleString(), body };
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
    return deviceFetch('/api/reports')
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .then(d => d.files || []);
}
function listLocal() {
    const names = [];
    for (let i = 0; i < localStorage.length; i++) {
        const k = localStorage.key(i);
        if (k && k.startsWith(LOCAL_PREFIX)) names.push(k.slice(LOCAL_PREFIX.length));
    }
    return Promise.resolve(names);
}

// Titles live inside the report file, so labelling a device report means
// reading it. That is one HTTP round-trip per report against a very small
// device, so the header of each device report is cached in localStorage and
// re-read only for files we have never seen (or whose title we just changed).
const META_PREFIX = 'rptmeta:';
function metaKey(src, name) { return META_PREFIX + src + ':' + name; }
function readMetaCache(src, name) {
    try {
        const m = JSON.parse(localStorage.getItem(metaKey(src, name)) || 'null');
        // Caches written before the comment marker existed carry no
        // hasComments field. Treating those as complete would leave every
        // already-listed report unmarked until it happened to be rewritten, so
        // reject them: fetchMissingMeta re-reads the file and overwrites the
        // same key with the current shape.
        return (m && typeof m.hasComments === 'boolean') ? m : null;
    } catch (e) { return null; }
}
function writeMetaCache(src, name, meta) {
    try { localStorage.setItem(metaKey(src, name), JSON.stringify(meta)); } catch (e) { /* quota — labels just fall back */ }
}
function dropMetaCache(src, name) {
    try { localStorage.removeItem(metaKey(src, name)); } catch (e) { /* ignore */ }
}

// Entries currently backing the dropdown, plus per-source availability so a
// failed device listing still renders its "(unavailable)" placeholder.
let s_entries = [];
let s_avail = { spiffs: true, local: true };

function makeEntry(src, name) {
    const p = parseReportName(name);
    const e = { src, name, base: p.base, date: p.date, session: '', title: '', hasComments: false, metaKnown: false };
    // Browser-stored reports are already in memory — parse the header directly.
    // Device reports use the cache, and are filled in later by fetchMissingMeta.
    const m = src === 'local' ? parseReport(localStorage.getItem(LOCAL_PREFIX + name) || '')
                              : readMetaCache(src, name);
    if (m) applyEntryMeta(e, m);
    return e;
}

function applyEntryMeta(e, m) {
    e.title = m.title || '';
    e.session = m.session || '';
    // Two shapes reach here: a parsed report (full `comments` text) and a
    // cached header (already reduced to the boolean). Accept either.
    e.hasComments = typeof m.hasComments === 'boolean'
                  ? m.hasComments
                  : !!(m.comments || '').trim();
    e.metaKnown = true;
    // Fall back to the header's Date line for files whose name has no stamp.
    if (!e.date && m.date) {
        const d = new Date(m.date);
        if (!isNaN(d.getTime())) e.date = d;
    }
}

function renderList() {
    const dd = document.getElementById('reportDropdown');
    if (!dd) return;
    // Selection survives a re-render (labels refresh once titles arrive), and
    // falls back to the open report when the list is built after it was opened.
    const keep = dd.value || (current.name ? current.src + ':' + current.name : '');
    dd.innerHTML = '';
    ['spiffs', 'local'].forEach(src => {
        const group = document.createElement('optgroup');
        group.label = SRC_LABEL[src];
        const rows = s_entries.filter(e => e.src === src);
        if (!s_avail[src] || rows.length === 0) {
            const o = document.createElement('option');
            o.textContent = s_avail[src] ? '(none)' : '(unavailable)';
            o.disabled = true;
            group.appendChild(o);
        } else {
            rows.forEach(e => {
                const o = document.createElement('option');
                o.value = e.src + ':' + e.name;
                o.textContent = reportLabel(e);
                o.title = e.name;
                group.appendChild(o);
            });
        }
        dd.appendChild(group);
    });
    // Keep whatever was selected (re-render happens once titles arrive).
    if (keep) dd.value = keep;
}

// Read the header of every device report we have no cached title for, one at a
// time so the device is never hit with a burst, then re-render the labels.
function fetchMissingMeta() {
    const pending = s_entries.filter(e => e.src === 'spiffs' && !e.metaKnown);
    if (!pending.length) return Promise.resolve();
    return pending.reduce((chain, e) => chain.then(() =>
        loadReport(e.src, e.name)
            .then(text => {
                const r = parseReport(text);
                const m = { title: r.title, session: r.session, date: r.date,
                            hasComments: !!(r.comments || '').trim() };
                applyEntryMeta(e, m);
                writeMetaCache(e.src, e.name, m);
            })
            .catch(() => { e.metaKnown = true; })   // unreadable: label from the filename
    ), Promise.resolve()).then(() => { s_entries.sort(compareReports); renderList(); });
}

export function refreshReportList() {
    const dd = document.getElementById('reportDropdown');
    if (!dd) return Promise.resolve();
    const sources = ['spiffs', 'local'];
    return Promise.allSettled([listSpiffs(), listLocal()]).then(results => {
        s_entries = [];
        results.forEach((res, i) => {
            const src = sources[i];
            s_avail[src] = res.status === 'fulfilled';
            lastLists[src] = res.status === 'fulfilled' ? res.value : [];
            lastLists[src].forEach(name => s_entries.push(makeEntry(src, name)));
        });
        s_entries.sort(compareReports);
        renderList();
        return fetchMissingMeta();
    });
}

function loadReport(src, name) {
    if (src === 'spiffs') {
        return deviceFetch('/api/reports/' + encodeURIComponent(name))
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

// ---- URL <-> open report -------------------------------------------------
// The open report lives in the hash as "#reports/<src>:<file>.rpt", so a report
// can be linked to, bookmarked, and restored on reload.
function hashRef() {
    const m = /^#reports\/([a-z]+):(.+)$/.exec(location.hash || '');
    if (!m) return null;
    let name = m[2];
    try { name = decodeURIComponent(name); } catch (e) { /* keep raw */ }
    return { src: m[1], name };
}

function setHash(src, name) {
    const want = '#reports/' + src + ':' + encodeURIComponent(name);
    if (location.hash !== want) location.hash = want;
}

function clearHash() {
    if (/^#reports\//.test(location.hash || '')) location.hash = '#reports';
}

// Load a report into the editor and point the dropdown + URL at it.
export function openReport(src, name) {
    return loadReport(src, name)
        .then(text => {
            const parsed = parseReport(text);
            const stamped = parseReportName(name).date;
            current = {
                name, src,
                session: parsed.session || sanitizeBase(name),
                date: parsed.date || (stamped ? stamped.toLocaleString() : ''),
                body: parsed.body,
            };
            const dd = document.getElementById('reportDropdown');
            if (dd) dd.value = src + ':' + name;
            showEditor({ title: parsed.title, comments: parsed.comments });
            setHash(src, name);
            showMessage('Viewing ' + name + ' (' + SRC_LABEL[src] + ')', 'success');
        })
        .catch(err => showMessage('Load failed: ' + err, 'error'));
}

// Dropdown "change" handler.
export function viewReport() {
    const sel = parseSel();
    if (!sel) { showMessage('Pick a report first', 'error'); return; }
    openReport(sel.src, sel.name);
}

// Open whatever report the URL names (on load and on back/forward). No-op when
// the hash names no report or the one already open.
export function syncReportFromHash() {
    const ref = hashRef();
    if (!ref) return;
    if (current.src === ref.src && current.name === ref.name) return;
    openReport(ref.src, ref.name);
}

export function deleteReport() {
    const sel = parseSel();
    if (!sel) { showMessage('Pick a report first', 'error'); return; }
    if (!confirm('Delete report "' + sel.name + '" from ' + SRC_LABEL[sel.src] + '?')) return;
    const done = () => {
        showMessage('Deleted ' + sel.name, 'success');
        deleteStoredLog(sel.name);         // drop the linked device log too
        dropMetaCache(sel.src, sel.name);  // and its cached header
        if (current.name === sel.name) {   // clear editor if we just deleted the open one
            current = { name: null, src: null, session: null, date: '', body: '' };
            const e = editorEls();
            if (e.wrap) e.wrap.style.display = 'none';
            if (e.box) e.box.style.display = 'none';
            clearHash();
        }
        refreshReportList();
    };
    if (sel.src === 'spiffs') {
        deviceFetch('/api/reports/' + encodeURIComponent(sel.name), { method: 'DELETE' })
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); done(); })
            .catch(err => showMessage('Delete failed: ' + err, 'error'));
    } else {
        localStorage.removeItem(LOCAL_PREFIX + sel.name);
        done();
    }
}
