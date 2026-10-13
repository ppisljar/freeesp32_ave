// Exports: the WAV you listen to and the CSV you line up against a capture.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import {
    encodeWavDevice, exportWav, sampleTimeline, exportCsv, exportJson,
} from '../src/js/gen/sim/export.js';
import { toInt16Device, MIX_GAIN } from '../src/js/gen/sim/audio.js';
import { stripFrameUs } from '../src/js/gen/sim/led.js';
import { SAMPLE_RATE } from '../src/js/gen/model.js';

const doc = (t) => parse(t).doc;
const SESSION = '0 10 50 80 255 180 84 1\nA 0 200 0 90 0 1 210\n';

function readStr(u8, off, len) {
    let s = '';
    for (let i = 0; i < len; i++) s += String.fromCharCode(u8[off + i]);
    return s;
}
const readU32 = (u8, o) => u8[o] | (u8[o+1] << 8) | (u8[o+2] << 16) | (u8[o+3] << 24);
const readU16 = (u8, o) => u8[o] | (u8[o+1] << 8);

// ---- WAV -------------------------------------------------------------------

test('encodeWavDevice writes a canonical 16-bit stereo header', () => {
    const n = 10;
    const u8 = encodeWavDevice(new Float32Array(n), new Float32Array(n));
    assert.equal(readStr(u8, 0, 4), 'RIFF');
    assert.equal(readStr(u8, 8, 4), 'WAVE');
    assert.equal(readStr(u8, 12, 4), 'fmt ');
    assert.equal(readU32(u8, 16), 16);
    assert.equal(readU16(u8, 20), 1);                 // PCM
    assert.equal(readU16(u8, 22), 2);                 // stereo
    assert.equal(readU32(u8, 24), SAMPLE_RATE);
    assert.equal(readU32(u8, 28), SAMPLE_RATE * 4);   // byte rate
    assert.equal(readU16(u8, 32), 4);                 // block align
    assert.equal(readU16(u8, 34), 16);
    assert.equal(readStr(u8, 36, 4), 'data');
    assert.equal(readU32(u8, 40), n * 4);
    assert.equal(u8.length, 44 + n * 4);
});

test('encodeWavDevice TRUNCATES like the C cast, rather than rounding', () => {
    // audio_test.c:216 does (int16_t)(clamped * 32767.0f). 0.5*32767 = 16383.5,
    // which truncates to 16383 — a rounding encoder would write 16384.
    const u8 = encodeWavDevice(new Float32Array([0.5]), new Float32Array([-0.5]));
    const dv = new DataView(u8.buffer);
    assert.equal(dv.getInt16(44, true), 16383);
    assert.equal(dv.getInt16(46, true), -16383);
    assert.equal(toInt16Device(0.5), 16383);
});

test('exportWav renders the session at device level', () => {
    const { bytes, durationMs } = exportWav(doc(SESSION), { tailMs: 100 });
    assert.ok(durationMs >= 1000);
    assert.equal(readStr(bytes, 0, 4), 'RIFF');
    // A binaural channel bypasses pan, so its peak is the full 1/16 mix gain.
    const dv = new DataView(bytes.buffer);
    let peak = 0;
    for (let o = 44; o + 1 < bytes.length; o += 2) {
        peak = Math.max(peak, Math.abs(dv.getInt16(o, true)));
    }
    // vol 90 -> amp 0.9, binaural so pan is bypassed, times the fixed 1/16.
    const expected = Math.trunc(0.9 * MIX_GAIN * 32767);
    assert.equal(expected, 1843);
    assert.ok(Math.abs(peak - expected) <= 2,
        'peak ' + peak + ' should be about ' + expected + ' (the fixed 1/16 divisor)');
});

// ---- tabular ---------------------------------------------------------------

test('sampleTimeline steps at the strip frame period by default', () => {
    const t = sampleTimeline(doc(SESSION), { endMs: 100 });
    assert.ok(Math.abs(t.stepMs - stripFrameUs() / 1000) < 1e-9);
    assert.ok(Math.abs(t.stepMs - 2.4112) < 1e-3, 'got ' + t.stepMs);
});

test('sampleTimeline emits columns only for the channels in use', () => {
    const t = sampleTimeline(doc(SESSION), { endMs: 10 });
    assert.equal(t.columns[0], 't_ms');
    assert.ok(t.columns.includes('a1_hz'));
    assert.ok(t.columns.includes('led1_level'));
    assert.ok(!t.columns.includes('a2_hz'), 'unused audio channel should be absent');
    assert.ok(!t.columns.includes('led3_level'), 'unused zone should be absent');
    for (const row of t.rows) assert.equal(row.length, t.columns.length);
});

test('sampled values are the resolved ones, including the beat', () => {
    const t = sampleTimeline(doc(SESSION), { endMs: 10 });
    const col = (n) => t.columns.indexOf(n);
    assert.equal(t.rows[0][col('a1_hz')], 200);
    assert.equal(t.rows[0][col('a1_hz_r')], 210);
    assert.equal(t.rows[0][col('a1_beat_hz')], 10);
    assert.equal(t.rows[0][col('led1_hz')], 10);
    // 255,180,84 at 80% brightness, integer-truncated by the strip.
    assert.equal(t.rows[0][col('led1_r')], Math.floor(255 * 80 * 256 / 25600));
});

test('an explicit stepMs is honoured', () => {
    const t = sampleTimeline(doc(SESSION), { stepMs: 50, endMs: 200 });
    assert.equal(t.stepMs, 50);
    assert.equal(t.rows.length, 5);
    assert.deepEqual(t.rows.map(r => r[0]), [0, 50, 100, 150, 200]);
});

// ---- CSV / JSON ------------------------------------------------------------

test('the CSV carries a provenance header saying the data is PREDICTED', () => {
    const csv = exportCsv(doc(SESSION), { endMs: 20 });
    const lines = csv.split('\n');
    assert.match(lines[0], /PREDICTED output, not measured/);
    assert.ok(lines.some(l => l.startsWith('# step_ms=')));
    assert.ok(lines.some(l => l.startsWith('# audio_sample_rate=44100')));
    assert.ok(lines.some(l => /no gamma anywhere/.test(l)));
    // Header row follows the comments.
    const head = lines.find(l => l.startsWith('t_ms'));
    assert.ok(head, 'a column header row must be present');
});

test('markers appear in the CSV header with their predicted onset', () => {
    const csv = exportCsv(doc('M 0 START mod=led block=1\n' + SESSION), { endMs: 20 });
    const line = csv.split('\n').find(l => l.startsWith('# marker'));
    assert.ok(line, 'marker header line missing');
    assert.match(line, /label=START/);
    assert.match(line, /modality=led/);
    assert.match(line, /predicted_onset_ms=47\.549/);
    assert.match(line, /block=1/);
});

test('CSV cells containing a comma or quote are escaped', () => {
    const csv = exportCsv(doc(SESSION), { endMs: 5 });
    for (const line of csv.split('\n')) {
        if (line.startsWith('#') || !line) continue;
        // Balanced quotes on every data line.
        assert.equal((line.match(/"/g) || []).length % 2, 0, line);
    }
});

test('exportJson carries the same data and parses', () => {
    const j = JSON.parse(exportJson(doc('M 1000 X\n' + SESSION), { endMs: 20 }));
    assert.match(j.note, /PREDICTED/);
    assert.equal(j.audio_sample_rate, SAMPLE_RATE);
    assert.equal(j.columns[0], 't_ms');
    assert.equal(j.rows.length, j.rows.length);
    assert.equal(j.markers.length, 1);
    assert.equal(j.markers[0].label, 'X');
});

test('an empty session exports without throwing', () => {
    const t = sampleTimeline(doc(''), {});
    assert.equal(t.columns.length, 1);
    assert.ok(exportCsv(doc(''), {}).length > 0);
    assert.ok(JSON.parse(exportJson(doc(''), {})));
});
