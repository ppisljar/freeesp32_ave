// Segment compiler (Phase 5) — the Wizard projection -> flat `.led` model.
//
// A `session` of contiguous, non-overlapping segments is flattened to the same
// shared LedDoc the other views edit. The user never types a timestamp:
// absolute time = the running sum of segment durations.
//
//   session = { name, version:1, bg:null|Bg, segments:[Segment] }
//   Segment = { id, name, duration_ms, layers:[Layer] }
//   AudioLayer = { id, kind:'tone'|'binaural'|'noise', channel, channelR,
//                  stereo, wave_type, fields:{ freq, freqR, beat, pan, volume, mod } }
//   LightLayer = { id, kind:'light', channelMask, fields:{ freq, duty, bright, r, g, b } }
//
// Each `fields.*` is a Field (gen/field.js): value + optional ramp + optional mod.
//
// Compilation is three phases:
//   A. flatten — start[0]=0, start[i]=start[i-1]+duration[i-1]; each segment
//      emits its layers' steady values at its own start; a TERMINAL BOUNDARY
//      zeroes the energy field of any layer that drops out (bug #9).
//   B. ramps  — animate-on-start: a field's ramp prefix lives on THIS segment's
//      row (the START boundary) and the engine ramps toward the NEXT
//      same-channel row's value. A partial window (first_ms) inserts an anchor.
//   C. mods   — a field's pulse is emitted in place as PREFIXstart:end:period;
//      the next boundary restates the field so the mod ends cleanly.
//
// Channels are allocated session-wide and STABLE (bug #8/#10): a layer keeps the
// same channel(s) across every segment it appears in. LED regions become OR'd
// 8-bit masks (bug #1/#11). Pan is ×1 (bug #2); LED is always 8-field (bug #5);
// noise is wave_type 4/5/6 (bug #6); freq_r + wave_type emit with the placeholder
// rule (bug #3/#4) via serialize.js.

import {
    audioRow, ledRow, cell, commentRow, bgRow,
    MAX_ENTRIES, MAX_BATCH_SIZE, NUM_AUDIO_CHANNELS,
} from '../model.js';
import {
    RAMP_SHAPE_TO_INTERP, MOD_WAVE_TO_INTERP, partialWindowMs,
    fieldHasRamp, fieldHasMod,
} from '../field.js';

export const WIZARD_META_PREFIX = '@ave-wizard v1 ';
export const WIZARD_VERSION = 1;

// LED region -> mask bit (bug #11 — addresses all 8 channels).
export const LED_REGION_MASKS = {
    innerL: 0x01, outerL: 0x02, outerR: 0x04, innerR: 0x08,
    ch58: 0xF0, all: 0xFF,
};

// Wizard field name -> the model row Cell field it maps to.
const AUDIO_FIELD_TO_CELL = { freq: 'freq', pan: 'pan', volume: 'vol', mod: 'mod' };
const LED_FIELD_TO_CELL = { freq: 'freq', duty: 'duty', bright: 'bright', r: 'r', g: 'g', b: 'b' };

const EPS = 1e-9;
function approx(a, b) { return Math.abs(a - b) <= EPS; }
function num(v, dflt) { return Number.isFinite(v) ? v : (dflt || 0); }

// ---- iteration helpers -----------------------------------------------------

function eachLayer(session, cb) {
    const segs = (session && session.segments) || [];
    for (let si = 0; si < segs.length; si++) {
        const layers = (segs[si].layers) || [];
        for (let li = 0; li < layers.length; li++) cb(layers[li], si, li);
    }
}

function layerId(layer, si, li) {
    return (layer && layer.id !== undefined && layer.id !== null)
        ? String(layer.id) : ('s' + si + 'l' + li);
}

// ---- channel allocation (session-wide, stable) -----------------------------

export function allocateChannels(session) {
    const map = {};            // layerId -> { channel, channelR|null }
    const used = new Set();    // claimed audio channels

    // Pass 1: honour explicit pins so auto-allocation routes around them.
    eachLayer(session, (layer, si, li) => {
        if (!layer || layer.kind === 'light') return;
        if (Number.isFinite(layer.channel) && layer.channel >= 1 && layer.channel <= NUM_AUDIO_CHANNELS) {
            used.add(layer.channel);
        }
        if (isStereo(layer) && Number.isFinite(layer.channelR) &&
            layer.channelR >= 1 && layer.channelR <= NUM_AUDIO_CHANNELS) {
            used.add(layer.channelR);
        }
    });

    function nextFree() {
        for (let c = 1; c <= NUM_AUDIO_CHANNELS; c++) {
            if (!used.has(c)) { used.add(c); return c; }
        }
        return null; // 16-channel cap reached
    }

    // Pass 2: assign one stable channel (or two for stereo) per layer id, on
    // first occurrence.
    const seen = new Set();
    eachLayer(session, (layer, si, li) => {
        if (!layer || layer.kind === 'light') return;
        const id = layerId(layer, si, li);
        if (seen.has(id)) return;
        seen.add(id);
        let ch = (Number.isFinite(layer.channel) && layer.channel >= 1 && layer.channel <= NUM_AUDIO_CHANNELS)
            ? layer.channel : nextFree();
        let chR = null;
        if (isStereo(layer)) {
            chR = (Number.isFinite(layer.channelR) && layer.channelR >= 1 && layer.channelR <= NUM_AUDIO_CHANNELS)
                ? layer.channelR : nextFree();
        }
        map[id] = { channel: ch, channelR: chR };
    });
    return map;
}

function isStereo(layer) {
    return layer && layer.kind === 'binaural' && !!layer.stereo;
}

// ---- slot expansion --------------------------------------------------------
// A slot is one channel/mask's presence during one segment: the steady cells +
// the dynamic Fields whose ramp/mod the compiler resolves. Stereo binaural and
// the freq_r/wave side-channels are resolved here so the per-channel emission is
// uniform.

function field(layer, name) {
    const f = layer && layer.fields && layer.fields[name];
    return f || { value: 0, ramp: null, mod: null };
}

// Build the slots for one (layer, segment) at absolute time `t`.
function expandLayer(layer, alloc, t, si, warnings) {
    const slots = [];
    if (!layer) return slots;

    if (layer.kind === 'light') {
        const cells = {};
        const dyn = {};
        for (const wf in LED_FIELD_TO_CELL) {
            const f = field(layer, wf);
            cells[LED_FIELD_TO_CELL[wf]] = cell(num(f.value));
            dyn[LED_FIELD_TO_CELL[wf]] = f;
        }
        slots.push({
            kind: 'led', key: (layer.channelMask & 0xFF) || 0xFF, time: t, si,
            energy: 'bright', cells, dyn, freqR: 0, waveType: null,
        });
        return slots;
    }

    // ---- audio kinds ----
    const freqF = field(layer, 'freq');
    const panF = field(layer, 'pan');
    const volF = field(layer, 'volume');
    const modF = field(layer, 'mod');
    const wave = (layer.wave_type === undefined || layer.wave_type === null)
        ? null : layer.wave_type;

    if (layer.kind === 'noise') {
        // bug #6: noise = wave_type 4/5/6, no carrier dependency.
        const wt = (wave === null) ? 4 : wave;
        const cells = {
            freq: cell(0), pan: cell(num(panF.value)), vol: cell(num(volF.value)), mod: cell(0),
        };
        slots.push({
            kind: 'audio', key: alloc.channel, time: t, si, energy: 'vol',
            cells, dyn: { pan: panF, vol: volF }, freqR: 0, waveType: wt,
        });
        return slots;
    }

    if (layer.kind === 'binaural') {
        const base = num(freqF.value, 200);
        const beat = num(field(layer, 'beat').value, 10);
        if (isStereo(layer)) {
            // Two hard-panned channels; freq ramps/mods apply to the carrier on
            // both (R carries the +beat offset).
            const lCells = { freq: cell(base), pan: cell(-100), vol: cell(num(volF.value)), mod: cell(0) };
            const rCells = { freq: cell(base + beat), pan: cell(100), vol: cell(num(volF.value)), mod: cell(0) };
            slots.push({
                kind: 'audio', key: alloc.channel, time: t, si, energy: 'vol',
                cells: lCells, dyn: { freq: freqF, vol: volF, mod: modF }, freqR: 0, waveType: wave,
            });
            slots.push({
                kind: 'audio', key: alloc.channelR, time: t, si, energy: 'vol',
                // R carrier = base + beat; reuse freq ramp/mod intent on the offset value.
                cells: rCells,
                dyn: { freq: offsetField(freqF, beat), vol: volF, mod: modF },
                freqR: 0, waveType: wave,
            });
            return slots;
        }
        // Mono binaural: one channel + freq_r = base + beat (bug #4). freq_r is
        // NOT interpolatable (token 7, no prefix) -> beat changes are stepwise.
        if (fieldHasRamp(field(layer, 'beat'))) {
            warnings.push('Binaural beat in "' + (layer.name || 'binaural') +
                '" cannot ramp in mono (freq_r has no prefix); use true-stereo for a glide.');
        }
        const cells = {
            freq: cell(base), pan: cell(num(panF.value)), vol: cell(num(volF.value)), mod: cell(0),
        };
        slots.push({
            kind: 'audio', key: alloc.channel, time: t, si, energy: 'vol',
            cells, dyn: { freq: freqF, pan: panF, vol: volF, mod: modF },
            freqR: base + beat, waveType: wave,
        });
        return slots;
    }

    // tone
    const cells = {
        freq: cell(num(freqF.value)), pan: cell(num(panF.value)),
        vol: cell(num(volF.value)), mod: cell(num(modF.value)),
    };
    slots.push({
        kind: 'audio', key: alloc.channel, time: t, si, energy: 'vol',
        cells, dyn: { freq: freqF, pan: panF, vol: volF, mod: modF },
        freqR: 0, waveType: wave,
    });
    return slots;
}

// A synthetic Field whose value is shifted by `delta` (used for the R carrier of
// stereo binaural) while preserving the ramp/mod intent.
function offsetField(f, delta) {
    return { value: num(f.value) + delta, ramp: f.ramp, mod: f.mod };
}

// ---- the compiler ----------------------------------------------------------

// compileSession(session, { withMeta }) -> { doc, warnings, rowCount }
export function compileSession(session, opts) {
    opts = opts || {};
    const withMeta = opts.withMeta !== false;
    const warnings = [];
    const segs = (session && session.segments) || [];

    // start times + total end.
    const start = [];
    let acc = 0;
    for (let i = 0; i < segs.length; i++) {
        start[i] = acc;
        acc += Math.max(0, num(segs[i].duration_ms));
    }
    const totalEnd = acc;

    const alloc = allocateChannels(session);

    // Expand every (layer, segment) into slots, then group by channel/mask key.
    // Key namespace: audio channels and LED masks can collide numerically, so
    // tag with the kind.
    const groups = new Map(); // 'a:'+ch / 'l:'+mask -> { kind, key, slots:[] }
    let orderCounter = 0;
    eachLayer(session, (layer, si, li) => {
        const id = layerId(layer, si, li);
        const a = alloc[id] || { channel: null, channelR: null };
        const slots = expandLayer(layer, a, start[si], si, warnings);
        for (const s of slots) {
            if (s.kind === 'audio' && (s.key === null || s.key === undefined)) {
                warnings.push('Ran out of audio channels (cap ' + NUM_AUDIO_CHANNELS +
                    '); layer "' + (layer.name || layer.kind) + '" was dropped.');
                continue;
            }
            const gkey = (s.kind === 'audio' ? 'a:' : 'l:') + s.key;
            if (!groups.has(gkey)) groups.set(gkey, { kind: s.kind, key: s.key, slots: [] });
            s.order = orderCounter++;
            groups.get(gkey).slots.push(s);
        }
    });

    const emitted = []; // { time, order, row }

    for (const grp of groups.values()) {
        const slots = grp.slots.slice().sort((x, y) => (x.time - y.time) || (x.si - y.si));
        for (let k = 0; k < slots.length; k++) {
            const slot = slots[k];
            const next = slots[k + 1] || null;
            const contiguous = next && (next.si === slot.si + 1);
            // Does any same-channel row follow (so an animate-on-start ramp has a
            // target)? Either a contiguous next slot, or a terminal off-row.
            const segDur = Math.max(0, num(segs[slot.si].duration_ms));
            const boundary = slot.time + segDur;            // start of the next segment / session end
            const dropsAfter = !contiguous;                 // gap or end -> emit a terminal off
            const targetExists = !!next || dropsAfter;      // off-row also counts as a target

            // ---- per-field dynamics (Phase B + C) ----
            const cells = {};
            for (const name in slot.cells) cells[name] = slot.cells[name];
            const anchors = [];     // partial-window anchor rows for this slot
            for (const name in slot.dyn) {
                const f = slot.dyn[name];
                const steadyVal = num(slot.cells[name] ? slot.cells[name].value : f.value);
                if (fieldHasMod(f)) {
                    // Phase C — emit the pulse in place.
                    cells[name] = cell(steadyVal, MOD_WAVE_TO_INTERP[f.mod.wave] || 'sine',
                        f.mod.end, f.mod.period_ms);
                    if (fieldHasRamp(f)) {
                        warnings.push('Field "' + name + '" has both a transition and a pulse; ' +
                            'the pulse wins (a token carries one prefix).');
                    }
                    continue;
                }
                if (fieldHasRamp(f) && targetExists) {
                    // Phase B — animate-on-start ramp on THIS row; engine ramps to
                    // the next same-channel row's value.
                    const interp = RAMP_SHAPE_TO_INTERP[f.ramp.shape] || 'lin';
                    cells[name] = cell(steadyVal, interp);
                    const pw = partialWindowMs(f);
                    if (pw !== null && pw > 0 && next && pw < (next.time - slot.time)) {
                        // Insert an anchor at start+first_ms holding the TARGET value
                        // (next slot's value for this field) so the ramp finishes
                        // early and then holds.
                        const targetVal = num(next.cells[name] ? next.cells[name].value : steadyVal);
                        anchors.push({ t: slot.time + pw, field: name, value: targetVal });
                    }
                }
                // else: plain step (already in cells).
            }

            emitted.push({ time: slot.time, order: slot.order, row: makeRow(grp, slot.time, slot, cells) });

            // ---- partial-window anchor rows ----
            for (const an of anchors) {
                const aCells = {};
                for (const name in slot.cells) {
                    if (name === an.field) { aCells[name] = cell(an.value); continue; }
                    // Co-ramping fields: hold their start value (approximate; a
                    // simultaneous whole-window ramp on another field is restated
                    // at its start here — documented limitation, flagged once).
                    aCells[name] = cell(num(slot.cells[name].value));
                }
                emitted.push({ time: an.t, order: orderCounter++, row: makeRow(grp, an.t, slot, aCells) });
            }

            // ---- terminal off boundary (Phase A, bug #9) ----
            if (dropsAfter && boundary > slot.time) {
                emitted.push({
                    time: boundary, order: orderCounter++,
                    row: makeOffRow(grp, boundary, slot),
                });
            }
        }
    }

    // Stable global ordering: by time, then by allocation/emit order (same-time
    // rows form a firmware batch).
    emitted.sort((a, b) => (a.time - b.time) || (a.order - b.order));

    // Coalesce redundant consecutive holds per channel (no field changed, no
    // ramp/mod on either side) to respect MAX_ENTRIES.
    const kept = coalesce(emitted);

    const rows = [];
    if (withMeta) rows.push(commentRow('# ' + WIZARD_META_PREFIX + JSON.stringify(session)));
    if (session && session.bg) {
        rows.push(bgRow({ url: session.bg.url, pan: session.bg.pan, loudness: session.bg.loudness }));
    }
    for (const e of kept) rows.push(e.row);

    // ---- limit warnings ----
    const entryCount = kept.length;
    if (entryCount > MAX_ENTRIES) {
        warnings.push(entryCount + ' timeline entries exceeds MAX_ENTRIES (' +
            MAX_ENTRIES + '); the firmware will drop the overflow.');
    }
    const perTime = {};
    for (const e of kept) perTime[e.time] = (perTime[e.time] || 0) + 1;
    for (const t in perTime) {
        if (perTime[t] > MAX_BATCH_SIZE) {
            warnings.push(perTime[t] + ' entries share t=' + t +
                ' ms, exceeding the same-timestamp batch cap (' + MAX_BATCH_SIZE + ').');
        }
    }

    return {
        doc: { rows, bg: (session && session.bg) || null },
        warnings,
        rowCount: entryCount,
        totalEnd,
    };
}

function makeRow(grp, time, slot, cells) {
    if (grp.kind === 'led') {
        return ledRow({
            time, freq: cells.freq, duty: cells.duty, bright: cells.bright,
            r: cells.r, g: cells.g, b: cells.b, mask: grp.key,
        });
    }
    return audioRow({
        time, freq: cells.freq, pan: cells.pan, vol: cells.vol, mod: cells.mod,
        channel: grp.key, freqR: slot.freqR, waveType: slot.waveType,
    });
}

// Terminal off-row: hold non-energy fields steady, zero the energy field
// (vol for audio, bright for LED) so the layer goes off cleanly.
function makeOffRow(grp, time, slot) {
    if (grp.kind === 'led') {
        return ledRow({
            time, freq: cell(num(slot.cells.freq.value)),
            duty: cell(num(slot.cells.duty.value)), bright: cell(0),
            r: cell(num(slot.cells.r.value)), g: cell(num(slot.cells.g.value)),
            b: cell(num(slot.cells.b.value)), mask: grp.key,
        });
    }
    return audioRow({
        time, freq: cell(num(slot.cells.freq.value)),
        pan: cell(num(slot.cells.pan.value)), vol: cell(0), mod: cell(0),
        channel: grp.key, freqR: slot.freqR, waveType: slot.waveType,
    });
}

// Drop a row that merely re-holds the previous row's exact step values on the
// same channel (foreign-batch echo), unless either side carries a ramp/mod.
function coalesce(emitted) {
    const lastByKey = {};
    const out = [];
    for (const e of emitted) {
        const r = e.row;
        const key = r.kind === 'audio' ? 'a:' + r.channel : 'l:' + r.mask;
        const prev = lastByKey[key];
        if (prev && sameStepRow(prev.row, r) && !rowHasInterp(prev.row) && !rowHasInterp(r)) {
            // redundant hold — skip, but keep the time pointer on prev.
            continue;
        }
        out.push(e);
        lastByKey[key] = e;
    }
    return out;
}

function cellsEqual(a, b) {
    return a && b && a.interp === b.interp && approx(a.value, b.value) &&
        ((a.modEnd == null && b.modEnd == null) || approx(a.modEnd, b.modEnd)) &&
        ((a.modPeriodMs == null && b.modPeriodMs == null) || approx(a.modPeriodMs, b.modPeriodMs));
}

function sameStepRow(a, b) {
    if (a.kind !== b.kind) return false;
    if (a.kind === 'audio') {
        return a.channel === b.channel && a.freqR === b.freqR && a.waveType === b.waveType &&
            cellsEqual(a.freq, b.freq) && cellsEqual(a.pan, b.pan) &&
            cellsEqual(a.vol, b.vol) && cellsEqual(a.mod, b.mod);
    }
    return a.mask === b.mask &&
        cellsEqual(a.freq, b.freq) && cellsEqual(a.duty, b.duty) && cellsEqual(a.bright, b.bright) &&
        cellsEqual(a.r, b.r) && cellsEqual(a.g, b.g) && cellsEqual(a.b, b.b);
}

function rowHasInterp(r) {
    const fields = r.kind === 'audio'
        ? [r.freq, r.pan, r.vol, r.mod]
        : [r.freq, r.duty, r.bright, r.r, r.g, r.b];
    return fields.some(c => c && c.interp !== 'none');
}

// ---- round-trip: session <-> doc -------------------------------------------

export function findWizardMeta(doc) {
    if (!doc || !doc.rows) return null;
    for (const r of doc.rows) {
        if (r.kind !== 'comment') continue;
        const stripped = String(r.text || '').replace(/^#+\s*/, '');
        if (stripped.indexOf(WIZARD_META_PREFIX) !== 0) continue;
        const json = stripped.slice(WIZARD_META_PREFIX.length);
        try {
            const obj = JSON.parse(json);
            if (obj && Array.isArray(obj.segments)) return obj;
        } catch (e) { /* fall through to structural import */ }
    }
    return null;
}

// sessionFromDoc(doc) -> { session, imported } where imported=true flags a
// best-effort structural import (names/structure are guesses).
export function sessionFromDoc(doc) {
    const meta = findWizardMeta(doc);
    if (meta) {
        if (meta.version === undefined) meta.version = WIZARD_VERSION;
        return { session: meta, imported: false };
    }
    return { session: reconstructSession(doc), imported: true };
}

// Best-effort: infer segments from the distinct timestamps (each full
// restatement = a boundary), build layers per channel/mask active at each.
export function reconstructSession(doc) {
    const rows = (doc && doc.rows) ? doc.rows.filter(r => r.kind === 'led' || r.kind === 'audio') : [];
    const times = Array.from(new Set(rows.map(r => r.time))).sort((a, b) => a - b);
    const segments = [];
    for (let i = 0; i < times.length; i++) {
        const t = times[i];
        const nextT = (i + 1 < times.length) ? times[i + 1] : t;
        const duration_ms = Math.max(0, nextT - t);
        const layers = [];
        for (const r of rows) {
            if (r.time !== t) continue;
            if (r.kind === 'audio') {
                layers.push(audioRowToLayer(r));
            } else {
                layers.push(ledRowToLayer(r));
            }
        }
        // Skip a pure terminal-off boundary (everything zeroed) as its own segment.
        if (duration_ms === 0 && i === times.length - 1) {
            // last boundary with no duration — treat as the end, fold in only if it
            // carries non-off content.
            const meaningful = layers.some(l => layerIsAudible(l));
            if (!meaningful) continue;
        }
        segments.push({ id: 'seg' + i, name: 'Segment ' + (i + 1), duration_ms, layers });
    }
    const out = { name: 'Imported session', version: WIZARD_VERSION, segments };
    if (doc && doc.bg) out.bg = { url: doc.bg.url, pan: doc.bg.pan, loudness: doc.bg.loudness };
    return out;
}

function layerIsAudible(l) {
    if (l.kind === 'light') return num(l.fields.bright.value) > 0;
    return num(l.fields.volume.value) > 0;
}

function importField(c) {
    // local copy to avoid importing cellToField cycle ambiguity
    if (!c) return { value: 0, ramp: null, mod: null };
    if (c.interp === 'lin') return { value: c.value, ramp: { shape: 'linear', window: 'whole' }, mod: null };
    if (c.interp === 'quad') return { value: c.value, ramp: { shape: 'quadratic', window: 'whole' }, mod: null };
    const wave = { tri: 'triangle', sine: 'sine', sawup: 'sawup', sawdn: 'sawdown', sq: 'square' }[c.interp];
    if (wave) return { value: c.value, ramp: null, mod: { wave, end: c.modEnd, period_ms: c.modPeriodMs } };
    return { value: c.value, ramp: null, mod: null };
}

function audioRowToLayer(r) {
    const noise = r.waveType !== null && r.waveType !== undefined && r.waveType >= 4;
    const binaural = r.freqR && r.freqR > 0;
    const kind = noise ? 'noise' : (binaural ? 'binaural' : 'tone');
    const fields = {
        freq: importField(r.freq), pan: importField(r.pan),
        volume: importField(r.vol), mod: importField(r.mod),
    };
    if (binaural) {
        fields.beat = { value: Math.max(0, r.freqR - r.freq.value), ramp: null, mod: null };
        fields.freqR = { value: r.freqR, ramp: null, mod: null };
    }
    return {
        id: 'ch' + r.channel, kind, channel: r.channel, channelR: null, stereo: false,
        wave_type: (r.waveType === undefined) ? null : r.waveType, fields,
    };
}

function ledRowToLayer(r) {
    return {
        id: 'mask' + r.mask, kind: 'light', channelMask: r.mask,
        fields: {
            freq: importField(r.freq), duty: importField(r.duty), bright: importField(r.bright),
            r: importField(r.r), g: importField(r.g), b: importField(r.b),
        },
    };
}

// sessionToDoc — alias with the standard view contract naming.
export function sessionToDoc(session, opts) { return compileSession(session, opts); }
