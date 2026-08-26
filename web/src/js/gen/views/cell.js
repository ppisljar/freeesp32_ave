// Shared compound "value + interp" cell widget (Phase 3).
//
// A `Cell` (see gen/model.js) bundles a value with an interpolation kind:
//   step (none) / linear `>` / quadratic `*` ramps, and the five periodic
//   modulations triangle `^` / sine `~` / saw-up `/` / saw-down `\` /
//   square `_` (which additionally carry modEnd + modPeriodMs).
//
// This module renders, for any Cell-valued field:
//   - a compact inline TRIGGER (value + a glyph hinting the interp), and
//   - a POPOVER/inspector that edits the full Cell struct (radio interp picker;
//     `end` + `period` fields only for periodic mods; ramp shows the resolved
//     "→ value at t=next" target).
//
// It is intentionally DOM-only-on-call (no DOM touched at import) so the pure
// helpers below can be unit-tested in plain Node, mirroring views/text.js.
//
// Used by the Table view now; Lane/Wizard reuse it in later phases.

import { isModInterp, DEFAULT_MOD_PERIOD_MS, cell } from '../model.js';

// interp kind -> human label for the radio picker (order per the plan:
// Step / > / * / ^ / ~ / / / \ / _).
export const INTERP_OPTIONS = [
    ['none',  'Step'],
    ['lin',   'Linear  →'],
    ['quad',  'Quadratic  x²'],
    ['tri',   'Triangle  ^'],
    ['sine',  'Sine  ~'],
    ['sawup', 'Saw-up  /'],
    ['sawdn', 'Saw-down  \\'],
    ['sq',    'Square  _'],
];

// ---- Pure helpers (unit-tested) -------------------------------------------

// A short inline glyph hinting the interp kind on the trigger button.
//   none → ''   lin → '→'   quad → 'x²'   periodic mods → '∿'
export function cellInlineGlyph(c) {
    if (!c || c.interp === 'none') return '';
    if (c.interp === 'lin') return '→';
    if (c.interp === 'quad') return 'x²';
    if (isModInterp(c.interp)) return '∿';
    return '';
}

// The text shown on the compact trigger button for a cell.
//   step      → "12"
//   ramp      → "→ 12" / "x² 12"
//   periodic  → "10∿20"   (start∿end, the period lives in the popover)
export function cellLabel(c) {
    if (!c) return '0';
    if (isModInterp(c.interp)) {
        const end = (c.modEnd === null || c.modEnd === undefined) ? c.value : c.modEnd;
        return c.value + '∿' + end;
    }
    const g = cellInlineGlyph(c);
    return (g ? g + ' ' : '') + c.value;
}

// The sub-label shown UNDER the trigger, mirroring the mm:ss.mmm hint under a
// time field. A ramp's destination is otherwise invisible until you open the
// popover — the colour tells you a value moves, not where it lands.
//
// `target` is the resolved value at the next same-field entry (null when there
// isn't one). Only ramps get a hint: a step has no destination, and a periodic
// mod already shows "start∿end" on the trigger itself.
//
// The marker is "↳", not the "→" the trigger uses as its linear-interp glyph.
// Reusing "→" would stack "→ 20" over "→ 60", two arrows meaning different
// things — one "this ramps", one "to here".
export function cellTargetHint(c, target) {
    if (!c || (c.interp !== 'lin' && c.interp !== 'quad')) return '';
    // No later entry to ramp toward: the device holds the start value, which is
    // worth saying outright — a ramp that silently does nothing looks like a bug.
    if (target === null || target === undefined) return '\u21b3 holds';
    return '\u21b3 ' + target;
}

// ---- Popover plumbing ------------------------------------------------------

let openClose = null; // the close() of the currently-open popover, if any

// Open `content` as a floating popover anchored to `anchor`. Returns close().
// On narrow screens CSS turns `.gen-popover` into a full-screen bottom sheet.
export function openPopover(anchor, content, title) {
    closeOpenPopover();

    const back = document.createElement('div');
    back.className = 'gen-popover-back';

    const pop = document.createElement('div');
    pop.className = 'gen-popover';

    if (title) {
        const h = document.createElement('div');
        h.className = 'gen-popover-title';
        h.textContent = title;
        pop.appendChild(h);
    }
    pop.appendChild(content);

    const foot = document.createElement('div');
    foot.className = 'gen-popover-foot';
    const done = document.createElement('button');
    done.textContent = 'Done';
    done.className = 'gen-popover-done';
    foot.appendChild(done);
    pop.appendChild(foot);

    document.body.appendChild(back);
    document.body.appendChild(pop);

    // Desktop positioning near the anchor (CSS overrides to a sheet on mobile).
    try {
        const r = anchor.getBoundingClientRect();
        const pw = pop.offsetWidth || 240;
        const ph = pop.offsetHeight || 160;
        let left = r.left;
        let top = r.bottom + 4;
        if (left + pw > window.innerWidth - 8) left = Math.max(8, window.innerWidth - pw - 8);
        if (top + ph > window.innerHeight - 8) top = Math.max(8, r.top - ph - 4);
        pop.style.left = left + 'px';
        pop.style.top = top + 'px';
    } catch (e) { /* anchor may be detached in tests */ }

    const close = () => {
        if (openClose === close) openClose = null;
        back.remove();
        pop.remove();
        document.removeEventListener('keydown', onKey);
    };
    const onKey = (e) => { if (e.key === 'Escape') close(); };

    back.addEventListener('click', close);
    done.addEventListener('click', close);
    document.addEventListener('keydown', onKey);

    openClose = close;
    return close;
}

export function closeOpenPopover() {
    if (openClose) openClose();
}

// ---- Cell inspector form ---------------------------------------------------

// Build the inspector form for one Cell. Calls onChange(newCell) live on every
// edit. `resolveTarget` (optional) returns the resolved ramp target value (or
// null) shown for `>`/`*` interps. Returns the form element.
export function buildCellEditor(initial, opts) {
    opts = opts || {};
    const onChange = opts.onChange || function () {};
    const resolveTarget = opts.resolveTarget || (() => null);
    // Optional fields (the pulse ones) can be turned off entirely; the core
    // fields cannot, so the Clear action is opt-in rather than always present.
    const onClear = opts.onClear || null;

    // Optional fields (the pulse ones) may arrive with no cell at all: the field
    // is omitted and the device uses its default. That is a real state the
    // editor has to round-trip, not an empty value to coerce to 0.
    const optional     = !!opts.optional;
    const defaultLabel = opts.defaultLabel || '';
    let cur = cell(initial ? initial.value : 0,
                   initial ? initial.interp : 'none',
                   initial ? initial.modEnd : null,
                   initial ? initial.modPeriodMs : null);

    const form = document.createElement('div');
    form.className = 'gen-cell-editor';

    // Value -----------------------------------------------------------------
    const vRow = document.createElement('label');
    vRow.className = 'gen-cell-field';
    vRow.append('Value');
    const vIn = document.createElement('input');
    vIn.type = 'number';
    vIn.value = (optional && !initial) ? '' : cur.value;
    if (optional) {
        vIn.placeholder = defaultLabel;
        vIn.title = 'Empty = off (device uses its default' +
                    (defaultLabel ? ': ' + defaultLabel : '') + ')';
    }
    if (opts.step !== undefined) vIn.step = opts.step;
    vRow.appendChild(vIn);
    form.appendChild(vRow);

    // Interp radios ---------------------------------------------------------
    const radios = document.createElement('div');
    radios.className = 'gen-cell-interp';
    const name = 'ci' + Math.random().toString(36).slice(2);
    const radioEls = {};
    for (const [kind, label] of INTERP_OPTIONS) {
        const l = document.createElement('label');
        l.className = 'gen-cell-radio';
        const rb = document.createElement('input');
        rb.type = 'radio';
        rb.name = name;
        rb.value = kind;
        if (kind === cur.interp) rb.checked = true;
        radioEls[kind] = rb;
        l.appendChild(rb);
        l.append(' ' + label);
        radios.appendChild(l);
    }
    form.appendChild(radios);

    // Periodic-mod extras ---------------------------------------------------
    const extras = document.createElement('div');
    extras.className = 'gen-cell-extras';
    const eRow = document.createElement('label');
    eRow.className = 'gen-cell-field';
    eRow.append('End');
    const eIn = document.createElement('input');
    eIn.type = 'number';
    eIn.value = (cur.modEnd === null || cur.modEnd === undefined) ? cur.value : cur.modEnd;
    eRow.appendChild(eIn);
    const pRow = document.createElement('label');
    pRow.className = 'gen-cell-field';
    pRow.append('Period (ms)');
    const pIn = document.createElement('input');
    pIn.type = 'number';
    pIn.value = (cur.modPeriodMs === null || cur.modPeriodMs === undefined)
        ? DEFAULT_MOD_PERIOD_MS : cur.modPeriodMs;
    pRow.appendChild(pIn);
    extras.appendChild(eRow);
    extras.appendChild(pRow);
    form.appendChild(extras);

    // Ramp target readout ---------------------------------------------------
    const tgt = document.createElement('div');
    tgt.className = 'gen-cell-target';
    form.appendChild(tgt);

    // ---- Visibility + emit ------------------------------------------------
    function syncVisibility() {
        const mod = isModInterp(cur.interp);
        const ramp = cur.interp === 'lin' || cur.interp === 'quad';
        extras.style.display = mod ? '' : 'none';
        if (ramp) {
            const t = resolveTarget();
            tgt.style.display = '';
            tgt.textContent = (t === null || t === undefined)
                ? '→ no later same-field entry to ramp toward (holds at start)'
                : '→ ramps to ' + t + ' at the next entry';
        } else {
            tgt.style.display = 'none';
        }
    }

    function emit() {
        // Emptying the value on an optional field turns it off again — the
        // inverse of typing, in the same place, so unsetting is never a hunt.
        if (optional && vIn.value.trim() === '') {
            onChange(undefined);
            return;
        }
        const v = parseFloat(vIn.value);
        cur = cell(Number.isFinite(v) ? v : 0, cur.interp);
        if (isModInterp(cur.interp)) {
            const e = parseFloat(eIn.value);
            const p = parseFloat(pIn.value);
            cur.modEnd = Number.isFinite(e) ? e : cur.value;
            cur.modPeriodMs = Number.isFinite(p) ? p : DEFAULT_MOD_PERIOD_MS;
        }
        onChange(cur);
    }

    vIn.addEventListener('input', emit);
    eIn.addEventListener('input', emit);
    pIn.addEventListener('input', emit);
    for (const kind in radioEls) {
        radioEls[kind].addEventListener('change', () => {
            cur.interp = kind;
            // An off field has no value to interpolate: picking a curve is a
            // clear statement of intent, so seed it with the device default
            // rather than silently doing nothing.
            if (optional && vIn.value.trim() === '' && kind !== 'none') {
                vIn.value = defaultLabel !== '' ? defaultLabel : '0';
            }
            // Seed sensible mod extras when switching into a periodic mod.
            if (isModInterp(kind)) {
                if (cur.modEnd === null || cur.modEnd === undefined) eIn.value = cur.value;
                if (cur.modPeriodMs === null || cur.modPeriodMs === undefined) {
                    if (!pIn.value) pIn.value = DEFAULT_MOD_PERIOD_MS;
                }
            }
            syncVisibility();
            emit();
        });
    }

    if (onClear) {
        const clear = document.createElement('button');
        clear.type = 'button';
        clear.className = 'gen-cell-clear';
        clear.textContent = 'Clear (off)';
        clear.title = 'Omit this field — the device falls back to its default';
        clear.addEventListener('click', () => { closeOpenPopover(); onClear(); });
        form.appendChild(clear);
    }

    syncVisibility();
    return form;
}

// ---- Compound cell trigger -------------------------------------------------

// Create the inline trigger element for a Cell-valued model field.
//   opts.getCell()        -> current Cell
//   opts.onChange(cell)   -> commit a new Cell to the model
//   opts.resolveTarget()  -> resolved ramp target (number|null), optional
//   opts.title            -> popover title, optional
//   opts.step             -> numeric step for the value input, optional
// Returns the trigger <button>. Its label updates live while the popover edits.
export function createCompoundCell(opts) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'gen-cell-trigger';
    // Two stacked spans rather than bare text, so the ramp destination can sit
    // under the value the way mm:ss sits under a time in ms. Callers that used
    // to poke btn.textContent must call btn.repaint() instead — writing
    // textContent here would drop both spans and freeze the cell.
    const main = document.createElement('span');
    main.className = 'gen-cell-main';
    const hint = document.createElement('span');
    hint.className = 'gen-cell-hint';
    btn.appendChild(main);
    btn.appendChild(hint);

    function paint() {
        const c = opts.getCell();
        // An optional field with no cell shows its default, greyed — the cell
        // keeps one shape for every state, so it never morphs under the cursor
        // and the way back to "off" is always the same click.
        // NB the !! on every toggle. classList.toggle(name, force) treats an
        // `undefined` force as "no force argument given" and FLIPS the class
        // instead of clearing it, so `opts.optional && ...` (undefined on the
        // core fields) made every repaint alternate the state.
        const unset = !!(opts.optional && (c === undefined || c === null));
        main.textContent = unset ? (opts.defaultLabel || '—') : cellLabel(c);
        const h = unset ? '' : cellTargetHint(c, opts.resolveTarget ? opts.resolveTarget() : null);
        hint.textContent = h;
        hint.style.display = h ? '' : 'none';
        btn.classList.toggle('is-unset', unset);
        btn.classList.toggle('is-ramp', !!(!unset && c && (c.interp === 'lin' || c.interp === 'quad')));
        btn.classList.toggle('is-mod', !!(!unset && c && isModInterp(c.interp)));
    }
    paint();
    // Let owners refresh a trigger whose ramp target changed elsewhere.
    btn.repaint = paint;

    btn.addEventListener('click', () => {
        const form = buildCellEditor(opts.getCell(), {
            resolveTarget: opts.resolveTarget,
            step: opts.step,
            optional: opts.optional,
            defaultLabel: opts.defaultLabel,
            onChange: (c) => { opts.onChange(c); paint(); },
            onClear: opts.onClear,
        });
        openPopover(btn, form, opts.title || 'Edit value');
    });

    return btn;
}
