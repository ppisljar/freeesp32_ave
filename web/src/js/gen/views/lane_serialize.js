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

import {
    cell, ledRow, audioRow, commentRow, bgRow,
    GLYPH_TO_INTERP, INTERP_GLYPHS, isModInterp,
    DEFAULT_MOD_PERIOD_MS, MAX_ENTRIES, MAX_BATCH_SIZE,
    NUM_LED_CHANNELS,
} from '../model.js';
import { lerp, quad } from '../interp.js';

// ---- Metadata marker -------------------------------------------------------
export const LANE_META_PREFIX = '@ave-lane v1 ';
export const LANE_VERSION = 1;

// Cell-valued fields per lane kind (each maps to a model Cell on the row).
export const AUDIO_CELL_FIELDS = ['freq', 'pan', 'vol', 'mod'];
export const LED_CELL_FIELDS = ['freq', 'duty', 'bright', 'r', 'g', 'b'];

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

// Hold-style (stepped) value for freqR / wave sublanes at time t.
function steppedAt(keys, t, dflt) {
    if (!keys || !keys.length) return dflt;
    let v = keys[0].v; // hold first value before the first keyframe
    for (const k of keys) { if (k.t <= t) v = k.v; else break; }
    return v;
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
    const rows = [];

    if (withMeta) {
        rows.push(commentRow('# ' + LANE_META_PREFIX + JSON.stringify(lanes)));
    }
    if (lanes && lanes.bg) {
        rows.push(bgRow({ url: lanes.bg.url, pan: lanes.bg.pan, loudness: lanes.bg.loudness }));
    }

    const laneList = (lanes && lanes.lanes) ? lanes.lanes : [];
    const emitted = []; // { time, li, row }

    laneList.forEach((lane, li) => {
        const times = laneUnionTimes(lane);
        const sub = lane.sub || {};
        for (const t of times) {
            if (lane.kind === 'audio') {
                const fields = {};
                for (const f of AUDIO_CELL_FIELDS) {
                    const keys = sub[f] && sub[f].keys;
                    const em = emitCellAt(keys, t);
                    fields[f] = em ? emissionToCell(em) : cell(0);
                    if (em && em.lfoInterior) {
                        conflicts.push({ laneKey: lane.key, kind: 'audio', field: f, t });
                    }
                }
                const freqR = sub.freqR ? steppedAt(sub.freqR.keys, t, 0) : 0;
                const waveType = (sub.wave && sub.wave.keys && sub.wave.keys.length)
                    ? steppedAt(sub.wave.keys, t, null) : null;
                emitted.push({ time: t, li, row: audioRow({
                    time: t, freq: fields.freq, pan: fields.pan, vol: fields.vol,
                    mod: fields.mod, channel: lane.key, freqR: freqR, waveType: waveType,
                }) });
            } else {
                const fields = {};
                for (const f of LED_CELL_FIELDS) {
                    const keys = sub[f] && sub[f].keys;
                    const em = emitCellAt(keys, t);
                    fields[f] = em ? emissionToCell(em) : cell(0);
                    if (em && em.lfoInterior) {
                        conflicts.push({ laneKey: lane.key, kind: 'led', field: f, t });
                    }
                }
                emitted.push({ time: t, li, row: ledRow({
                    time: t, freq: fields.freq, duty: fields.duty, bright: fields.bright,
                    r: fields.r, g: fields.g, b: fields.b, mask: lane.key,
                }) });
            }
        }
    });

    // Stable global ordering: by time, then by lane order (same-time = batch).
    emitted.sort((a, b) => (a.time - b.time) || (a.li - b.li));
    for (const e of emitted) rows.push(e.row);

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
export function coalesceRampKeys(keys) {
    const ks = sortedKeys(keys);
    const out = [];
    for (let i = 0; i < ks.length; i++) {
        const b = ks[i];
        const a = out[out.length - 1];
        const c = ks[i + 1];
        if (a) {
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

// Build a stepped sublane (freqR / wave) from per-row values, dropping
// consecutive duplicates so unchanged fields don't explode into keyframes.
function buildSteppedSublane(rows, getVal, isActive) {
    const keys = [];
    let prev;
    for (const r of rows) {
        if (isActive && !isActive(r)) continue;
        const v = getVal(r);
        if (keys.length && approxEqual(prev, v)) continue;
        keys.push(keyframe(r.time, v, 'step'));
        prev = v;
    }
    return keys.length ? { keys } : null;
}

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
        const grp = audioGroups.get(ch).slice().sort(sortByTime);
        const sub = {};
        for (const f of AUDIO_CELL_FIELDS) {
            const keys = coalesceRampKeys(grp.map(r => cellToKeyframe(r.time, r[f])));
            sub[f] = { keys };
        }
        const freqR = buildSteppedSublane(grp, r => r.freqR || 0);
        if (freqR && freqR.keys.some(k => k.v > 0)) sub.freqR = freqR;
        const wave = buildSteppedSublane(grp,
            r => (r.waveType === null || r.waveType === undefined) ? -1 : r.waveType,
            r => r.waveType !== null && r.waveType !== undefined);
        if (wave) sub.wave = wave;
        lanes.push({
            kind: 'audio', key: ch, name: 'Audio ch ' + ch,
            collapsed: false, sub,
        });
    });

    Array.from(ledGroups.keys()).sort((a, b) => a - b).forEach(mask => {
        const grp = ledGroups.get(mask).slice().sort(sortByTime);
        const sub = {};
        for (const f of LED_CELL_FIELDS) {
            const keys = coalesceRampKeys(grp.map(r => cellToKeyframe(r.time, r[f])));
            sub[f] = { keys };
        }
        // Color is "split" if any of R/G/B carries an interp; else combined.
        const split = ['r', 'g', 'b'].some(f =>
            sub[f].keys.some(k => k.shape !== 'step'));
        lanes.push({
            kind: 'led', key: mask, name: maskName(mask),
            collapsed: false, colorSplit: split, sub,
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
    if (meta) {
        if (meta.version === undefined) meta.version = LANE_VERSION;
        return meta;
    }
    return reconstructLanes(doc);
}

// ---- Interior-LFO snap (the guided fix offered on conflict) ----------------

// Snap any foreign keyframe that falls strictly inside an LFO band to the
// nearest band boundary. Returns a new lanes object (does not mutate input).
export function snapToBandBoundaries(lanes) {
    const clone = JSON.parse(JSON.stringify(lanes));
    for (const lane of clone.lanes) {
        const sub = lane.sub || {};
        // Collect LFO bands across all cell sublanes of this lane.
        const bands = [];
        for (const f in sub) {
            const keys = sortedKeys(sub[f].keys);
            for (let i = 0; i < keys.length; i++) {
                if (isPeriodicShape(keys[i].shape)) {
                    const start = keys[i].t;
                    const end = (i + 1 < keys.length) ? keys[i + 1].t : Infinity;
                    bands.push({ field: f, start, end });
                }
            }
        }
        if (!bands.length) continue;
        for (const f in sub) {
            const keys = sub[f].keys || [];
            for (const k of keys) {
                for (const band of bands) {
                    if (band.field === f) continue; // own band boundary is fine
                    if (k.t > band.start && k.t < band.end) {
                        // Snap to whichever boundary is closer.
                        const toStart = k.t - band.start;
                        const toEnd = band.end - k.t;
                        k.t = (toEnd < toStart && Number.isFinite(band.end))
                            ? band.end : band.start;
                    }
                }
            }
            sub[f].keys = sortedKeys(keys);
        }
    }
    return clone;
}

// Re-export glyph helpers so the canvas can render prefixes without importing
// model.js separately.
export { GLYPH_TO_INTERP, INTERP_GLYPHS };
