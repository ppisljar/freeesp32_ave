// Bug-regression tests: assert the serializer satisfies every row of the
// Bug-avoidance contract (plan §"Bug-avoidance contract") by construction.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { cell, ledRow, audioRow, bgRow, bg, emptyDoc } from '../src/js/gen/model.js';
import { serialize, cellStr } from '../src/js/gen/serialize.js';
import { parse } from '../src/js/gen/parse.js';

function lineOf(row) {
    return serialize({ rows: [row], bg: null }).trim();
}

// #1 — LED channel is a real OR'd 8-bit mask, never a 1-based counter.
test('bug#1: LED mask is a real bitmask, not a counter', () => {
    const row = ledRow({ time: 0, freq: cell(8), duty: cell(50), bright: cell(30),
                         r: cell(0), g: cell(0), b: cell(255), mask: 0b10010001 }); // 145
    const line = lineOf(row);
    const tokens = line.split(' ');
    assert.equal(tokens[tokens.length - 1], '145', 'mask token is the OR\'d value');
    // Re-parse preserves the mask bits.
    assert.equal(parse(line).doc.rows[0].mask, 145);
});

// #2 — pan emitted ×1 in -100..+100 (never ×127).
test('bug#2: pan is ±100, not ×127', () => {
    const row = audioRow({ time: 0, freq: cell(200), pan: cell(-100), vol: cell(60),
                           mod: cell(0), channel: 1 });
    const line = lineOf(row);
    assert.equal(line, 'A 0 200 -100 60 0 1');
    assert.ok(!line.includes('-12700'), 'pan must not be scaled by 127');
});

// #3 — wave_type emits a 0 placeholder in the freq_r slot when freq_r==0.
test('bug#3: wave_type emits a 0 freq_r placeholder', () => {
    const row = audioRow({ time: 0, freq: cell(200), pan: cell(0), vol: cell(60),
                           mod: cell(0), channel: 1, freqR: 0, waveType: 2 });
    assert.equal(lineOf(row), 'A 0 200 0 60 0 1 0 2');
});

// #4 — freq_r emitted for single-channel binaural.
test('bug#4: freq_r emitted for binaural', () => {
    const row = audioRow({ time: 0, freq: cell(200), pan: cell(-100), vol: cell(60),
                           mod: cell(0), channel: 1, freqR: 208 });
    assert.equal(lineOf(row), 'A 0 200 -100 60 0 1 208');
});

// #5 — LED lines always emit 8-field R G B (unless an untouched legacy import).
test('bug#5: LED always emits 8-field RGB', () => {
    const row = ledRow({ time: 1000, freq: cell(10), duty: cell(50), bright: cell(75),
                         r: cell(12), g: cell(34), b: cell(56), mask: 1 });
    const tokens = lineOf(row).split(' ');
    assert.equal(tokens.length, 8, '8 fields: time freq duty bright R G B mask');
    assert.deepEqual(tokens, ['1000', '10', '50', '75', '12', '34', '56', '1']);
});

test('bug#5: untouched legacy import stays 5-field', () => {
    const doc = parse('0 8 50 30 9\n').doc;
    assert.equal(doc.rows[0].legacy5, true);
    assert.equal(serialize(doc), '0 8 50 30 9\n');
});

// #6 — noise = audio layer with wave_type 4/5/6, no carrier dependency.
test('bug#6: noise serializes as wave_type 4/5/6', () => {
    for (const wt of [4, 5, 6]) {
        const row = audioRow({ time: 0, freq: cell(0), pan: cell(0), vol: cell(20),
                               mod: cell(0), channel: 9, freqR: 0, waveType: wt });
        assert.equal(lineOf(row), 'A 0 0 0 20 0 9 0 ' + wt);
    }
});

// #7 — all 7 prefixes supported; mods emit explicit end + period.
test('bug#7: all 7 interp prefixes render correctly', () => {
    assert.equal(cellStr(cell(12, 'lin')), '>12');
    assert.equal(cellStr(cell(8, 'quad')), '*8');
    assert.equal(cellStr(cell(10, 'tri', 20, 500)), '^10:20:500');
    assert.equal(cellStr(cell(60, 'sine', 90, 100000)), '~60:90:100000');
    assert.equal(cellStr(cell(1, 'sawup', 2, 300)), '/1:2:300');
    assert.equal(cellStr(cell(5, 'sawdn', 1, 200)), '\\5:1:200');
    assert.equal(cellStr(cell(0, 'sq', 255, 1000)), '_0:255:1000');
});

test('bug#7: periodic mod without explicit end/period still emits both', () => {
    // A periodic cell missing modEnd/modPeriodMs must still serialize an
    // explicit start:end:period (defaults end=start, period=1000).
    assert.equal(cellStr({ value: 40, interp: 'sine', modEnd: null, modPeriodMs: null }),
                 '~40:40:1000');
});

// Minimal-trailing-token rules.
test('audio: minimal trailing tokens (drop channel/freq_r/wave when default)', () => {
    const a = audioRow({ time: 0, freq: cell(200), pan: cell(-100), vol: cell(60), mod: cell(0) });
    assert.equal(lineOf(a), 'A 0 200 -100 60 0'); // channel null => omitted
    const b = audioRow({ time: 0, freq: cell(200), pan: cell(-100), vol: cell(60), mod: cell(0), channel: 1 });
    assert.equal(lineOf(b), 'A 0 200 -100 60 0 1'); // freq_r 0 + wave null => omitted
});

// BG line emits raw -100..100 / 0..100 tokens.
test('BG line serializes raw pan/loudness tokens', () => {
    const doc = emptyDoc();
    doc.rows.push(bgRow(bg('https://x/y.wav', -25, 80)));
    doc.bg = doc.rows[0].bg;
    assert.equal(serialize(doc), 'BG https://x/y.wav -25 80\n');
});
