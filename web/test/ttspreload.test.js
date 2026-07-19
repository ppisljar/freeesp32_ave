// Unit tests for the PURE TTS-preload extraction logic (no fetch / IndexedDB).
// Mirrors the plain-Node style of bgaudio.test.js.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { effVoice, extractPhrases } from '../src/js/gen/ttspreload.js';

// ---- effVoice: must mirror synthSpeech()'s mapping so cache keys line up ----

test('effVoice maps default/empty to the engine default, preserves explicit', () => {
    assert.equal(effVoice('puter', 'default'), 'en-US');
    assert.equal(effVoice('google', 'default'), 'en');
    assert.equal(effVoice('puter', ''), 'en-US');
    assert.equal(effVoice('google', undefined), 'en');
    assert.equal(effVoice('puter', 'en-GB'), 'en-GB');
});

// ---- extractPhrases: dedup across configs by engine|voice|text --------------

const A = `# session A
0 10 50 20 255 160 32 9 1 0
S 3000 default 65 "Breathe in."
A 0 200 0 50 6 1
S 60000 default 65 "Let go."
`;
const B = `# session B — shares "Breathe in." with A, adds an en-GB variant + empty
S 1000 default 65 "Breathe in."
S 2000 en-GB 70 "Breathe in."
S 5000 default 65 ""
`;

test('extractPhrases dedups shared phrases but keeps distinct voices', () => {
    const p = extractPhrases('puter', [A, B]);
    // en-US "Breathe in.", en-US "Let go.", en-GB "Breathe in." → 3 unique
    assert.equal(p.length, 3);
    assert.equal(p.filter(x => x.text === 'Breathe in.').length, 2); // en-US + en-GB
    assert.ok(p.some(x => x.text === 'Let go.'));
    assert.ok(!p.some(x => x.text === ''));   // empty-text S row ignored
});

test('extractPhrases: same engine + duplicate input collapses to unique set', () => {
    const p = extractPhrases('google', [A, A]);
    assert.equal(p.length, 2);   // "Breathe in." + "Let go."
    for (const x of p) assert.ok(x.key.startsWith('google|en|'));
});

test('extractPhrases: no speech rows → empty', () => {
    assert.equal(extractPhrases('puter', ['# just a comment\n0 10 50 20 255 0 0 9 1 0\n']).length, 0);
});
