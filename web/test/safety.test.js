// Tests for the epilepsy/photosensitivity safety opt-in pure logic:
// flicker detection over a doc and the localStorage-backed acceptance flag.
// The modal glue (ensureSafetyAccepted's DOM path) is not unit-tested.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    SAFETY_KEY, SAFETY_VERSION,
    ledRowFlickers, docHasFlicker,
    isSafetyAccepted, setSafetyAccepted, resetSafetyAccepted,
} from '../src/js/safety.js';
import { ledRow, audioRow, speechRow, commentRow, cell } from '../src/js/gen/model.js';

// ---- flicker detection -----------------------------------------------------

test('ledRowFlickers: LED with freq value > 0 flickers', () => {
    assert.equal(ledRowFlickers(ledRow({ freq: cell(10) })), true);
});

test('ledRowFlickers: LED with freq 0 does not flicker', () => {
    assert.equal(ledRowFlickers(ledRow({ freq: cell(0) })), false);
});

test('ledRowFlickers: modulation endpoint > 0 counts as flicker', () => {
    // freq steps at 0 but modulates up to 8 Hz.
    const c = cell(0, 'sine', 8, 1000);
    assert.equal(ledRowFlickers(ledRow({ freq: c })), true);
});

test('ledRowFlickers: non-LED rows never flicker', () => {
    assert.equal(ledRowFlickers(audioRow({ freq: cell(440) })), false);
    assert.equal(ledRowFlickers(speechRow({ text: 'hi' })), false);
    assert.equal(ledRowFlickers(commentRow('x')), false);
    assert.equal(ledRowFlickers(null), false);
});

test('docHasFlicker: true when any LED row flickers', () => {
    const doc = { rows: [audioRow({ freq: cell(440) }), ledRow({ freq: cell(20) })] };
    assert.equal(docHasFlicker(doc), true);
});

test('docHasFlicker: false for audio-only / static-LED sessions', () => {
    assert.equal(docHasFlicker({ rows: [audioRow({ freq: cell(440) })] }), false);
    assert.equal(docHasFlicker({ rows: [ledRow({ freq: cell(0), bright: cell(80) })] }), false);
    assert.equal(docHasFlicker({ rows: [] }), false);
    assert.equal(docHasFlicker(null), false);
});

// ---- acceptance flag (fake storage) ---------------------------------------

function fakeStore() {
    const m = new Map();
    return {
        getItem: k => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => { m.set(k, String(v)); },
        removeItem: k => { m.delete(k); },
        _map: m,
    };
}

test('acceptance flag: set / read / reset round-trip', () => {
    const s = fakeStore();
    assert.equal(isSafetyAccepted(s), false);
    setSafetyAccepted(s);
    assert.equal(s._map.get(SAFETY_KEY), SAFETY_VERSION);
    assert.equal(isSafetyAccepted(s), true);
    resetSafetyAccepted(s);
    assert.equal(isSafetyAccepted(s), false);
});

test('acceptance flag: stale/mismatched value is not accepted', () => {
    const s = fakeStore();
    s.setItem(SAFETY_KEY, '0');
    assert.equal(isSafetyAccepted(s), false);
});

test('acceptance flag: null store degrades to not-accepted (no throw)', () => {
    assert.equal(isSafetyAccepted(null), false);
    assert.doesNotThrow(() => setSafetyAccepted(null));
    assert.doesNotThrow(() => resetSafetyAccepted(null));
});
