// Generator-tab controller.
//
// Owns the single shared `doc` (the parse/serialize core model), an
// onModelChanged event bus the view layers subscribe to, view switching
// scaffold (Text/Table/Lanes/Wizard — placeholders this phase), and wiring for
// configstore Load/Save/Save-As + the transport bar. Phase 1 UI is a read-only
// serialized preview pane + a row-count / problem meter.

import { showMessage } from '../util.js';
import { fetchConfigGroups, loadConfigText, saveConfigText, isValidConfigName,
         configSources, isReadOnlySource } from '../configstore.js';
import { emptyDoc } from './model.js';
import { parse } from './parse.js';
import { serialize } from './serialize.js';
import { validate } from './validate.js';
import { playDoc, stop, stopBg, patchLine, pushBg } from './transport.js';
import { bounceSession, pushSessionBg } from './bounce.js';
import { playSession } from './play.js';
import { getEngine } from './tts.js';
import * as bgstore from './bgstore.js';
import { initTextView } from './views/text.js';
import { initTableView } from './views/table.js';
import { initLaneView } from './views/lane.js';
import { initWizardView } from './views/wizard.js';
import { initSimView } from './views/sim.js';
import { communityShow } from './community.js';
import { STANDALONE, isFileProtocol } from '../env.js';
import { DEMOS } from './sim/demos.js';

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
let simView = null;

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
        // `M` markers occupy a slot in the device's timeline array just as LED
        // and audio rows do (they are CONFIG_ENTRY_MARKER in the same
        // entries[]), so they count against CONFIG_PARSER_MAX_ENTRIES. `S`
        // rows do not, because serializeForDevice strips them before the POST.
        let entries = 0, comments = 0, blanks = 0, raws = 0, bgs = 0, markers = 0;
        for (const r of doc.rows) {
            if (r.kind === 'led' || r.kind === 'audio') entries++;
            else if (r.kind === 'marker') { entries++; markers++; }
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
        if (markers) txt += ' · ' + markers + ' marker' + (markers > 1 ? 's' : '');
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
    const simRoot = document.getElementById('genSimView');
    const commRoot = document.getElementById('genCommunityView');
    const placeholder = document.getElementById('genOtherView');

    // Hide everything first.
    if (textView) textView.hide(); else if (textRoot) textRoot.style.display = 'none';
    if (tableView) tableView.hide(); else if (tableRoot) tableRoot.style.display = 'none';
    if (laneView) laneView.hide(); else if (laneRoot) laneRoot.style.display = 'none';
    if (wizardView) wizardView.hide(); else if (wizardRoot) wizardRoot.style.display = 'none';
    if (simView) simView.hide(); else if (simRoot) simRoot.style.display = 'none';
    if (commRoot) commRoot.style.display = 'none';
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
    } else if (view === 'simulate') {
        if (simView) simView.show();
        else if (simRoot) simRoot.style.display = '';
    } else if (view === 'community') {
        if (commRoot) commRoot.style.display = '';
        communityShow();   // lazy-fetches the GitHub index on first show
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

// Destinations Save As may offer: every configured source minus the read-only
// ones. The GitHub library lists and loads but cannot be written to.
function writableDestinations() {
    const LABEL = { spiffs: 'Device (SPIFFS)', local: 'Browser (local)' };
    return configSources()
        .filter(src => !isReadOnlySource(src))
        .map(src => ({ src, label: LABEL[src] || src }));
}

function saveCurrent() {
    if (!doc.rows.length) { showMessage('Nothing to save', 'error'); return; }
    // A session opened FROM the GitHub library has nowhere to save back to, so
    // Save behaves as Save As rather than failing.
    if (!currentSrc || !currentName || isReadOnlySource(currentSrc)) { saveAs(); return; }
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
    box.style.cssText = 'background:#12151f;color:var(--fg);border:1px solid var(--edge-2);padding:20px;border-radius:12px;min-width:300px;box-shadow:0 20px 60px rgba(0,0,0,.6);font-size:14px;';
    box.innerHTML =
        '<h3 style="margin:0 0 12px 0;">Save Config As</h3>'
      + '<div style="margin-bottom:10px;"><label>Filename<br><input id="genSaveName" type="text" style="width:100%;padding:6px;" value="' + (currentName || 'untitled.ledc') + '"></label></div>'
      + '<div style="margin-bottom:14px;"><label>Destination<br><select id="genSaveDest" style="width:100%;padding:6px;">'
      + writableDestinations().map(d =>
            '<option value="' + d.src + '">' + d.label + '</option>').join('')
      + '</select></label></div>'
      + '<div style="text-align:right;"><button id="genSaveCancel">Cancel</button> <button id="genSaveOk" style="background:var(--ok);color:#06240f;border-color:transparent;">Save</button></div>';
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
// Play the session through the shared path (also used by the Home Play button):
// device timeline (A + LED), speech merge, and push:// BG streaming.
function onPlay() {
    return playSession(doc);
}

// Bounce the whole session (BG + Speech + A) to a WAV → save to library + download.
async function onBounce() {
    if (!doc.rows.length) { showMessage('Nothing to bounce', 'error'); return; }
    try {
        const engine = getEngine();
        const res = await bounceSession(doc, { scope: 'all', engine, onProgress: m => showMessage(m, 'info') });
        if (res.note) showMessage(res.note, 'info');
        await bgstore.put({ name: res.name, wavBlob: res.blob, durationMs: res.durationMs, sourceKind: 'bounce' }).catch(() => {});
        const a = document.createElement('a');
        a.href = URL.createObjectURL(res.blob);
        a.download = res.name + '.wav';
        document.body.appendChild(a); a.click();
        setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
        showMessage('Bounced → saved to library + downloaded (' + res.name + ')', 'success');
    } catch (err) {
        showMessage('Bounce error: ' + (err.message || err), 'error');
    }
}
function onStop() {
    stopBg();  // abort the in-flight browser BG push so it stops uploading too
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

/*
 * Reshape the toolbar for a host with no device.
 *
 * Three of the five transport buttons only mean anything with hardware
 * attached — Play posts the timeline to /api/play-config, Apply live patches a
 * running session, Bounce renders a WAV to push over the BG WebSocket — so on
 * the docs site they are removed rather than left to fail. Two that do make
 * sense in a browser take their place: open a .ledc from disk, and download the
 * one you have been editing.
 *
 * Removing the device Play also closes a real trap: it sits a few pixels above
 * the Simulate view's own Play, and pressing it renders the whole session to a
 * WAV synchronously, which locks the tab solid on a long session.
 */
function applyStandaloneChrome() {
    for (const id of ['btnGenPlay', 'btnGenApply', 'btnGenBounce']) {
        const el = document.getElementById(id);
        if (el) el.remove();
    }
    const bar = document.querySelector('#page-generator .gen-transport');
    if (!bar) return;

    // Example picker. The sessions are bundled as TEXT rather than fetched,
    // because the docs site deliberately opens from a local checkout over
    // file://, where fetching a sibling file is blocked by the opaque origin.
    if (DEMOS.length) {
        const lbl = document.createElement('label');
        lbl.className = 'gen-demo';
        lbl.appendChild(document.createTextNode('Example: '));
        const sel = document.createElement('select');
        sel.id = 'genDemoPicker';
        for (const d of DEMOS) {
            const o = document.createElement('option');
            o.value = d.name;
            o.textContent = d.title;
            if (d.blurb) o.title = d.blurb;
            sel.appendChild(o);
        }
        sel.addEventListener('change', () => loadDemo(sel.value));
        lbl.appendChild(sel);
        bar.insertBefore(lbl, bar.firstChild);
    }

    const open = document.createElement('button');
    open.id = 'btnGenOpen';
    open.textContent = '\u{1F4C2} Open';
    const file = document.createElement('input');
    file.type = 'file';
    file.accept = '.ledc,.led,text/plain';
    file.style.display = 'none';
    file.addEventListener('change', () => {
        const f = file.files && file.files[0];
        if (!f) { return; }
        f.text().then(t => {
            currentSrc = null; currentName = f.name;
            setText(t);
            setLoadedLabel('opened: ' + f.name);
            showMessage('Opened ' + f.name, 'success');
        }).catch(err => showMessage('Could not read that file: ' + err, 'error'));
        file.value = '';
    });
    open.addEventListener('click', () => file.click());
    bar.appendChild(open);
    bar.appendChild(file);

    const dl = document.createElement('button');
    dl.id = 'btnGenDownload';
    dl.textContent = '\u2B07 Download';
    dl.addEventListener('click', () => {
        if (!doc.rows.length) { showMessage('Nothing to download', 'error'); return; }
        const blob = new Blob([serialize(doc)], { type: 'text/plain' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = currentName || 'session.ledc';
        document.body.appendChild(a); a.click();
        setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
    });
    bar.appendChild(dl);

    if (isFileProtocol()) {
        const note = document.createElement('span');
        note.className = 'gen-meter';
        note.textContent = 'Opened from a file \u2014 the Community browser needs a web server.';
        bar.appendChild(note);
    }
}

/*
 * Jump to a source line in the Text view.
 *
 * Handed to the Simulate view through its ctx rather than imported from here,
 * because generator.js already imports views/sim.js — going the other way would
 * make a cycle for the sake of one callback.
 */
function revealSourceLine(rowIndex) {
    switchView('text');
    if (textView && textView.revealLine) textView.revealLine(rowIndex);
}

function setLoadedLabel(text) {
    const lbl = document.getElementById('genLoadedName');
    if (lbl) lbl.textContent = text ? '(' + text + ')' : '';
}

function loadDemo(name) {
    const d = DEMOS.find(x => x.name === name);
    if (!d) return;
    currentSrc = null;
    currentName = d.name;
    setText(d.text);
    setLoadedLabel('example: ' + d.title);
}

export function generatorInit() {
    document.querySelectorAll('.gen-subtab').forEach(b =>
        b.addEventListener('click', () => switchView(b.dataset.view)));
    bind('btnGenRefresh', refreshGenList);
    bind('btnGenLoad', loadSelected);
    bind('btnGenSave', saveCurrent);
    bind('btnGenSaveAs', saveAs);
    bind('btnGenPlay', onPlay);
    bind('btnGenApply', onApplyLive);
    bind('btnGenBounce', onBounce);

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

    // Simulate view: play the session in the browser with no device attached.
    // Read-only with respect to the model (it never calls setDoc), so it needs
    // no re-entrancy guard -- but it DOES hold an AudioContext and a rAF loop,
    // which show()/hide() start and stop.
    simView = initSimView({ getDoc, setDoc, revealSourceLine });
    onModelChanged(d => { if (simView) simView.refresh(d); });

    if (STANDALONE) {
        applyStandaloneChrome();
        // Land on Simulate with an example already loaded: someone arriving
        // from "try it online" should see the thing working rather than an
        // empty text box. Nothing makes a sound until they press Play.
        if (DEMOS.length) {
            loadDemo(DEMOS[0].name);
            switchView('simulate');
        } else {
            switchView('text');
            setDoc(emptyDoc());
        }
    } else {
        switchView('text');
        setDoc(emptyDoc());     // initial empty preview/meter + text view
    }
    refreshGenList();       // populate the config dropdown
}
