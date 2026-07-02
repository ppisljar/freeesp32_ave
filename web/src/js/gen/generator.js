// Generator-tab controller.
//
// Owns the single shared `doc` (the parse/serialize core model), an
// onModelChanged event bus the view layers subscribe to, view switching
// scaffold (Text/Table/Lanes/Wizard — placeholders this phase), and wiring for
// configstore Load/Save/Save-As + the transport bar. Phase 1 UI is a read-only
// serialized preview pane + a row-count / problem meter.

import { showMessage } from '../util.js';
import { fetchConfigGroups, loadConfigText, saveConfigText, isValidConfigName }
    from '../configstore.js';
import { emptyDoc } from './model.js';
import { parse } from './parse.js';
import { serialize } from './serialize.js';
import { validate } from './validate.js';
import { playDoc, stop, patchLine } from './transport.js';
import { initTextView } from './views/text.js';
import { initTableView } from './views/table.js';
import { initLaneView } from './views/lane.js';
import { initWizardView } from './views/wizard.js';
import { initBgPanel } from './views/bg_panel.js';

// ---- Shared model + event bus ---------------------------------------------
let doc = emptyDoc();
let lastDiagnostics = [];
const listeners = [];
let currentSrc = null;
let currentName = null;
let currentView = 'text';
let textView = null;
let tableView = null;
let laneView = null;
let wizardView = null;
let bgPanel = null;

export function getDoc() { return doc; }

export function onModelChanged(fn) {
    listeners.push(fn);
    return () => {
        const i = listeners.indexOf(fn);
        if (i >= 0) listeners.splice(i, 1);
    };
}

function emitModelChanged() {
    for (const fn of listeners) {
        try { fn(doc); } catch (e) { /* a view error must not break others */ }
    }
}

// Replace the model (e.g. after Load / parse) and notify everyone.
export function setDoc(newDoc, diagnostics) {
    doc = newDoc || emptyDoc();
    lastDiagnostics = diagnostics || [];
    renderPreview();
    emitModelChanged();
}

// Re-parse text into the model (Text view / Load use this).
export function setText(text) {
    const { doc: d, diagnostics } = parse(text);
    setDoc(d, diagnostics);
}

// ---- Preview + meters ------------------------------------------------------
function renderPreview() {
    const pre = document.getElementById('genPreview');
    if (pre) pre.textContent = serialize(doc);

    const meter = document.getElementById('genRowMeter');
    if (meter) {
        let entries = 0, comments = 0, blanks = 0, raws = 0, bgs = 0;
        for (const r of doc.rows) {
            if (r.kind === 'led' || r.kind === 'audio') entries++;
            else if (r.kind === 'comment') comments++;
            else if (r.kind === 'blank') blanks++;
            else if (r.kind === 'raw') raws++;
            else if (r.kind === 'bg') bgs++;
        }
        const valDiags = validate(doc);
        const probs = valDiags.filter(d => d.severity === 'error').length +
                      lastDiagnostics.filter(d => d.severity === 'error').length;
        const warns = valDiags.filter(d => d.severity === 'warn').length;
        let txt = entries + ' / 100 entries';
        if (comments) txt += ' · ' + comments + ' comment' + (comments > 1 ? 's' : '');
        if (bgs) txt += ' · BG';
        if (raws) txt += ' · ' + raws + ' unparsable';
        if (probs) txt += ' · ' + probs + ' error' + (probs > 1 ? 's' : '');
        if (warns) txt += ' · ' + warns + ' warning' + (warns > 1 ? 's' : '');
        meter.textContent = txt;
        meter.className = 'gen-meter' + (probs ? ' has-error' : (warns ? ' has-warn' : ''));
    }
}

// ---- View switching --------------------------------------------------------
// Text + Table views are live (Phases 2/3); Lanes/Wizard remain placeholders.
function switchView(view) {
    currentView = view;
    document.querySelectorAll('.gen-subtab').forEach(b =>
        b.classList.toggle('active', b.dataset.view === view));

    const textRoot = document.getElementById('genTextView');
    const tableRoot = document.getElementById('genTableView');
    const laneRoot = document.getElementById('genLaneView');
    const wizardRoot = document.getElementById('genWizardView');
    const placeholder = document.getElementById('genOtherView');

    // Hide everything first.
    if (textView) textView.hide(); else if (textRoot) textRoot.style.display = 'none';
    if (tableView) tableView.hide(); else if (tableRoot) tableRoot.style.display = 'none';
    if (laneView) laneView.hide(); else if (laneRoot) laneRoot.style.display = 'none';
    if (wizardView) wizardView.hide(); else if (wizardRoot) wizardRoot.style.display = 'none';
    if (placeholder) placeholder.style.display = 'none';

    if (view === 'text') {
        if (textView) textView.show();
        else if (textRoot) textRoot.style.display = '';
    } else if (view === 'table') {
        if (tableView) tableView.show();
        else if (tableRoot) tableRoot.style.display = '';
    } else if (view === 'lanes') {
        if (laneView) laneView.show();
        else if (laneRoot) laneRoot.style.display = '';
    } else if (view === 'wizard') {
        if (wizardView) wizardView.show();
        else if (wizardRoot) wizardRoot.style.display = '';
    } else if (placeholder) {
        placeholder.textContent = view + ' view is not available.';
        placeholder.style.display = '';
    }
}

// ---- Config dropdown / Load / Save ----------------------------------------
function refreshGenList() {
    const dd = document.getElementById('genLedcDropdown');
    if (!dd) return Promise.resolve();
    return fetchConfigGroups().then(groups => {
        dd.innerHTML = '';
        groups.forEach(g => {
            const og = document.createElement('optgroup');
            og.label = g.label;
            if (g.error) {
                const o = document.createElement('option');
                o.textContent = '(unavailable)'; o.disabled = true; og.appendChild(o);
            } else if (g.names.length === 0) {
                const o = document.createElement('option');
                o.textContent = '(none)'; o.disabled = true; og.appendChild(o);
            } else {
                g.names.forEach(name => {
                    const o = document.createElement('option');
                    o.value = g.src + ':' + name;
                    o.textContent = name;
                    if (g.src === currentSrc && name === currentName) o.selected = true;
                    og.appendChild(o);
                });
            }
            dd.appendChild(og);
        });
    }).catch(() => { /* listing failure is non-fatal */ });
}

function loadSelected() {
    const dd = document.getElementById('genLedcDropdown');
    const val = dd && dd.value;
    if (!val || val.indexOf(':') < 0) { showMessage('Pick a config first', 'error'); return; }
    const src = val.slice(0, val.indexOf(':'));
    const name = val.slice(val.indexOf(':') + 1);
    loadConfigText(src, name)
        .then(text => {
            currentSrc = src; currentName = name;
            setText(text);
            const lbl = document.getElementById('genLoadedName');
            if (lbl) lbl.textContent = '(loaded: ' + name + ')';
            showMessage('Loaded ' + name + ' into Generator', 'success');
        })
        .catch(err => showMessage('Load failed: ' + err, 'error'));
}

function saveCurrent() {
    if (!doc.rows.length) { showMessage('Nothing to save', 'error'); return; }
    if (!currentSrc || !currentName) { saveAs(); return; }
    saveConfigText(currentSrc, currentName, serialize(doc))
        .then(() => { showMessage('Saved ' + currentName, 'success'); refreshGenList(); })
        .catch(err => showMessage('Save failed: ' + err, 'error'));
}

function saveAs() {
    if (!doc.rows.length) { showMessage('Nothing to save', 'error'); return; }
    const body = serialize(doc);
    const back = document.createElement('div');
    back.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.4);display:flex;align-items:center;justify-content:center;z-index:1000;';
    const box = document.createElement('div');
    box.style.cssText = 'background:#fff;padding:20px;border-radius:8px;min-width:300px;box-shadow:0 4px 20px rgba(0,0,0,0.3);font-size:14px;';
    box.innerHTML =
        '<h3 style="margin:0 0 12px 0;">Save Config As</h3>'
      + '<div style="margin-bottom:10px;"><label>Filename<br><input id="genSaveName" type="text" style="width:100%;padding:6px;" value="' + (currentName || 'untitled.ledc') + '"></label></div>'
      + '<div style="margin-bottom:14px;"><label>Destination<br><select id="genSaveDest" style="width:100%;padding:6px;">'
      + '<option value="spiffs">Device (SPIFFS)</option><option value="local">Browser (local)</option><option value="generator">Generator (server)</option>'
      + '</select></label></div>'
      + '<div style="text-align:right;"><button id="genSaveCancel">Cancel</button> <button id="genSaveOk" style="background:#28a745;">Save</button></div>';
    back.appendChild(box);
    document.body.appendChild(back);
    const close = () => document.body.removeChild(back);
    box.querySelector('#genSaveCancel').addEventListener('click', close);
    back.addEventListener('click', e => { if (e.target === back) close(); });
    box.querySelector('#genSaveOk').addEventListener('click', () => {
        const name = box.querySelector('#genSaveName').value.trim();
        const dest = box.querySelector('#genSaveDest').value;
        if (!isValidConfigName(name)) { showMessage('Invalid filename — must end in .ledc', 'error'); return; }
        saveConfigText(dest, name, body)
            .then(() => {
                currentSrc = dest; currentName = name;
                const lbl = document.getElementById('genLoadedName');
                if (lbl) lbl.textContent = '(loaded: ' + name + ')';
                showMessage('Saved ' + name, 'success');
                close(); refreshGenList();
            })
            .catch(err => showMessage('Save failed: ' + err, 'error'));
    });
    box.querySelector('#genSaveName').focus();
}

// ---- Transport bar ---------------------------------------------------------
function onPlay() {
    if (!doc.rows.length) { showMessage('Nothing to play', 'error'); return; }
    playDoc(doc)
        .then(res => {
            showMessage(res || 'Playing', 'success');
            // If the session's BG is a browser clip (push://), stream it now —
            // AFTER play-config (which auto-stops any prior BG) has started.
            if (bgPanel && bgPanel.pushForDoc) bgPanel.pushForDoc(doc);
        })
        .catch(err => showMessage('Play error: ' + err, 'error'));
}
function onStop() {
    stop().then(res => showMessage(res || 'Stopped', 'info'))
          .catch(err => showMessage('Stop error: ' + err, 'error'));
}
function onApplyLive() {
    // Phase 1 smoke test: patch the first timeline entry live (one line).
    const first = doc.rows.find(r => r.kind === 'led' || r.kind === 'audio');
    if (!first) { showMessage('No timeline entry to apply', 'error'); return; }
    const line = serialize({ rows: [first], bg: null }).trim();
    patchLine(line)
        .then(res => showMessage('Applied live: ' + (res || 'ok'), 'success'))
        .catch(err => showMessage('Apply failed: ' + err, 'error'));
}

function bind(id, fn) {
    const el = document.getElementById(id);
    if (el) el.addEventListener('click', fn);
}

export function generatorInit() {
    document.querySelectorAll('.gen-subtab').forEach(b =>
        b.addEventListener('click', () => switchView(b.dataset.view)));
    bind('btnGenRefresh', refreshGenList);
    bind('btnGenLoad', loadSelected);
    bind('btnGenSave', saveCurrent);
    bind('btnGenSaveAs', saveAs);
    bind('btnGenPlay', onPlay);
    bind('btnGenStopLocal', onStop);
    bind('btnGenApply', onApplyLive);

    // Text view (Phase 2): owns its textarea, parses input into the shared doc,
    // and re-renders from the model on every change (when not focused).
    textView = initTextView({ getDoc, setDoc });
    onModelChanged(d => { if (textView) textView.refresh(d); });

    // Table view (Phase 3): spreadsheet grid + compound cell + mobile cards.
    // Edits mutate the shared doc and commit via setDoc; incoming model changes
    // rebuild the grid (preserving scroll). Same ctx contract as the Text view.
    tableView = initTableView({ getDoc, setDoc });
    onModelChanged(d => { if (tableView) tableView.refresh(d); });

    // Lane view (Phase 4): DAW canvas lanes + ramp/LFO regions. Same ctx
    // contract; projects lanes -> flat doc via lane_serialize and re-imports on
    // incoming model changes from other views.
    laneView = initLaneView({ getDoc, setDoc });
    onModelChanged(d => { if (laneView) laneView.refresh(d); });

    // Wizard view (Phase 5): segment forms + Field/Transition/Pulse. Compiles
    // the session model to the flat doc via wizard_compile; re-imports via
    // sessionFromDoc on incoming model changes. Same ctx contract.
    wizardView = initWizardView({ getDoc, setDoc });
    onModelChanged(d => { if (wizardView) wizardView.refresh(d); });

    // BG panel (bg_browser_push_plan.md): browser-generated / loaded / bounced
    // background audio streamed to the device. Independent of the doc model
    // except when the user clicks "Set as BG" (which writes a push:// row).
    bgPanel = initBgPanel({ getDoc, setDoc });
    onModelChanged(d => { if (bgPanel) bgPanel.refresh(d); });

    switchView('text');
    setDoc(emptyDoc());     // initial empty preview/meter + text view
    refreshGenList();       // populate the config dropdown
}
