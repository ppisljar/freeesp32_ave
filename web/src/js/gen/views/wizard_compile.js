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
// coalesce() decides "is this row redundant?" by rendering the row, not by
// comparing a hand-kept list of fields — see rowIdentity().
import { serialize } from '../serialize.js';
// Rows the segment model has no slot for (speech, marker, comments, blanks,
// raw) ride through the compile in `session.passthrough` instead of being
// deleted. See views/passthrough.js for the ordering contract.
// An inline `# why` on an led/audio row cannot ride through whole (the segment
// model rebuilds those rows), so the NOTE rides in `session.inlineNotes` and is
// re-attached to the rebuilt row by identity. See views/passthrough.js.
import {
    mergePassthrough, syncPassthrough, reattachInlineNotes, WIZARD_META_PREFIX,
} from './passthrough.js';

// Canonical definition lives in passthrough.js, which owns the full set of
// view metadata prefixes so no view can carry another's snapshot. Re-exported
// here because this is where callers expect to find it.
export { WIZARD_META_PREFIX };
export const WIZARD_VERSION = 1;

// LED region -> mask bit (bug #11 — addresses all 8 channels).
export const LED_REGION_MASKS = {
    innerL: 0x01, outerL: 0x02, outerR: 0x04, innerR: 0x08,
    ch58: 0xF0, all: 0xFF,
};

// v2 pulse fields carried per-layer (layer.pulse) and re-emitted verbatim on every
// row that layer expands to — the wizard preserves them losslessly across edits.
// These must stay in step with the optional columns serialize.js emits
// (serializeAudio / serializeLed): a column missing here is silently dropped on
// a structural import, which is how `depth` — the v3 14th audio column
// (ledc_format.md "depth") — used to vanish. The guard is a test, not vigilance:
// wizard_coalesce.test.js round-trips a row carrying every optional column.
const AUDIO_PULSE_FIELDS = ['duty', 'env', 'phase', 'attack', 'jitter', 'depth'];
const LED_PULSE_FIELDS = ['env', 'phase', 'attack', 'jitter'];
// Capture a single row's present pulse fields (null=`-` kept, undefined=omit dropped).
function capturePulseRow(r, names) {
    const rec = {};
    for (const f of names) if (r[f] !== undefined) rec[f] = r[f];
    return Object.keys(rec).length ? rec : undefined;
}

// Wizard field name -> the model row Cell field it maps to.
const AUDIO_FIELD_TO_CELL = { freq: 'freq', pan: 'pan', volume: 'vol', mod: 'mod' };
const LED_FIELD_TO_CELL = { freq: 'freq', duty: 'duty', bright: 'bright', r: 'r', g: 'g', b: 'b' };

function num(v, dflt) { return Number.isFinite(v) ? v : (dflt || 0); }

// ---- the `-` sentinel ------------------------------------------------------
// A Field whose `value` is null means "leave this channel's value unchanged" —
// the `-` token (ledc_format.md, "One rule, two spellings": the parser records
// what the line actually SET in entry->present and the executor substitutes the
// live value for every clear bit; "a cleared bit is never treated as 0").
//
// It is not 0, and flattening it to 0 cost two things at once: the lamp went
// dark at that row instead of holding its level, and the rewritten row then
// matched the terminal off-row exactly, so coalesce() deleted the author's
// end-of-session entry and the session got shorter. The model carries the
// sentinel as a null CELL, which serialize.js renders as `-` (cellStr).
function isUnchanged(f) { return !!f && f.value === null; }
// The plain step cell for a Field, keeping a `-` as a `-`.
function stepCell(f) { return isUnchanged(f) ? null : cell(num(f && f.value)); }
// Hold an already-built cell (null = the `-` sentinel rides through untouched).
function holdCell(c) { return (c === null || c === undefined) ? null : cell(num(c.value)); }
// For a column this layer kind DERIVES rather than reads (binaural's mod rate,
// noise's carrier): the constant still wins, but a `-` is never rewritten to 0.
function derivedCell(f, constant) { return isUnchanged(f) ? null : constant; }

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
        if (needsPair(layer) && Number.isFinite(layer.channelR) &&
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
        if (needsPair(layer)) {
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
// Monaural: two CENTRE-panned channels (pan=0) at freq / freq+beat. Like stereo
// it needs a second channel, but the beat forms acoustically (works on one
// speaker / one ear). Stereo wins if both flags are set.
function isMonaural(layer) {
    return layer && layer.kind === 'binaural' && !!layer.monaural && !layer.stereo;
}
function needsPair(layer) {
    return isStereo(layer) || isMonaural(layer);
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
    // Per-layer v2 pulse fields, stamped onto every slot so makeRow/makeOffRow
    // re-emit them on each expanded row (lossless preservation across edits).
    const pulse = layer.pulse || null;

    if (layer.kind === 'light') {
        const cells = {};
        const dyn = {};
        for (const wf in LED_FIELD_TO_CELL) {
            const f = field(layer, wf);
            cells[LED_FIELD_TO_CELL[wf]] = stepCell(f);
            dyn[LED_FIELD_TO_CELL[wf]] = f;
        }
        slots.push({
            kind: 'led', key: (layer.channelMask & 0xFF) || 0xFF, time: t, si,
            pulse, energy: 'bright', cells, dyn, freqR: 0, waveType: null,
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
            freq: derivedCell(freqF, cell(0)), pan: stepCell(panF),
            vol: stepCell(volF), mod: derivedCell(modF, cell(0)),
        };
        slots.push({
            kind: 'audio', key: alloc.channel, time: t, si, pulse, energy: 'vol',
            cells, dyn: { pan: panF, vol: volF }, freqR: 0, waveType: wt,
        });
        return slots;
    }

    if (layer.kind === 'binaural') {
        const base = num(freqF.value, 200);
        const beat = num(field(layer, 'beat').value, 10);
        if (isStereo(layer)) {
            // Two hard-panned channels; freq ramps/mods apply to the carrier on
            // both (R carries the +beat offset). The ±beat pair needs a concrete
            // carrier to offset, so a `-` carrier resolves to num()'s default
            // here rather than riding through as a sentinel.
            const lCells = { freq: cell(base), pan: cell(-100), vol: stepCell(volF), mod: derivedCell(modF, cell(0)) };
            const rCells = { freq: cell(base + beat), pan: cell(100), vol: stepCell(volF), mod: derivedCell(modF, cell(0)) };
            slots.push({
                kind: 'audio', key: alloc.channel, time: t, si, pulse, energy: 'vol',
                cells: lCells, dyn: { freq: freqF, vol: volF, mod: modF }, freqR: 0, waveType: wave,
            });
            slots.push({
                kind: 'audio', key: alloc.channelR, time: t, si, pulse, energy: 'vol',
                // R carrier = base + beat; reuse freq ramp/mod intent on the offset value.
                cells: rCells,
                dyn: { freq: offsetField(freqF, beat), vol: volF, mod: modF },
                freqR: 0, waveType: wave,
            });
            return slots;
        }
        if (isMonaural(layer)) {
            // Two CENTRE-panned channels (pan=0): freq / freq+beat. The Δf beat
            // forms acoustically (single speaker / one ear). freq ramps/mods apply
            // to the carrier on both (R carries the +beat offset). Same `-`
            // caveat as the stereo pair above.
            const lCells = { freq: cell(base), pan: cell(0), vol: stepCell(volF), mod: derivedCell(modF, cell(0)) };
            const rCells = { freq: cell(base + beat), pan: cell(0), vol: stepCell(volF), mod: derivedCell(modF, cell(0)) };
            slots.push({
                kind: 'audio', key: alloc.channel, time: t, si, pulse, energy: 'vol',
                cells: lCells, dyn: { freq: freqF, vol: volF, mod: modF }, freqR: 0, waveType: wave,
            });
            slots.push({
                kind: 'audio', key: alloc.channelR, time: t, si, pulse, energy: 'vol',
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
            freq: stepCell(freqF), pan: stepCell(panF),
            vol: stepCell(volF), mod: derivedCell(modF, cell(0)),
        };
        // A `-` carrier has no base to add the beat to: base+beat would invent
        // one (num()'s 200 Hz default) and rewrite the author's freq_r. The
        // absolute freq_r the structural import carried alongside the beat
        // (audioRowToLayer) is the only truthful source in that case.
        const absR = layer.fields && layer.fields.freqR;
        const freqR = (isUnchanged(freqF) && absR) ? num(absR.value) : (base + beat);
        slots.push({
            kind: 'audio', key: alloc.channel, time: t, si, pulse, energy: 'vol',
            cells, dyn: { freq: freqF, pan: panF, vol: volF, mod: modF },
            freqR, waveType: wave,
        });
        return slots;
    }

    // tone
    const cells = {
        freq: stepCell(freqF), pan: stepCell(panF),
        vol: stepCell(volF), mod: stepCell(modF),
    };
    slots.push({
        kind: 'audio', key: alloc.channel, time: t, si, pulse, energy: 'vol',
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
    let lastTouch = null; // { time, grp, slot } — the latest slot emitted

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
                // A `-` field has no value to ramp from or oscillate around, and
                // the format cannot spell one either (a prefix needs a literal:
                // parse.js returns the sentinel before it looks at glyphs). Leave
                // the sentinel alone rather than inventing a 0 start.
                if (cells[name] === null) continue;
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
                    aCells[name] = holdCell(slot.cells[name]);
                }
                emitted.push({ time: an.t, order: orderCounter++, row: makeRow(grp, an.t, slot, aCells) });
            }

            // ---- terminal off boundary (Phase A, bug #9) ----
            const offAt = (dropsAfter && boundary > slot.time) ? boundary : null;
            if (offAt !== null) {
                emitted.push({
                    time: offAt, order: orderCounter++,
                    row: makeOffRow(grp, offAt, slot),
                });
            }

            // Remember the chronologically last slot (anchor rows always land
            // inside its own window, so its off boundary — or its own start when
            // it has none — is the latest row it can produce). The session-end
            // anchor below restates this slot's off state at totalEnd.
            const slotEnd = (offAt === null) ? slot.time : offAt;
            if (!lastTouch || slotEnd >= lastTouch.time) lastTouch = { time: slotEnd, grp, slot };
        }
    }

    // Stable global ordering: by time, then by allocation/emit order (same-time
    // rows form a firmware batch).
    emitted.sort((a, b) => (a.time - b.time) || (a.order - b.order));

    // ---- session-end anchor ----
    // A trailing segment with NO layers (a pure rest) emits nothing of its own:
    // the previous layer's terminal off-row sits at the rest's START, so the
    // compiled timeline ends there and the rest never happens. The firmware ends
    // the session one step past the last entry (config_parser.c:2111-2124), so
    // the only way to hold a rest open is an entry at its end. Restate the last
    // touched slot's OFF state at totalEnd: same values as the off-row already
    // in force, so nothing moves, but the session lasts as long as the author
    // built it. (A trailing rest that holds a DARK/SILENT layer already emits
    // its own row at totalEnd; coalesce() is what used to delete that one.)
    if (lastTouch && lastTouch.time < totalEnd) {
        emitted.push({
            time: totalEnd, order: orderCounter++,
            row: makeOffRow(lastTouch.grp, totalEnd, lastTouch.slot),
        });
    }

    // Coalesce redundant consecutive holds per channel (no field changed, no
    // ramp/mod on either side) to respect MAX_ENTRIES.
    const kept = coalesce(emitted);

    // Rows pinned to the very top of the file: the round-trip metadata comment
    // and the BG clip row. BG is file-scoped, not a timeline entry — the parser
    // sniffs it per line before parse_line (config_parser.c:454) and the last
    // one wins, so it belongs above the timeline rather than merged into it.
    const header = [];
    if (withMeta) header.push(commentRow('# ' + WIZARD_META_PREFIX + JSON.stringify(session)));
    if (session && session.bg) {
        header.push(bgRow({ url: session.bg.url, pan: session.bg.pan, loudness: session.bg.loudness }));
    }
    // Put each carried inline note back on its rebuilt row (matched on kind +
    // time + channel/mask). Matched against `kept`, not `emitted`: a note whose
    // row coalesce() folded away as a redundant hold has no row to sit on, and
    // must come back as a whole-line comment rather than vanish.
    const carried = (session && session.passthrough) || [];
    const orphanNotes = reattachInlineNotes(kept, session && session.inlineNotes);
    // `kept` is already [{ time, order, row }] — exactly the `emitted` shape the
    // merge wants, so carried rows interleave with compiled rows by time.
    const rows = mergePassthrough(header, kept,
        orphanNotes.length ? carried.concat(orphanNotes) : carried);

    // ---- limit warnings ----
    // Carried rows are deliberately excluded: CONFIG_PARSER_MAX_ENTRIES caps the
    // timeline array the parser grows per accepted entry (config_parser.c:540-542),
    // and a comment / blank / `S` / `M` row never reaches that array.
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
        return ledRow(Object.assign({
            time, freq: cells.freq, duty: cells.duty, bright: cells.bright,
            r: cells.r, g: cells.g, b: cells.b, mask: grp.key,
        }, slot.pulse || null));
    }
    return audioRow(Object.assign({
        time, freq: cells.freq, pan: cells.pan, vol: cells.vol, mod: cells.mod,
        channel: grp.key, freqR: slot.freqR, waveType: slot.waveType,
    }, slot.pulse || null));
}

// Terminal off-row: hold non-energy fields steady, zero the energy field
// (vol for audio, bright for LED) so the layer goes off cleanly.
function makeOffRow(grp, time, slot) {
    // holdCell(), not cell(num(...)): a `-` field keeps holding whatever the
    // channel has. Only the ENERGY field is written, and it is written as a
    // hard 0 — "off" is an instruction, not something to leave unchanged.
    if (grp.kind === 'led') {
        return ledRow(Object.assign({
            time, freq: holdCell(slot.cells.freq),
            duty: holdCell(slot.cells.duty), bright: cell(0),
            r: holdCell(slot.cells.r), g: holdCell(slot.cells.g),
            b: holdCell(slot.cells.b), mask: grp.key,
        }, slot.pulse || null));
    }
    return audioRow(Object.assign({
        time, freq: holdCell(slot.cells.freq),
        pan: holdCell(slot.cells.pan), vol: cell(0), mod: cell(0),
        channel: grp.key, freqR: slot.freqR, waveType: slot.waveType,
    }, slot.pulse || null));
}

/*
 * Drop a row that merely re-holds the previous row's exact step values on the
 * same channel (foreign-batch echo), unless either side carries a ramp/mod.
 *
 * Two rows count as the same hold iff they SERIALIZE the same (rowIdentity).
 * The predicate used to be a hand-written field list — the interpolatable cells
 * plus mask/channel/freq_r/wave_type — and every format column added since was
 * missing from it, so a row whose only change was a v2 pulse field (LED
 * env/phase/attack/jitter, audio duty/env/phase/attack/jitter, v3 depth) looked
 * redundant and was DELETED with `warnings` empty. The new value then survived
 * only on the terminal off-row, where brightness/volume is already 0, so the
 * firmware never applied it.
 *
 * The same list was wrong in the other direction too: its cell comparison was
 * `a && b && ...`, so two `-` cells (null) compared UNEQUAL and a row that was
 * byte-for-byte the previous one survived as dead weight. Comparing the rendered
 * line fixes both halves at once — sessions/test/t12_format_edges.ledc loses one
 * such row and sessions/test/t06_led_phase_jitter.ledc keeps five real ones.
 *
 * `emitted` is in final file order (the caller sorted it), so the last row's
 * time is the session's end.
 */
function coalesce(emitted) {
    // The session's end time is load-bearing even when the VALUES that carry it
    // merely restate what is already in force: the timeline task declares the
    // session over one step past the last entry (config_parser.c:2111-2124) and
    // stops flicker on every channel in that same dispatch
    // (config_parser.c:2374, mask 0xFF). Dropping the row that carries the end
    // time moves the end earlier and a trailing rest segment disappears —
    // measured as a 6000 ms session compiling to 2000 ms.
    //
    // The end time is a property of the FILE, not of a channel, so ONE entry at
    // it is enough. Keeping every channel's redundant hold there instead cost
    // 1-3 no-op entries on 9 of the shipped library sessions whose end time was
    // never at risk, against a hard MAX_ENTRIES of 100 (config_parser.c:540-542).
    // Hence: coalesce normally, then re-admit a single dropped hold only if the
    // end time did not survive on its own.
    let endTime = 0;
    for (const e of emitted) if (e.time > endTime) endTime = e.time;

    const lastByKey = {};
    const out = [];
    const droppedAtEnd = [];   // redundant holds at endTime, in file order
    let keptAtEnd = false;
    for (const e of emitted) {
        const r = e.row;
        const key = r.kind === 'audio' ? 'a:' + r.channel : 'l:' + r.mask;
        const prev = lastByKey[key];
        const id = rowIdentity(r);
        if (prev && prev.id === id && !rowHasInterp(prev.row) && !rowHasInterp(r)) {
            if (e.time === endTime) droppedAtEnd.push(e);
            continue;
        }
        out.push(e);
        if (e.time === endTime) keptAtEnd = true;
        lastByKey[key] = { row: r, time: e.time, id };
    }
    // Nothing survived at the end time, so the only rows that could have
    // carried it were holds. Re-admit the first of them: semantically a no-op
    // (it restates a state already in force) but it pins the last timestamp,
    // which is the whole duration of a trailing rest/dark segment.
    //
    // Being a restatement, it cannot INTRODUCE a lit ending the author did not
    // build — where they did end on a lit lamp, the firmware's 0xFF stop
    // cancels it and avmeasure says so (LEDC_LAST_ENTRY_CANCELLED,
    // ledc_expect.py:1255). That warning is the right outcome: it names the
    // authoring choice, whereas deleting the row instead shortened the session
    // to the previous boundary and said nothing.
    if (!keptAtEnd && droppedAtEnd.length) out.push(droppedAtEnd[0]);
    return out;
}

// A row's identity for coalescing: the line it serializes to with the timestamp
// zeroed. Going through the real serializer is what keeps a future format column
// from being forgotten here — if a column can change the bytes on disk, it
// changes the identity, with no list to maintain. Equal bytes is also the only
// defensible licence to delete a row: the file, and therefore the firmware,
// cannot tell the difference.
function rowIdentity(r) {
    return serialize({ rows: [Object.assign({}, r, { time: 0 })] });
}

// Does any compound cell on this row carry a prefix? Scanned off the row object
// instead of a field list, for the same reason as rowIdentity(): the v2/v3 pulse
// columns are compound cells too (LED phase/attack, audio duty/phase/attack/
// depth — ledc_format.md "Compound cells"). A ramp there reads its target from
// the next entry that actually SETS the field, so a hold row in between is a
// ramp endpoint, not spare weight.
function rowHasInterp(r) {
    for (const k in r) {
        const c = r[k];
        if (c && typeof c === 'object' && typeof c.interp === 'string' && c.interp !== 'none') {
            return true;
        }
    }
    return false;
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
    const imported = !meta;
    let session;
    if (meta) {
        if (meta.version === undefined) meta.version = WIZARD_VERSION;
        session = meta;
    } else {
        session = reconstructSession(doc);
    }

    // Passthrough is ALWAYS recomputed from the live doc, never read back from
    // the metadata comment. The metadata is a snapshot from whenever the wizard
    // last compiled; the Text view may have added, edited or deleted
    // speech/marker/comment rows since. Trusting the snapshot would resurrect
    // rows the user deleted and drop rows they added.
    //
    // No view's metadata comment is carried — not our own (compileSession
    // regenerates it) and not the lane view's (a stale snapshot would nest and
    // grow without bound, and would revert this view's edits on a switch).
    // One rule for both: adopt a BG row the snapshot missed, then recompute
    // the carried set from the live doc.
    syncPassthrough(session, doc);

    return { session, imported };
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
    const energy = (l.kind === 'light') ? l.fields.bright : l.fields.volume;
    // A `-` energy field is not an explicit zero — it holds whatever the channel
    // already had — so a final boundary that writes `-` is content the author
    // meant to keep, not a terminal off-row to fold away.
    if (isUnchanged(energy)) return true;
    return num(energy && energy.value) > 0;
}

function importField(c) {
    // local copy to avoid importing cellToField cycle ambiguity
    // A null cell is the `-` sentinel, NOT 0: it is carried as a null-valued
    // Field (isUnchanged) so the compiler re-emits `-`. Returning `{value: 0}`
    // here turned "leave brightness alone" into "brightness 0" — the lamp went
    // dark at that row, and the rewritten row then matched the terminal off-row
    // bit-for-bit so coalesce() deleted the author's end-of-session entry.
    if (c === null) return { value: null, ramp: null, mod: null };
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
        // A `-` carrier (r.freq === null) leaves the channel's live frequency
        // alone, so the beat — freq_r MINUS the carrier — is not derivable from
        // this line. Carry beat 0 and let the absolute freq_r stand; expandLayer
        // prefers it over base+beat exactly in that case. (Reading
        // `r.freq.value` unconditionally threw a TypeError here.)
        const carrier = (r.freq === null) ? null : num(r.freq.value);
        fields.beat = {
            value: (carrier === null) ? 0 : Math.max(0, r.freqR - carrier),
            ramp: null, mod: null,
        };
        fields.freqR = { value: r.freqR, ramp: null, mod: null };
    }
    return {
        id: 'ch' + r.channel, kind, channel: r.channel, channelR: null, stereo: false,
        wave_type: (r.waveType === undefined) ? null : r.waveType, fields,
        pulse: capturePulseRow(r, AUDIO_PULSE_FIELDS),
    };
}

function ledRowToLayer(r) {
    return {
        id: 'mask' + r.mask, kind: 'light', channelMask: r.mask,
        fields: {
            freq: importField(r.freq), duty: importField(r.duty), bright: importField(r.bright),
            r: importField(r.r), g: importField(r.g), b: importField(r.b),
        },
        pulse: capturePulseRow(r, LED_PULSE_FIELDS),
    };
}

// sessionToDoc — alias with the standard view contract naming.
export function sessionToDoc(session, opts) { return compileSession(session, opts); }
