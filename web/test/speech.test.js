// Tests for `S` speech rows: parse, serialize, round-trip, device-filtering,
// and TTS text chunking.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize, serializeForDevice } from '../src/js/gen/serialize.js';
import { speechRow, audioRow, cell } from '../src/js/gen/model.js';
import { chunkText } from '../src/js/gen/tts.js';

test('parse S line: time voice volume "quoted text"', () => {
    const { doc } = parse('S 5000 en-US 80 "Relax and breathe"');
    const r = doc.rows.find(x => x.kind === 'speech');
    assert.ok(r);
    assert.equal(r.time, 5000);
    assert.equal(r.voice, 'en-US');
    assert.equal(r.volume, 80);
    assert.equal(r.text, 'Relax and breathe');
});

test('parse S: text may contain # and spaces (no inline-comment split)', () => {
    const { doc } = parse('S 0 en 100 "Step 1: inhale # deeply"');
    const r = doc.rows[0];
    assert.equal(r.kind, 'speech');
    assert.equal(r.text, 'Step 1: inhale # deeply');
});

test('serialize S round-trips', () => {
    const doc = { rows: [speechRow({ time: 1200, voice: 'sl', volume: 70, text: 'Pozdravljeni' })] };
    const text = serialize(doc);
    assert.equal(text.trim(), 'S 1200 sl 70 "Pozdravljeni"');
    const re = parse(text).doc.rows[0];
    assert.equal(re.kind, 'speech');
    assert.equal(re.time, 1200);
    assert.equal(re.voice, 'sl');
    assert.equal(re.volume, 70);
    assert.equal(re.text, 'Pozdravljeni');
});

test('serializeForDevice strips S rows but keeps audio', () => {
    const doc = { rows: [
        audioRow({ time: 0, channel: 1, freq: cell(200), vol: cell(50) }),
        speechRow({ time: 1000, voice: 'en', volume: 80, text: 'hi there' }),
    ] };
    const dev = serializeForDevice(doc);
    assert.ok(dev.indexOf('S ') === -1, 'no S line in device output');
    assert.ok(dev.indexOf('A ') === 0 || dev.indexOf('\nA ') >= 0 || dev.trim().startsWith('A'), 'audio kept');
    // Full serialize keeps it.
    assert.ok(serialize(doc).indexOf('S 1000 en 80 "hi there"') >= 0);
});

test('chunkText splits on word boundaries under the cap', () => {
    const words = Array.from({ length: 60 }, (_, i) => 'word' + i).join(' ');
    const chunks = chunkText(words, 40);
    assert.ok(chunks.length > 1);
    for (const c of chunks) assert.ok(c.length <= 40, 'chunk within cap: "' + c + '"');
    // No word is split across chunks (rejoining with spaces reproduces input).
    assert.equal(chunks.join(' '), words);
});

test('chunkText hard-splits an over-long single token', () => {
    const long = 'x'.repeat(100);
    const chunks = chunkText(long, 40);
    assert.ok(chunks.length >= 3);
    assert.equal(chunks.join(''), long);
});
