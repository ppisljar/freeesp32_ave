// Lane projection <-> flat `.led` model (Phase 4 — the critical correctness
// piece).
//
// Two pure, unit-tested transforms:
//   lanesToDoc(lanes)  — project the DAW lane model onto a flat LedDoc whose
//                        rows the firmware (config_parser.c) plays correctly.
//   docToLanes(doc)    — the inverse: reconstruct lanes from a flat doc,
//                        coalescing implicit breakpoints (no row explosion),
//                        detecting mod glyphs / freq_r ghost / wave markers.
//
// The lane model is JSON-serializable so it can be embedded verbatim in a
// `# @ave-lane v1 {json}` comment for byte-perfect reopen. The firmware ignores
// the comment; presence => exact restore, absence => structural reconstruction.
//
// Sweep-direction convention (verified against config_parser.c:1993-2000,
// "NEW CONVENTION (animate-on-start)"): the interp prefix lives on the START
// entry; the ramp runs from the start entry's value toward the NEXT
// same-channel/field entry's value over the time gap. A keyframe therefore owns
// its OUTGOING segment shape. LFOs are self-contained `PREFIXstart:end:period`.
//
// The `-` sentinel (format v2) is NOT a keyframe — it carries no value at all.
// Its time rides in the sublane's `hold` list and is projected straight back as
// `-`; see isHeldAt for why materialising a number there would change what the
// device does. The same `hold` list says "this row wrote no token at all" for
// the stepped freqR / wave sublanes — see steppedStateAt.
//
// ONE ROW PER (lane, time) is a hard constraint of the projection, not a bug:
// laneUnionTimes is a SET of times and emitCellAt resolves a single value per
// time. A file may legally put two rows on one channel at one timestamp, so
// such a group is split across lanes that SHARE a key — see
// splitOverlappingTimes for why merging them is not available.

import {
    cell, ledRow, audioRow, commentRow, bgRow,
    GLYPH_TO_INTERP, INTERP_GLYPHS, isModInterp,
    DEFAULT_MOD_PERIOD_MS, MAX_ENTRIES, MAX_BATCH_SIZE,
    NUM_LED_CHANNELS,
} from '../model.js';
import { lerp, quad } from '../interp.js';
// Rows the lane model has no slot for (speech, marker, comments, blanks, raw)
// ride through the projection in `lanes.passthrough` instead of being deleted.
// See views/passthrough.js for the ordering contract.
// An inline `# why` on an led/audio row cannot ride through whole (the lane
// model rebuilds those rows), so the NOTE rides in `lanes.inlineNotes` and is
// re-attached to the rebuilt row by identity. See views/passthrough.js.
import {
    mergePassthrough, syncPassthrough, reattachInlineNotes, LANE_META_PREFIX,
} from './passthrough.js';

// ---- Metadata marker -------------------------------------------------------
// Canonical definition lives in passthrough.js, which owns the full set of
// view metadata prefixes so no view can carry another's snapshot. Re-exported
// here because this is where callers expect to find it.
export { LANE_META_PREFIX };
export const LANE_VERSION = 1;

// Cell-valued fields per lane kind (each maps to a model Cell on the row).
export const AUDIO_CELL_FIELDS = ['freq', 'pan', 'vol', 'mod'];
export const LED_CELL_FIELDS = ['freq', 'duty', 'bright', 'r', 'g', 'b'];
// v2 pulse fields carried per-row as a time-keyed sidecar (lane.pulse) rather than
// as animatable sublanes — the lane view preserves them losslessly across edits.
export const AUDIO_PULSE_FIELDS = ['duty', 'env', 'phase', 'attack', 'jitter'];
export const LED_PULSE_FIELDS = ['env', 'phase', 'attack', 'jitter'];

// ---- shape <-> interp mapping ---------------------------------------------
// The lane model uses 'step' for the no-interp case; the Cell model uses 'none'.
// Every other shape name is identical to its INTERP_KINDS counterpart.
export function shapeToInterp(shape) { return shape === 'step' ? 'none' : shape; }
export function interpToShape(interp) { return interp === 'none' ? 'step' : interp; }
export function isPeriodicShape(shape) { return isModInterp(shapeToInterp(shape)); }
function isRampShape(shape) { return shape === 'lin' || shape === 'quad'; }

const EPS = 1e-6;
function approxEqual(a, b) {
    const d = Math.abs(a - b);
    return d <= EPS || d <= EPS * Math.max(Math.abs(a), Math.abs(b));
}

// ---- Keyframe / sublane helpers -------------------------------------------

export function keyframe(t, v, shape, lfoEnd, lfoPeriodMs) {
    const kf = { t: t || 0, v: (v === undefined || v === null) ? 0 : v,
                 shape: shape || 'step' };
    if (isPeriodicShape(kf.shape)) {
        kf.lfoEnd = (lfoEnd === undefined || lfoEnd === null) ? kf.v : lfoEnd;
        kf.lfoPeriodMs = (lfoPeriodMs === undefined || lfoPeriodMs === null)
            ? DEFAULT_MOD_PERIOD_MS : lfoPeriodMs;
    }
    return kf;
}

function sortedKeys(keys) {
    return (keys || []).slice().sort((a, b) => a.t - b.t);
}

// ---- The `-` sentinel ("leave unchanged") ----------------------------------
// A cell field written as a lone `-` leaves the field's present bit clear
// (config_parser.c:1598-1600 LED, :1777-1800 audio) and the execute path then
// substitutes the channel's LIVE value so the write is a no-op (LED:
// config_parser.c:2994-3014, audio: :2645-2650).
//
// That live value is NOT something this editor can compute: it is whatever the
// device had actually reached — a value mid-sweep, a modulated value, or one
// written by another entry whose mask/channel overlaps this lane. So a `-` gets
// no keyframe (there is no value to put in one, and inventing the editor's own
// guess would silently change what the session does on hardware). The sublane
// records the TIME in `hold` instead, and lanesToDoc emits `-` back there.
//
// A real keyframe at exactly the same time always wins over the hold: dropping
// a keyframe onto a held row is the user deliberately replacing the `-` with a
// value, and the edit must survive the next projection. coalesceRampKeys is
// told about `hold` for the same reason — deleting the keyframe that sits at a
// held time would hand the row back to the hold and silently invert this rule.
function isHeldAt(s, t) {
    if (!s || !s.hold || s.hold.indexOf(t) < 0) return false;
    return !(s.keys || []).some(k => k.t === t);
}

// Resolve the emission for one cell sublane at union time `t`.
// Returns { v, shape, lfoEnd?, lfoPeriodMs?, approx?, lfoInterior? } or null.
function emitCellAt(keys, t) {
    if (!keys || !keys.length) return null;
    let active = null, idx = -1;
    for (let i = 0; i < keys.length; i++) {
        if (keys[i].t <= t) { active = keys[i]; idx = i; } else break;
    }
    if (!active) {
        // Before the first keyframe — hold the first value as a step.
        return { v: keys[0].v, shape: 'step' };
    }
    if (active.t === t) {
        const out = { v: active.v, shape: active.shape };
        if (isPeriodicShape(active.shape)) {
            out.lfoEnd = active.lfoEnd;
            out.lfoPeriodMs = active.lfoPeriodMs;
        }
        return out;
    }
    // Interior of the active keyframe's outgoing segment.
    if (active.shape === 'step') return { v: active.v, shape: 'step' };
    const next = keys[idx + 1];
    if (isPeriodicShape(active.shape)) {
        // Re-emitting a field mid-LFO restarts its phase — flagged as a conflict.
        // We restate the identical LFO spec so the band visually continues.
        return {
            v: active.v, shape: active.shape,
            lfoEnd: active.lfoEnd, lfoPeriodMs: active.lfoPeriodMs,
            lfoInterior: !!next,
        };
    }
    if (!next) {
        // Ramp with no later target — holds at start (firmware behaviour).
        return { v: active.v, shape: 'step' };
    }
    const p = (next.t === active.t) ? 0 : (t - active.t) / (next.t - active.t);
    if (active.shape === 'lin') {
        // Piecewise-linear == linear: exact under resampling.
        return { v: lerp(active.v, next.v, p), shape: 'lin' };
    }
    // Quadratic crossed by a foreign keyframe — quad-then-quad approximation.
    return { v: quad(active.v, next.v, p), shape: 'quad', approx: true };
}

// Resolve a stepped (freqR / wave) sublane at time t.
//
// Returns { v } when the source row at t WROTE that token, and null when it did
// not. The difference is the whole point: both tokens are "absent / `-` =>
// leave the live value alone" fields (freq_r config_parser.c:1827, wave_type
// :1845), and the engine acts on the present bit, not on the number — the
// freq_r write is guarded by AUD_SET_FREQR (config_parser.c:1182) and the wave
// write by has_wave_type (:1174). An explicit `0` is NOT the same thing: it is
// a real write, collapsing the channel to mono / selecting the sine waveform.
//
// So absence is encoded, not inferred from the value:
//   - no sublane at all      => no row in this lane ever wrote the token;
//   - before the first key   => the writes start later;
//   - a `hold` time          => this row wrote nothing while others did.
// Keys are de-duplicated along a run of equal values (buildSteppedSublane), so
// a time inside a run and not in `hold` did write the token and resolves to the
// run's value.
// Returns the resolved KEYFRAME, so freqR's `>`/`*` glide prefix survives too
// (see freqRKeyframe).
function steppedStateAt(s, t) {
    const keys = (s && s.keys) || [];
    if (!keys.length) return null;
    if (keys[0].t > t) return null;              // the writes start after t
    if (isHeldAt(s, t)) return null;             // this row wrote no token
    let kf = keys[0];
    for (const k of keys) { if (k.t <= t) kf = k; else break; }
    return kf;
}

function emissionToCell(em) {
    const interp = shapeToInterp(em.shape);
    if (isModInterp(interp)) return cell(em.v, interp, em.lfoEnd, em.lfoPeriodMs);
    return cell(em.v, interp);
}

// All distinct keyframe times across every sublane of a lane (sorted unique).
function laneUnionTimes(lane) {
    const set = new Set();
    const sub = lane.sub || {};
    for (const f in sub) {
        for (const k of (sub[f].keys || [])) set.add(k.t);
        // Hold times are keyframeless but they are still rows: without them a
        // row whose every cell field is `-` (sessions/test/t12_format_edges.ledc
        // line 89 sets only jitter) would disappear from the projection.
        for (const t of (sub[f].hold || [])) set.add(t);
    }
    // Include pulse-sidecar times so a row that carries only pulse fields (its cell
    // keyframes coalesced away as redundant) is still re-emitted with its pulse data.
    if (lane.pulse) {
        for (const t in lane.pulse) set.add(Number(t));
    }
    const out = Array.from(set);
    out.sort((a, b) => a - b);
    return out;
}

// ---- lanes -> doc ----------------------------------------------------------

// Project the lane model onto a flat LedDoc.
// Returns { doc, warnings:[String], conflicts:[{laneKey,kind,field,t}] }.
// opts.withMeta (default true) embeds the round-trip `# @ave-lane` comment.
export function lanesToDoc(lanes, opts) {
    opts = opts || {};
    const withMeta = opts.withMeta !== false;
    const warnings = [];
    const conflicts = [];
    // Rows pinned to the very top of the file: the round-trip metadata comment
    // and the BG clip row. BG is file-scoped, not a timeline entry — the parser
    // sniffs it per line before parse_line (config_parser.c:454) and the last
    // one wins, so it belongs above the timeline, not merged into it. Everything
    // else is merged by time so carried rows land next to their own block.
    const header = [];

    if (withMeta) {
        header.push(commentRow('# ' + LANE_META_PREFIX + JSON.stringify(lanes)));
    }
    if (lanes && lanes.bg) {
        header.push(bgRow({ url: lanes.bg.url, pan: lanes.bg.pan, loudness: lanes.bg.loudness }));
    }

    const laneList = (lanes && lanes.lanes) ? lanes.lanes : [];
    const emitted = []; // { time, order, row } — `order` is the lane index

    laneList.forEach((lane, li) => {
        const times = laneUnionTimes(lane);
        const sub = lane.sub || {};
        for (const t of times) {
            if (lane.kind === 'audio') {
                const fields = {};
                for (const f of AUDIO_CELL_FIELDS) {
                    const keys = sub[f] && sub[f].keys;
                    const em = emitCellAt(keys, t);
                    // null => the `-` sentinel (see isHeldAt). The conflict test
                    // below still runs on a held field: the firmware stops EVERY
                    // modulation on the channel at each entry and restarts only
                    // the ones that line restates (config_parser.c:2745-2751),
                    // so an interior row truncates the band whether it restates
                    // the field or leaves it `-`.
                    fields[f] = isHeldAt(sub[f], t) ? null
                        : (em ? emissionToCell(em) : cell(0));
                    if (em && em.lfoInterior) {
                        conflicts.push({ laneKey: lane.key, kind: 'audio', field: f, t });
                    }
                }
                // Both of these are tri-state (absent / explicit value), so the
                // PRESENT BIT travels with the number — see steppedStateAt.
                // Without it the serializer has to guess from the value, and it
                // guesses "0 means mono" (serialize.js:142), so a row that said
                // nothing about freq_r came back saying `0` and killed a live
                // binaural pair. `freqRPresent: false` makes it write `-`
                // instead when a later column forces the slot (serialize.js:153).
                const fr = steppedStateAt(sub.freqR, t);
                const wv = steppedStateAt(sub.wave, t);
                const pulse = (lane.pulse && lane.pulse[t]) || null;
                emitted.push({ time: t, order: li, row: audioRow(Object.assign({
                    time: t, freq: fields.freq, pan: fields.pan, vol: fields.vol,
                    mod: fields.mod, channel: lane.key,
                    freqR: fr ? fr.v : 0, freqRPresent: !!fr,
                    // A `>`/`*` on freq_r only survives on the compound cell
                    // that rides alongside the number (model.js freqRCell,
                    // preferred by serialize.js:139). Without it the glide
                    // became a step: the firmware wires a real FREQ_R sweep
                    // off that prefix (config_parser.c:2937).
                    freqRCell: (fr && fr.shape !== 'step')
                        ? emissionToCell(fr) : undefined,
                    waveType: wv ? wv.v : null,
                }, pulse)) });
            } else {
                const fields = {};
                for (const f of LED_CELL_FIELDS) {
                    const keys = sub[f] && sub[f].keys;
                    const em = emitCellAt(keys, t);
                    // null => the `-` sentinel (see isHeldAt); same conflict
                    // rule as the audio branch above, the LED mods being
                    // stopped per entry at config_parser.c:3099-3105.
                    fields[f] = isHeldAt(sub[f], t) ? null
                        : (em ? emissionToCell(em) : cell(0));
                    if (em && em.lfoInterior) {
                        conflicts.push({ laneKey: lane.key, kind: 'led', field: f, t });
                    }
                }
                const pulse = (lane.pulse && lane.pulse[t]) || null;
                emitted.push({ time: t, order: li, row: ledRow(Object.assign({
                    time: t, freq: fields.freq, duty: fields.duty, bright: fields.bright,
                    r: fields.r, g: fields.g, b: fields.b, mask: lane.key,
                }, pulse)) });
            }
        }
    });

    // Stable global ordering: by time, then by lane order (same-time = batch).
    emitted.sort((a, b) => (a.time - b.time) || (a.order - b.order));
    // Put each carried inline note back on its rebuilt row (matched on kind +
    // time + channel/mask). Done AFTER the sort so notes are consumed in the
    // same order the rows will be written. A note whose row no longer exists
    // comes back as a whole-line comment instead of being deleted.
    const carried = (lanes && lanes.passthrough) || [];
    const orphanNotes = reattachInlineNotes(emitted, lanes && lanes.inlineNotes);
    // Splice the carried rows back in. They are deliberately NOT counted in the
    // limit warnings below: CONFIG_PARSER_MAX_ENTRIES caps the timeline array
    // the parser grows per accepted entry (config_parser.c:540-542), and a
    // comment / blank / `S` / `M` row never reaches that array.
    const rows = mergePassthrough(header, emitted,
        orphanNotes.length ? carried.concat(orphanNotes) : carried);

    // Limit warnings (MAX_ENTRIES / batch size).
    const entryCount = emitted.length;
    if (entryCount > MAX_ENTRIES) {
        warnings.push(entryCount + ' timeline entries exceeds MAX_ENTRIES (' +
            MAX_ENTRIES + '); the firmware will drop the overflow.');
    }
    const perTime = {};
    for (const e of emitted) perTime[e.time] = (perTime[e.time] || 0) + 1;
    for (const t in perTime) {
        if (perTime[t] > MAX_BATCH_SIZE) {
            warnings.push(perTime[t] + ' entries share t=' + t +
                ' ms, exceeding the same-timestamp batch cap (' + MAX_BATCH_SIZE + ').');
        }
    }
    // Two lanes sharing a key can land a row on the same (channel|mask, time).
    // That is legal and it is how an overlap lane round-trips (see
    // splitOverlappingTimes), but the author should know, because the firmware
    // runs both entries in one batch and the LATER one wins field by field —
    // and because dragging a point onto an occupied timestamp is how the
    // situation gets created by hand.
    const overlaps = new Set();
    for (const e of emitted) {
        const r = e.row;
        const id = r.kind + ':' + (r.kind === 'audio' ? r.channel : r.mask) + ':' + e.time;
        if (overlaps.has(id)) overlaps.add('!' + id); else overlaps.add(id);
    }
    const overlapCount = Array.from(overlaps).filter(k => k[0] === '!').length;
    if (overlapCount) {
        warnings.push(overlapCount + ' timestamp(s) carry two or more rows on one ' +
            'channel/mask. Both are written, in lane order, and the firmware ' +
            'applies them in that order — the later row wins each field it sets.');
    }
    if (conflicts.length) {
        warnings.push(conflicts.length + ' interior-LFO conflict(s): a foreign ' +
            'keyframe falls inside an LFO band and restarts its phase. Snap those ' +
            'points to the band boundaries to fix.');
    }

    return { doc: { rows, bg: (lanes && lanes.bg) || null }, warnings, conflicts };
}

// ---- doc -> lanes ----------------------------------------------------------

// Find + parse the `# @ave-lane v1 {json}` metadata comment, if any.
export function findLaneMeta(doc) {
    if (!doc || !doc.rows) return null;
    for (const r of doc.rows) {
        if (r.kind !== 'comment') continue;
        const stripped = String(r.text || '').replace(/^#+\s*/, '');
        if (stripped.indexOf(LANE_META_PREFIX) !== 0) continue;
        const json = stripped.slice(LANE_META_PREFIX.length);
        try {
            const obj = JSON.parse(json);
            if (obj && Array.isArray(obj.lanes)) return obj;
        } catch (e) { /* fall through to structural reconstruction */ }
    }
    return null;
}

// Caller's contract: `c` is a real Cell. A null cell is the `-` sentinel and
// has no value to keyframe — buildCellSublane filters those out first.
function cellToKeyframe(time, c) {
    const shape = interpToShape(c.interp);
    if (isPeriodicShape(shape)) {
        return keyframe(time, c.value, shape, c.modEnd, c.modPeriodMs);
    }
    return keyframe(time, c.value, shape);
}

// Coalesce implicit breakpoints: drop interior keyframes that lie on the
// interpolation line of a same-shape ramp run (undoes lanesToDoc's resampling).
// Quad spans recorded in metadata are coalesced exactly; here we coalesce any
// interior point whose value matches the curve within EPS.
//
// `hold` is the sublane's `-` times, and a keyframe sitting on one of them is
// NEVER dropped. Dropping it does not leave the value "implied by the curve"
// the way every other drop here does: isHeldAt starts answering true for that
// time (the keyframe that outvoted the hold is gone), so the row is re-emitted
// as `-` and the value becomes whatever the device had reached. That is the
// opposite of the rule isHeldAt documents, and it would materialise-by-deletion
// the one thing the sentinel exists to protect.
export function coalesceRampKeys(keys, hold) {
    const ks = sortedKeys(keys);
    const held = (hold && hold.length) ? new Set(hold) : null;
    const out = [];
    for (let i = 0; i < ks.length; i++) {
        const b = ks[i];
        const a = out[out.length - 1];
        const c = ks[i + 1];
        if (a && !(held && held.has(b.t))) {
            // (1) Interior of a same-shape ramp whose value lies on the curve:
            //     a->b->c collapses to a single a->c ramp (undo resampling).
            if (a && c && isRampShape(a.shape) && a.shape === b.shape && c.t > a.t) {
                const p = (b.t - a.t) / (c.t - a.t);
                const onCurve = a.shape === 'lin'
                    ? approxEqual(b.v, lerp(a.v, c.v, p))
                    : approxEqual(b.v, quad(a.v, c.v, p));
                if (onCurve) continue; // drop b; a stays the anchor
            }
            // (2) Redundant constant step: a holds a.v; b is the same value held
            //     again — removing it changes nothing (foreign-keyframe echo).
            if (a.shape === 'step' && b.shape === 'step' && approxEqual(a.v, b.v)) {
                continue;
            }
        }
        out.push(b);
    }
    return out;
}

// Build one cell sublane from the rows of a lane: a keyframe per row that
// states a value, and a `hold` time per row that wrote `-` (see isHeldAt).
// `hold` is omitted when empty so a session with no sentinel serializes its
// `# @ave-lane` snapshot exactly as it did before this mechanism existed.
function buildCellSublane(grp, field) {
    const keys = [];
    const hold = [];
    for (const r of grp) {
        const c = r[field];
        // null = `-`. undefined cannot come out of ledRow/audioRow (they
        // default a missing core cell to cell(0)), but a row assembled by hand
        // is held rather than keyframed at a value nobody wrote.
        if (c === null || c === undefined) hold.push(r.time);
        else keys.push(cellToKeyframe(r.time, c));
    }
    const s = { keys: coalesceRampKeys(keys, hold) };
    if (hold.length) s.hold = hold;
    return s;
}

// Build a stepped sublane (freqR / wave) from per-row values.
//
// Consecutive duplicates are dropped so an unchanged field does not explode
// into keyframes — measured over the 99 shipped sessions, the 1449 audio rows
// that write freq_r reduce to 379 keyframes and the 1433 that write wave_type
// to 367, and the `# @ave-lane` snapshot is one line in the file.
//
// Rows that write NOTHING are recorded instead as `hold` times, the same way a
// `-` is on a cell sublane: run-length encoding alone cannot tell "absent" from
// "the run continues", and absent is a different instruction to the engine (see
// steppedStateAt). Only absences AFTER the first write need recording — before
// it there is no sublane value to leak backwards — which is what keeps the cost
// to 110 freq_r and 104 wave_type hold times across all 99 sessions.
function buildSteppedSublane(rows, makeKey, isWritten) {
    const keys = [];
    const hold = [];
    let prev = null;
    for (const r of rows) {
        if (isWritten && !isWritten(r)) {
            if (keys.length) hold.push(r.time);
            continue;
        }
        const kf = makeKey(r);
        // Only a run of the same value AND the same shape is redundant: `250`
        // after `>250` restates the value but cancels the glide.
        if (prev && approxEqual(prev.v, kf.v) && prev.shape === kf.shape) continue;
        keys.push(kf);
        prev = kf;
    }
    if (!keys.length) return null;
    const s = { keys };
    if (hold.length) s.hold = hold;
    return s;
}

// freq_r is a compound cell in format v3, so its keyframe comes from the parsed
// cell when the token carried a prefix (parse.js keeps `freqRCell` only then)
// and from the plain number otherwise.
function freqRKeyframe(r) {
    if (r.freqRCell) return cellToKeyframe(r.time, r.freqRCell);
    return keyframe(r.time, r.freqR || 0, 'step');
}

// Capture the v2 pulse fields of each row into a time-keyed sidecar. Only stores
// keys that are present (null = `-` is kept; undefined = omit is dropped so JSON
// round-trips it back to undefined). Returns undefined when nothing is present.
function capturePulse(grp, fieldNames) {
    const pulse = {};
    for (const r of grp) {
        const rec = {};
        for (const f of fieldNames) if (r[f] !== undefined) rec[f] = r[f];
        if (Object.keys(rec).length) pulse[r.time] = rec;
    }
    return Object.keys(pulse).length ? pulse : undefined;
}

/*
 * Split one channel/mask group into lanes that can each hold it.
 *
 * A lane cannot hold two rows at one timestamp — laneUnionTimes is a SET of
 * times and emitCellAt resolves one value per time (the same limit
 * snapObstacle refuses to walk into). The file may legally contain two, so slot
 * n gets the (n+1)-th row at each timestamp: slot 0 is the ordinary lane, and a
 * further slot is an OVERLAP lane carrying the rows that used to be collapsed
 * away. Returns [rows[]] — one entry for a group with no overlap, so the
 * ordinary case is untouched.
 *
 * MERGING THE ROWS INSTEAD IS NOT AVAILABLE, even field by field. The row COUNT
 * is itself load-bearing: find_next_audio_for_bit returns the FIRST later
 * same-channel entry (config_parser.c:2471) as a sweep's target, and the
 * parse-time insertion sort breaks ties with `<=` to keep file order
 * (config_parser.c:572), so deleting the earlier of two rows at one timestamp
 * re-points every sweep that aimed at it. Measured on
 * sessions/library/test2a.ledc: the `A 20000 294 0 50 >4 4` mod ramp aims at
 * `A 30000 - - - 12 4` (mod 12); with that row collapsed into
 * `A 30000 440 -100 45 0 4` the target became mod 0, turning a 10 s 4->12 Hz
 * AM ramp into 4->0 Hz — and the collapse also materialised that row's three
 * `-` tokens into 440 / -100 / 45. The file went from 51 `-` tokens to 48.
 *
 * Splitting keeps both rows, in file order, and lanesToDoc orders its output by
 * (time, lane index) — so with the overlap lane sitting immediately after its
 * primary, the two rows come back out at one timestamp in the order they went
 * in, which is the order the firmware preserves.
 */
function splitOverlappingTimes(grp) {
    const slots = [];
    const seen = new Map();
    for (const r of grp) {
        const n = seen.get(r.time) || 0;
        seen.set(r.time, n + 1);
        if (!slots[n]) slots[n] = [];
        slots[n].push(r);
    }
    return slots;
}

// Name for slot > 0, so the view does not show two identically-named lanes.
function overlapName(base, slot) { return base + ' · overlap ' + (slot + 1); }

// Reconstruct lanes from a flat doc with no metadata (best-effort structure).
export function reconstructLanes(doc) {
    const rows = (doc && doc.rows) ? doc.rows : [];
    const audioGroups = new Map(); // channel -> rows[]
    const ledGroups = new Map();   // mask -> rows[]
    for (const r of rows) {
        if (r.kind === 'audio') {
            const ch = (r.channel === null || r.channel === undefined) ? 0 : r.channel;
            if (!audioGroups.has(ch)) audioGroups.set(ch, []);
            audioGroups.get(ch).push(r);
        } else if (r.kind === 'led') {
            if (!ledGroups.has(r.mask)) ledGroups.set(r.mask, []);
            ledGroups.get(r.mask).push(r);
        }
    }

    const lanes = [];
    const sortByTime = (a, b) => a.time - b.time;

    Array.from(audioGroups.keys()).sort((a, b) => a - b).forEach(ch => {
        // Array.prototype.sort is stable (ES2019), so rows that share a
        // timestamp keep file order — which splitOverlappingTimes relies on.
        const whole = audioGroups.get(ch).slice().sort(sortByTime);
        splitOverlappingTimes(whole).forEach((grp, slot) => {
            const sub = {};
            for (const f of AUDIO_CELL_FIELDS) sub[f] = buildCellSublane(grp, f);
            // Kept whenever ANY row wrote the token, even `0`: an explicit 0 is
            // a mono collapse, not "no freq_r" (config_parser.c:1834 sets the
            // present bit either way), so gating on a positive value deleted
            // the `0` that ends a binaural beat.
            const freqR = buildSteppedSublane(grp, freqRKeyframe,
                                              r => r.freqRPresent !== false);
            if (freqR) sub.freqR = freqR;
            const wave = buildSteppedSublane(grp,
                r => keyframe(r.time, r.waveType, 'step'),
                r => r.waveType !== null && r.waveType !== undefined);
            if (wave) sub.wave = wave;
            lanes.push({
                kind: 'audio', key: ch,
                name: slot ? overlapName('Audio ch ' + ch, slot) : 'Audio ch ' + ch,
                collapsed: false, sub, pulse: capturePulse(grp, AUDIO_PULSE_FIELDS),
            });
        });
    });

    Array.from(ledGroups.keys()).sort((a, b) => a - b).forEach(mask => {
        const whole = ledGroups.get(mask).slice().sort(sortByTime);
        splitOverlappingTimes(whole).forEach((grp, slot) => {
            const sub = {};
            for (const f of LED_CELL_FIELDS) sub[f] = buildCellSublane(grp, f);
            // Color is "split" if any of R/G/B carries an interp; else combined.
            const split = ['r', 'g', 'b'].some(f =>
                sub[f].keys.some(k => k.shape !== 'step'));
            lanes.push({
                kind: 'led', key: mask,
                name: slot ? overlapName(maskName(mask), slot) : maskName(mask),
                collapsed: false, colorSplit: split, sub,
                pulse: capturePulse(grp, LED_PULSE_FIELDS),
            });
        });
    });

    const out = { version: LANE_VERSION, lanes };
    if (doc && doc.bg) out.bg = { url: doc.bg.url, pan: doc.bg.pan, loudness: doc.bg.loudness };
    return out;
}

function maskName(mask) {
    if (mask === 0xFF) return 'LED all (0xFF)';
    if (mask === 0x0F) return 'LED legacy 4 (0x0F)';
    const bits = [];
    for (let i = 0; i < NUM_LED_CHANNELS; i++) if (mask & (1 << i)) bits.push(i + 1);
    return 'LED ch ' + bits.join('+');
}

// docToLanes: prefer metadata (exact restore), else structural reconstruction.
export function docToLanes(doc) {
    const meta = findLaneMeta(doc);
    let lanes;
    if (meta) {
        if (meta.version === undefined) meta.version = LANE_VERSION;
        lanes = meta;
    } else {
        lanes = reconstructLanes(doc);
    }

    // Passthrough is ALWAYS recomputed from the live doc, never read back from
    // the metadata comment. The metadata is a snapshot from whenever the lane
    // view last committed; the Text view may have added, edited or deleted
    // speech/marker/comment rows since. Trusting the snapshot would resurrect
    // rows the user deleted and drop rows they added.
    //
    // No view's metadata comment is carried — not our own (lanesToDoc
    // regenerates it) and not the wizard's (a stale snapshot would nest and
    // grow without bound, and would revert this view's edits on a switch).
    // One rule for both: adopt a BG row the snapshot missed, then recompute
    // the carried set from the live doc.
    syncPassthrough(lanes, doc);
    return lanes;
}

// ---- Interior-LFO snap (the guided fix offered on conflict) ----------------

// LFO bands of one lane: each periodic keyframe owns the span from itself to
// the next keyframe of the SAME field (its outgoing segment — see the
// animate-on-start note at the top). The last one has no later keyframe to end
// it, so its end is Infinity: an open band with only one real boundary.
function lfoBands(sub) {
    const bands = [];
    for (const f in sub) {
        const keys = sortedKeys(sub[f].keys);
        for (let i = 0; i < keys.length; i++) {
            if (isPeriodicShape(keys[i].shape)) {
                bands.push({
                    field: f,
                    start: keys[i].t,
                    end: (i + 1 < keys.length) ? keys[i + 1].t : Infinity,
                });
            }
        }
    }
    return bands;
}

// The boundary of `band` nearer to time `t`. An open band (end == Infinity) has
// only its start: there is no later keyframe whose time would be the real end
// of the oscillation.
//
// Only the NEARER boundary is ever a destination. Snapping is defined as the
// minimal correction that lifts a keyframe out of the band's interior; pushing
// it to the FAR end instead carries the author's event across every remaining
// cycle of the oscillation, which is a different session rather than a fix. A
// band running 0..600000 ms would otherwise relocate a keyframe the author put
// at 1500 ms to t=600000 the moment t=0 turned out to be occupied — a ten
// minute jump from one click. When the nearer boundary is blocked the move is
// reported instead (see snapToBandBoundaries).
function nearerBoundary(band, t) {
    if (!Number.isFinite(band.end)) return band.start;
    // Ties (t exactly mid-band) resolve to the start, so the choice is
    // deterministic and independent of sublane iteration order.
    return (band.end - t < t - band.start) ? band.end : band.start;
}

/*
 * Snap foreign keyframes that fall strictly inside an LFO band onto a band
 * boundary — the one-click fix offered next to the interior-LFO warning.
 *
 * It REFUSES to move a keyframe whose destination is not free, because the
 * projection cannot represent two keyframes of one sublane at one timestamp:
 * lanesToDoc emits a single row per (lane, time) and emitCellAt takes the LAST
 * keyframe at that time, so the other one would just vanish from the file. Two
 * cases bit us:
 *   (a) two keyframes inside one band both pick the same boundary and collapse
 *       onto each other;
 *   (b) a band that starts at 0 and spans most of the session has t=0 as the
 *       nearest boundary for nearly every interior point, so "nearest" drags
 *       keyframes onto the session's opening row and overwrites what it set.
 * Both silently destroyed an entry the author wrote. Now the move is skipped
 * and reported instead, and a conflict left standing is still visible in the
 * warning strip (lanesToDoc recomputes it on the next commit).
 *
 * The same band length that caused (b) also bounds how far a keyframe may
 * travel: only a band's nearer boundary is a destination (see nearerBoundary),
 * so a blocked boundary means "report", never "fling it to the other end".
 *
 * Returns { lanes, snapped, skipped, warnings }:
 *   lanes     the new lanes object (the input is never mutated)
 *   snapped   [{ laneKey, field, from, to }] — moves actually applied
 *   skipped   [{ laneKey, field, t, reason }] — conflicts deliberately left
 *   warnings  ready-to-display lines describing what could not be snapped
 */
export function snapToBandBoundaries(lanes) {
    const clone = JSON.parse(JSON.stringify(lanes));
    const snapped = [];
    const skipped = [];
    for (const lane of (clone.lanes || [])) {
        const sub = lane.sub || {};
        let bands = lfoBands(sub);
        if (!bands.length) continue;
        for (const f in sub) {
            const keys = sub[f].keys || [];
            // Snapshot the keyframe objects first: a keyframe must be
            // considered exactly once even as earlier moves re-order the lane.
            for (const k of keys.slice()) {
                // Bands of this field cannot contain another of its own
                // keyframes (a band ends AT the next one), so only foreign
                // bands can trap k.
                const inside = bands.filter(b => b.field !== f &&
                                                 k.t > b.start && k.t < b.end);
                if (!inside.length) continue;
                // One candidate destination per trapping band: that band's
                // nearer boundary (see nearerBoundary). Several bands can trap
                // the same keyframe, and the nearer boundary of one may sit
                // inside another, so collect them all and try them nearest
                // first — snapObstacle rejects the ones that only move the
                // conflict somewhere else.
                const cands = inside.map(b => nearerBoundary(b, k.t));
                const uniq = Array.from(new Set(cands));
                uniq.sort((a, b) => Math.abs(a - k.t) - Math.abs(b - k.t) || a - b);
                let reason = null, to = null;
                for (const cand of uniq) {
                    reason = snapObstacle(sub[f], bands, f, k, cand);
                    if (!reason) { to = cand; break; }
                }
                if (to === null) {
                    skipped.push({ laneKey: lane.key, field: f, t: k.t, reason });
                    continue;
                }
                snapped.push({ laneKey: lane.key, field: f, from: k.t, to });
                k.t = to;
                // A periodic keyframe that just moved redefines its own band,
                // so the remaining decisions are made against the new geometry.
                bands = lfoBands(sub);
            }
            sub[f].keys = sortedKeys(keys);
        }
    }
    return { lanes: clone, snapped, skipped, warnings: snapWarnings(snapped, skipped) };
}

// Why keyframe `k` of sublane `s` may not move to time `cand`, or null when the
// move is safe. Every answer here is a case where snapping would lose or
// silently rewrite something the author wrote.
function snapObstacle(s, bands, field, k, cand) {
    const keys = s.keys || [];
    for (const o of keys) {
        // Another keyframe already holds that timestamp: one of the two would
        // be dropped by the projection (lanesToDoc emits one row per time).
        if (o !== k && o.t === cand) return 'occupied';
    }
    // Reordering is the same loss one step removed: moving k past a neighbour
    // changes which keyframe owns which outgoing segment.
    for (const o of keys) {
        if (o === k) continue;
        if (o.t < k.t && cand < o.t) return 'reorder';
        if (o.t > k.t && cand > o.t) return 'reorder';
    }
    // A `-` at the destination is an explicit "leave unchanged" (see isHeldAt).
    // Landing a keyframe there would replace it with a number — exactly the
    // materialisation the sentinel exists to prevent.
    if ((s.hold || []).indexOf(cand) >= 0) return 'held';
    // Finally, the destination must not be interior to another band, or we
    // would merely move the conflict instead of resolving it.
    for (const b of bands) {
        if (b.field === field) continue;
        if (cand > b.start && cand < b.end) return 'nested';
    }
    return null;
}

// At most a few lines: the warning strip is one row of text, not a log.
const SNAP_WARN_MAX = 3;
function snapWarnings(snapped, skipped) {
    const warnings = [];
    if (!skipped.length) return warnings;
    const why = {
        occupied: 'another keyframe already sits on the boundary',
        reorder: 'it would have to jump over a neighbouring keyframe',
        held: 'the boundary row leaves that field unchanged (`-`)',
        nested: 'both boundaries fall inside another LFO band',
    };
    warnings.push(skipped.length + ' conflicting keyframe(s) left alone: ' +
        'snapping them would have overwritten another entry. ' +
        (snapped.length ? snapped.length + ' other(s) were snapped. ' : '') +
        'Move them by hand, or widen the LFO band.');
    for (const s of skipped.slice(0, SNAP_WARN_MAX)) {
        warnings.push('• ' + s.field + ' at t=' + s.t + ' ms on lane ' + s.laneKey +
            ': ' + (why[s.reason] || s.reason) + '.');
    }
    if (skipped.length > SNAP_WARN_MAX) {
        warnings.push('• …and ' + (skipped.length - SNAP_WARN_MAX) + ' more.');
    }
    return warnings;
}

// Re-export glyph helpers so the canvas can render prefixes without importing
// model.js separately.
export { GLYPH_TO_INTERP, INTERP_GLYPHS };
