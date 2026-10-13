// The pure mixing helpers that survived the move of synthesis into sim/audio.js.
// The waveform / interpolation / channel-grouping / render coverage that used to
// live in synth.test.js now lives in sim_audio.test.js and sim_resolve.test.js,
// against the single engine both the bounce and the simulator use.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { zeroBuffers, mixInto, clampBuffers } from '../src/js/gen/mix.js';
import { SAMPLE_RATE } from '../src/js/gen/model.js';

test('zeroBuffers: length follows the duration, contents are silent', () => {
    const b = zeroBuffers(1000);
    assert.equal(b.left.length, SAMPLE_RATE);
    assert.equal(b.right.length, SAMPLE_RATE);
    assert.equal(b.sampleRate, SAMPLE_RATE);
    assert.ok(b.left.every(v => v === 0));
});

test('zeroBuffers: never returns an empty buffer', () => {
    assert.equal(zeroBuffers(0).left.length, 1);
});

test('mixInto: adds at the offset, scaled by gain', () => {
    const { left, right } = zeroBuffers(0);
    const L = new Float32Array(10), R = new Float32Array(10);
    const src = new Float32Array([1, 1, 1]);
    mixInto(L, R, src, src, 4, 0.5);
    assert.deepEqual(Array.from(L.slice(3, 8)), [0, 0.5, 0.5, 0.5, 0]);
    assert.deepEqual(Array.from(R.slice(3, 8)), [0, 0.5, 0.5, 0.5, 0]);
});

test('mixInto: ADDS rather than overwrites, so two sources sum', () => {
    const L = new Float32Array([0.25, 0.25]), R = new Float32Array(2);
    mixInto(L, R, new Float32Array([0.5, 0.5]), null, 0, 1);
    assert.deepEqual(Array.from(L), [0.75, 0.75]);
});

test('mixInto: a mono source feeds both outputs', () => {
    const L = new Float32Array(3), R = new Float32Array(3);
    mixInto(L, R, new Float32Array([1, 1, 1]), null, 0, 1);
    assert.deepEqual(Array.from(R), [1, 1, 1]);
});

test('mixInto: truncates past the end instead of growing or throwing', () => {
    const L = new Float32Array(4), R = new Float32Array(4);
    mixInto(L, R, new Float32Array([1, 1, 1, 1, 1, 1]), null, 2, 1);
    assert.deepEqual(Array.from(L), [0, 0, 1, 1]);
});

test('mixInto: a negative offset drops the samples before zero', () => {
    const L = new Float32Array(4), R = new Float32Array(4);
    mixInto(L, R, new Float32Array([1, 2, 3, 4]), null, -2, 1);
    assert.deepEqual(Array.from(L), [3, 4, 0, 0]);
});

test('mixInto: loopToFill repeats the source to the end of the buffer', () => {
    const L = new Float32Array(7), R = new Float32Array(7);
    mixInto(L, R, new Float32Array([1, 2]), null, 1, 1, true);
    assert.deepEqual(Array.from(L), [0, 1, 2, 1, 2, 1, 2]);
});

test('mixInto: an empty source is a no-op rather than a divide by zero', () => {
    const L = new Float32Array(3), R = new Float32Array(3);
    mixInto(L, R, new Float32Array(0), null, 0, 1, true);
    assert.deepEqual(Array.from(L), [0, 0, 0]);
});

test('clampBuffers: hard-clamps both channels to [-1,1] in place', () => {
    const L = new Float32Array([2, -2, 0.5, -0.5]);
    const R = new Float32Array([-3, 3, 1, -1]);
    clampBuffers(L, R);
    assert.deepEqual(Array.from(L), [1, -1, 0.5, -0.5]);
    assert.deepEqual(Array.from(R), [-1, 1, 1, -1]);
});
