// Round-trip property tests for the Generator core (parse + serialize).
//   - parse(serialize(parse(text))).doc  deepEquals  parse(text).doc
//   - serialize(parse(serialize(parse(text))))  ===  serialize(parse(text))
//     (the canonical form is a byte-stable fixed point; against an aligned
//      source it matches modulo whitespace, per the plan.)

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';
import { EXAMPLE, EDGE_CASES, loadLedcCorpus } from './fixtures.js';

function corpus() {
    const items = [];
    items.push(['firmware-example', EXAMPLE]);
    for (const [name, text] of Object.entries(EDGE_CASES)) items.push(['edge:' + name, text]);
    for (const [name, text] of Object.entries(loadLedcCorpus())) items.push(['ledc:' + name, text]);
    return items;
}

for (const [name, text] of corpus()) {
    test('round-trip: model is a fixed point — ' + name, () => {
        const d1 = parse(text).doc;
        const canonical = serialize(d1);
        const d2 = parse(canonical).doc;
        assert.deepStrictEqual(d2, d1, 'parse(serialize(parse(text))) must equal parse(text)');
    });

    test('round-trip: canonical text is byte-stable — ' + name, () => {
        const once = serialize(parse(text).doc);
        const twice = serialize(parse(once).doc);
        assert.equal(twice, once, 'canonical serialization must be idempotent');
    });

    test('round-trip: corpus parses without errors — ' + name, () => {
        const { diagnostics } = parse(text);
        const errs = diagnostics.filter(d => d.severity === 'error');
        assert.deepStrictEqual(errs, [], 'no parse errors expected for known-good corpus');
    });
}

// Field-level spot checks on the example to guard against silent structural drift.
test('round-trip: example field-level checks', () => {
    const { doc } = parse(EXAMPLE);
    const led = doc.rows.filter(r => r.kind === 'led');
    const audio = doc.rows.filter(r => r.kind === 'audio');
    assert.equal(led.length, 4, '4 LED rows');
    assert.equal(audio.length, 8, '8 audio rows');

    // First LED: 0 8 50 30 0 0 255 9
    assert.equal(led[0].time, 0);
    assert.equal(led[0].freq.value, 8);
    assert.equal(led[0].b.value, 255);
    assert.equal(led[0].mask, 9);
    assert.equal(led[0].legacy5, false);

    // Sweep LED at 10 s: g and b carry linear ramps.
    assert.equal(led[1].g.interp, 'lin');
    assert.equal(led[1].b.interp, 'lin');
    assert.equal(led[1].freq.interp, 'lin');

    // Quadratic ease LED at 20 s.
    assert.equal(led[2].freq.interp, 'quad');
    assert.equal(led[2].r.interp, 'quad');

    // First audio: A 0 200 -100 60 0 1
    assert.equal(audio[0].freq.value, 200);
    assert.equal(audio[0].pan.value, -100);
    assert.equal(audio[0].channel, 1);
    assert.equal(audio[0].freqR, 0);
    assert.equal(audio[0].waveType, null);
    assert.equal(audio[0].inlineComment, 'ch1 audio: 200 Hz, left');
});

// Comments / blank lines / BG survive a round-trip.
test('round-trip: comments, blanks and BG survive', () => {
    const { doc } = parse(EDGE_CASES.bg_line);
    assert.ok(doc.bg, 'doc.bg set');
    assert.equal(doc.bg.url, 'http://example.com/river.wav');
    assert.equal(doc.bg.pan, 50);
    assert.equal(doc.bg.loudness, 30);
    // BG row preserved in position.
    assert.equal(doc.rows[0].kind, 'bg');
    assert.match(serialize(doc), /^BG http:\/\/example\.com\/river\.wav 50 30$/m);

    const c = parse(EDGE_CASES.blanks_and_comments).doc;
    assert.ok(c.rows.some(r => r.kind === 'blank'), 'blank rows preserved');
    assert.ok(c.rows.some(r => r.kind === 'comment'), 'comment rows preserved');
});

// Periodic modulations preserve start/end/period exactly.
test('round-trip: periodic mods keep start:end:period', () => {
    const { doc } = parse(EDGE_CASES.all_periodic);
    const led = doc.rows.find(r => r.kind === 'led');
    assert.equal(led.duty.interp, 'tri');
    assert.equal(led.duty.value, 10);
    assert.equal(led.duty.modEnd, 20);
    assert.equal(led.duty.modPeriodMs, 500);
    assert.equal(led.r.interp, 'sawup');
    assert.equal(led.g.interp, 'sawdn');
    assert.equal(led.b.interp, 'sq');
    assert.equal(led.b.modEnd, 255);
    assert.equal(led.b.modPeriodMs, 1000);
    assert.equal(serialize(doc), EDGE_CASES.all_periodic);
});
