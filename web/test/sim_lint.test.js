// Simulator-only lints — the findings that need a RESOLVED timeline rather than
// just the grammar.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { lintSession } from '../src/js/gen/sim/lint.js';

const lint = (text) => lintSession(parse(text).doc);
const byCode = (text, code) => lint(text).filter(f => f.code === code);

test('a clean session produces no warnings or errors', () => {
    const f = lint(
        '# a plain 10 Hz alpha session\n'
      + '0 10 50 80 255 180 84 1\n'
      + 'A 0 200 0 80 0 1 210\n');
    assert.deepEqual(f.filter(x => x.severity !== 'info'), []);
});

test('RAMP_TO_ZERO: a ramp whose target row writes `-` glides to zero', () => {
    const f = byCode('A 0 >200 0 80 0 1\nA 1000 - 0 80 0 1\n', 'RAMP_TO_ZERO');
    assert.equal(f.length, 1);
    assert.equal(f[0].severity, 'warn');
    assert.equal(f[0].row, 0, 'should point at the row carrying the prefix');
    assert.equal(f[0].timeMs, 0);
    assert.match(f[0].msg, /glides\s+to ZERO/);
});

test('RAMP_TO_ZERO: fires for LED core fields too', () => {
    const f = byCode('0 >10 50 80 255 0 0 1\n1000 - 50 80 255 0 0 1\n', 'RAMP_TO_ZERO');
    assert.equal(f.length, 1);
    assert.match(f[0].msg, /^LED 1/);
});

test('RAMP_TO_ZERO: stays quiet when the target row sets the field', () => {
    assert.deepEqual(byCode('A 0 >200 0 80 0 1\nA 1000 300 0 80 0 1\n', 'RAMP_TO_ZERO'), []);
});

test('LED_ROW_DROPPED: an out-of-range row is an error, not a clamp', () => {
    const f = byCode('0 10 50 80 255 0 0 1\n5000 150 50 80 255 0 0 1\n', 'LED_ROW_DROPPED');
    assert.equal(f.length, 1);
    assert.equal(f[0].severity, 'error');
    assert.equal(f[0].row, 1);
    assert.match(f[0].msg, /REFUSES to install/);
});

test('GLOBAL_JITTER: a single late jitter column still applies from t=0', () => {
    const f = byCode('A 0 200 0 80 0 1 210\n'
                   + 'A 600000 200 0 80 0 1 210 0 - - - - 0.5:10000\n', 'GLOBAL_JITTER');
    assert.equal(f.length, 1);
    assert.match(f[0].msg, /applies from t=0/);
    assert.equal(f[0].timeMs, 600000);
});

test('GLOBAL_JITTER: with several jitter columns, only the last one wins', () => {
    const f = byCode(
        'A 0 200 0 80 0 1 210 0 - - - - 0.2:5000\n'
      + 'A 300000 200 0 80 0 1 210 0 - - - - 1.5:20000\n', 'GLOBAL_JITTER');
    assert.equal(f.length, 1);
    assert.match(f[0].msg, /last one\s+in the file wins/);
    assert.match(f[0].msg, /amp 1\.5/);
    assert.equal(f[0].timeMs, 300000);
});

test('GLOBAL_JITTER: a single jitter column at t=0 is unremarkable', () => {
    assert.deepEqual(
        byCode('A 0 200 0 80 0 1 210 0 - - - - 0.5:10000\n', 'GLOBAL_JITTER'), []);
});

test('DUTY_NO_OP: duty next to env 1 or env 2 has no effect', () => {
    // env 1 (raised sine) and env 2 (triangle) never read duty.
    const sine = byCode('A 0 200 0 80 10 1 0 0 40 1\n', 'DUTY_NO_OP');
    assert.equal(sine.length, 1);
    assert.match(sine[0].msg, /env 1 \(sine\)/);
    const tri = byCode('A 0 200 0 80 10 1 0 0 40 2\n', 'DUTY_NO_OP');
    assert.equal(tri.length, 1);
    assert.match(tri[0].msg, /always exactly 50 % on/);
    // env 0 and env 3 DO read it.
    assert.deepEqual(byCode('A 0 200 0 80 10 1 0 0 40 0\n', 'DUTY_NO_OP'), []);
    assert.deepEqual(byCode('A 0 200 0 80 10 1 0 0 40 3\n', 'DUTY_NO_OP'), []);
});

test('DUTY_NO_OP: the envelope is sticky, so a duty set later still counts', () => {
    // env 1 latched on the first row carries forward to the second.
    const f = byCode('A 0 200 0 80 10 1 0 0 - 1\nA 1000 200 0 80 10 1 0 0 40\n',
                     'DUTY_NO_OP');
    assert.equal(f.length, 1);
    assert.equal(f[0].row, 1);
});

test('LED_ENV_FOLDED: env 4 on a light row folds to square', () => {
    const f = byCode('0 10 50 80 255 0 0 1 4\n', 'LED_ENV_FOLDED');
    assert.equal(f.length, 1);
    assert.match(f[0].msg, /Only the AUDIO path has env 4/);
    assert.deepEqual(byCode('0 10 50 80 255 0 0 1 3\n', 'LED_ENV_FOLDED'), []);
});

test('STRIP_COARSE: a 40 Hz rate outruns the strip, not the flicker timer', () => {
    // The ISR is fine at 40 Hz — it hits its 10 kHz ceiling there and still
    // gets the full 250 ticks per cycle. What cannot keep up is the WIRE: one
    // 74-pixel WS2812 frame is 2.41 ms, so a 25 ms cycle gets ~10 updates.
    const f = byCode('0 40 50 80 255 0 0 1 1\n', 'STRIP_COARSE');
    assert.equal(f.length, 1);
    assert.equal(f[0].severity, 'info');
    assert.match(f[0].msg, /2\.41 ms to clock/);
    assert.match(f[0].msg, /about 10 times per cycle/);
    assert.match(f[0].msg, /timer itself is running at 10000 Hz, its ceiling/);
    // A 10 Hz cycle fits ~41 frames and needs no note.
    assert.deepEqual(byCode('0 10 50 80 255 0 0 1 1\n', 'STRIP_COARSE'), []);
});

test('STRIP_COARSE: says a square carrier does not care', () => {
    const sq = byCode('0 40 50 80 255 0 0 1 0\n', 'STRIP_COARSE');
    assert.equal(sq.length, 1);
    assert.match(sq[0].msg, /only needs two edges, so this is fine/);
    const sine = byCode('0 40 50 80 255 0 0 1 1\n', 'STRIP_COARSE');
    assert.match(sine[0].msg, /expect it to look coarser on the device/);
});

test('STRIP_COARSE: reports each distinct rate once, not once per entry', () => {
    const f = byCode('0 40 50 80 255 0 0 1\n1000 40 50 80 255 0 0 1\n'
                   + '2000 40 50 80 255 0 0 1\n', 'STRIP_COARSE');
    assert.equal(f.length, 1);
});

test('SILENT_CHANNEL: a channel that never rises above vol 0', () => {
    const f = byCode('A 0 200 0 0 0 1\n', 'SILENT_CHANNEL');
    assert.equal(f.length, 1);
    assert.equal(f[0].severity, 'info');
    // A ramp that reaches a real volume is not silent.
    assert.deepEqual(byCode('A 0 200 0 >0 0 1\nA 1000 200 0 80 0 1\n',
                            'SILENT_CHANNEL'), []);
    // Nor is an LFO that swings up.
    assert.deepEqual(byCode('A 0 200 0 ~0:60:4000 0 1\n', 'SILENT_CHANNEL'), []);
});

test('findings carry a row index and a time so the UI can link to both', () => {
    const f = lint('# header\n\n0 10 50 80 255 0 0 1\n5000 150 50 80 255 0 0 1\n');
    const drop = f.find(x => x.code === 'LED_ROW_DROPPED');
    assert.equal(drop.row, 3, 'row index should count comments and blanks');
    assert.equal(drop.timeMs, 5000);
});

test('findings come back ordered by time', () => {
    const f = lint(
        'A 0 200 0 80 10 1 0 0 40 1\n'
      + '5000 150 50 80 255 0 0 1\n'
      + 'A 9000 200 0 80 10 2 0 0 40 2\n');
    const times = f.map(x => x.timeMs);
    for (let i = 1; i < times.length; i++) {
        assert.ok(times[i] >= times[i - 1], 'out of order: ' + times.join(','));
    }
});

test('an empty or absent doc lints to nothing rather than throwing', () => {
    assert.deepEqual(lintSession(null), []);
    assert.deepEqual(lintSession({}), []);
    assert.deepEqual(lint(''), []);
});
