// Session report storage + viewer. Reports are always uploaded to the generator
// by config.js (when a generator URL is set); this module additionally persists
// them to the destination chosen in Settings (report_storage: none|local|spiffs)
// and provides a viewer over the locally/device-stored reports.
//   - spiffs: device cfgfs partition, ".rpt" files (/api/reports...)
//   - local:  browser localStorage (key prefix "report:")
import { showMessage } from './util.js';

const LOCAL_PREFIX = 'report:';
const SRC_LABEL = { spiffs: 'Device (SPIFFS)', local: 'Browser (local)' };
const lastLists = { spiffs: [], local: [] };

function reportName() {
    const d = new Date();
    const p = n => String(n).padStart(2, '0');
    return 'rep-' + d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate())
         + '-' + p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds()) + '.rpt';
}

// Persist a freshly-generated report to the destination configured in Settings.
// Called by config.js after it builds the report text and uploads to generator.
export function saveReport(text) {
    return fetch('/api/settings')
        .then(r => r.ok ? r.json() : {})
        .then(s => {
            const dest = (s && s.report_storage) || 'none';
            if (dest === 'none') return null;
            const name = reportName();
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
            const box = document.getElementById('reportBox');
            box.textContent = text; box.style.display = 'block';
            showMessage('Viewing ' + sel.name + ' (' + SRC_LABEL[sel.src] + ')', 'success');
        })
        .catch(err => showMessage('Load failed: ' + err, 'error'));
}

export function deleteReport() {
    const sel = parseSel();
    if (!sel) { showMessage('Pick a report first', 'error'); return; }
    if (!confirm('Delete report "' + sel.name + '" from ' + SRC_LABEL[sel.src] + '?')) return;
    const done = () => { showMessage('Deleted ' + sel.name, 'success'); refreshReportList(); };
    if (sel.src === 'spiffs') {
        fetch('/api/reports/' + encodeURIComponent(sel.name), { method: 'DELETE' })
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); done(); })
            .catch(err => showMessage('Delete failed: ' + err, 'error'));
    } else {
        localStorage.removeItem(LOCAL_PREFIX + sel.name);
        done();
    }
}
