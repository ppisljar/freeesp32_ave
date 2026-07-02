// Unit tests for the offline session synth's pure helpers.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { waveform, evalField, extractAudioChannels, renderSession } from '../src/js/gen/synth.js';
import { cell, audioRow } from '../src/js/gen/model.js';

// ---- waveform --------------------------------------------------------------

test('waveform square: +1 first half, -1 second half', () => {
    assert.equal(waveform(1, 0), 1);
    assert.equal(waveform(1, 0x40000000), 1);
    assert.equal(waveform(1, 0x80000000), -1);
    assert.equal(waveform(1, 0xC0000000), -1);
});

test('waveform triangle: -1 at 0, +1 near half', () => {
    assert.ok(Math.abs(waveform(2, 0) - (-1)) < 1e-6);
    assert.ok(waveform(2, 0x7FFFFFFF) > 0.99);
});

test('waveform sine: 0 at phase 0, ~+1 at quarter', () => {
    assert.ok(Math.abs(waveform(0, 0)) < 1e-6);
    assert.ok(waveform(0, 0x40000000) > 0.999);
});

// ---- evalField -------------------------------------------------------------

test('evalField step: returns base when no interp', () => {
    assert.equal(evalField(cell(440), null, 0, null, 500), 440);
});

test('evalField linear: midpoint is halfway to next', () => {
    const c = cell(100, 'lin');
    const n = cell(200);
    assert.equal(evalField(c, n, 0, 1000, 500), 150);
    assert.equal(evalField(c, n, 0, 1000, 0), 100);
    assert.equal(evalField(c, n, 0, 1000, 1000), 200);
});

test('evalField quadratic ease is symmetric around midpoint', () => {
    const c = cell(0, 'quad');
    const n = cell(100);
    const at25 = evalField(c, n, 0, 1000, 250);
    const at75 = evalField(c, n, 0, 1000, 750);
    // ease-in-out: 0.25 -> 2*0.0625=0.125 -> 12.5 ; 0.75 -> 1-2*0.0625=0.875 -> 87.5
    assert.ok(Math.abs(at25 - 12.5) < 1e-6);
    assert.ok(Math.abs(at75 - 87.5) < 1e-6);
});

test('evalField sine mod oscillates between value and modEnd', () => {
    const c = cell(0, 'sine', 100, 1000); // value 0, end 100, period 1s
    assert.ok(Math.abs(evalField(c, null, 0, null, 0) - 0) < 1e-6);   // ph 0 -> shape 0
    assert.ok(Math.abs(evalField(c, null, 0, null, 500) - 100) < 1e-6); // ph .5 -> shape 1
});

// ---- extract + render ------------------------------------------------------

test('extractAudioChannels groups + sorts by time', () => {
    const doc = { rows: [
        audioRow({ time: 2000, channel: 1, freq: cell(200) }),
        audioRow({ time: 0, channel: 1, freq: cell(100) }),
        audioRow({ time: 0, channel: 2, freq: cell(440) }),
    ] };
    const { channels, maxTimeMs } = extractAudioChannels(doc);
    assert.equal(channels.size, 2);
    assert.equal(maxTimeMs, 2000);
    assert.equal(channels.get(1)[0].time, 0);
    assert.equal(channels.get(1)[1].time, 2000);
});

test('renderSession produces stereo buffers of the right length and finite samples', () => {
    const doc = { rows: [ audioRow({ time: 0, channel: 1, freq: cell(440), vol: cell(80) }) ] };
    const { channels } = extractAudioChannels(doc);
    const { left, right, sampleRate } = renderSession(channels, { totalMs: 100 });
    assert.equal(sampleRate, 44100);
    assert.equal(left.length, Math.ceil(0.1 * 44100));
    assert.equal(right.length, left.length);
    // After the 5ms fade-in there should be non-zero, in-range signal.
    let peak = 0;
    for (let i = 0; i < left.length; i++) peak = Math.max(peak, Math.abs(left[i]));
    assert.ok(peak > 0.1 && peak <= 1.0);
});

test('renderSession clamps to [-1,1]', () => {
    // Many loud channels summed then /nActive should stay in range.
    const rows = [];
    for (let c = 1; c <= 8; c++) rows.push(audioRow({ time: 0, channel: c, freq: cell(300 + c), vol: cell(100) }));
    const { channels } = extractAudioChannels({ rows });
    const { left, right } = renderSession(channels, { totalMs: 50 });
    for (let i = 0; i < left.length; i++) {
        assert.ok(left[i] >= -1 && left[i] <= 1);
        assert.ok(right[i] >= -1 && right[i] <= 1);
    }
});
