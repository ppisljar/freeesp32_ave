// v2 pulse-field pass-through regression tests.
//
// The lane and wizard views recompile every row through the model constructors
// on each edit. Before this fix they forwarded only the legacy fields, so ANY
// edit silently stripped the v2 pulse fields (LED env/phase/attack/jitter;
// audio duty/env/phase/attack/jitter) and the `-` (null = leave unchanged)
// sentinel. These tests lock in lossless preservation across a view round-trip.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { lanesToDoc, docToLanes } from '../src/js/gen/views/lane_serialize.js';
import { sessionFromDoc, compileSession } from '../src/js/gen/views/wizard_compile.js';

// A source exercising every pulse field on both row kinds, plus a `-` (null)
// field that must survive as "leave unchanged" rather than degrade to a value.
//   LED:   time freq duty bright R G B mask [env phase attack jitter]
//   AUDIO: A time freq pan vol mod [ch freqR wave duty env phase attack jitter]
const SRC = [
    '0 10 50 80 0 128 255 1 1 180 5 0.2',
    'A 0 200 0 80 0 0 210 0 50 3 90 4',
    '1000 12 50 80 0 128 255 1 2 - 5 0.3',   // LED phase = `-` (null)
    'A 1000 200 0 80 0 0 212 0 - 3 90 4',    // audio duty = `-` (null)
].join('\n');

const LED_PULSE = ['env', 'phase', 'attack', 'jitter'];
const AUD_PULSE = ['duty', 'env', 'phase', 'attack', 'jitter'];

// Normalize through JSON — the exact transport used by the @ave-lane / @ave-wizard
// metadata comments. This collapses `{amp, period: undefined}` and `{amp}` (which
// serialize.js emits identically) to the same shape on BOTH sides, while a
// genuinely dropped field still shows as a mismatch.
function pick(row, fields) {
    const o = {};
    for (const f of fields) o[f] = row[f];
    return JSON.parse(JSON.stringify(o));
}
function ledAt(doc, t) { return doc.rows.find(r => r.kind === 'led' && r.time === t); }
function audAt(doc, t) { return doc.rows.find(r => r.kind === 'audio' && r.time === t); }

// Assert the pulse fields on each kind's t=0 and t=1000 rows match the original.
function assertPreserved(orig, out, label) {
    for (const t of [0, 1000]) {
        assert.deepEqual(pick(ledAt(out, t), LED_PULSE), pick(ledAt(orig, t), LED_PULSE),
            `${label}: LED pulse fields preserved at t=${t}`);
        assert.deepEqual(pick(audAt(out, t), AUD_PULSE), pick(audAt(orig, t), AUD_PULSE),
            `${label}: audio pulse fields preserved at t=${t}`);
    }
    // The `-` sentinel must survive as null (not undefined, not a value).
    assert.equal(ledAt(out, 1000).phase, null, `${label}: LED phase '-' stays null`);
    assert.equal(audAt(out, 1000).duty, null, `${label}: audio duty '-' stays null`);
}

test('lane round-trip (structural, no metadata) preserves pulse fields', () => {
    const orig = parse(SRC).doc;
    const out = lanesToDoc(docToLanes(orig), { withMeta: false }).doc;
    assertPreserved(orig, out, 'lane-structural');
});

test('lane round-trip (via @ave-lane metadata) preserves pulse fields', () => {
    const orig = parse(SRC).doc;
    // First projection embeds metadata; re-importing must read pulse from the JSON.
    const withMeta = lanesToDoc(docToLanes(orig), { withMeta: true }).doc;
    const out = lanesToDoc(docToLanes(withMeta), { withMeta: false }).doc;
    assertPreserved(orig, out, 'lane-metadata');
});

test('wizard round-trip (imported, no metadata) preserves pulse fields', () => {
    const orig = parse(SRC).doc;
    const { session } = sessionFromDoc(orig);   // imported = true (no @ave-wizard meta)
    const out = compileSession(session).doc;
    assertPreserved(orig, out, 'wizard-imported');
});
