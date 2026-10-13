// Timeline resolution — the one place a `.ledc` doc becomes per-channel state.
//
// Everything downstream (the audio renderer, the LED visualiser, the timeline
// inspector) consumes the output of this module rather than walking doc.rows,
// because walking doc.rows directly gets three things wrong, each of which is
// silent:
//
//   1. THE `-` SENTINEL. A `-` in any field means "leave unchanged", so the
//      effective value at an entry is carried from the previous entry ON THE
//      SAME CHANNEL, starting from the firmware's channel default. An entry
//      list that hasn't been carried renders a session built out of `-` rows
//      completely differently from the device.
//
//   2. LED MASKS. One LED row drives every channel in its `mask` bitfield, and
//      each of those channels has its own independent previous-entry and
//      next-entry. config_parser.c:3151 walks the bits separately for exactly
//      this reason ("bits diverge across N next-entries — splitting into
//      per-group sweep installs"), so a mask of 0x03 is two timelines, not one.
//
//   3. RAMP TARGETS. The `>`/`*` prefix lives on the START row and targets the
//      NEXT entry's value (config_parser.c:2806, "NEW CONVENTION
//      (animate-on-start)"). Two subtleties that are easy to get backwards:
//
//        - CORE fields (audio freq/pan/vol/mod, LED freq/duty/bright/R/G/B)
//          target the next entry on the channel, present-bit or not. Because
//          the parser memsets the entry struct (config_parser.c:1591, :1781),
//          a next entry that writes `-` for that field has a ZERO there — so
//          `>` into a `-` ramps to 0, it does not hold. That is the device's
//          real behaviour, warts and all, and `rampsIntoDash` below reports it
//          so the UI can warn rather than the simulator quietly inventing a
//          nicer rule.
//
//        - PULSE fields (audio duty/env/phase/attack/jitter/depth, LED
//          env/phase/attack/jitter) target the next entry that ACTUALLY SETS
//          the field (config_parser.c:2445 find_next_audio_with, :3233 the
//          `next_bit->present & LED_SET_PHASE` guard). Here an omitted field
//          really does mean "hold".
//
// Pure: no DOM, no network, no Web Audio. Unit-tested in web/test/sim_resolve.test.js.

import { NUM_LED_CHANNELS, cell } from '../model.js';

// ---- Firmware channel defaults ---------------------------------------------
// What a `-` resolves to when it appears at a channel's FIRST entry (there is
// no previous entry to carry from, so the channel's reset state is the answer).

// led_matrix_example.c:160 — the flicker_state[] initialiser, plus the
// module-wide carrier/attack/jitter defaults latched at flicker start (:198).
export const LED_DEFAULTS = {
    freq: 0,        // Hz — 0 means DC (constant on, no flicker gate)
    duty: 50,       // %
    bright: 100,    // %
    r: 255, g: 255, b: 255,
    env: 0,         // LED_CARRIER_SQUARE
    phase: 0,       // degrees
    attack: 3,      // ms — s_flicker_attack_ms
    jitter: null,   // s_flicker_jitter_millihz = 0 (off)
};

// audio_generator.c:143 — s_iso_env / s_iso_duty / s_iso_attack_ms, and
// config_parser.c:2669 `.mod_depth = 0.1f` (hardcoded; the `depth` column
// overrides it when present).
export const AUDIO_DEFAULTS = {
    freq: 0,        // Hz
    pan: 0,         // -100..100
    vol: 0,         // 0..100
    mod: 0,         // Hz — 0 disables the envelope entirely
    freqR: 0,       // Hz, ABSOLUTE right-ear carrier; 0 = mono
    wave: 0,        // sine
    duty: 50,       // % — s_iso_duty 0.5
    env: 4,         // ISO_ENV_TREMOLO (the legacy bipolar sine tremolo)
    phase: 0,       // degrees
    attack: 3,      // ms — s_iso_attack_ms
    jitter: null,   // off
    depth: null,    // null = "no depth column" → params.mod_depth (0.1)
};

// Which fields hold when a later entry omits them, vs. which ramp to a zero.
// Order matters only for display.
export const AUDIO_CORE_FIELDS  = ['freq', 'pan', 'vol', 'mod'];
export const AUDIO_PULSE_FIELDS = ['duty', 'env', 'phase', 'attack', 'jitter', 'depth'];
export const LED_CORE_FIELDS    = ['freq', 'duty', 'bright', 'r', 'g', 'b'];
export const LED_PULSE_FIELDS   = ['env', 'phase', 'attack', 'jitter'];

// ---- Compound-cell evaluation ----------------------------------------------

const TWO_PI = Math.PI * 2;

// Piecewise quadratic ease-in-out. Shared by both engines verbatim:
// audio_generator.c interpolate_sweep and led_matrix_example.c:556 led_quad_q16
// ("Identical to the audio generator's piecewise quadratic ease-in-out").
export function easeQuad(p) {
    return p < 0.5 ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p);
}

// Periodic modulation shape, 0..1, from a 0..1 phase. Mirrors the wave switch in
// led_matrix_example.c:685 led_eval_mod_iram (and the same five shapes on the
// audio side). `sine` is a TRUE raised sine — the firmware replaced a
// 4·p·(1−p) parabola here because the parabola's derivative discontinuity put a
// 3rd harmonic at a quarter of the fundamental (measured h2=0.286, h3=0.145),
// which inside an entrainment session is not a cosmetic difference.
export function modShape(kind, phase01) {
    switch (kind) {
        case 'tri':   return phase01 < 0.5 ? 2 * phase01 : 2 - 2 * phase01;
        case 'sine':  return 0.5 - 0.5 * Math.cos(TWO_PI * phase01);
        case 'sawup': return phase01;
        case 'sawdn': return 1 - phase01;
        case 'sq':    return phase01 < 0.5 ? 0 : 1;
        default:      return 0;
    }
}

// Evaluate one compound cell at time tMs.
//
//   c       the cell carrying the prefix (the START row — animate-on-start)
//   target  the ramp target VALUE (a number), or null for "no target → hold"
//   thisMs  the start row's timestamp
//   nextMs  the target row's timestamp, or null
//
// A ramp with no target holds at its start value, which is what the device does
// when there is no next entry on the channel (config_parser.c:2800 `if (next !=
// NULL)` — no next, no sweep installed, so the step value simply stands).
export function evalCell(c, target, thisMs, nextMs, tMs) {
    if (!c) return 0;
    const base = c.value || 0;
    const kind = c.interp || 'none';

    if (kind === 'lin' || kind === 'quad') {
        if (target === null || target === undefined || nextMs === null ||
            nextMs === undefined || nextMs <= thisMs) {
            return base;
        }
        let p = (tMs - thisMs) / (nextMs - thisMs);
        if (p < 0) p = 0; else if (p > 1) p = 1;
        if (kind === 'quad') p = easeQuad(p);
        return base + (target - base) * p;
    }

    if (kind === 'tri' || kind === 'sine' || kind === 'sawup' ||
        kind === 'sawdn' || kind === 'sq') {
        // Self-contained oscillation. The phase origin is the entry's own
        // timestamp: both engines stamp start_time_us when the mod slot is
        // installed, i.e. at dispatch (led_matrix_example.c:700 reads
        // `now_us - m->start_time_us`). period 0 would divide by zero — the
        // firmware returns start_q in that case.
        const period = c.modPeriodMs;
        if (!period) return base;
        const end = (c.modEnd === null || c.modEnd === undefined) ? base : c.modEnd;
        let ph = ((tMs - thisMs) % period) / period;
        if (ph < 0) ph += 1;
        return base + (end - base) * modShape(kind, ph);
    }

    return base;
}

// ---- Field plumbing --------------------------------------------------------

// A raw row field is "set" when it is neither absent (undefined) nor the `-`
// sentinel (null). Both mean "leave unchanged" to the engine — the distinction
// only matters to the serializer — so both carry forward here.
function isSet(v) { return v !== undefined && v !== null; }

// Pull a comparable NUMBER out of whatever a field holds: a compound cell, a
// bare number (env, freqR), or a jitter object.
function valueOf(v) {
    if (v === null || v === undefined) return null;
    if (typeof v === 'number') return v;
    if (typeof v === 'object' && 'value' in v) return v.value;
    return null;
}

// Wrap a plain number into a step cell so every field has one shape downstream.
function asCell(v) {
    if (v === null || v === undefined) return null;
    if (typeof v === 'object' && 'value' in v) return v;
    if (typeof v === 'number') return cell(v);
    return null;
}

// Does this cell carry a `>` / `*` ramp (as opposed to a step or an LFO)?
function isRamp(c) {
    const k = c && c.interp;
    return k === 'lin' || k === 'quad';
}

/*
 * Annotate a sorted, carried entry list with per-field ramp targets.
 *
 * Each entry gains `.t = { <field>: { target, targetMs } }`. `target` is the
 * number evalCell should ramp toward, or null to hold.
 *
 * coreFields use the next entry unconditionally and read its RAW value, so an
 * unset field contributes 0 — the memset behaviour described in the header.
 * pulseFields scan forward for the next entry that actually sets the field.
 */
function annotateTargets(entries, coreFields, pulseFields) {
    for (let i = 0; i < entries.length; i++) {
        const e = entries[i];
        const next = entries[i + 1] || null;
        e.t = {};
        e.rampsIntoDash = [];

        for (const f of coreFields) {
            if (!next) { e.t[f] = { target: null, targetMs: null }; continue; }
            // The device reads next->field out of a memset struct, so a `-`
            // there is a hard zero rather than the carried value.
            const raw = next.raw[f];
            const target = isSet(raw) ? valueOf(raw) : 0;
            e.t[f] = { target, targetMs: next.time };
            if (!isSet(raw) && isRamp(e.cells[f])) e.rampsIntoDash.push(f);
        }

        for (const f of pulseFields) {
            let target = null, targetMs = null;
            for (let j = i + 1; j < entries.length; j++) {
                if (isSet(entries[j].raw[f])) {
                    target = valueOf(entries[j].raw[f]);
                    targetMs = entries[j].time;
                    break;
                }
            }
            e.t[f] = { target, targetMs };
        }
    }
    return entries;
}

// Build the carried entry list for one channel from its raw rows.
//   rawRows   [{ time, raw: {field: value|null|undefined} }] sorted by time
//   defaults  the firmware channel defaults
//   fields    every field name to carry
function carryForward(rawRows, defaults, fields) {
    const cur = {};
    for (const f of fields) cur[f] = asCell(defaults[f]);
    const out = [];
    for (const r of rawRows) {
        const cells = {};
        for (const f of fields) {
            if (isSet(r.raw[f])) cur[f] = asCell(r.raw[f]);
            cells[f] = cur[f];
        }
        // `row` is the source row object, kept so a finding can name the line it
        // came from and the editor can scroll to it. jitter is an object
        // ({amp, period}) rather than a cell, so it rides raw alongside the
        // cells instead of being wrapped in a fake one.
        out.push({
            time: r.time,
            row: r.row,
            raw: r.raw,
            cells,
            jitterObj: r.raw.jitter !== undefined
                ? r.raw.jitter
                : (out.length ? out[out.length - 1].jitterObj : defaults.jitter),
        });
    }
    return out;
}

// Stable sort by time. A plain .sort() is already stable in every engine we
// target, but same-timestamp ordering decides which entry wins for a batch, so
// say so explicitly rather than relying on it by accident.
function byTime(rows) {
    return rows.map((r, i) => ({ r, i }))
               .sort((a, b) => (a.r.time - b.r.time) || (a.i - b.i))
               .map(x => x.r);
}

// ---- Audio -----------------------------------------------------------------

/*
 * Group audio rows into per-channel resolved entry lists.
 *
 * Returns { channels: Map<channelIndex, entries[]>, maxTimeMs }.
 *
 * CHANNEL DEFAULT IS 0, NOT 1. config_parser.c:1816 reads
 * `int req = (token_count >= 6 && !tok_is_dash(tokens[5])) ? atoi(tokens[5]) : 0`
 * and clamps out-of-range to 0 with a warning. gen/synth.js's
 * extractAudioChannels used `(row.channel == null) ? 1 : row.channel`, which
 * put channel-less `A` lines in a different bucket from the device's — so a
 * session mixing bare `A` lines with explicit `... 0` lines was grouped one way
 * in the browser and another on the hardware.
 */
export function resolveAudioChannels(doc) {
    const buckets = new Map();
    let maxTimeMs = 0;

    for (const row of (doc && doc.rows) || []) {
        if (row.kind !== 'audio') continue;
        let ch = (row.channel === null || row.channel === undefined) ? 0 : row.channel;
        if (!(ch >= 0 && ch < 16)) ch = 0;          // same clamp as the parser
        const time = row.time || 0;
        if (time > maxTimeMs) maxTimeMs = time;

        // freqR rides as a compound cell when the line carried a prefix
        // (parse.js keeps `freqRCell` only then), otherwise as a plain number.
        //
        // An ABSENT or `-` freq_r token must carry the previous entry's value,
        // not read as 0: the device leaves the right carrier alone when
        // AUD_SET_FREQR is clear, so a later row that moves `freq` and omits
        // `freqR` stays binaural. parse.js flags that with freqRPresent:false;
        // a row built by an editor leaves it undefined and is treated as
        // present, so its explicit 0 still means mono.
        const freqRRaw = (row.freqRPresent === false)
            ? undefined
            : ((row.freqRCell !== undefined) ? row.freqRCell : row.freqR);

        if (!buckets.has(ch)) buckets.set(ch, []);
        buckets.get(ch).push({
            time,
            row,
            raw: {
                freq: row.freq, pan: row.pan, vol: row.vol, mod: row.mod,
                freqR: freqRRaw,
                wave: row.waveType,
                duty: row.duty, env: row.env, phase: row.phase,
                attack: row.attack, jitter: row.jitter, depth: row.depth,
            },
        });
    }

    const fields = ['freq', 'pan', 'vol', 'mod', 'freqR', 'wave',
                    'duty', 'env', 'phase', 'attack', 'depth'];
    const channels = new Map();
    for (const [ch, rows] of buckets) {
        const entries = carryForward(byTime(rows), AUDIO_DEFAULTS, fields);
        // freqR sits in the present-aware bucket with the pulse fields, not with
        // the core four: its ramp is installed as an AUDIO_PARAM_FREQ_R sweep
        // that scans forward for the next entry actually setting it, so a
        // `>freqR` glide holds rather than collapsing to 0 when later rows omit
        // the column (which is the normal way a pinned-carrier session is written).
        annotateTargets(entries, ['freq', 'pan', 'vol', 'mod'],
                        ['freqR', 'duty', 'phase', 'attack', 'depth']);
        channels.set(ch, entries);
    }
    return { channels, maxTimeMs };
}

// ---- LED -------------------------------------------------------------------

/*
 * Expand LED rows across their channel masks into per-channel resolved lists.
 *
 * Returns { channels: Map<0..7, entries[]>, maxTimeMs, masked: boolean }.
 *
 * A row with mask 0 is SKIPPED, matching config_parser.c:1693 ("LED line at
 * %u ms has channel_mask=0 — skipping"). parse.js already rejects a literal
 * `0`, but a `-` in the mask column becomes null there while the firmware's
 * plain atoi() yields 0 — so a `-` mask is dropped here too rather than
 * silently applying to a channel the device would never drive.
 */
export function resolveLedChannels(doc) {
    const buckets = new Map();
    let maxTimeMs = 0;

    for (const row of (doc && doc.rows) || []) {
        if (row.kind !== 'led') continue;
        const mask = row.mask;
        if (!mask) continue;                       // 0 / null / undefined → skipped
        const time = row.time || 0;
        if (time > maxTimeMs) maxTimeMs = time;

        for (let bit = 0; bit < NUM_LED_CHANNELS; bit++) {
            if (!(mask & (1 << bit))) continue;
            if (!buckets.has(bit)) buckets.set(bit, []);
            buckets.get(bit).push({
                time,
                row,
                raw: {
                    freq: row.freq, duty: row.duty, bright: row.bright,
                    r: row.r, g: row.g, b: row.b,
                    env: row.env, phase: row.phase,
                    attack: row.attack, jitter: row.jitter,
                },
            });
        }
    }

    const fields = ['freq', 'duty', 'bright', 'r', 'g', 'b', 'env', 'phase', 'attack'];
    const channels = new Map();
    for (const [bit, rows] of buckets) {
        const entries = carryForward(byTime(rows), LED_DEFAULTS, fields);
        // LED pulse fields are present-aware on BOTH ends (config_parser.c:3233);
        // the six core fields are not.
        annotateTargets(entries, ['freq', 'duty', 'bright', 'r', 'g', 'b'],
                        ['phase', 'attack']);
        channels.set(bit, entries);
    }
    return { channels, maxTimeMs };
}

// ---- Lookup ----------------------------------------------------------------

// Index of the entry in force at tMs, or -1 before the channel's first entry.
// Entries sharing a timestamp resolve to the LAST of them, which is what the
// device's batch dispatcher leaves standing.
export function activeIndexAt(entries, tMs) {
    let lo = 0, hi = entries.length - 1, found = -1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (entries[mid].time <= tMs) { found = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return found;
}

/*
 * Resolve every field of one channel at tMs, evaluating against a SPECIFIC
 * entry index.
 *
 * The index is a parameter rather than derived, because "the entry in force"
 * and "the last entry whose timestamp has passed" are not always the same: the
 * LED engine refuses to install an out-of-range row and leaves the channel on
 * its previous state (see sim/led.js), so the caller may need to evaluate an
 * earlier entry than activeIndexAt would pick. The ramp annotations still come
 * from the full list, so a ramp installed by entry i keeps targeting whatever
 * follows it — including a row the device dropped, which is what
 * find_next_led_for_bit does.
 */
export function stateAtIndex(entries, i, tMs, fields) {
    if (i < 0 || i >= entries.length) return null;
    const e = entries[i];
    const out = { _entryIndex: i, _entryTime: e.time, _jitter: e.jitterObj || null };
    for (const f of fields) {
        const c = e.cells[f];
        if (!c) { out[f] = 0; continue; }
        const t = e.t[f] || { target: null, targetMs: null };
        out[f] = evalCell(c, t.target, e.time, t.targetMs, tMs);
    }
    return out;
}

// Resolve every field of one channel at tMs into plain numbers.
// Returns null before the channel starts (nothing is driving it yet).
export function stateAt(entries, tMs, fields) {
    return stateAtIndex(entries, activeIndexAt(entries, tMs), tMs, fields);
}

// Total session length: the last timestamp of anything on the timeline. Speech
// and BG rows carry their own duration, which only the bounce knows about, so
// this is the TIMELINE end rather than the audible end.
export function sessionEndMs(doc) {
    let end = 0;
    for (const row of (doc && doc.rows) || []) {
        if (row.kind === 'led' || row.kind === 'audio' || row.kind === 'speech') {
            if ((row.time || 0) > end) end = row.time || 0;
        }
    }
    return end;
}
