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
    ledRow, audioRow, commentRow, bgRow, bg, cell,
    WAVE_TYPES, NUM_AUDIO_CHANNELS, NUM_LED_CHANNELS,
} from '../model.js';
import { createCompoundCell, closeOpenPopover, openPopover, cellLabel } from './cell.js';

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

// ---- Row helpers -----------------------------------------------------------

function cloneCell(c) { return c ? { ...c } : cell(0); }

function cloneRow(r) {
    switch (r.kind) {
        case 'led': return ledRow({
            time: r.time, freq: cloneCell(r.freq), duty: cloneCell(r.duty),
            bright: cloneCell(r.bright), r: cloneCell(r.r), g: cloneCell(r.g),
            b: cloneCell(r.b), mask: r.mask, legacy5: r.legacy5,
            inlineComment: r.inlineComment,
        });
        case 'audio': return audioRow({
            time: r.time, freq: cloneCell(r.freq), pan: cloneCell(r.pan),
            vol: cloneCell(r.vol), mod: cloneCell(r.mod), channel: r.channel,
            freqR: r.freqR, waveType: r.waveType, inlineComment: r.inlineComment,
        });
        case 'comment': return commentRow(r.text);
        case 'bg': return bgRow(bg(r.bg.url, r.bg.pan, r.bg.loudness));
        case 'blank': return { kind: 'blank' };
        default: return { kind: 'raw', text: r.text, error: r.error };
    }
}

function newRowOfKind(kind, time) {
    switch (kind) {
        case 'led':     return ledRow({ time: time || 0 });
        case 'audio':   return audioRow({ time: time || 0, channel: 1 });
        case 'bg':      return bgRow(bg('', 0, 50));
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
    let filter = 'all'; // 'all' | 'led' | 'audio'

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

    // ---- Commit helpers ---------------------------------------------------
    // Value edit: preview/text update, but DON'T rebuild our own grid.
    function commitValue() {
        applyingEdit = true;
        try { ctx.setDoc(ctx.getDoc()); } finally { applyingEdit = false; }
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
        for (const [val, label] of [['led', 'LED'], ['audio', 'A'], ['bg', 'BG'], ['comment', '#']]) {
            const o = document.createElement('option');
            o.value = val; o.textContent = label;
            if ((val === 'led' && row.kind === 'led') ||
                (val === 'audio' && row.kind === 'audio') ||
                (val === 'bg' && row.kind === 'bg') ||
                (val === 'comment' && row.kind === 'comment')) o.selected = true;
            sel.appendChild(o);
        }
        sel.addEventListener('change', () => {
            const d = ctx.getDoc();
            const t = (row.kind === 'led' || row.kind === 'audio') ? row.time : 0;
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
                    if (triggers[f]) triggers[f].textContent = cellLabel(row[f]);
                }
            }
            makeChan('r', 'R');
            makeChan('g', 'G');
            makeChan('b', 'B');

            openPopover(btn, form, 'Color');
        });
        return btn;
    }

    function compound(row, idx, field, title, resolve) {
        return createCompoundCell({
            title: title,
            getCell: () => row[field],
            onChange: (c) => { row[field] = c; commitValue(); },
            resolveTarget: resolve,
        });
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
            item('Move up', () => {
                if (idx > 0) { const r = d.rows.splice(idx, 1)[0]; d.rows.splice(idx - 1, 0, r); recomputeBg(); commitStructure(); }
            });
            item('Move down', () => {
                if (idx < d.rows.length - 1) { const r = d.rows.splice(idx, 1)[0]; d.rows.splice(idx + 1, 0, r); recomputeBg(); commitStructure(); }
            });
            openPopover(btn, menu, 'Row actions');
        });
        return wrap;
    }

    function bgUrlInput(row) {
        const url = document.createElement('input');
        url.type = 'text'; url.className = 'gen-bg-url';
        url.placeholder = 'http(s):// or sdcard://';
        url.value = row.bg.url;
        url.addEventListener('input', () => { row.bg.url = url.value; recomputeBg(); commitValue(); });
        return url;
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
        wrap.append('url ', bgUrlInput(row), ' pan ', bgPanInput(row), ' vol ', bgVolInput(row));
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

    // Should a row be shown given the current filter?
    function passesFilter(row) {
        if (filter === 'all') return true;
        if (row.kind === 'led') return filter === 'led';
        if (row.kind === 'audio') return filter === 'audio';
        return true; // comments / blank / raw / bg always shown
    }

    // ---- Filter bar -------------------------------------------------------
    function renderFilters() {
        filterBar.innerHTML = '';
        const label = document.createElement('span');
        label.className = 'gen-filter-label';
        label.textContent = 'Show:';
        filterBar.appendChild(label);
        for (const [val, txt] of [['all', 'All'], ['led', 'LED only'], ['audio', 'Audio only']]) {
            const b = document.createElement('button');
            b.type = 'button';
            b.className = 'gen-filter-chip' + (filter === val ? ' on' : '');
            b.textContent = txt;
            b.addEventListener('click', () => { filter = val; rebuild(ctx.getDoc()); });
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
    }

    // ---- Grid (desktop) ---------------------------------------------------
    // LED and Audio share the three middle knob columns:
    //   Duty↔Pan, Bright↔Vol, Color↔Mod. FreqR/Wave are audio-only trailers.
    const COLS = ['Type', 'Time', 'Ch / Mask', 'Freq',
                  'Duty / Pan', 'Bright / Vol', 'Color / Mod', 'FreqR', 'Wave', ''];

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

        doc.rows.forEach((row, idx) => {
            if (!passesFilter(row)) return;
            const tr = document.createElement('tr');
            tr.className = 'gen-grid-row gen-grid-' + row.kind;

            const tdType = document.createElement('td');
            if (row.kind === 'led' || row.kind === 'audio' || row.kind === 'bg' || row.kind === 'comment') {
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
            } else if (row.kind === 'audio') {
                const tdTime = blankCell(); tdTime.appendChild(timeCell(row)); tr.appendChild(tdTime);
                const tdCh = blankCell(); tdCh.appendChild(channelSelect(row)); tr.appendChild(tdCh);
                const tdFreq = blankCell(); tdFreq.appendChild(compound(row, idx, 'freq', 'Frequency (Hz)', () => audioRampTarget(idx, row.channel, 'freq'))); tr.appendChild(tdFreq);
                const tdPan = blankCell(); tdPan.appendChild(compound(row, idx, 'pan', 'Pan (-100..100)', () => audioRampTarget(idx, row.channel, 'pan'))); tr.appendChild(tdPan);    // Duty / Pan
                const tdVol = blankCell(); tdVol.appendChild(compound(row, idx, 'vol', 'Volume (%)', () => audioRampTarget(idx, row.channel, 'vol'))); tr.appendChild(tdVol);        // Bright / Vol
                const tdMod = blankCell(); tdMod.appendChild(compound(row, idx, 'mod', 'Mod (Hz)', () => audioRampTarget(idx, row.channel, 'mod'))); tr.appendChild(tdMod);          // Color / Mod
                const tdFr = blankCell(); tdFr.appendChild(freqRInput(row)); tr.appendChild(tdFr);
                const tdWave = blankCell(); tdWave.appendChild(waveSelect(row)); tr.appendChild(tdWave);
            } else if (row.kind === 'bg') {
                // URL spans Time+Ch/Mask+Freq (it's the long field); pan/loudness
                // land in the shared Duty/Pan and Bright/Vol columns like audio.
                const tdUrl = document.createElement('td');
                tdUrl.colSpan = 3;
                tdUrl.appendChild(bgUrlInput(row));
                tr.appendChild(tdUrl);
                const tdPan = blankCell(); tdPan.appendChild(bgPanInput(row)); tr.appendChild(tdPan);   // Duty / Pan
                const tdVol = blankCell(); tdVol.appendChild(bgVolInput(row)); tr.appendChild(tdVol);   // Bright / Vol
                tr.appendChild(blankCell()); // Color / Mod
                tr.appendChild(blankCell()); // FreqR
                tr.appendChild(blankCell()); // Wave
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
        doc.rows.forEach((row, idx) => {
            if (!passesFilter(row)) return;
            const card = document.createElement('div');
            card.className = 'gen-card gen-card-' + row.kind;

            const head = document.createElement('div');
            head.className = 'gen-card-head';
            if (row.kind === 'led' || row.kind === 'audio' || row.kind === 'bg' || row.kind === 'comment') {
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
                card.appendChild(more);
            } else if (row.kind === 'bg') {
                card.appendChild(field('BG', bgInputs(row)));
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
