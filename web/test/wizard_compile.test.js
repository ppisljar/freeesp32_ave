// Phase 5 — segment compiler + macros tests.
//
// Covers the plan's acceptance criteria:
//   (a) a 3-segment session compiles to valid rows with correct animate-on-start
//       ramp wiring + session-wide stable channels + terminal off-rows;
//   (b) each macro expands to the documented multi-channel/multi-entry rows;
//   (c) `# @ave-wizard` metadata reopens losslessly;
//   (d) compiler output passes validate.js with no errors and respects MAX_ENTRIES.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { serialize } from '../src/js/gen/serialize.js';
import { parse } from '../src/js/gen/parse.js';
import { validate } from '../src/js/gen/validate.js';
import { MAX_ENTRIES } from '../src/js/gen/model.js';
import { field, ramp, mod } from '../src/js/gen/field.js';
import {
    compileSession, allocateChannels, findWizardMeta, sessionFromDoc,
} from '../src/js/gen/views/wizard_compile.js';
import {
    binaural, stereoDelayedTone, harmonics, fadeIn, fadeOut, NOISE_PRESETS,
} from '../src/js/gen/macros.js';

// ---- helpers ---------------------------------------------------------------
function audioRows(doc) { return doc.rows.filter(r => r.kind === 'audio'); }
function ledRows(doc) { return doc.rows.filter(r => r.kind === 'led'); }
function atTime(rows, t) { return rows.filter(r => r.time === t); }

function toneLayer(id, freqVal, opts) {
    opts = opts || {};
    return {
        id, kind: 'tone', channel: opts.channel, wave_type: opts.wave || null,
        fields: {
            freq: field(freqVal, opts.freqRamp || null),
            pan: field(opts.pan || 0),
            volume: field(opts.volume === undefined ? 60 : opts.volume, opts.volRamp || null),
            mod: field(opts.mod === undefined ? 0 : opts.mod, null, opts.modPulse || null),
        },
    };
}

function lightLayer(id, mask, brightVal) {
    return {
        id, kind: 'light', channelMask: mask,
        fields: {
            freq: field(8), duty: field(50), bright: field(brightVal),
            r: field(0), g: field(0), b: field(255),
        },
    };
}

// ---- (a) 3-segment session -------------------------------------------------

test('(a) 3-segment session: animate-on-start ramps, stable channels, terminal off-rows', () => {
    const session = {
        name: 'Alpha session', version: 1, segments: [
            { id: 's0', name: 'Glide', duration_ms: 10000, layers: [
                toneLayer('lead', 200, { freqRamp: ramp('linear') }),  // glide 200 -> 180
                lightLayer('glow', 0x09, 50),
            ]},
            { id: 's1', name: 'Hold', duration_ms: 10000, layers: [
                toneLayer('lead', 180),
                lightLayer('glow', 0x09, 50),  // identical -> coalesced
            ]},
            { id: 's2', name: 'Wake', duration_ms: 10000, layers: [
                toneLayer('lead', 180, { freqRamp: ramp('linear') }),  // no glow this segment
            ]},
        ],
    };

    const { doc, rowCount } = compileSession(session, { withMeta: false });
    const a = audioRows(doc);
    const l = ledRows(doc);

    // Stable channel: every 'lead' audio row is on the same channel.
    assert.ok(a.length > 0);
    assert.ok(a.every(r => r.channel === 1), 'lead channel stable = 1');
    // LED mask is a real OR'd bitmask, never a counter.
    assert.ok(l.every(r => r.mask === 0x09), 'glow mask stable = 0x09');

    // animate-on-start: seg0 row carries the ramp prefix at the START value 200;
    // the NEXT same-channel row (seg1, t=10000) holds the target 180.
    const lead0 = atTime(a, 0)[0];
    assert.equal(lead0.freq.value, 200);
    assert.equal(lead0.freq.interp, 'lin');

    const lead1 = atTime(a, 10000)[0];
    assert.equal(lead1.freq.value, 180);
    assert.equal(lead1.freq.interp, 'none');

    // Terminal off-row for the audio layer at the session end (t=30000), vol 0.
    const leadEnd = atTime(a, 30000)[0];
    assert.ok(leadEnd, 'terminal off-row at session end');
    assert.equal(leadEnd.vol.value, 0);

    // Terminal off-row for the dropped LED layer at t=20000 (start of seg2), bright 0.
    const glow0 = atTime(l, 0)[0];
    assert.equal(glow0.bright.value, 50);
    const glowOff = atTime(l, 20000)[0];
    assert.ok(glowOff, 'LED terminal off-row when layer drops');
    assert.equal(glowOff.bright.value, 0);

    // The redundant hold (seg1 glow identical to seg0) is coalesced away: only
    // the on-row (t=0) and the off-row (t=20000) remain.
    assert.equal(l.length, 2, 'redundant LED hold coalesced');

    // No row explosion.
    assert.ok(rowCount <= MAX_ENTRIES);
});

// ---- stable allocation across non-contiguous segments ----------------------

test('allocateChannels: same layer id keeps one channel; distinct ids get distinct channels', () => {
    const session = { segments: [
        { layers: [toneLayer('a', 100), toneLayer('b', 200)] },
        { layers: [toneLayer('a', 110), toneLayer('c', 300)] },
    ]};
    const map = allocateChannels(session);
    assert.equal(map.a.channel, 1);
    assert.equal(map.b.channel, 2);
    assert.equal(map.c.channel, 3); // a reused ch1, so c gets the next free
});

// ---- binaural (mono + stereo) ----------------------------------------------

test('binaural mono layer emits one channel with freq_r = base + beat', () => {
    const session = { segments: [{ duration_ms: 5000, layers: [{
        id: 'bb', kind: 'binaural', stereo: false,
        fields: { freq: field(200), beat: field(8), pan: field(0), volume: field(60), mod: field(0) },
    }]}]};
    const { doc } = compileSession(session, { withMeta: false });
    const a = atTime(audioRows(doc), 0);
    assert.equal(a.length, 1);
    assert.equal(a[0].freq.value, 200);
    assert.equal(a[0].freqR, 208);
});

test('binaural stereo layer emits two hard-panned channels (R = base + beat)', () => {
    const session = { segments: [{ duration_ms: 5000, layers: [{
        id: 'bb', kind: 'binaural', stereo: true,
        fields: { freq: field(200), beat: field(12), pan: field(0), volume: field(60), mod: field(0) },
    }]}]};
    const { doc } = compileSession(session, { withMeta: false });
    const a = atTime(audioRows(doc), 0).sort((x, y) => x.channel - y.channel);
    assert.equal(a.length, 2);
    assert.equal(a[0].channel, 1);
    assert.equal(a[0].freq.value, 200);
    assert.equal(a[0].pan.value, -100);
    assert.equal(a[1].channel, 2);
    assert.equal(a[1].freq.value, 212);
    assert.equal(a[1].pan.value, 100);
});

// ---- mods (Phase C) --------------------------------------------------------

test('a field pulse compiles to PREFIXstart:end:period in place', () => {
    const session = { segments: [{ duration_ms: 5000, layers: [
        toneLayer('lead', 200, { modPulse: mod('sine', 8, 2000) }),
    ]}]};
    const { doc } = compileSession(session, { withMeta: false });
    const row = atTime(audioRows(doc), 0)[0];
    assert.equal(row.mod.interp, 'sine');
    assert.equal(row.mod.modEnd, 8);
    assert.equal(row.mod.modPeriodMs, 2000);
    // serialized form carries the explicit end+period.
    const txt = serialize(doc);
    assert.ok(/~0:8:2000/.test(txt), 'serialized mod has explicit end+period: ' + txt);
});

// ---- (b) macros ------------------------------------------------------------

test('(b) binaural macro: mono => freq_r; stereo => two hard-panned rows', () => {
    const monoR = binaural({ base: 200, beat: 10 });
    assert.equal(monoR.length, 1);
    assert.equal(monoR[0].freqR, 210);

    const st = binaural({ base: 200, beat: 10, stereo: true });
    assert.equal(st.length, 2);
    assert.equal(st[0].pan.value, -100);
    assert.equal(st[0].freq.value, 200);
    assert.equal(st[1].pan.value, 100);
    assert.equal(st[1].freq.value, 210);
});

test('(b) stereoDelayedTone: two channels, second offset later', () => {
    const rows = stereoDelayedTone({ freq: 300, delayMs: 25, volume: 70 });
    assert.equal(rows.length, 2);
    assert.equal(rows[0].time, 0);
    assert.equal(rows[0].pan.value, -100);
    assert.equal(rows[1].time, 25);
    assert.equal(rows[1].pan.value, 100);
    assert.ok(rows.every(r => r.freq.value === 300));
    assert.equal(rows[0].channel, 1);
    assert.equal(rows[1].channel, 2);
});

test('(b) harmonics: one channel per harmonic, scaled volume', () => {
    const rows = harmonics({ base: 100, ratios: [1, 2, 1.5], volume: 80, harmonicVolume: 0.5 });
    assert.equal(rows.length, 3);
    assert.deepEqual(rows.map(r => r.freq.value), [100, 200, 150]);
    assert.deepEqual(rows.map(r => r.channel), [1, 2, 3]);
    assert.deepEqual(rows.map(r => r.vol.value), [80, 40, 20]);
});

test('(b) fade in/out: volume ramps on the start row (animate-on-start)', () => {
    const fin = fadeIn({ target: 60, duration: 2000 });
    assert.equal(fin[0].vol.value, 0);
    assert.equal(fin[0].vol.interp, 'lin');
    assert.equal(fin[1].vol.value, 60);
    assert.equal(fin[1].vol.interp, 'none');

    const fout = fadeOut({ from: 60, duration: 2000 });
    assert.equal(fout[0].vol.value, 60);
    assert.equal(fout[0].vol.interp, 'lin');
    assert.equal(fout[1].vol.value, 0);
});

test('(b) noise preset uses wave_type 4/5/6 (not freq=0 sine)', () => {
    const session = { segments: [{ duration_ms: 5000, layers: [{
        id: 'rain', kind: 'noise', wave_type: NOISE_PRESETS.rain.wave,
        fields: { freq: field(0), pan: field(0), volume: field(40), mod: field(0) },
    }]}]};
    const { doc } = compileSession(session, { withMeta: false });
    const row = atTime(audioRows(doc), 0)[0];
    assert.equal(row.waveType, 5); // pink
    assert.equal(row.freq.value, 0);
    const txt = serialize(doc).split('\n').find(l => l.startsWith('A'));
    assert.ok(/\b5$/.test(txt), 'noise serialized with wave_type 5: ' + txt);
});

// ---- (c) round-trip via embedded metadata ----------------------------------

test('(c) # @ave-wizard metadata reopens losslessly', () => {
    const session = {
        name: 'Round trip', version: 1,
        bg: { url: 'sdcard://river.wav', pan: 0, loudness: 30 },
        segments: [
            { id: 's0', name: 'Glide', duration_ms: 10000, layers: [
                toneLayer('lead', 200, { freqRamp: ramp('quadratic') }),
            ]},
            { id: 's1', name: 'Hold', duration_ms: 10000, layers: [toneLayer('lead', 180)] },
        ],
    };
    const { doc } = compileSession(session, { withMeta: true });

    // The metadata comment is present and round-trips through serialize/parse.
    const text = serialize(doc);
    const reparsed = parse(text).doc;
    const meta = findWizardMeta(reparsed);
    assert.ok(meta, 'metadata comment found after serialize/parse');

    const { session: reopened, imported } = sessionFromDoc(reparsed);
    assert.equal(imported, false, 'lossless reopen, not a structural guess');
    // Lossless through the JSON metadata channel (undefined fields are dropped
    // by JSON.stringify; that is the round-trip contract).
    assert.deepEqual(reopened, JSON.parse(JSON.stringify(session)),
        'session restored byte-for-byte');
});

test('structural import (no metadata) yields segments from timestamp clusters', () => {
    const text = [
        'A 0 200 -100 60 0 1',
        'A 10000 180 -100 60 0 1',
    ].join('\n') + '\n';
    const doc = parse(text).doc;
    const { session, imported } = sessionFromDoc(doc);
    assert.equal(imported, true);
    assert.ok(session.segments.length >= 1);
    assert.equal(session.segments[0].layers[0].kind, 'tone');
});

// ---- (d) validation + MAX_ENTRIES ------------------------------------------

test('(d) compiler output passes validate.js with no errors', () => {
    const session = {
        name: 'Valid', version: 1, segments: [
            { id: 's0', duration_ms: 5000, layers: [
                toneLayer('lead', 200, { freqRamp: ramp('linear') }),
                lightLayer('glow', 0xFF, 50),
                { id: 'bb', kind: 'binaural', stereo: true,
                  fields: { freq: field(200), beat: field(10), pan: field(0), volume: field(50), mod: field(0) } },
            ]},
            { id: 's1', duration_ms: 5000, layers: [toneLayer('lead', 180), lightLayer('glow', 0xFF, 50)] },
        ],
    };
    const { doc } = compileSession(session, { withMeta: true });
    const diags = validate(doc);
    const errors = diags.filter(d => d.severity === 'error');
    assert.equal(errors.length, 0, 'no validation errors: ' + JSON.stringify(errors));
});

test('(d) MAX_ENTRIES overflow is warned', () => {
    // 60 segments x 2 distinct tone layers, all changing => well over 100 rows.
    const segments = [];
    for (let i = 0; i < 60; i++) {
        segments.push({ id: 's' + i, duration_ms: 1000, layers: [
            toneLayer('a', 100 + i), toneLayer('b', 200 + i),
        ]});
    }
    const { warnings, rowCount } = compileSession({ segments }, { withMeta: false });
    assert.ok(rowCount > MAX_ENTRIES);
    assert.ok(warnings.some(w => /MAX_ENTRIES/.test(w)), 'MAX_ENTRIES warning present');
});

// ---- partial-window ramp ---------------------------------------------------

test('partial-window ramp inserts an anchor row at start + first_ms', () => {
    const session = { segments: [
        { id: 's0', duration_ms: 10000, layers: [
            toneLayer('lead', 200, { freqRamp: ramp('linear', 3000) }),
        ]},
        { id: 's1', duration_ms: 10000, layers: [toneLayer('lead', 100)] },
    ]};
    const { doc } = compileSession(session, { withMeta: false });
    const a = audioRows(doc);
    // start row at 0 carries the ramp; an anchor at 3000 holds the target 100.
    assert.equal(atTime(a, 0)[0].freq.interp, 'lin');
    const anchor = atTime(a, 3000)[0];
    assert.ok(anchor, 'anchor row at first_ms');
    assert.equal(anchor.freq.value, 100);
    assert.equal(anchor.freq.interp, 'none');
});
