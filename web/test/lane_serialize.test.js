// Phase 4 lane-serialization tests — the critical correctness layer.
//
// Covers, per the plan's acceptance criteria:
//   (a) a linear ramp crossing a foreign keyframe round-trips to a piecewise
//       chain that interp.js evaluates IDENTICALLY to the intended straight ramp;
//   (b) flat -> lane import coalesces implicit breakpoints (no row explosion);
//   (c) an LFO region emits a correct `PREFIXstart:end:period_ms` and re-imports
//       to the same band;
//   (d) `# @ave-lane` metadata gives a byte-perfect reopen;
//   (e) a hand-written `.led` with no metadata imports into sane lanes.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';
import {
    lanesToDoc, docToLanes, reconstructLanes, coalesceRampKeys, keyframe,
    findLaneMeta,
} from '../src/js/gen/views/lane_serialize.js';
import { audioStateAtTime } from '../src/js/gen/interp.js';
import { EXAMPLE, EDGE_CASES } from './fixtures.js';

// ---- interp.js adapter -----------------------------------------------------
// interp.js puts the ramp prefix on the entry being ramped TO (the report
// convention), whereas the gen model / firmware put it on the START entry
// (config_parser.c:1995 "animate-on-start"). Shift the prefix forward one
// entry so interp.js evaluates the gen-model rows correctly.
function audioReport(rows, channel) {
    const map = { lin: 'linear', quad: 'quadratic' };
    const seq = rows.filter(r => r.kind === 'audio' && r.channel === channel)
                    .sort((a, b) => a.time - b.time);
    return seq.map((r, i) => {
        const prev = seq[i - 1];
        const mk = (f) => ({
            v: r[f].value,
            interp: (prev && (prev[f].interp === 'lin' || prev[f].interp === 'quad'))
                ? map[prev[f].interp] : 'none',
        });
        return { time: r.time, channel: r.channel, freq: mk('freq'), pan: mk('pan'),
                 vol: mk('vol'), mod: mk('mod') };
    });
}

function audioRowsOnly(doc) { return doc.rows.filter(r => r.kind === 'audio'); }

// ---- (a) ramp across a foreign keyframe ------------------------------------

test('(a) linear ramp crossing a foreign keyframe == the straight ramp under interp.js', () => {
    // freq ramps 100->200 over [0,10000]; a foreign PAN keyframe sits at t=5000.
    const lanes = {
        version: 1,
        lanes: [{
            kind: 'audio', key: 1, name: 'ch1', collapsed: false,
            sub: {
                freq: { keys: [keyframe(0, 100, 'lin'), keyframe(10000, 200, 'step')] },
                pan: { keys: [keyframe(5000, 0, 'step')] },
                vol: { keys: [keyframe(0, 60, 'step')] },
                mod: { keys: [] },
            },
        }],
    };
    const { doc } = lanesToDoc(lanes, { withMeta: false });
    const audio = audioRowsOnly(doc);
    // Union grid => rows at 0, 5000, 10000.
    assert.deepEqual(audio.map(r => r.time), [0, 5000, 10000]);
    // The implicit breakpoint at 5000 carries the interpolated value + the SAME prefix.
    const mid = audio.find(r => r.time === 5000);
    assert.equal(mid.freq.value, 150);
    assert.equal(mid.freq.interp, 'lin');

    // The straight 2-point ramp (intent) and the resampled chain must evaluate
    // identically through interp.js at every probe time.
    const straightRows = [
        { kind: 'audio', channel: 1, time: 0, freq: { value: 100, interp: 'lin' }, pan: { value: 0, interp: 'none' }, vol: { value: 60, interp: 'none' }, mod: { value: 0, interp: 'none' } },
        { kind: 'audio', channel: 1, time: 10000, freq: { value: 200, interp: 'none' }, pan: { value: 0, interp: 'none' }, vol: { value: 60, interp: 'none' }, mod: { value: 0, interp: 'none' } },
    ];
    const straight = audioReport(straightRows, 1);
    const chain = audioReport(doc.rows, 1);
    for (const t of [0, 1000, 2500, 5000, 7500, 9000, 10000]) {
        const a = audioStateAtTime(straight, t, 1).freq;
        const b = audioStateAtTime(chain, t, 1).freq;
        assert.ok(Math.abs(a - b) < 1e-9, 'freq mismatch at t=' + t + ' (' + a + ' vs ' + b + ')');
    }
});

// ---- (b) coalescing reverses implicit breakpoints --------------------------

test('(b) flat -> lane coalesces a collinear breakpoint chain without row explosion', () => {
    // A linear ramp broken into 3 keyframes whose middle lies on the line, plus
    // a constant pan restated at every time. Import must collapse both.
    // Build the doc directly so we control the interp prefixes precisely.
    const d = parse(
        'A 0 >100 -100 60 0 1\n' +
        'A 5000 >150 -100 60 0 1\n' +
        'A 10000 200 -100 60 0 1\n'
    ).doc;
    const lanes = docToLanes(d);
    const lane = lanes.lanes.find(l => l.kind === 'audio' && l.key === 1);
    assert.ok(lane, 'ch1 lane reconstructed');
    // freq: 3 rows -> 2 keyframes (interior 150 on the 100->200 line dropped).
    assert.equal(lane.sub.freq.keys.length, 2, 'collinear breakpoint coalesced');
    assert.deepEqual(lane.sub.freq.keys.map(k => k.v), [100, 200]);
    // pan: constant -100 at all three rows -> a single keyframe.
    assert.equal(lane.sub.pan.keys.length, 1);

    // Re-projecting must not re-explode: union grid is {0, 10000} => 2 rows.
    const { doc: doc2 } = lanesToDoc(lanes, { withMeta: false });
    assert.equal(audioRowsOnly(doc2).length, 2, 'no row explosion on re-serialize');
});

test('(b2) coalesceRampKeys keeps a point that is OFF the line', () => {
    const keys = [keyframe(0, 100, 'lin'), keyframe(5000, 130, 'lin'), keyframe(10000, 200, 'step')];
    // 130 != lerp(100,200,0.5)=150 => the breakpoint must be preserved.
    assert.equal(coalesceRampKeys(keys).length, 3);
});

// ---- (c) LFO region --------------------------------------------------------

test('(c) an LFO region emits PREFIXstart:end:period and re-imports to the same band', () => {
    const lanes = {
        version: 1,
        lanes: [{
            kind: 'audio', key: 1, name: 'ch1', collapsed: false,
            sub: {
                freq: { keys: [keyframe(0, 200, 'step')] },
                pan: { keys: [] },
                vol: { keys: [keyframe(0, 60, 'step')] },
                mod: { keys: [keyframe(0, 10, 'sine', 20, 500), keyframe(10000, 0, 'step')] },
            },
        }],
    };
    const { doc } = lanesToDoc(lanes, { withMeta: false });
    const text = serialize(doc);
    assert.match(text, /~10:20:500/, 'sine mod emitted with explicit end + period');

    // Structural re-import (no metadata) rebuilds the band.
    const reim = docToLanes(parse(text).doc);
    const lane = reim.lanes.find(l => l.kind === 'audio' && l.key === 1);
    const k0 = lane.sub.mod.keys[0];
    assert.equal(k0.shape, 'sine');
    assert.equal(k0.v, 10);
    assert.equal(k0.lfoEnd, 20);
    assert.equal(k0.lfoPeriodMs, 500);
});

test('(c2) interior-LFO conflict is detected when a foreign keyframe lands in a band', () => {
    const lanes = {
        version: 1,
        lanes: [{
            kind: 'audio', key: 1, name: 'ch1', collapsed: false,
            sub: {
                freq: { keys: [keyframe(0, 200, 'step')] },
                pan: { keys: [keyframe(5000, 50, 'step')] },   // foreign, inside the band
                vol: { keys: [keyframe(0, 60, 'step')] },
                mod: { keys: [keyframe(0, 10, 'sine', 20, 500), keyframe(10000, 0, 'step')] },
            },
        }],
    };
    const { conflicts } = lanesToDoc(lanes, { withMeta: false });
    assert.ok(conflicts.length >= 1, 'a conflict is reported');
    assert.equal(conflicts[0].field, 'mod');
    assert.equal(conflicts[0].t, 5000);
});

// ---- (d) byte-perfect metadata reopen --------------------------------------

test('(d) # @ave-lane metadata gives a byte-perfect reopen', () => {
    const lanes = {
        version: 1,
        lanes: [
            {
                kind: 'audio', key: 1, name: 'Left binaural', collapsed: false,
                sub: {
                    freq: { keys: [keyframe(0, 200, 'lin'), keyframe(20000, 240, 'step')] },
                    pan: { keys: [keyframe(0, -100, 'step')] },
                    vol: { keys: [keyframe(0, 0, 'lin'), keyframe(3000, 60, 'step')] },
                    mod: { keys: [keyframe(0, 5, 'sine', 12, 4000)] },
                    freqR: { keys: [keyframe(0, 208, 'step')] },
                    wave: { keys: [keyframe(0, 1, 'step')] },
                },
            },
            {
                kind: 'led', key: 9, name: 'Inner zones', collapsed: true, colorSplit: true,
                sub: {
                    freq: { keys: [keyframe(0, 8, 'step')] },
                    duty: { keys: [keyframe(0, 50, 'step')] },
                    bright: { keys: [keyframe(0, 30, 'quad'), keyframe(10000, 80, 'step')] },
                    r: { keys: [keyframe(0, 0, 'lin'), keyframe(10000, 255, 'step')] },
                    g: { keys: [keyframe(0, 0, 'step')] },
                    b: { keys: [keyframe(0, 255, 'lin'), keyframe(10000, 0, 'step')] },
                },
            },
        ],
    };
    const text1 = serialize(lanesToDoc(lanes).doc);
    assert.ok(findLaneMeta(parse(text1).doc), 'metadata comment present');

    const reopened = docToLanes(parse(text1).doc);
    assert.deepStrictEqual(reopened, lanes, 'metadata restores the exact lane model');

    const text2 = serialize(lanesToDoc(reopened).doc);
    assert.equal(text2, text1, 'reopen is byte-perfect');
});

// ---- (e) hand-written .led with no metadata --------------------------------

test('(e) a hand-written .led (no metadata) imports into sane lanes', () => {
    const doc = parse(EXAMPLE).doc;
    assert.equal(findLaneMeta(doc), null, 'example has no lane metadata');
    const lanes = reconstructLanes(doc);

    const audioLanes = lanes.lanes.filter(l => l.kind === 'audio');
    const ledLanes = lanes.lanes.filter(l => l.kind === 'led');
    assert.deepEqual(audioLanes.map(l => l.key), [1, 2], 'audio channels 1 and 2');
    assert.deepEqual(ledLanes.map(l => l.key).sort((a, b) => a - b), [9, 15], 'LED masks 9 and 15');

    // ch1 carries a volume fade (>0) at the end => a lin keyframe survives.
    const ch1 = audioLanes.find(l => l.key === 1);
    assert.ok(ch1.sub.freq.keys.length >= 1);
    assert.ok(ch1.sub.vol.keys.some(k => k.shape === 'lin'), 'ch1 volume fade preserved');

    // The blue->white LED lane has color ramps => colorSplit inferred.
    const led9 = ledLanes.find(l => l.key === 9);
    assert.equal(led9.colorSplit, true);
});

test('(e2) freq_r => binaural ghost, wave_type => markers on structural import', () => {
    const doc = parse(EDGE_CASES.binaural_wave).doc; // A 0 200 -100 60 0 1 208 1
    const lanes = reconstructLanes(doc);
    const lane = lanes.lanes.find(l => l.kind === 'audio' && l.key === 1);
    assert.ok(lane.sub.freqR, 'freq_r ghost sublane present');
    assert.equal(lane.sub.freqR.keys[0].v, 208);
    assert.ok(lane.sub.wave, 'wave marker sublane present');
    assert.equal(lane.sub.wave.keys[0].v, 1);
});

// ---- bug-avoidance: LED lane still emits a real OR'd 8-field mask -----------

test('lane LED projection emits an 8-field line with the real mask (bug #1/#5)', () => {
    const lanes = {
        version: 1,
        lanes: [{
            kind: 'led', key: 9, name: 'm9', collapsed: false, colorSplit: false,
            sub: {
                freq: { keys: [keyframe(0, 8, 'step')] },
                duty: { keys: [keyframe(0, 50, 'step')] },
                bright: { keys: [keyframe(0, 30, 'step')] },
                r: { keys: [keyframe(0, 0, 'step')] },
                g: { keys: [keyframe(0, 0, 'step')] },
                b: { keys: [keyframe(0, 255, 'step')] },
            },
        }],
    };
    const text = serialize(lanesToDoc(lanes, { withMeta: false }).doc);
    assert.match(text, /^0 8 50 30 0 0 255 9$/m, '8-field LED line with mask 9');
});
