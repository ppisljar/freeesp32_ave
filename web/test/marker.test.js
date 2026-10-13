// `M <time> <LABEL> [key=value ...]` stimulus-marker rows.
//
// The device parses these (config_parser.c:126) and docs/sessions/markers.html
// documents them, but the web parser used to turn every one into a `raw` row —
// so any session using the marker bus showed as a file full of errors in the
// editor. These tests pin the grammar, including the places where the device is
// deliberately LENIENT and a stricter browser parser would reject files the
// hardware runs happily.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';
import { markerRow, MARKER_LABEL_RE } from '../src/js/gen/model.js';

const rowsOf = (t) => parse(t).doc.rows;
const first = (t) => rowsOf(t)[0];
const diagsOf = (t) => parse(t).diagnostics;

test('a bare marker parses into a marker row', () => {
    const r = first('M 0 BASELINE_START\n');
    assert.equal(r.kind, 'marker');
    assert.equal(r.time, 0);
    assert.equal(r.label, 'BASELINE_START');
    assert.deepEqual(r.kv, []);
    assert.equal(r.modality, null, 'modality is inferred at dispatch by default');
});

test('key=value pairs are captured in order', () => {
    const r = first('M 30000 STIM_START condition=alpha10 block=3\n');
    assert.equal(r.time, 30000);
    assert.deepEqual(r.kv, [{ k: 'condition', v: 'alpha10' }, { k: 'block', v: '3' }]);
});

test('mod= is reserved: it sets modality and is never stored as a pair', () => {
    const r = first('M 100 LED_ON mod=led condition=x\n');
    assert.equal(r.modality, 'led');
    assert.deepEqual(r.kv, [{ k: 'condition', v: 'x' }]);
});

test('an unknown mod= value falls back to inference rather than erroring', () => {
    const r = first('M 100 X mod=sideways\n');
    assert.equal(r.modality, null);
    assert.deepEqual(r.kv, [], 'and it is still not stored as a pair');
});

test('all four modalities are accepted', () => {
    for (const m of ['audio', 'led', 'both', 'none']) {
        assert.equal(first('M 0 X mod=' + m + '\n').modality, m);
    }
});

test('a token with no "=" is skipped, not fatal — the device only warns', () => {
    const r = first('M 0 X junk block=2\n');
    assert.equal(r.kind, 'marker');
    assert.deepEqual(r.kv, [{ k: 'block', v: '2' }]);
    assert.deepEqual(diagsOf('M 0 X junk block=2\n'), []);
});

test('pairs past the fourth are dropped, not fatal', () => {
    const r = first('M 0 X a=1 b=2 c=3 d=4 e=5 f=6\n');
    assert.equal(r.kv.length, 4);
    assert.deepEqual(r.kv.map(p => p.k), ['a', 'b', 'c', 'd']);
});

test('a value may contain "=" — only the first one splits', () => {
    const r = first('M 0 X expr=a=b\n');
    assert.deepEqual(r.kv, [{ k: 'expr', v: 'a=b' }]);
});

test('a missing label is an error and the line survives as raw', () => {
    const t = 'M 1000\n';
    assert.equal(first(t).kind, 'raw');
    assert.equal(diagsOf(t).length, 1);
    assert.match(diagsOf(t)[0].msg, /needs at least/);
});

test('a label outside [A-Z0-9_] is an error', () => {
    for (const bad of ['lowercase', 'has-dash', 'has space', '']) {
        const t = 'M 0 ' + bad + '\n';
        const r = first(t);
        assert.ok(r.kind === 'raw' || r.label !== bad, 'accepted bad label: ' + bad);
    }
    assert.match(diagsOf('M 0 bad-label\n')[0].msg, /\[A-Z0-9_\]/);
});

test('MARKER_LABEL_RE matches the firmware charset and length', () => {
    assert.ok(MARKER_LABEL_RE.test('A'));
    assert.ok(MARKER_LABEL_RE.test('STIM_1'));
    assert.ok(MARKER_LABEL_RE.test('X'.repeat(31)));
    assert.ok(!MARKER_LABEL_RE.test('X'.repeat(32)), '32 chars does not fit char label[32]');
    assert.ok(!MARKER_LABEL_RE.test(''));
    assert.ok(!MARKER_LABEL_RE.test('lower'));
});

test('a lowercase "m" keyword is accepted, like the firmware', () => {
    assert.equal(first('m 0 X\n').kind, 'marker');
});

test('a marker is not mistaken for an LED row, and vice versa', () => {
    assert.equal(first('M 0 X\n').kind, 'marker');
    assert.equal(first('0 10 50 80 255 0 0 1\n').kind, 'led');
    // A word starting with M but not the bare keyword is NOT a marker.
    assert.equal(first('MX 0 1\n').kind, 'raw');
});

test('an inline comment is kept', () => {
    assert.equal(first('M 0 X   # block one\n').inlineComment, 'block one');
});

// ---- round-trip ------------------------------------------------------------

test('serialize emits the canonical form with mod= first', () => {
    const doc = { rows: [markerRow({ time: 500, label: 'STIM', modality: 'both',
                                     kv: [{ k: 'a', v: '1' }] })], bg: null };
    assert.equal(serialize(doc), 'M 500 STIM mod=both a=1\n');
});

test('parse -> serialize -> parse is a fixpoint', () => {
    const src = [
        '# markers around a block',
        'M 0      BASELINE_START',
        'M 30000  STIM_START mod=both condition=alpha10 block=3',
        'M 90000  STIM_END',
        '',
    ].join('\n');
    const once = serialize(parse(src).doc);
    const twice = serialize(parse(once).doc);
    assert.equal(once, twice);
    assert.equal(once.split('\n')[1], 'M 0 BASELINE_START');
});

test('markers coexist with the rest of a session and keep their position', () => {
    const src = 'M 0 START\n0 10 50 80 255 0 0 1\nA 0 200 0 80 0 1\nM 5000 END\n';
    const kinds = rowsOf(src).map(r => r.kind);
    assert.deepEqual(kinds, ['marker', 'led', 'audio', 'marker']);
    assert.equal(serialize(parse(src).doc), src);
});

// ---- resolution: inferred modality and the predicted onset -----------------

import { resolveMarkers } from '../src/js/gen/sim/resolve.js';
import { markerPipelineLagUs, AUDIO_DMA_PIPELINE_LAG_US,
         LED_US_PER_PIXEL, DEFAULT_CHANNEL_MAP } from '../src/js/gen/sim/led.js';

const resolve = (t) => resolveMarkers(parse(t).doc, markerPipelineLagUs);

test('markerPipelineLagUs matches marker_pipeline_lag_us', () => {
    // main/marker.c:130. LED entries carry the SAME DMA lag as audio (that
    // symmetry is what makes A/V sync hold), plus the strip write on top at
    // 30 us/pixel halved, integer-divided.
    const px = DEFAULT_CHANNEL_MAP.length;              // 74 on the shipped build
    assert.equal(markerPipelineLagUs('audio'), 46439);
    assert.equal(markerPipelineLagUs('led'),
                 AUDIO_DMA_PIPELINE_LAG_US + Math.trunc(px * LED_US_PER_PIXEL / 2));
    assert.equal(markerPipelineLagUs('led'), 47549);
    assert.equal(markerPipelineLagUs('both'), markerPipelineLagUs('led'),
                 'both takes the LATER of the two lags');
    assert.equal(markerPipelineLagUs('none'), 0,
                 'bookkeeping only — onset equals dispatch');
});

test('modality is inferred from what shares the timestamp', () => {
    const m = resolve(
        'M 0 LED_ONLY\n0 10 50 80 255 0 0 1\n' +
        'M 1000 AUDIO_ONLY\nA 1000 200 0 80 0 1\n' +
        'M 2000 BOTH\n2000 10 50 80 255 0 0 1\nA 2000 200 0 80 0 1\n' +
        'M 3000 ALONE\n');
    assert.deepEqual(m.map(x => [x.label, x.modality, x.inferred]), [
        ['LED_ONLY', 'led', true],
        ['AUDIO_ONLY', 'audio', true],
        ['BOTH', 'both', true],
        ['ALONE', 'none', true],
    ]);
});

test('an explicit mod= overrides the inference', () => {
    const m = resolve('M 0 X mod=audio\n0 10 50 80 255 0 0 1\n');
    assert.equal(m[0].modality, 'audio');
    assert.equal(m[0].inferred, false);
});

test('the predicted onset is the scheduled time plus the pipeline lag', () => {
    const m = resolve('M 5000 X mod=led\n');
    assert.equal(m[0].timeMs, 5000);
    assert.equal(m[0].lagUs, 47549);
    assert.ok(Math.abs(m[0].onsetMs - 5047.549) < 1e-9);
    // A bookkeeping marker predicts onset == scheduled.
    assert.equal(resolve('M 5000 X mod=none\n')[0].onsetMs, 5000);
});

test('resolveMarkers returns them in time order and ignores other rows', () => {
    const m = resolve('M 9000 LATE\n0 10 50 80 255 0 0 1\nM 100 EARLY\n');
    assert.deepEqual(m.map(x => x.label), ['EARLY', 'LATE']);
});

test('a session with no markers resolves to nothing', () => {
    assert.deepEqual(resolve('0 10 50 80 255 0 0 1\n'), []);
    assert.deepEqual(resolveMarkers(null, markerPipelineLagUs), []);
});
