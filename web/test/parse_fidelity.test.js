// Parse fidelity: three spellings the parser used to reinterpret or delete.
//
// Each group pins a rule read out of main/config_parser.c, because in all three
// cases the "obvious" browser-side reading differs from what the device
// actually does with the bytes:
//
//   1. `A1000 …` — the firmware's audio branch drops the WHOLE first token
//      (config_parser.c:1544), so a glued timestamp is NOT the timestamp. The
//      generator must not quietly re-emit the line as a different, valid
//      looking command.
//   2. `mod=<unknown>` on a marker — the device warns and infers
//      (config_parser.c:174) without rejecting the line, so the row survives
//      and the author's spelling must survive with it.
//   3. a trailing `# comment` on an `S` row — the speech payload is read from
//      the RAW line (config_parser.c:1556) between the first and the LAST quote
//      (config_parser.c:1743/:1748), so '#' is only a comment delimiter AFTER
//      the closing quote.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';

const first = (text) => parse(text).doc.rows[0];
const diags = (text) => parse(text).diagnostics;

// ---------------------------------------------------------------------------
// 1. An audio prefix glued to its timestamp
// ---------------------------------------------------------------------------

test('A glued to its timestamp is held verbatim, not rewritten', () => {
    const line = 'A1000 240 0 60 0 1 250 0';
    const { doc, diagnostics } = parse(line + '\n');
    assert.equal(doc.rows.length, 1);
    assert.equal(doc.rows[0].kind, 'raw', 'must not become an audio row');
    assert.equal(doc.rows[0].text, line, 'bytes held exactly');
    assert.equal(serialize(doc), line + '\n', 'round-trips byte-identically');
    // The old behaviour: parseAudio never saw the glued digits, so the row
    // serialized as `A 240 0 60 0 1 250 0` — a different command, with the
    // author's 1000 ms gone and no diagnostic at all.
    assert.ok(!serialize(doc).includes('A 240'), 'no silent rewrite to t=240');
    assert.equal(diagnostics.length, 1);
    assert.equal(diagnostics[0].severity, 'error');
    assert.equal(diagnostics[0].line, 1);
    assert.match(diagnostics[0].msg, /lone "A"/);
    assert.match(diagnostics[0].msg, /1000/, 'names the token that is lost');
});

test('the lowercase and non-numeric glued forms are caught too', () => {
    // config_parser.c:1542 tests only the first BYTE, in either case, and
    // config_parser.c:1544 then throws the whole token away — so every spelling
    // longer than one character loses a field, not just a numeric one.
    for (const t of ['a1000', 'Audio', 'A-']) {
        const r = first(t + ' 240 0 60 0 1\n');
        assert.equal(r.kind, 'raw', t + ' must be held raw');
        assert.equal(r.text, t + ' 240 0 60 0 1');
    }
});

test('the correct spelling still parses, with the timestamp intact', () => {
    const r = first('A 1000 240 0 60 0 1 250 0\n');
    assert.equal(r.kind, 'audio');
    assert.equal(r.time, 1000);
    assert.equal(r.freq.value, 240);
    assert.equal(r.pan.value, 0);
    assert.equal(r.vol.value, 60);
    assert.equal(r.channel, 1);
    assert.equal(r.freqR, 250);
});

test('a lone A with too few fields keeps its own token-count error', () => {
    const d = diags('A 100 200\n');
    assert.equal(d.length, 1);
    assert.match(d[0].msg, /at least 5 tokens/, 'not the glued-prefix message');
});

// ---------------------------------------------------------------------------
// 2. An unrecognised marker modality
// ---------------------------------------------------------------------------

test('an unknown mod= value is kept verbatim instead of vanishing', () => {
    const { doc, diagnostics } = parse('M 100 STIM mod=sideways\n');
    const r = doc.rows[0];
    assert.equal(r.kind, 'marker', 'the device still runs the line');
    // Unchanged, and asserted here so the preservation cannot be "fixed" by
    // making an unknown value look like a real modality or an ordinary pair.
    assert.equal(r.modality, null, 'modality is still inferred at dispatch');
    assert.deepEqual(r.kv, [], 'still not stored as an ordinary pair');
    assert.equal(r.modalityRaw, 'sideways', 'but the spelling survives');
    assert.equal(diagnostics.length, 1);
    assert.equal(diagnostics[0].severity, 'warn', 'non-fatal: the line runs');
    assert.match(diagnostics[0].msg, /sideways/);
});

test('a known mod= value leaves modalityRaw empty', () => {
    for (const m of ['audio', 'led', 'both', 'none']) {
        const r = first('M 0 X mod=' + m + '\n');
        assert.equal(r.modality, m);
        assert.equal(r.modalityRaw, '');
    }
    assert.deepEqual(diags('M 0 X mod=both\n'), []);
});

test('an unknown mod= does not eat one of the four kv slots', () => {
    // config_parser.c:175 `continue`s out of the loop before the kv cap at
    // config_parser.c:177, so `mod=` never counts against the device's four
    // pairs. Parking an unknown value in `kv` would have evicted `d=4`, which
    // the device keeps.
    const r = first('M 0 X mod=sideways a=1 b=2 c=3 d=4\n');
    assert.deepEqual(r.kv.map(p => p.k + '=' + p.v), ['a=1', 'b=2', 'c=3', 'd=4']);
    assert.equal(r.modalityRaw, 'sideways');
});

// ---------------------------------------------------------------------------
// 3. A trailing inline comment on a speech row
//
// NOTE: serialize.js does not re-emit speechRow.inlineComment yet (its
// serializeSpeech has no `# ` tail, unlike serializeLed / serializeMarker), so
// these tests assert the PARSE side only. Completing the round-trip is a
// one-line serializer change, reported as a dependency.
// ---------------------------------------------------------------------------

test('a comment after the closing quote is split off', () => {
    const r = first('S 1000 en 80 "breathe in" # cue for the inhale\n');
    assert.equal(r.kind, 'speech');
    assert.equal(r.text, 'breathe in');
    assert.equal(r.inlineComment, 'cue for the inhale');
});

test('a # INSIDE the quotes stays spoken text', () => {
    const r = first('S 0 en 100 "Step 1: inhale # deeply"\n');
    assert.equal(r.text, 'Step 1: inhale # deeply');
    assert.equal(r.inlineComment, '');
});

test('a # inside the quotes and a comment after them are both honoured', () => {
    const r = first('S 0 en 80 "a # b" # c\n');
    assert.equal(r.text, 'a # b');
    assert.equal(r.inlineComment, 'c');
});

test('whitespace after the closing quote is not a comment', () => {
    const r = first('S 0 en 80 "hi"   \n');
    assert.equal(r.kind, 'speech');
    assert.equal(r.text, 'hi');
    assert.equal(r.inlineComment, '');
});

test('a quote inside the comment moves the closing quote — as on the device', () => {
    // THE AMBIGUOUS SPELLING, resolved by the C rather than guessed: q2 is
    // `strrchr(q1 + 1, '"')` (config_parser.c:1748), the LAST quote on the raw
    // line, so the device hears one long phrase and the `#` is part of it. We
    // read it identically, and because the first and last quote are re-emitted
    // where they stood the line is still byte-stable.
    const line = 'S 0 en 80 "hi" # he said "no"';
    const { doc } = parse(line + '\n');
    assert.equal(doc.rows[0].kind, 'speech');
    assert.equal(doc.rows[0].text, 'hi" # he said "no');
    assert.equal(doc.rows[0].inlineComment, '');
    assert.equal(serialize(doc), line + '\n', 'byte-stable despite the ambiguity');
});

test('non-comment text after the closing quote is held verbatim', () => {
    // The device ignores it (config_parser.c:1756 stops reading at the closing
    // quote) and the model has nowhere to put it, so it is neither executed nor
    // deleted: the line is kept exactly and flagged.
    const line = 'S 0 en 80 "hi" oops';
    const { doc, diagnostics } = parse(line + '\n');
    assert.equal(doc.rows[0].kind, 'raw');
    assert.equal(doc.rows[0].text, line);
    assert.equal(serialize(doc), line + '\n');
    assert.equal(diagnostics.length, 1);
    assert.match(diagnostics[0].msg, /oops/);
});

test('an ordinary quoted speech line is unaffected', () => {
    const line = 'S 5000 en-US 80 "Relax and breathe"';
    const { doc, diagnostics } = parse(line + '\n');
    assert.deepEqual(diagnostics, []);
    assert.equal(doc.rows[0].inlineComment, '');
    assert.equal(serialize(doc), line + '\n');
});
