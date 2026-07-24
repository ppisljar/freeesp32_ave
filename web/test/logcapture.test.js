// Unit tests for the PURE log-stitching helpers (no DOM / IndexedDB).
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { overlapLen, mergeLogSnapshots } from '../src/js/logcapture.js';

// ---- overlapLen ------------------------------------------------------------

test('overlapLen: longest suffix of a that is a prefix of b', () => {
    assert.equal(overlapLen('abcdef', 'defgh'), 3);   // "def"
    assert.equal(overlapLen('abcdef', 'def'), 3);     // full suffix
    assert.equal(overlapLen('abcdef', 'xyz'), 0);     // nothing shared
    assert.equal(overlapLen('', 'abc'), 0);
    assert.equal(overlapLen('abc', ''), 0);
});

test('overlapLen: does not falsely match an interior repeat', () => {
    // "ababab" ends with "abab"; b starts with "abX" — only "ab" overlaps.
    assert.equal(overlapLen('ababab', 'abXYZ'), 2);
});

// ---- mergeLogSnapshots -----------------------------------------------------

test('mergeLogSnapshots: first snapshot seeds the buffer', () => {
    assert.equal(mergeLogSnapshots('', 'line1\nline2\n'), 'line1\nline2\n');
});

test('mergeLogSnapshots: fully-overlapping snapshot appends nothing', () => {
    assert.equal(mergeLogSnapshots('abcdef', 'def'), 'abcdef');
});

test('mergeLogSnapshots: partially-new snapshot appends only the new tail', () => {
    assert.equal(mergeLogSnapshots('abcdef', 'defgh'), 'abcdefgh');
});

test('mergeLogSnapshots: no overlap inserts a gap marker (ring wrapped)', () => {
    const out = mergeLogSnapshots('abcdef', 'xyz');
    assert.match(out, /log ring wrapped/);
    assert.ok(out.startsWith('abcdef'));
    assert.ok(out.endsWith('xyz'));
});

// ---- ring-slide simulation -------------------------------------------------
// Simulate the device's 32 KB wrapping ring as a fixed-size window sliding over
// a growing stream, polled by the browser. As long as the window is larger than
// the per-poll growth, accumulation must reconstruct the stream EXACTLY.

test('mergeLogSnapshots: reconstructs a full stream from sliding windows', () => {
    const WINDOW = 50;   // stand-in for the 32 KB ring
    const STEP = 12;     // bytes emitted between polls (< WINDOW → no gaps)
    let stream = '';
    let acc = '';
    for (let i = 0; i < 40; i++) {
        stream += 'evt' + i + ':some-log-line;';        // grow the stream
        const snap = stream.slice(Math.max(0, stream.length - WINDOW)); // ring view
        acc = mergeLogSnapshots(acc, snap);
    }
    assert.equal(acc, stream);
    assert.ok(!acc.includes('log ring wrapped'));
});

test('mergeLogSnapshots: flags a gap when growth outruns the window', () => {
    const WINDOW = 20;
    const STEP = 40;     // more emitted per poll than the window holds → data lost
    let stream = '';
    let acc = '';
    for (let i = 0; i < 6; i++) {
        stream += 'X'.repeat(STEP) + i;
        const snap = stream.slice(Math.max(0, stream.length - WINDOW));
        acc = mergeLogSnapshots(acc, snap);
    }
    assert.match(acc, /log ring wrapped/);
});
