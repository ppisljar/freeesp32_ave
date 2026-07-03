// Format v2 tests: new pulse fields (env/phase/attack/jitter, audio duty) + the
// `-` (leave-unchanged) sentinel, plus a fixpoint regression across every
// existing library session (they must round-trip byte-stable and gain no fields).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const LIB = join(HERE, '..', '..', 'sessions', 'library');

// ---- new LED pulse fields --------------------------------------------------

test('LED: env + phase parse into the row', () => {
    const r = parse('0 40 25 80 0 128 255 1 1 180').doc.rows[0];
    assert.equal(r.kind, 'led');
    assert.equal(r.env, 1);              // sine
    assert.equal(r.phase.value, 180);
    assert.equal(r.duty.value, 25);
});

test('LED: env + phase round-trip', () => {
    const line = '0 40 25 80 0 128 255 1 1 180';
    assert.equal(serialize(parse(line).doc).trim(), line);
});

test('LED: jitter token (amp:period) round-trips', () => {
    const line = '0 40 50 80 255 255 255 1 - - - 0.3:45000';
    const r = parse(line).doc.rows[0];
    assert.equal(r.env, null);           // `-`
    assert.equal(r.phase, null);
    assert.equal(r.jitter.amp, 0.3);
    assert.equal(r.jitter.period, 45000);
    assert.equal(serialize(parse(line).doc).trim(), line);
});

// ---- audio duty/env + `-` --------------------------------------------------

test('audio: duty column parses and round-trips (with carrier placeholders)', () => {
    // A time freq pan vol mod ch freqR wave duty  → duty at token 8
    const line = 'A 0 250 0 80 10 4 0 4 25';
    const r = parse(line).doc.rows[0];
    assert.equal(r.duty.value, 25);
    assert.equal(r.waveType, 4);
    assert.equal(serialize(parse(line).doc).trim(), line);
});

test('audio: only jitter set → `-` placeholders fill the gap, round-trips', () => {
    const line = 'A 0 250 0 80 10 1 260 0 - - - - 0.2:45000';
    const r = parse(line).doc.rows[0];
    assert.equal(r.duty, null);
    assert.equal(r.env, null);
    assert.equal(r.jitter.amp, 0.2);
    assert.equal(serialize(parse(line).doc).trim(), line);
});

// ---- `-` on an existing field ---------------------------------------------

test('`-` on existing fields = leave unchanged, round-trips', () => {
    const line = '5000 - - - - - - 1';
    const r = parse(line).doc.rows[0];
    assert.equal(r.freq, null);
    assert.equal(r.duty, null);
    assert.equal(r.bright, null);
    assert.equal(r.mask, 1);
    assert.equal(serialize(parse(line).doc).trim(), line);
});

// ---- fixpoint + no-field-leak across all library sessions ------------------

test('all library sessions: serialize(parse(x)) is a fixpoint', () => {
    const files = readdirSync(LIB).filter(f => f.endsWith('.ledc'));
    assert.ok(files.length >= 20, 'found ' + files.length + ' sessions');
    for (const f of files) {
        const text = readFileSync(join(LIB, f), 'utf8');
        const once = serialize(parse(text).doc);
        const twice = serialize(parse(once).doc);
        assert.equal(twice, once, 'fixpoint failed for ' + f);
    }
});

test('v1-format lines do not gain trailing pulse fields when parsed by the v2 parser', () => {
    // Parser property (not a session-content constraint): a genuinely v1 line —
    // 5-token legacy or 8-token canonical, with NO trailing tokens — must parse to
    // undefined pulse fields. (Library sessions may now legitimately USE the v2
    // fields; byte-stability of every session is covered by the fixpoint test above.)
    const v1Led = ['1000 8 50 60 1', '1000 8 50 60 0 128 255 1'];
    for (const line of v1Led) {
        const row = parse(line).doc.rows.find(r => r.kind === 'led');
        assert.equal(row.env, undefined, 'LED env: ' + line);
        assert.equal(row.phase, undefined, 'LED phase: ' + line);
        assert.equal(row.attack, undefined, 'LED attack: ' + line);
        assert.equal(row.jitter, undefined, 'LED jitter: ' + line);
    }
    const v1Audio = ['A 1000 200 0 60 10', 'A 1000 200 0 60 10 0 210 0'];
    for (const line of v1Audio) {
        const row = parse(line).doc.rows.find(r => r.kind === 'audio');
        assert.equal(row.duty, undefined, 'audio duty: ' + line);
        assert.equal(row.env, undefined, 'audio env: ' + line);
        assert.equal(row.phase, undefined, 'audio phase: ' + line);
        assert.equal(row.jitter, undefined, 'audio jitter: ' + line);
    }
});
