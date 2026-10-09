// DAW lane view (Phase 4).
//
// A projection of the single shared `doc` (same ctx { getDoc, setDoc } contract
// as views/text.js and views/table.js). While this view is active the lane
// model is the working source of truth; every edit re-projects to the flat doc
// via lane_serialize.lanesToDoc (which also embeds the `# @ave-lane` metadata
// comment for byte-perfect reopen). Incoming model changes from other views are
// re-imported with lane_serialize.docToLanes.
//
//   - canvas render + gestures  -> views/lane_canvas.js
//   - lanes <-> flat doc        -> views/lane_serialize.js
//   - undo / redo               -> gen/undo.js
//   - keyframe inspector        -> reuses views/cell.js (buildCellEditor)

import { cell, WAVE_TYPES, DEFAULT_MOD_PERIOD_MS } from '../model.js';
import {
    docToLanes, lanesToDoc, snapToBandBoundaries,
    shapeToInterp, interpToShape, isPeriodicShape, keyframe,
} from './lane_serialize.js';
import { createLaneCanvas } from './lane_canvas.js';
import { createUndoStack } from '../undo.js';
import { buildCellEditor, openPopover, closeOpenPopover } from './cell.js';
import {
    parsePulseCell, formatPulseCell, parsePulseEnv, formatPulseEnv,
    parsePulseJitter, formatPulseJitter,
} from '../pulse.js';
import { getState } from '../transport.js';

import { startPolling } from '../../poll.js';
function fakeAnchor(x, y) {
    return { getBoundingClientRect: () => ({ left: x, top: y, bottom: y, right: x, width: 0, height: 0 }) };
}

export function initLaneView(ctx) {
    const root = document.getElementById('genLaneView');
    if (!root) return { refresh() {}, show() {}, hide() {} };

    let lanes = { version: 1, lanes: [] };
    let applyingEdit = false;
    let visible = false;
    let lastConflicts = [];
    let playheadMs = null;
    let pollTimer = null;

    // ---- chrome -----------------------------------------------------------
    root.innerHTML = '';
    const toolbar = document.createElement('div');
    toolbar.className = 'gen-lane-toolbar';
    root.appendChild(toolbar);

    const warnStrip = document.createElement('div');
    warnStrip.className = 'gen-lane-warn';
    warnStrip.style.display = 'none';
    root.appendChild(warnStrip);

    const scroller = document.createElement('div');
    scroller.className = 'gen-lane-scroll';
    const canvas = document.createElement('canvas');
    canvas.className = 'gen-lane-canvas';
    scroller.appendChild(canvas);
    root.appendChild(scroller);

    // ---- undo -------------------------------------------------------------
    const undo = createUndoStack({
        get: () => lanes,
        set: (s) => { lanes = s; commitFromLanes(); rerender(); },
    });

    // ---- model <-> doc ----------------------------------------------------
    function importFromDoc() {
        lanes = docToLanes(ctx.getDoc());
        if (!lanes.version) lanes.version = 1;
        if (!Array.isArray(lanes.lanes)) lanes.lanes = [];
    }

    // Re-project lanes -> doc and commit (without rebuilding our own canvas).
    function commitFromLanes() {
        const { doc, warnings, conflicts } = lanesToDoc(lanes, { withMeta: true });
        lastConflicts = conflicts;
        renderWarnings(warnings, conflicts);
        applyingEdit = true;
        try {
            const d = ctx.getDoc();
            d.rows = doc.rows;
            d.bg = doc.bg;
            ctx.setDoc(d);
        } finally { applyingEdit = false; }
    }

    // canvas callbacks
    const lc = createLaneCanvas(canvas, {
        getModel: () => lanes,
        beginEdit: () => undo.push(),
        commit: () => commitFromLanes(),
        getPlayheadMs: () => playheadMs,
        setPlayheadMs: (ms) => { playheadMs = ms; },
        openInspector: openInspector,
        openContextMenu: openContextMenu,
    });

    function rerender() { lc.render(); }

    // ---- inspector --------------------------------------------------------
    function nextKeyValue(lane, field, t) {
        const keys = (lane.sub[field] && lane.sub[field].keys) || [];
        let best = null;
        for (const k of keys.slice().sort((a, b) => a.t - b.t)) {
            if (k.t > t) { best = k.v; break; }
        }
        return best;
    }

    function openInspector(lane, field, kf, pt, onChanged) {
        undo.push();
        if (field === 'wave') { openWaveInspector(lane, field, kf, pt, onChanged); return; }
        if (field === 'freqR') { openFreqRInspector(lane, field, kf, pt, onChanged); return; }

        const init = cell(kf.v, shapeToInterp(kf.shape), kf.lfoEnd, kf.lfoPeriodMs);
        const form = buildCellEditor(init, {
            resolveTarget: () => nextKeyValue(lane, field, kf.t),
            onChange: (c) => {
                kf.v = c.value;
                kf.shape = interpToShape(c.interp);
                if (isPeriodicShape(kf.shape)) {
                    kf.lfoEnd = (c.modEnd === null || c.modEnd === undefined) ? c.value : c.modEnd;
                    kf.lfoPeriodMs = (c.modPeriodMs === null || c.modPeriodMs === undefined)
                        ? DEFAULT_MOD_PERIOD_MS : c.modPeriodMs;
                } else { delete kf.lfoEnd; delete kf.lfoPeriodMs; }
                commitFromLanes();
                if (onChanged) onChanged();
            },
        });
        openPopover(fakeAnchor(pt.x, pt.y), form, lane.name + ' · ' + field);
    }

    function openWaveInspector(lane, field, kf, pt, onChanged) {
        const form = document.createElement('div');
        form.className = 'gen-cell-editor';
        const lab = document.createElement('label');
        lab.className = 'gen-cell-field';
        lab.append('Wave');
        const sel = document.createElement('select');
        for (let i = 0; i < WAVE_TYPES.length; i++) {
            const o = document.createElement('option');
            o.value = String(i); o.textContent = WAVE_TYPES[i];
            if (kf.v === i) o.selected = true;
            sel.appendChild(o);
        }
        sel.addEventListener('change', () => {
            kf.v = parseInt(sel.value, 10); commitFromLanes(); if (onChanged) onChanged();
        });
        lab.appendChild(sel);
        form.appendChild(lab);
        openPopover(fakeAnchor(pt.x, pt.y), form, lane.name + ' · wave marker');
    }

    function openFreqRInspector(lane, field, kf, pt, onChanged) {
        const form = document.createElement('div');
        form.className = 'gen-cell-editor';
        const lab = document.createElement('label');
        lab.className = 'gen-cell-field';
        lab.append('Binaural freq_r (Hz, 0 = mono)');
        const inp = document.createElement('input');
        inp.type = 'number'; inp.value = String(kf.v);
        inp.addEventListener('input', () => {
            const v = parseFloat(inp.value); kf.v = Number.isFinite(v) ? v : 0;
            commitFromLanes(); if (onChanged) onChanged();
        });
        lab.appendChild(inp);
        form.appendChild(lab);
        openPopover(fakeAnchor(pt.x, pt.y), form, lane.name + ' · binaural');
    }

    function openContextMenu(lane, field, kf, pt, onChanged) {
        const menu = document.createElement('div');
        menu.className = 'gen-row-menu';
        const item = (label, fn) => {
            const b = document.createElement('button');
            b.type = 'button'; b.className = 'gen-row-menu-item'; b.textContent = label;
            b.addEventListener('click', () => { closeOpenPopover(); fn(); if (onChanged) onChanged(); });
            menu.appendChild(b);
        };
        const keys = () => lane.sub[field].keys;
        item('Delete keyframe', () => {
            undo.push();
            const arr = keys();
            const i = arr.indexOf(kf);
            if (i >= 0) arr.splice(i, 1);
            commitFromLanes();
        });
        item('Duplicate', () => {
            undo.push();
            const dup = keyframe(kf.t + 1000, kf.v, kf.shape, kf.lfoEnd, kf.lfoPeriodMs);
            keys().push(dup); keys().sort((a, b) => a.t - b.t);
            commitFromLanes();
        });
        item('Set curve: step', () => { undo.push(); setShape(kf, 'step'); commitFromLanes(); });
        item('Set curve: linear  >', () => { undo.push(); setShape(kf, 'lin'); commitFromLanes(); });
        item('Set curve: quadratic  *', () => { undo.push(); setShape(kf, 'quad'); commitFromLanes(); });
        item('Convert to modulation (sine)', () => { undo.push(); setShape(kf, 'sine'); commitFromLanes(); });
        item('Pulse fields at this time…', () => { openPulseInspector(lane, kf.t, pt); });
        openPopover(fakeAnchor(pt.x, pt.y), menu, 'Keyframe');
    }

    // Edit the v2 pulse fields (env/phase/attack/jitter, audio duty) for this
    // lane at time `t`. Stored in the time-keyed lane.pulse sidecar (see
    // lane_serialize.js) — not a sublane, since pulse fields are step-only.
    function openPulseInspector(lane, t, pt) {
        undo.push();
        if (!lane.pulse) lane.pulse = {};
        const rec = Object.assign({}, lane.pulse[t]);
        const isAudio = lane.kind === 'audio';
        const form = document.createElement('div');
        form.className = 'gen-cell-editor gen-pulse-fields';

        function apply(fieldName, val) {
            if (val === undefined) delete rec[fieldName];
            else rec[fieldName] = val;
            if (Object.keys(rec).length) lane.pulse[t] = rec;
            else delete lane.pulse[t];
            commitFromLanes();
        }
        function labeled(text, el) {
            const l = document.createElement('label');
            l.className = 'gen-cell-field';
            l.append(text);
            l.appendChild(el);
            form.appendChild(l);
        }
        function textField(text, fieldName, fmt, parse) {
            const inp = document.createElement('input');
            inp.type = 'text'; inp.className = 'gen-num';
            inp.placeholder = 'off'; inp.value = fmt(rec[fieldName]);
            inp.title = 'blank = off · - = leave unchanged · value = set';
            inp.addEventListener('change', () => apply(fieldName, parse(inp.value)));
            labeled(text, inp);
        }

        // env — select
        const envSel = document.createElement('select');
        const names = isAudio
            ? ['square', 'sine', 'triangle', 'trapezoid', 'tremolo']
            : ['square', 'sine', 'triangle', 'trapezoid'];
        const mk = (v, lbl) => { const o = document.createElement('option'); o.value = v; o.textContent = lbl; envSel.appendChild(o); };
        mk('', '(off)'); mk('-', '— leave');
        names.forEach((n, i) => mk(String(i), i + ' ' + n));
        envSel.value = formatPulseEnv(rec.env);
        envSel.addEventListener('change', () => apply('env', parsePulseEnv(envSel.value)));

        if (isAudio) textField('Duty %', 'duty', formatPulseCell, parsePulseCell);
        labeled('Env', envSel);
        textField('Phase°', 'phase', formatPulseCell, parsePulseCell);
        textField('Attack ms', 'attack', formatPulseCell, parsePulseCell);
        textField('Jitter', 'jitter', formatPulseJitter, parsePulseJitter);

        openPopover(fakeAnchor(pt.x, pt.y), form, lane.name + ' · pulse @ ' + t + 'ms');
    }

    function setShape(kf, shape) {
        kf.shape = shape;
        if (isPeriodicShape(shape)) {
            if (kf.lfoEnd === undefined || kf.lfoEnd === null) kf.lfoEnd = kf.v;
            if (kf.lfoPeriodMs === undefined || kf.lfoPeriodMs === null) kf.lfoPeriodMs = DEFAULT_MOD_PERIOD_MS;
        } else { delete kf.lfoEnd; delete kf.lfoPeriodMs; }
    }

    // ---- toolbar + warnings ----------------------------------------------
    function renderToolbar() {
        toolbar.innerHTML = '';
        const btn = (label, title, fn) => {
            const b = document.createElement('button');
            b.type = 'button'; b.textContent = label; if (title) b.title = title;
            b.addEventListener('click', fn);
            toolbar.appendChild(b);
            return b;
        };
        btn('↶ Undo', 'Ctrl/Cmd-Z', () => { undo.undo(); });
        btn('↷ Redo', 'Shift+Ctrl/Cmd-Z', () => { undo.redo(); });
        btn('⤢ Fit', 'Fit session to view', () => { lc.fitToContent(); rerender(); });
        btn('+ Audio lane', 'Add an audio channel lane', () => addLane('audio'));
        btn('+ LED lane', 'Add an LED mask lane', () => addLane('led'));
        const reimport = btn('⟳ From doc', 'Re-import lanes from the shared model', () => {
            importFromDoc(); lc.fitToContent(); rerender();
        });
        reimport.style.marginLeft = 'auto';
    }

    function addLane(kind) {
        undo.push();
        if (kind === 'audio') {
            const used = new Set(lanes.lanes.filter(l => l.kind === 'audio').map(l => l.key));
            let ch = 1; while (used.has(ch) && ch < 16) ch++;
            lanes.lanes.push({ kind: 'audio', key: ch, name: 'Audio ch ' + ch,
                collapsed: false, sub: { freq: { keys: [keyframe(0, 200, 'step')] },
                    pan: { keys: [] }, vol: { keys: [keyframe(0, 60, 'step')] }, mod: { keys: [] } } });
        } else {
            const used = new Set(lanes.lanes.filter(l => l.kind === 'led').map(l => l.key));
            let mask = 1; while (used.has(mask) && mask < 256) mask <<= 1;
            lanes.lanes.push({ kind: 'led', key: mask & 0xFF || 1, name: 'LED ch (mask ' + (mask & 0xFF) + ')',
                collapsed: false, colorSplit: false,
                sub: { freq: { keys: [keyframe(0, 8, 'step')] }, duty: { keys: [keyframe(0, 50, 'step')] },
                    bright: { keys: [keyframe(0, 50, 'step')] },
                    r: { keys: [keyframe(0, 255, 'step')] }, g: { keys: [keyframe(0, 255, 'step')] },
                    b: { keys: [keyframe(0, 255, 'step')] } } });
        }
        commitFromLanes();
        rerender();
    }

    function renderWarnings(warnings, conflicts) {
        const msgs = (warnings || []).slice();
        warnStrip.innerHTML = '';
        if (!msgs.length) { warnStrip.style.display = 'none'; return; }
        warnStrip.style.display = '';
        for (const m of msgs) {
            const d = document.createElement('div');
            d.className = 'gen-lane-warn-item';
            d.textContent = '⚠ ' + m;
            warnStrip.appendChild(d);
        }
        if (conflicts && conflicts.length) {
            const fix = document.createElement('button');
            fix.type = 'button';
            fix.className = 'gen-lane-snap';
            fix.textContent = 'Snap conflicting points to band boundaries';
            fix.addEventListener('click', () => {
                undo.push();
                lanes = snapToBandBoundaries(lanes);
                commitFromLanes();
                rerender();
            });
            warnStrip.appendChild(fix);
        }
    }

    // ---- playhead polling -------------------------------------------------
    function pollPlayhead() {
        getState().then(s => {
            const pos = (s && (s.position_ms !== undefined ? s.position_ms
                : (s.timeline && s.timeline.position_ms)));
            playheadMs = (pos === undefined) ? null : pos;
            if (visible) lc.render();
        }).catch(() => { /* offline / no device — leave playhead as-is */ });
    }

    // ---- keyboard (undo/redo) --------------------------------------------
    function onKey(e) {
        if (!visible) return;
        if (undo.handleKey(e)) e.preventDefault();
    }
    document.addEventListener('keydown', onKey);

    // ---- view contract ----------------------------------------------------
    function refresh(doc) {
        if (applyingEdit) return;   // our own edit — canvas already current
        if (!visible) return;       // re-import lazily on show()
        importFromDoc();
        rerender();
    }
    function show() {
        visible = true;
        root.style.display = '';
        importFromDoc();
        renderToolbar();
        lc.fitToContent();
        rerender();
        if (!pollTimer) pollTimer = startPolling(pollPlayhead, 1000);
    }
    function hide() {
        visible = false;
        root.style.display = 'none';
        closeOpenPopover();
        if (pollTimer) { pollTimer(); pollTimer = null; }
    }

    return { refresh, show, hide };
}
