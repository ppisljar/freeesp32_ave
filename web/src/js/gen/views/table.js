// Generator Table view (Phase 3).
//
// A typed spreadsheet grid over `doc.rows`, plus a mobile card-per-row
// fallback. It is a projection of the single shared `doc` owned by
// gen/generator.js (same ctx { getDoc, setDoc } contract as views/text.js).
//
// Bidirectional sync:
//   - A grid edit mutates the relevant Row field in place, then commits via
//     ctx.setDoc(getDoc()), which re-renders the preview and emits
//     onModelChanged so the Text view + preview reflect it. Field edits set an
//     internal `applyingEdit` guard so our own onModelChanged echo does NOT
//     rebuild the grid (caret/scroll preserved); structural edits (add / delete
//     / move / type / chips) intentionally rebuild.
//   - An incoming onModelChanged (e.g. from the Text view) calls refresh(doc),
//     which rebuilds the grid, preserving scroll position.
//
// Per the Bug-avoidance contract, the grid only ever SETS model fields; the
// shared serialize.js turns them into correct text (OR'd mask, ±100 pan,
// 8-field RGB, wave_type + freq_r placeholder, noise, all 7 prefixes).

import {
    ledRow, audioRow, commentRow, bgRow, bg, speechRow, cell,
    WAVE_TYPES, NUM_AUDIO_CHANNELS, NUM_LED_CHANNELS,
} from '../model.js';
import { createCompoundCell, closeOpenPopover, openPopover } from './cell.js';
import { parsePulseEnv, formatPulseEnv } from '../pulse.js';
import { previewSpeech } from '../tts.js';
import { decodeFile, audioBufferToWav16, wavBlob } from '../bgaudio.js';
import * as bgstore from '../bgstore.js';
import { showMessage } from '../../util.js';

// ---- Pure helpers (unit-tested) -------------------------------------------

// 8 booleans (bit 0..7 => LED channels 1..8) -> OR'd 8-bit mask.
export function chipsToMask(chips) {
    let m = 0;
    for (let i = 0; i < NUM_LED_CHANNELS; i++) if (chips[i]) m |= (1 << i);
    return m;
}

// 8-bit mask -> array of 8 booleans (bit 0..7 => channels 1..8).
export function maskToChips(mask) {
    const out = [];
    for (let i = 0; i < NUM_LED_CHANNELS; i++) out.push(!!(mask & (1 << i)));
    return out;
}

// Parse a Time field: plain integer ms OR `mm:ss.mmm` (also `m:ss`, `:ss.mmm`).
// Returns milliseconds (Number) or null when unparsable.
export function parseTimeInput(str) {
    if (str === null || str === undefined) return null;
    str = String(str).trim();
    if (str === '') return null;
    if (/^\d+$/.test(str)) return parseInt(str, 10);
    const m = str.match(/^(\d*):([0-5]?\d)(?:\.(\d{1,3}))?$/);
    if (!m) return null;
    const mm = m[1] === '' ? 0 : parseInt(m[1], 10);
    const ss = parseInt(m[2], 10);
    const frac = m[3] ? parseInt(m[3].padEnd(3, '0'), 10) : 0;
    return mm * 60000 + ss * 1000 + frac;
}

// Milliseconds -> `mm:ss.mmm` (mm not zero-padded; ss + mmm padded).
export function formatTimeMs(ms) {
    ms = Math.max(0, Math.round(ms || 0));
    const mm = Math.floor(ms / 60000);
    const ss = Math.floor((ms % 60000) / 1000);
    const mmm = ms % 1000;
    return mm + ':' + String(ss).padStart(2, '0') + '.' + String(mmm).padStart(3, '0');
}

// Group row indices by timestamp so the grid can rule off one moment from the
// next. Several rows normally share a time — an audio line, its LED line and a
// speech cue at t=3000 are one instant of the session — and without a divider
// the grid reads as one undifferentiated run.
//
// Untimed rows (comments, bg) join the block that FOLLOWS them, so a section
// comment heads its group rather than trailing the previous one. Trailing
// untimed rows stay with the last block. Returns an array of group ids
// parallel to `rows`, starting at 1.
export function computeTimeGroups(rows) {
    const isTimed = r => r.kind === 'led' || r.kind === 'audio' || r.kind === 'speech';
    const groups = new Array(rows.length).fill(0);
    let group = 0, prevTime = null, pending = [];
    for (let i = 0; i < rows.length; i++) {
        if (!isTimed(rows[i])) { pending.push(i); continue; }
        if (prevTime === null || rows[i].time !== prevTime) { group++; prevTime = rows[i].time; }
        for (const j of pending) groups[j] = group;
        pending = [];
        groups[i] = group;
    }
    for (const j of pending) groups[j] = group || 1;
    return groups;
}

// Display order within one instant: speech, then LED, then audio. Untimed rows
// (comments, bg) lead the block they head. Model order breaks ties, so rows of
// the same kind keep the order the file gave them.
//
// A canonical order makes a moment readable at a glance — the cue you hear, the
// light that goes with it, the tone underneath — instead of whatever sequence
// the file happened to be written in.
const DISPLAY_KIND_RANK = { comment: 0, bg: 0, blank: 0, raw: 0, speech: 1, led: 2, audio: 3 };

// Returns model-row indices in display order. The view reorders; `doc.rows` and
// therefore the serialized file are untouched.
export function orderRowsForDisplay(rows) {
    const groups = computeTimeGroups(rows);
    // Groups keep the order they first appear in the file, so the timeline still
    // reads top-to-bottom even though rows move within a group.
    const firstSeen = new Map();
    groups.forEach((g, i) => { if (!firstSeen.has(g)) firstSeen.set(g, i); });
    const rank = r => (r.kind in DISPLAY_KIND_RANK) ? DISPLAY_KIND_RANK[r.kind] : 9;
    return rows.map((_, i) => i).sort((a, b) => {
        if (groups[a] !== groups[b]) return firstSeen.get(groups[a]) - firstSeen.get(groups[b]);
        const ra = rank(rows[a]), rb = rank(rows[b]);
        if (ra !== rb) return ra - rb;
        return a - b;   // explicit tie-break: same kind keeps file order
    });
}

// ---- Row helpers -----------------------------------------------------------

function cloneCell(c) { return c ? { ...c } : cell(0); }

// Preserve the v2 pulse-field tri-state: undefined (omit) / null (`-` leave
// unchanged) / value. Only compound cells and the jitter object are deep-copied;
// enums, numbers, null and undefined pass through untouched.
function cloneOpt(v) {
    if (v === undefined || v === null) return v;
    if (typeof v === 'object') return { ...v };
    return v;
}

function cloneRow(r) {
    switch (r.kind) {
        case 'led': return ledRow({
            time: r.time, freq: cloneCell(r.freq), duty: cloneCell(r.duty),
            bright: cloneCell(r.bright), r: cloneCell(r.r), g: cloneCell(r.g),
            b: cloneCell(r.b), mask: r.mask, legacy5: r.legacy5,
            // v2 pulse fields — tri-state preserved so Duplicate/type-change is lossless.
            env: cloneOpt(r.env), phase: cloneOpt(r.phase),
            attack: cloneOpt(r.attack), jitter: cloneOpt(r.jitter),
            inlineComment: r.inlineComment,
        });
        case 'audio': return audioRow({
            time: r.time, freq: cloneCell(r.freq), pan: cloneCell(r.pan),
            vol: cloneCell(r.vol), mod: cloneCell(r.mod), channel: r.channel,
            freqR: r.freqR, waveType: r.waveType,
            // v2 pulse fields — tri-state preserved.
            duty: cloneOpt(r.duty), env: cloneOpt(r.env), phase: cloneOpt(r.phase),
            attack: cloneOpt(r.attack), jitter: cloneOpt(r.jitter),
            inlineComment: r.inlineComment,
        });
        case 'comment': return commentRow(r.text);
        case 'bg': return bgRow(bg(r.bg.url, r.bg.pan, r.bg.loudness));
        case 'speech': return speechRow({ time: r.time, voice: r.voice, volume: r.volume, text: r.text });
        case 'blank': return { kind: 'blank' };
        default: return { kind: 'raw', text: r.text, error: r.error };
    }
}

function newRowOfKind(kind, time) {
    switch (kind) {
        case 'led':     return ledRow({ time: time || 0 });
        case 'audio':   return audioRow({ time: time || 0, channel: 1 });
        case 'bg':      return bgRow(bg('', 0, 50));
        case 'speech':  return speechRow({ time: time || 0, voice: 'en-US', volume: 80, text: '' });
        case 'comment': return commentRow('# ');
        default:        return ledRow({ time: time || 0 });
    }
}

// ---- View ------------------------------------------------------------------

export function initTableView(ctx) {
    const root = document.getElementById('genTableView');
    if (!root) return { refresh() {}, show() {}, hide() {} };

    let applyingEdit = false; // true while WE commit a value edit (skip our own rebuild)
    let visible = false;
    // Per-kind visibility. Independent toggles rather than one mutually
    // exclusive filter: "LED only" used to leave speech and comments on screen,
    // and there was no way to drop them, so the LED view was never actually
    // only LED. All on by default. bg/raw always show; blank rows never do.
    const shown = { led: true, audio: true, speech: true, comment: true };

    // Build the static chrome (filter bar / scroller / cards / add bar) once.
    root.innerHTML = '';

    const filterBar = document.createElement('div');
    filterBar.className = 'gen-table-filters';
    root.appendChild(filterBar);

    const gridScroll = document.createElement('div');
    gridScroll.className = 'gen-grid-scroll';
    root.appendChild(gridScroll);

    const cards = document.createElement('div');
    cards.className = 'gen-cards';
    root.appendChild(cards);

    const addBar = document.createElement('div');
    addBar.className = 'gen-table-add';
    root.appendChild(addBar);

    // Every compound trigger currently on screen. A ramp's sub-label reports a
    // value that lives in a DIFFERENT row, so editing the row a ramp points at
    // has to refresh the ramp's hint — and a value edit deliberately skips the
    // grid rebuild that would otherwise do it. Rebuilt with the grid.
    let cellTriggers = [];

    // ---- Commit helpers ---------------------------------------------------
    // Value edit: preview/text update, but DON'T rebuild our own grid.
    function commitValue() {
        applyingEdit = true;
        try { ctx.setDoc(ctx.getDoc()); } finally { applyingEdit = false; }
        for (const t of cellTriggers) t.repaint();
    }
    // Structural edit: commit AND rebuild (setDoc emits -> refresh rebuilds).
    function commitStructure() {
        ctx.setDoc(ctx.getDoc());
    }

    function recomputeBg() {
        const d = ctx.getDoc();
        let last = null;
        for (const r of d.rows) if (r.kind === 'bg') last = r.bg;
        d.bg = last;
    }

    // ---- Ramp target resolution (for the compound-cell readout) -----------
    function ledRampTarget(idx, mask, field) {
        const rows = ctx.getDoc().rows;
        for (let j = idx + 1; j < rows.length; j++) {
            const r = rows[j];
            if (r.kind === 'led' && (r.mask & mask) && r[field]) return r[field].value;
        }
        return null;
    }
    function audioRampTarget(idx, channel, field) {
        const rows = ctx.getDoc().rows;
        for (let j = idx + 1; j < rows.length; j++) {
            const r = rows[j];
            if (r.kind === 'audio' && r.channel === channel && r[field]) return r[field].value;
        }
        return null;
    }

    // ---- Small widget builders -------------------------------------------
    function typeSelect(row, idx) {
        const sel = document.createElement('select');
        sel.className = 'gen-type-select';
        for (const [val, label] of [['led', 'LED'], ['audio', 'A'], ['bg', 'BG'], ['speech', 'S'], ['comment', '#']]) {
            const o = document.createElement('option');
            o.value = val; o.textContent = label;
            if (val === row.kind) o.selected = true;
            sel.appendChild(o);
        }
        sel.addEventListener('change', () => {
            const d = ctx.getDoc();
            const t = (row.kind === 'led' || row.kind === 'audio' || row.kind === 'speech') ? row.time : 0;
            d.rows[idx] = newRowOfKind(sel.value, t);
            recomputeBg();
            commitStructure();
        });
        return sel;
    }

    function timeCell(row) {
        const wrap = document.createElement('span');
        wrap.className = 'gen-time-cell';
        const inp = document.createElement('input');
        inp.type = 'text';
        inp.className = 'gen-time-input';
        inp.value = String(row.time);
        const hint = document.createElement('span');
        hint.className = 'gen-time-hint';
        hint.textContent = formatTimeMs(row.time);
        inp.addEventListener('input', () => {
            const ms = parseTimeInput(inp.value);
            if (ms === null) { inp.classList.add('invalid'); return; }
            inp.classList.remove('invalid');
            row.time = ms;
            hint.textContent = formatTimeMs(ms);
            commitValue();
        });
        wrap.appendChild(inp);
        wrap.appendChild(hint);
        return wrap;
    }

    function maskChips(row) {
        if (row.mask === null) return unchangedChip('mask');
        const wrap = document.createElement('span');
        wrap.className = 'gen-mask-chips';
        const chips = [];
        function repaint() {
            const on = maskToChips(row.mask);
            for (let i = 0; i < chips.length; i++) chips[i].classList.toggle('on', on[i]);
            wrap.classList.toggle('invalid', (row.mask & 0xFF) === 0);
        }
        for (let i = 0; i < NUM_LED_CHANNELS; i++) {
            const c = document.createElement('button');
            c.type = 'button';
            c.className = 'gen-chip';
            c.textContent = String(i + 1);
            c.addEventListener('click', () => {
                row.mask = (row.mask ^ (1 << i)) & 0xFF;
                repaint();
                commitValue();
            });
            chips.push(c);
            wrap.appendChild(c);
        }
        for (const [label, val] of [['4', 0x0F], ['all', 0xFF]]) {
            const p = document.createElement('button');
            p.type = 'button';
            p.className = 'gen-chip gen-chip-preset';
            p.textContent = label;
            p.addEventListener('click', () => { row.mask = val; repaint(); commitValue(); });
            wrap.appendChild(p);
        }
        repaint();
        return wrap;
    }

    function channelSelect(row) {
        const sel = document.createElement('select');
        sel.className = 'gen-ch-select';
        const none = document.createElement('option');
        none.value = ''; none.textContent = '—';
        sel.appendChild(none);
        for (let i = 1; i <= NUM_AUDIO_CHANNELS; i++) {
            const o = document.createElement('option');
            o.value = String(i); o.textContent = String(i);
            if (row.channel === i) o.selected = true;
            sel.appendChild(o);
        }
        if (row.channel === null || row.channel === undefined) none.selected = true;
        sel.addEventListener('change', () => {
            row.channel = sel.value === '' ? null : parseInt(sel.value, 10);
            commitValue();
        });
        return sel;
    }

    function waveSelect(row) {
        const sel = document.createElement('select');
        sel.className = 'gen-wave-select';
        const none = document.createElement('option');
        none.value = ''; none.textContent = '(sine)';
        sel.appendChild(none);
        for (let i = 0; i < WAVE_TYPES.length; i++) {
            const o = document.createElement('option');
            o.value = String(i); o.textContent = WAVE_TYPES[i];
            if (row.waveType === i) o.selected = true;
            sel.appendChild(o);
        }
        if (row.waveType === null || row.waveType === undefined) none.selected = true;
        sel.addEventListener('change', () => {
            row.waveType = sel.value === '' ? null : parseInt(sel.value, 10);
            commitValue();
        });
        return sel;
    }

    function freqRInput(row) {
        const inp = document.createElement('input');
        inp.type = 'number'; inp.className = 'gen-num';
        inp.value = String(row.freqR || 0);
        inp.addEventListener('input', () => {
            const v = parseFloat(inp.value);
            row.freqR = Number.isFinite(v) ? v : 0;
            commitValue();
        });
        return inp;
    }

    function colorSwatch(row) {
        if (row.r === null || row.g === null || row.b === null) return unchangedChip('color');
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'gen-swatch';
        function paint() {
            btn.style.background = 'rgb(' + (row.r.value | 0) + ',' + (row.g.value | 0) + ',' + (row.b.value | 0) + ')';
            const ramped = [row.r, row.g, row.b].some(c => c.interp !== 'none');
            btn.textContent = ramped ? '■→■' : '';
            btn.classList.toggle('is-ramp', ramped);
        }
        paint();
        btn.addEventListener('click', () => {
            const form = document.createElement('div');
            form.className = 'gen-color-editor';

            const native = document.createElement('input');
            native.type = 'color';
            const hex = (n) => ('0' + Math.max(0, Math.min(255, n | 0)).toString(16)).slice(-2);
            native.value = '#' + hex(row.r.value) + hex(row.g.value) + hex(row.b.value);
            native.addEventListener('input', () => {
                const v = native.value;
                row.r.value = parseInt(v.slice(1, 3), 16);
                row.g.value = parseInt(v.slice(3, 5), 16);
                row.b.value = parseInt(v.slice(5, 7), 16);
                paint();
                refreshChannelTriggers();
                commitValue();
            });
            const nl = document.createElement('label');
            nl.className = 'gen-cell-field';
            nl.append('Picker');
            nl.appendChild(native);
            form.appendChild(nl);

            const triggers = {};
            function makeChan(field, label) {
                const wrap = document.createElement('div');
                wrap.className = 'gen-color-chan';
                const lab = document.createElement('span');
                lab.textContent = label;
                const t = createCompoundCell({
                    title: label + ' channel',
                    getCell: () => row[field],
                    onChange: (c) => { row[field] = c; paint(); commitValue(); },
                    resolveTarget: () => null,
                });
                triggers[field] = t;
                wrap.appendChild(lab);
                wrap.appendChild(t);
                form.appendChild(wrap);
            }
            function refreshChannelTriggers() {
                // The compound triggers self-repaint via their own paint() only on
                // popover edits; nudge their labels after a native-picker change.
                for (const f of ['r', 'g', 'b']) {
                    if (triggers[f]) triggers[f].repaint();
                }
            }
            makeChan('r', 'R');
            makeChan('g', 'G');
            makeChan('b', 'B');

            openPopover(btn, form, 'Color');
        });
        return btn;
    }

    // A read-only "—" chip shown for a `-` (leave-unchanged / null) field. Editing
    // `-` fields in the grid is a later UI task; for now they render safely.
    function unchangedChip(title) {
        const s = document.createElement('span');
        s.className = 'gen-unchanged';
        s.textContent = '—';
        s.title = (title ? title + ': ' : '') + 'leave unchanged (-)';
        return s;
    }

    function compound(row, idx, field, title, resolve) {
        if (row[field] === null) return unchangedChip(title);
        const t = createCompoundCell({
            title: title,
            getCell: () => row[field],
            onChange: (c) => { row[field] = c; commitValue(); },
            resolveTarget: resolve,
        });
        cellTriggers.push(t);
        return t;
    }

    // ---- v2 pulse-field controls (env / phase / attack / jitter, audio duty) ----
    // Tri-state convention shared by every control: blank = omit (field absent),
    // '-' = leave unchanged (null), a value = set. env is a <select> (enum); the
    // compound fields + jitter are text inputs. Ramps ON pulse fields are a deferred
    // device feature — author those in the Text view; a numeric edit here sets a step.
    const LED_ENV_NAMES = ['square', 'sine', 'triangle', 'trapezoid'];
    const AUDIO_ENV_NAMES = ['square', 'sine', 'triangle', 'trapezoid', 'tremolo'];

    function pulseEnvSelect(row, isAudio) {
        const names = isAudio ? AUDIO_ENV_NAMES : LED_ENV_NAMES;
        const sel = document.createElement('select');
        sel.className = 'gen-wave-select';
        const mk = (val, label) => { const o = document.createElement('option'); o.value = val; o.textContent = label; sel.appendChild(o); };
        // Same wording as the tri-state selects beside it — env is the one pulse
        // field whose "set" state is an enum, not a number, but the three states
        // are identical and should read identically.
        mk('', 'off');
        mk('-', 'leave');
        names.forEach((n, i) => mk(String(i), i + ' ' + n));
        sel.value = formatPulseEnv(row.env);
        sel.title = 'off = omit the field · leave = "-" (device keeps its current value) · or pick an envelope';
        sel.addEventListener('change', () => { row.env = parsePulseEnv(sel.value); commitValue(); });
        return sel;
    }

    // The tri-state as a control rather than a spelling test. These fields were
    // text boxes where "" meant off, "-" meant leave-unchanged and a number meant
    // set — three different things you had to know to type, with an empty box
    // that looked broken rather than deliberate. Pick the state from a select;
    // the value input only exists when there is a value to give.
    const PULSE_MODES = [['', 'off'], ['-', 'leave'], ['v', 'set']];

    function pulseModeSelect(current) {
        const sel = document.createElement('select');
        sel.className = 'gen-pulse-mode';
        for (const [val, label] of PULSE_MODES) {
            const o = document.createElement('option');
            o.value = val;
            o.textContent = label;
            sel.appendChild(o);
        }
        sel.value = current === undefined ? '' : (current === null ? '-' : 'v');
        sel.title = 'off = omit the field · leave = "-" (device keeps its current value) · set = use the value';
        return sel;
    }

    function numberInput(placeholder, title) {
        const inp = document.createElement('input');
        inp.type = 'number';
        inp.className = 'gen-num';
        inp.placeholder = placeholder;
        if (title) inp.title = title;
        return inp;
    }

    // phase / attack / audio duty.
    function pulseCellInput(row, fieldName, title) {
        const wrap = document.createElement('span');
        wrap.className = 'gen-pulse-ctl';
        const sel = pulseModeSelect(row[fieldName]);
        const num = numberInput('0', title || fieldName);
        const cur = row[fieldName];
        num.value = (cur && typeof cur === 'object') ? String(cur.value) : '';

        const sync = () => { num.style.display = sel.value === 'v' ? '' : 'none'; };
        const commit = () => {
            if (sel.value === '') { row[fieldName] = undefined; }
            else if (sel.value === '-') { row[fieldName] = null; }
            else {
                const n = parseFloat(num.value);
                const v = Number.isFinite(n) ? n : 0;
                const prev = row[fieldName];
                // A ramp or modulation authored in the Text view lives in this
                // same cell. Change only the number so editing the value here
                // does not quietly flatten it to a step.
                row[fieldName] = (prev && typeof prev === 'object') ? { ...prev, value: v } : cell(v);
            }
            commitValue();
        };
        sel.addEventListener('change', () => {
            if (sel.value === 'v' && num.value === '') num.value = '0';
            sync();
            commit();
        });
        num.addEventListener('change', commit);
        sync();
        wrap.appendChild(sel);
        wrap.appendChild(num);
        return wrap;
    }

    // jitter: amplitude in Hz, plus an optional period. Two labelled boxes beat
    // the "amp:period" micro-syntax the text field used to require.
    function pulseJitterInput(row) {
        const wrap = document.createElement('span');
        wrap.className = 'gen-pulse-ctl';
        const sel = pulseModeSelect(row.jitter);
        const amp = numberInput('Hz', 'Jitter amplitude (Hz)');
        const per = numberInput('45000', 'Jitter period (ms) — blank uses the 45000 ms default');
        const j = row.jitter;
        // Accept the bare-number jitter the model permits as well as {amp, period}.
        amp.value = (j && typeof j === 'object') ? String(j.amp)
                  : (typeof j === 'number' ? String(j) : '');
        per.value = (j && typeof j === 'object' && j.period !== undefined && j.period !== null)
                  ? String(j.period) : '';

        const sync = () => {
            const on = sel.value === 'v';
            amp.style.display = on ? '' : 'none';
            per.style.display = on ? '' : 'none';
        };
        const commit = () => {
            if (sel.value === '') { row.jitter = undefined; }
            else if (sel.value === '-') { row.jitter = null; }
            else {
                const a = parseFloat(amp.value);
                const p = parseFloat(per.value);
                // period undefined round-trips as a bare amp, matching serialize.js.
                row.jitter = { amp: Number.isFinite(a) ? a : 0,
                               period: per.value === '' || !Number.isFinite(p) ? undefined : p };
            }
            commitValue();
        };
        sel.addEventListener('change', () => {
            if (sel.value === 'v' && amp.value === '') amp.value = '0';
            sync();
            commit();
        });
        amp.addEventListener('change', commit);
        per.addEventListener('change', commit);
        sync();
        wrap.appendChild(sel);
        wrap.appendChild(amp);
        wrap.appendChild(per);
        return wrap;
    }

    // The pulse-field control set for a row (LED or audio), used by the grid
    // more-row and the mobile card 'more' block.
    function pulseFields(row) {
        const wrap = document.createElement('div');
        wrap.className = 'gen-pulse-fields';
        const add = (label, el) => wrap.appendChild(field(label, el));
        if (row.kind === 'audio') add('Duty %', pulseCellInput(row, 'duty', 'Duty %'));
        add('Env', pulseEnvSelect(row, row.kind === 'audio'));
        add('Phase°', pulseCellInput(row, 'phase', 'Phase (deg)'));
        add('Attack ms', pulseCellInput(row, 'attack', 'Attack (ms)'));
        add('Jitter', pulseJitterInput(row));
        return wrap;
    }

    function actionsMenu(idx) {
        const wrap = document.createElement('span');
        wrap.className = 'gen-row-actions';
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'gen-row-menu-btn';
        btn.textContent = '⋮';
        wrap.appendChild(btn);
        btn.addEventListener('click', () => {
            const menu = document.createElement('div');
            menu.className = 'gen-row-menu';
            const item = (label, fn) => {
                const b = document.createElement('button');
                b.type = 'button';
                b.className = 'gen-row-menu-item';
                b.textContent = label;
                b.addEventListener('click', () => { closeOpenPopover(); fn(); });
                menu.appendChild(b);
            };
            const d = ctx.getDoc();
            item('Duplicate', () => {
                d.rows.splice(idx + 1, 0, cloneRow(d.rows[idx]));
                recomputeBg(); commitStructure();
            });
            item('Insert after', () => {
                const cur = d.rows[idx];
                const t = (cur.kind === 'led' || cur.kind === 'audio') ? cur.time : 0;
                const k = (cur.kind === 'led' || cur.kind === 'audio') ? cur.kind : 'led';
                d.rows.splice(idx + 1, 0, newRowOfKind(k, t));
                commitStructure();
            });
            item('Delete', () => {
                d.rows.splice(idx, 1);
                recomputeBg(); commitStructure();
            });
            // Move must follow what you can SEE. The grid sorts each instant into
            // speech/LED/audio, so a naive model-order swap can rewrite the file
            // while the grid looks identical — an invisible edit. Move the row to
            // its display neighbour's slot instead, and only offer the item when
            // the result actually differs on screen (reordering two rows the sort
            // puts straight back is a no-op worth hiding rather than performing).
            // Rendered rows only: blank lines and toggled-off kinds are in the
            // display order but not on screen, and stepping onto one of those
            // produces exactly the invisible edit this is here to prevent.
            const renderedOrder = (rows) => orderRowsForDisplay(rows).filter(i => passesFilter(rows[i]));
            const moveTrial = (dir) => {
                const order = renderedOrder(d.rows);
                const pos = order.indexOf(idx);
                if (pos < 0) return null;
                const target = order[pos + dir];
                if (target === undefined) return null;
                const trial = d.rows.slice();
                const moved = trial.splice(idx, 1)[0];
                trial.splice(target > idx ? target - 1 : target, 0, moved);
                // Compare by row identity, so "different" means visibly different.
                const before = order.map(i => d.rows[i]);
                const after = renderedOrder(trial).map(i => trial[i]);
                const same = after.length === before.length && after.every((r, i) => r === before[i]);
                return same ? null : trial;
            };
            const applyMove = (trial) => {
                d.rows.length = 0;
                for (const r of trial) d.rows.push(r);
                recomputeBg(); commitStructure();
            };
            const upTrial = moveTrial(-1);
            if (upTrial) item('Move up', () => applyMove(upTrial));
            const downTrial = moveTrial(+1);
            if (downTrial) item('Move down', () => applyMove(downTrial));
            openPopover(btn, menu, 'Row actions');
        });
        return wrap;
    }

    function bgUrlInput(row) {
        const url = document.createElement('input');
        url.type = 'text'; url.className = 'gen-bg-url';
        url.placeholder = 'http(s):// · sdcard:// · push://name';
        url.value = row.bg.url;
        url.addEventListener('input', () => { row.bg.url = url.value; recomputeBg(); commitValue(); });
        return url;
    }
    // Full BG source control: URL text + "📁 file" (loads a local file into the
    // library and points the row at push://name) + a library dropdown. A URL, a
    // local file, or an existing clip all end up as row.bg.url.
    function bgSourceControls(row) {
        const wrap = document.createElement('span');
        wrap.className = 'gen-bg-inputs';
        const url = bgUrlInput(row);

        const fileBtn = document.createElement('button');
        fileBtn.type = 'button'; fileBtn.className = 'gen-bg-filebtn'; fileBtn.textContent = '📁';
        fileBtn.title = 'Load a local audio file into the library and use it as BG';
        const fileInput = document.createElement('input');
        fileInput.type = 'file'; fileInput.accept = 'audio/*'; fileInput.style.display = 'none';
        fileBtn.addEventListener('click', () => fileInput.click());
        fileInput.addEventListener('change', async () => {
            const f = fileInput.files && fileInput.files[0];
            if (!f) return;
            showMessage('Decoding ' + f.name + '…', 'info');
            try {
                const abuf = await decodeFile(f);
                const name = f.name.replace(/\.[^.]+$/, '');
                await bgstore.put({ name, wavBlob: wavBlob(audioBufferToWav16(abuf)),
                                    durationMs: Math.round(abuf.duration * 1000), sourceKind: 'file' });
                row.bg.url = 'push://' + name;
                recomputeBg(); commitStructure();
                showMessage('Loaded "' + name + '" → BG set to push://' + name, 'success');
            } catch (e) { showMessage('Load failed: ' + e.message, 'error'); }
            fileInput.value = '';
        });

        const lib = document.createElement('select');
        lib.className = 'gen-bg-lib'; lib.title = 'Pick a clip from the library';
        const none = document.createElement('option');
        none.value = ''; none.textContent = 'library…';
        lib.appendChild(none);
        bgstore.list().then(items => {
            for (const m of items) {
                const o = document.createElement('option');
                o.value = m.name; o.textContent = m.name;
                if (row.bg.url === 'push://' + m.name) o.selected = true;
                lib.appendChild(o);
            }
        }).catch(() => {});
        lib.addEventListener('change', () => {
            if (!lib.value) return;
            row.bg.url = 'push://' + lib.value;
            recomputeBg(); commitStructure();
        });

        wrap.append(url, fileBtn, fileInput, lib);
        return wrap;
    }
    function bgPanInput(row) {
        const pan = document.createElement('input');
        pan.type = 'number'; pan.className = 'gen-num'; pan.title = 'pan -100..100';
        pan.value = String(row.bg.pan);
        pan.addEventListener('input', () => { const v = parseFloat(pan.value); row.bg.pan = Number.isFinite(v) ? v : 0; recomputeBg(); commitValue(); });
        return pan;
    }
    function bgVolInput(row) {
        const loud = document.createElement('input');
        loud.type = 'number'; loud.className = 'gen-num'; loud.title = 'loudness 0..100';
        loud.value = String(row.bg.loudness);
        loud.addEventListener('input', () => { const v = parseFloat(loud.value); row.bg.loudness = Number.isFinite(v) ? v : 0; recomputeBg(); commitValue(); });
        return loud;
    }
    // Composite used by the mobile card (single field, all three inline).
    function bgInputs(row) {
        const wrap = document.createElement('span');
        wrap.className = 'gen-bg-inputs';
        wrap.append(bgSourceControls(row), ' pan ', bgPanInput(row), ' vol ', bgVolInput(row));
        return wrap;
    }

    function commentInput(row) {
        const inp = document.createElement('input');
        inp.type = 'text'; inp.className = 'gen-comment-input';
        inp.value = row.text;
        inp.addEventListener('input', () => { row.text = inp.value; commitValue(); });
        inp.addEventListener('change', () => {
            if (row.text && row.text.trim() && !row.text.trim().startsWith('#')) {
                row.text = '# ' + row.text.trim();
                commitStructure();
            }
        });
        return inp;
    }

    // Speech (`S`) row: voice + volume + text + a local Preview button. These
    // are browser-only (TTS-synthesized into the bounce, never sent to device).
    function speechInputs(row) {
        const wrap = document.createElement('span');
        wrap.className = 'gen-speech-inputs';
        const voice = document.createElement('input');
        voice.type = 'text'; voice.className = 'gen-speech-voice'; voice.title = 'voice / language (e.g. en-US, Joanna, sl)';
        voice.value = row.voice; voice.placeholder = 'voice';
        voice.addEventListener('input', () => { row.voice = voice.value; commitValue(); });
        const vol = document.createElement('input');
        vol.type = 'number'; vol.className = 'gen-num'; vol.title = 'volume 0..100';
        vol.value = String(row.volume);
        vol.addEventListener('input', () => { const v = parseFloat(vol.value); row.volume = Number.isFinite(v) ? v : 0; commitValue(); });
        const text = document.createElement('input');
        text.type = 'text'; text.className = 'gen-speech-text'; text.placeholder = 'text to speak…';
        text.value = row.text;
        text.addEventListener('input', () => { row.text = text.value; commitValue(); });
        const prev = document.createElement('button');
        prev.type = 'button'; prev.className = 'gen-speech-preview'; prev.textContent = '🔊';
        prev.title = 'Preview aloud (browser voice; the bounce uses the selected TTS engine)';
        prev.addEventListener('click', () => { previewSpeech(row.text, row.voice); });
        wrap.append('voice ', voice, ' vol ', vol, ' ', text, prev);
        return wrap;
    }

    // Should a row be shown given the current toggles?
    function passesFilter(row) {
        if (Object.prototype.hasOwnProperty.call(shown, row.kind)) return shown[row.kind];
        // Blank lines are spacing for the text view; as grid rows they are just
        // empty bands, and filtering any kind out leaves a trail of them behind.
        // They stay in the model — only the grid declines to draw them — so the
        // text view and serialization keep the file's original formatting.
        if (row.kind === 'blank') return false;
        return true; // bg carries content, raw flags an unparsable line
    }

    // ---- Filter bar -------------------------------------------------------
    function renderFilters() {
        filterBar.innerHTML = '';
        const label = document.createElement('span');
        label.className = 'gen-filter-label';
        label.textContent = 'Show:';
        filterBar.appendChild(label);
        for (const [kind, txt] of [['led', 'LED'], ['audio', 'Audio'], ['speech', 'Speech'], ['comment', 'Comments']]) {
            const b = document.createElement('button');
            b.type = 'button';
            b.className = 'gen-filter-chip' + (shown[kind] ? ' on' : '');
            b.textContent = txt;
            b.setAttribute('aria-pressed', shown[kind] ? 'true' : 'false');
            b.title = (shown[kind] ? 'Hide' : 'Show') + ' ' + txt.toLowerCase() + ' rows';
            b.addEventListener('click', () => { shown[kind] = !shown[kind]; rebuild(ctx.getDoc()); });
            filterBar.appendChild(b);
        }
    }

    // ---- Add bar ----------------------------------------------------------
    function renderAddBar() {
        addBar.innerHTML = '';
        const lbl = document.createElement('span');
        lbl.textContent = 'Add row:';
        lbl.className = 'gen-add-label';
        addBar.appendChild(lbl);
        const add = (kind, label) => {
            const b = document.createElement('button');
            b.type = 'button';
            b.className = 'gen-add-btn';
            b.textContent = label;
            b.addEventListener('click', () => {
                const d = ctx.getDoc();
                const lastT = (() => {
                    for (let i = d.rows.length - 1; i >= 0; i--) {
                        const r = d.rows[i];
                        if (r.kind === 'led' || r.kind === 'audio') return r.time;
                    }
                    return 0;
                })();
                d.rows.push(newRowOfKind(kind, kind === 'bg' || kind === 'comment' ? 0 : lastT));
                recomputeBg();
                commitStructure();
            });
            addBar.appendChild(b);
        };
        add('led', '+ LED');
        add('audio', '+ A');
        add('bg', '+ BG');
        add('speech', '+ S');
    }

    // ---- Grid (desktop) ---------------------------------------------------
    // LED and Audio share the three middle knob columns:
    //   Duty↔Pan, Bright↔Vol, Color↔Mod. FreqR/Wave are audio-only trailers.
    // The pulse fields used to hide behind a per-row "⋯" disclosure. They are
    // ordinary settings, so they get ordinary columns; the grid scrolls
    // horizontally when it outgrows the page. "Duty %" is the audio pulse duty,
    // distinct from the shared Duty/Pan knob — LED has no equivalent.
    const COLS = ['Type', 'Time', 'Ch / Mask', 'Freq',
                  'Duty / Pan', 'Bright / Vol', 'Color / Mod', 'FreqR', 'Wave',
                  'Duty %', 'Env', 'Phase°', 'Attack ms', 'Jitter', ''];

    function blankCell() { return document.createElement('td'); }

    function buildGrid(doc) {
        const table = document.createElement('table');
        table.className = 'gen-grid';
        const thead = document.createElement('thead');
        const htr = document.createElement('tr');
        for (const c of COLS) {
            const th = document.createElement('th');
            th.textContent = c;
            htr.appendChild(th);
        }
        thead.appendChild(htr);
        table.appendChild(thead);
        const tbody = document.createElement('tbody');

        // Compared against the previous *rendered* row, so hiding a kind never
        // leaves a divider stranded or swallows one.
        const timeGroups = computeTimeGroups(doc.rows);
        let lastGroup = null;

        orderRowsForDisplay(doc.rows).forEach(idx => {
            const row = doc.rows[idx];
            if (!passesFilter(row)) return;
            const tr = document.createElement('tr');
            tr.className = 'gen-grid-row gen-grid-' + row.kind;
            if (lastGroup !== null && timeGroups[idx] !== lastGroup) tr.classList.add('is-timebreak');
            lastGroup = timeGroups[idx];

            const tdType = document.createElement('td');
            if (row.kind === 'led' || row.kind === 'audio' || row.kind === 'bg' || row.kind === 'speech' || row.kind === 'comment') {
                tdType.appendChild(typeSelect(row, idx));
            } else {
                tdType.textContent = row.kind;
            }
            tr.appendChild(tdType);

            if (row.kind === 'led') {
                const tdTime = blankCell(); tdTime.appendChild(timeCell(row)); tr.appendChild(tdTime);
                const tdMask = blankCell(); tdMask.appendChild(maskChips(row)); tr.appendChild(tdMask);
                const tdFreq = blankCell(); tdFreq.appendChild(compound(row, idx, 'freq', 'Frequency (Hz)', () => ledRampTarget(idx, row.mask, 'freq'))); tr.appendChild(tdFreq);
                const tdDuty = blankCell(); tdDuty.appendChild(compound(row, idx, 'duty', 'Duty (%)', () => ledRampTarget(idx, row.mask, 'duty'))); tr.appendChild(tdDuty);        // Duty / Pan
                const tdBr = blankCell(); tdBr.appendChild(compound(row, idx, 'bright', 'Brightness (%)', () => ledRampTarget(idx, row.mask, 'bright'))); tr.appendChild(tdBr);      // Bright / Vol
                const tdCol = blankCell(); tdCol.appendChild(colorSwatch(row)); tr.appendChild(tdCol);                                                                               // Color / Mod
                tr.appendChild(blankCell()); // FreqR
                tr.appendChild(blankCell()); // Wave
                tr.appendChild(blankCell()); // Duty % (audio-only pulse duty)
                const tdEnvL = blankCell(); tdEnvL.appendChild(pulseEnvSelect(row, false)); tr.appendChild(tdEnvL);
                const tdPhL = blankCell(); tdPhL.appendChild(pulseCellInput(row, 'phase', 'Phase (deg)')); tr.appendChild(tdPhL);
                const tdAtL = blankCell(); tdAtL.appendChild(pulseCellInput(row, 'attack', 'Attack (ms)')); tr.appendChild(tdAtL);
                const tdJtL = blankCell(); tdJtL.appendChild(pulseJitterInput(row)); tr.appendChild(tdJtL);
            } else if (row.kind === 'audio') {
                const tdTime = blankCell(); tdTime.appendChild(timeCell(row)); tr.appendChild(tdTime);
                const tdCh = blankCell(); tdCh.appendChild(channelSelect(row)); tr.appendChild(tdCh);
                const tdFreq = blankCell(); tdFreq.appendChild(compound(row, idx, 'freq', 'Frequency (Hz)', () => audioRampTarget(idx, row.channel, 'freq'))); tr.appendChild(tdFreq);
                const tdPan = blankCell(); tdPan.appendChild(compound(row, idx, 'pan', 'Pan (-100..100)', () => audioRampTarget(idx, row.channel, 'pan'))); tr.appendChild(tdPan);    // Duty / Pan
                const tdVol = blankCell(); tdVol.appendChild(compound(row, idx, 'vol', 'Volume (%)', () => audioRampTarget(idx, row.channel, 'vol'))); tr.appendChild(tdVol);        // Bright / Vol
                const tdMod = blankCell(); tdMod.appendChild(compound(row, idx, 'mod', 'Mod (Hz)', () => audioRampTarget(idx, row.channel, 'mod'))); tr.appendChild(tdMod);          // Color / Mod
                const tdFr = blankCell(); tdFr.appendChild(freqRInput(row)); tr.appendChild(tdFr);
                const tdWave = blankCell(); tdWave.appendChild(waveSelect(row)); tr.appendChild(tdWave);
                const tdDutyP = blankCell(); tdDutyP.appendChild(pulseCellInput(row, 'duty', 'Duty %')); tr.appendChild(tdDutyP);
                const tdEnvA = blankCell(); tdEnvA.appendChild(pulseEnvSelect(row, true)); tr.appendChild(tdEnvA);
                const tdPhA = blankCell(); tdPhA.appendChild(pulseCellInput(row, 'phase', 'Phase (deg)')); tr.appendChild(tdPhA);
                const tdAtA = blankCell(); tdAtA.appendChild(pulseCellInput(row, 'attack', 'Attack (ms)')); tr.appendChild(tdAtA);
                const tdJtA = blankCell(); tdJtA.appendChild(pulseJitterInput(row)); tr.appendChild(tdJtA);
            } else if (row.kind === 'bg') {
                // URL spans Time+Ch/Mask+Freq (it's the long field); pan/loudness
                // land in the shared Duty/Pan and Bright/Vol columns like audio.
                const tdUrl = document.createElement('td');
                tdUrl.colSpan = 3;
                tdUrl.appendChild(bgSourceControls(row));
                tr.appendChild(tdUrl);
                const tdPan = blankCell(); tdPan.appendChild(bgPanInput(row)); tr.appendChild(tdPan);   // Duty / Pan
                const tdVol = blankCell(); tdVol.appendChild(bgVolInput(row)); tr.appendChild(tdVol);   // Bright / Vol
                tr.appendChild(blankCell()); // Color / Mod
                tr.appendChild(blankCell()); // FreqR
                tr.appendChild(blankCell()); // Wave
                for (let i = 0; i < 5; i++) tr.appendChild(blankCell()); // pulse columns
            } else if (row.kind === 'speech') {
                // Time in its own column; voice+volume+text span the rest.
                const tdTime = blankCell(); tdTime.appendChild(timeCell(row)); tr.appendChild(tdTime);
                const td = document.createElement('td');
                td.colSpan = COLS.length - 3;
                td.appendChild(speechInputs(row));
                tr.appendChild(td);
            } else if (row.kind === 'comment') {
                const td = document.createElement('td');
                td.colSpan = COLS.length - 2;
                td.appendChild(commentInput(row));
                tr.appendChild(td);
            } else {
                const td = document.createElement('td');
                td.colSpan = COLS.length - 2;
                td.className = 'gen-grid-rawcell';
                td.textContent = row.kind === 'blank' ? '(blank line)' : row.text;
                if (row.kind === 'raw') td.title = row.error || '';
                tr.appendChild(td);
            }

            const tdAct = document.createElement('td');
            tdAct.appendChild(actionsMenu(idx));
            tr.appendChild(tdAct);

            tbody.appendChild(tr);

        });

        table.appendChild(tbody);
        if (!doc.rows.length) {
            const empty = document.createElement('div');
            empty.className = 'gen-grid-empty';
            empty.textContent = 'No rows yet — use “Add row” below.';
            gridScroll.appendChild(empty);
        }
        return table;
    }

    // ---- Cards (mobile) ---------------------------------------------------
    function field(label, el) {
        const f = document.createElement('div');
        f.className = 'gen-card-field';
        const l = document.createElement('span');
        l.className = 'gen-card-label';
        l.textContent = label;
        f.appendChild(l);
        f.appendChild(el);
        return f;
    }

    function buildCards(doc) {
        cards.innerHTML = '';
        const timeGroups = computeTimeGroups(doc.rows);
        let lastGroup = null;
        orderRowsForDisplay(doc.rows).forEach(idx => {
            const row = doc.rows[idx];
            if (!passesFilter(row)) return;
            const card = document.createElement('div');
            card.className = 'gen-card gen-card-' + row.kind;
            if (lastGroup !== null && timeGroups[idx] !== lastGroup) card.classList.add('is-timebreak');
            lastGroup = timeGroups[idx];

            const head = document.createElement('div');
            head.className = 'gen-card-head';
            if (row.kind === 'led' || row.kind === 'audio' || row.kind === 'bg' || row.kind === 'speech' || row.kind === 'comment') {
                head.appendChild(typeSelect(row, idx));
            } else {
                const k = document.createElement('span'); k.textContent = row.kind; head.appendChild(k);
            }
            if (row.kind === 'audio') {
                const ch = document.createElement('span');
                ch.className = 'gen-card-sub';
                ch.textContent = 'ch ' + (row.channel == null ? '—' : row.channel);
                head.appendChild(ch);
            }
            if (row.kind === 'led' || row.kind === 'audio') {
                const t = document.createElement('span');
                t.className = 'gen-card-sub';
                t.textContent = formatTimeMs(row.time);
                head.appendChild(t);
            }
            head.appendChild(actionsMenu(idx));
            card.appendChild(head);

            if (row.kind === 'led') {
                card.appendChild(field('Time', timeCell(row)));
                card.appendChild(field('Channels', maskChips(row)));
                card.appendChild(field('Freq', compound(row, idx, 'freq', 'Frequency (Hz)', () => ledRampTarget(idx, row.mask, 'freq'))));
                card.appendChild(field('Duty', compound(row, idx, 'duty', 'Duty (%)', () => ledRampTarget(idx, row.mask, 'duty'))));
                card.appendChild(field('Bright', compound(row, idx, 'bright', 'Brightness (%)', () => ledRampTarget(idx, row.mask, 'bright'))));
                card.appendChild(field('Color', colorSwatch(row)));
                const more = document.createElement('details');
                more.className = 'gen-card-more';
                const sum = document.createElement('summary');
                sum.textContent = 'pulse ▾';
                more.appendChild(sum);
                more.appendChild(pulseFields(row));
                card.appendChild(more);
            } else if (row.kind === 'audio') {
                card.appendChild(field('Time', timeCell(row)));
                card.appendChild(field('Channel', channelSelect(row)));
                card.appendChild(field('Freq', compound(row, idx, 'freq', 'Frequency (Hz)', () => audioRampTarget(idx, row.channel, 'freq'))));
                card.appendChild(field('Pan', compound(row, idx, 'pan', 'Pan (-100..100)', () => audioRampTarget(idx, row.channel, 'pan'))));
                card.appendChild(field('Vol', compound(row, idx, 'vol', 'Volume (%)', () => audioRampTarget(idx, row.channel, 'vol'))));
                const more = document.createElement('details');
                more.className = 'gen-card-more';
                const sum = document.createElement('summary');
                sum.textContent = 'more ▾';
                more.appendChild(sum);
                more.appendChild(field('Mod', compound(row, idx, 'mod', 'Mod (Hz)', () => audioRampTarget(idx, row.channel, 'mod'))));
                more.appendChild(field('FreqR', freqRInput(row)));
                more.appendChild(field('Wave', waveSelect(row)));
                more.appendChild(pulseFields(row));
                card.appendChild(more);
            } else if (row.kind === 'bg') {
                card.appendChild(field('BG', bgInputs(row)));
            } else if (row.kind === 'speech') {
                card.appendChild(field('Time', timeCell(row)));
                card.appendChild(field('Speech', speechInputs(row)));
            } else if (row.kind === 'comment') {
                card.appendChild(field('Comment', commentInput(row)));
            } else {
                const raw = document.createElement('div');
                raw.className = 'gen-grid-rawcell';
                raw.textContent = row.kind === 'blank' ? '(blank line)' : row.text;
                card.appendChild(raw);
            }

            cards.appendChild(card);
        });
        if (!doc.rows.length) {
            const empty = document.createElement('div');
            empty.className = 'gen-grid-empty';
            empty.textContent = 'No entries yet.';
            cards.appendChild(empty);
        }
    }

    // ---- Rebuild ----------------------------------------------------------
    function rebuild(doc) {
        const st = gridScroll.scrollTop, sl = gridScroll.scrollLeft;
        renderFilters();
        cellTriggers = [];   // the ones below are about to be discarded
        gridScroll.innerHTML = '';
        gridScroll.appendChild(buildGrid(doc));
        buildCards(doc);
        renderAddBar();
        gridScroll.scrollTop = st;
        gridScroll.scrollLeft = sl;
    }

    // ---- View contract ----------------------------------------------------
    function refresh(doc) {
        if (applyingEdit) return; // our own value edit — DOM already current
        if (!visible) return;     // rebuild lazily on show()
        rebuild(doc || ctx.getDoc());
    }
    function show() { visible = true; root.style.display = ''; rebuild(ctx.getDoc()); }
    function hide() { visible = false; root.style.display = 'none'; closeOpenPopover(); }

    return { refresh, show, hide };
}
