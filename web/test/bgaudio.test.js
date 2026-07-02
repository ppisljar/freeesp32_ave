// Unit tests for the PURE BG audio helpers (no Web Audio / DOM). Mirrors the
// plain-Node style of table_view.test.js.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    floatToInt16, interleaveInt16, encodeWav16, DEVICE_SAMPLE_RATE,
} from '../src/js/gen/bgaudio.js';
import { metaOf } from '../src/js/gen/bgstore.js';

// ---- floatToInt16 ----------------------------------------------------------

test('floatToInt16: 0 -> 0, +1 clamps to 32767, -1 -> -32768', () => {
    assert.equal(floatToInt16(0), 0);
    assert.equal(floatToInt16(1), 32767);
    assert.equal(floatToInt16(-1), -32768);
});

test('floatToInt16: out-of-range clamps', () => {
    assert.equal(floatToInt16(2.5), 32767);
    assert.equal(floatToInt16(-9), -32768);
});

test('floatToInt16: half scale', () => {
    assert.equal(floatToInt16(0.5), Math.round(0.5 * 32768)); // 16384
});

// ---- interleaveInt16 -------------------------------------------------------

test('interleaveInt16: two channels interleave L,R,L,R', () => {
    const L = new Float32Array([0, 1, -1]);
    const R = new Float32Array([1, 0, 0.5]);
    const out = interleaveInt16([L, R]);
    assert.equal(out.length, 6);
    assert.deepEqual(Array.from(out), [0, 32767, 32767, 0, -32768, 16384]);
});

// ---- encodeWav16 -----------------------------------------------------------

function readStr(u8, off, len) {
    let s = '';
    for (let i = 0; i < len; i++) s += String.fromCharCode(u8[off + i]);
    return s;
}
function readU32(u8, off) {
    return u8[off] | (u8[off + 1] << 8) | (u8[off + 2] << 16) | (u8[off + 3] << 24);
}
function readU16(u8, off) { return u8[off] | (u8[off + 1] << 8); }

test('encodeWav16: header fields for stereo 44100', () => {
    const frames = 10;
    const L = new Float32Array(frames);
    const R = new Float32Array(frames);
    const u8 = encodeWav16([L, R], DEVICE_SAMPLE_RATE);

    assert.equal(readStr(u8, 0, 4), 'RIFF');
    assert.equal(readStr(u8, 8, 4), 'WAVE');
    assert.equal(readStr(u8, 12, 4), 'fmt ');
    assert.equal(readU32(u8, 16), 16);          // PCM fmt chunk size
    assert.equal(readU16(u8, 20), 1);           // PCM
    assert.equal(readU16(u8, 22), 2);           // channels
    assert.equal(readU32(u8, 24), 44100);       // sample rate
    assert.equal(readU32(u8, 28), 44100 * 2 * 2); // byte rate
    assert.equal(readU16(u8, 32), 4);           // block align
    assert.equal(readU16(u8, 34), 16);          // bits
    assert.equal(readStr(u8, 36, 4), 'data');

    const dataBytes = frames * 2 /*ch*/ * 2 /*bytes*/;
    assert.equal(readU32(u8, 40), dataBytes);
    assert.equal(readU32(u8, 4), 36 + dataBytes); // RIFF chunk size
    assert.equal(u8.length, 44 + dataBytes);
});

test('encodeWav16: PCM payload round-trips a known ramp', () => {
    const L = new Float32Array([0, 0.5, -0.5, 1]);
    const R = new Float32Array([0, -0.5, 0.5, -1]);
    const u8 = encodeWav16([L, R], DEVICE_SAMPLE_RATE);
    const dv = new DataView(u8.buffer);
    // First stereo frame at byte 44: L0, R0
    assert.equal(dv.getInt16(44, true), 0);
    assert.equal(dv.getInt16(46, true), 0);
    // Second frame: L1=0.5, R1=-0.5
    assert.equal(dv.getInt16(48, true), 16384);
    assert.equal(dv.getInt16(50, true), -16384);
    // Last frame: L3=+1 -> 32767, R3=-1 -> -32768
    assert.equal(dv.getInt16(44 + 3 * 4, true), 32767);
    assert.equal(dv.getInt16(44 + 3 * 4 + 2, true), -32768);
});

// ---- bgstore.metaOf --------------------------------------------------------

test('metaOf: strips the blob, keeps metadata + byte size', () => {
    const fakeBlob = { size: 12345 };
    const rec = {
        name: 'ambient', createdAt: 42, durationMs: 60000,
        sourceKind: 'drone', wavBlob: fakeBlob,
    };
    const m = metaOf(rec);
    assert.equal(m.name, 'ambient');
    assert.equal(m.createdAt, 42);
    assert.equal(m.durationMs, 60000);
    assert.equal(m.sourceKind, 'drone');
    assert.equal(m.bytes, 12345);
    assert.equal(m.wavBlob, undefined);
});
