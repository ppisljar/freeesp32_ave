// The Wizard's compiler must not delete entries that matter.
//
// views/wizard_compile.js rebuilds the whole timeline from its segment model on
// every commit, and coalesce() is licensed to delete rows it judges redundant.
// Three ways that licence was too broad, each of which silently shortened or
// flattened a session with `warnings` empty:
//
//   (1) the redundancy predicate was a hand-kept field list (the interpolatable
//       cells plus mask/channel/freq_r/wave_type), so a boundary whose ONLY
//       change was a format-v2 pulse field looked like a duplicate hold and was
//       deleted. The new value then survived only on the terminal off-row,
//       where brightness/volume is already 0, so the firmware never applied it.
//   (2) importField() rewrote a `-` cell to a hard 0, turning "leave brightness
//       alone" into "brightness 0" (ledc_format.md "One rule, two spellings":
//       the executor substitutes the live value for every clear present bit, and
//       "a cleared bit is never treated as 0").
//   (3) a trailing rest / dark segment was coalesced away, so the compiled
//       session ended at the previous boundary. The firmware declares the
//       session over one step past the LAST ENTRY (config_parser.c:2111-2124),
//       so the last timestamp IS the session's duration.
//
// Everything here goes through parse/serialize rather than poking row objects,
// because the bytes on disk are what the firmware sees.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';
import { audioRow, ledRow, cell } from '../src/js/gen/model.js';
import { field } from '../src/js/gen/field.js';
import { compileSession, sessionFromDoc } from '../src/js/gen/views/wizard_compile.js';

// ---- helpers ---------------------------------------------------------------

// What a Wizard commit produces: open the file in the Wizard, change nothing,
// write it back. Every defect below is reachable by "touch something unrelated".
function wizardCommit(text) {
    const { session } = sessionFromDoc(parse(text).doc);
    return serialize(compileSession(session, { withMeta: false }).doc);
}

function compile(session) {
    const out = compileSession(session, { withMeta: false });
    return { text: serialize(out.doc), warnings: out.warnings, totalEnd: out.totalEnd };
}

function timeline(text) {
    return parse(text).doc.rows.filter(r => r.kind === 'led' || r.kind === 'audio');
}
function lastEntryTime(text) {
    return timeline(text).reduce((m, r) => Math.max(m, r.time), 0);
}

// One light layer on all 8 channels, optionally carrying v2 pulse fields.
function light(bright, pulse) {
    return {
        id: 'glow', kind: 'light', channelMask: 0xFF,
        fields: {
            freq: field(10), duty: field(50), bright: field(bright),
            r: field(255), g: field(0), b: field(0),
        },
        pulse,
    };
}
function tone(vol, pulse) {
    return {
        id: 'lead', kind: 'tone', channel: 1, wave_type: null,
        fields: { freq: field(200), pan: field(0), volume: field(vol), mod: field(0) },
        pulse,
    };
}
// session([duration_ms, layers], ...) — the Wizard's model: no timestamps, just
// contiguous durations, so absolute time is their running sum.
const session = (...segments) => ({
    name: 'coalesce fixture', version: 1,
    segments: segments.map(([duration_ms, layers], i) => ({
        id: 's' + i, name: 'Segment ' + (i + 1), duration_ms, layers,
    })),
});

// ---- (1) a pulse-only change is not a redundant hold -----------------------

test('(1) a boundary whose only change is an LED pulse field is kept', () => {
    // env selects the flicker ENVELOPE (0 square .. 4 tremolo, ledc_format.md
    // "env"), so segment 2 is a different stimulus from segment 1 even though
    // every interpolatable cell matches. Segment 3 repeats segment 2 and SHOULD
    // still coalesce — the fix is a complete comparison, not a disabled one.
    const { text, warnings } = compile(session(
        [2000, [light(80, { env: 0 })]],
        [2000, [light(80, { env: 2 })]],
        [2000, [light(80, { env: 2 })]],
    ));
    const led = timeline(text);
    assert.deepEqual(led.map(r => r.time), [0, 2000, 6000],
        'the env change at 2000 survives; the identical hold at 4000 does not');
    assert.equal(led[1].env, 2, 'and it carries the new envelope');
    // The old predicate deleted the 2000 row and left env 2 reachable only on
    // the terminal off-row at 6000, where brightness is already 0.
    assert.equal(led[2].bright.value, 0, 'the 6000 row is the terminal off-row');
    assert.deepEqual(warnings, [], 'nothing about this needs warning about');
});

test('(1) a boundary whose only change is an audio pulse field is kept', () => {
    // duty is the isochronic gate width (ledc_format.md "duty"): 20% vs 80% at
    // the same carrier/volume is a different gate, not a duplicate hold.
    const { text } = compile(session(
        [2000, [tone(60, { duty: cell(20) })]],
        [2000, [tone(60, { duty: cell(80) })]],
    ));
    const a = timeline(text);
    assert.deepEqual(a.map(r => r.time), [0, 2000, 4000]);
    assert.equal(a[0].duty.value, 20);
    assert.equal(a[1].duty.value, 80, 'the duty change at 2000 survives');
});

// ---- (1b) the comparison cannot be forgotten for a FUTURE column -----------
//
// The bug above was caused by a list of fields kept in sync by hand, so the
// guard against it must not be another one. coalesce() now compares the rows'
// SERIALIZED bytes, and these two tests pin the other half of the same
// promise: every optional column the model can carry survives a Wizard commit,
// and the probe table below is asserted to COVER the model's optional columns,
// so adding a 15th column without teaching the compiler about it fails here
// rather than silently dropping the column (which is how `depth` vanished).

// field name -> a value distinctive enough to spot in the serialized line.
const AUDIO_PROBES = {
    duty: cell(33), env: 2, phase: cell(45), attack: cell(7),
    jitter: { amp: 12, period: 900 }, depth: cell(66),
};
const LED_PROBES = {
    env: 3, phase: cell(90), attack: cell(15), jitter: { amp: 0.5, period: null },
};
// Not columns of their own: both only change how the freq_r TOKEN renders
// (model.js freqRCell / freqRPresent), so they have no token to survive on.
const NOT_COLUMNS = new Set(['freqRCell', 'freqRPresent']);

// The model marks an absent optional column by leaving the key `undefined`
// (ledRow/audioRow assign `opts.x` straight through), which is what makes this
// enumeration possible without a second hand-kept list.
function optionalKeys(row) {
    return Object.keys(row).filter(k => row[k] === undefined && !NOT_COLUMNS.has(k));
}

test('(1b) the probe table covers every optional column the model carries', () => {
    assert.deepEqual(optionalKeys(audioRow({})).sort(), Object.keys(AUDIO_PROBES).sort(),
        'a new optional audio column needs a probe value in AUDIO_PROBES');
    assert.deepEqual(optionalKeys(ledRow({})).sort(), Object.keys(LED_PROBES).sort(),
        'a new optional LED column needs a probe value in LED_PROBES');
});

test('(1b) every optional column survives a Wizard commit', () => {
    const base = {
        audio: { time: 0, freq: cell(200), pan: cell(0), vol: cell(80), mod: cell(0), channel: 1 },
        led: { time: 0, freq: cell(10), duty: cell(50), bright: cell(80),
               r: cell(255), g: cell(0), b: cell(0), mask: 1 },
    };
    const cases = [
        ...Object.entries(AUDIO_PROBES).map(([k, v]) =>
            ['audio.' + k, audioRow(Object.assign({}, base.audio, { [k]: v }))]),
        ...Object.entries(LED_PROBES).map(([k, v]) =>
            ['led.' + k, ledRow(Object.assign({}, base.led, { [k]: v }))]),
    ];
    for (const [label, row] of cases) {
        const src = serialize({ rows: [row] });
        assert.equal(wizardCommit(src), src, label + ' survives byte-for-byte');
    }
});

// ---- (2) the `-` sentinel is not a 0 ---------------------------------------

// The repro from the field: "at 2 s change only the flicker rate". Every column
// the author left as `-` must stay `-`; rewriting them to 0 darkens the lamp at
// that row instead of holding its level.
const DASH_ONLY = [
    '0 10 50 80 255 255 255 255',
    '2000 14 - - - - - 255',
].join('\n') + '\n';

test('(2) a `-`-only row round-trips through the Wizard byte-for-byte', () => {
    assert.equal(wizardCommit(DASH_ONLY), DASH_ONLY);
    const row = timeline(wizardCommit(DASH_ONLY)).find(r => r.time === 2000);
    assert.ok(row, 'the row exists at all — rewriting `-` to 0 made it look like a terminal off-row');
    assert.equal(row.freq.value, 14, 'the flicker rate the author came to change');
    for (const f of ['duty', 'bright', 'r', 'g', 'b']) {
        assert.equal(row[f], null, f + ' stays the `-` sentinel, not 0');
    }
});

test('(2) rewriting `-` to 0 also cost the author their end-of-session entry', () => {
    // The second, compounding loss that was measured: with the `-`s flattened
    // the 2000 row matched the terminal off-row bit-for-bit, so coalesce()
    // deleted the author's explicit end-of-session entry and the session's last
    // timestamp moved from 6000 ms back to 2000 ms.
    const src = DASH_ONLY.trimEnd() + '\n6000 10 50 0 255 255 255 255\n';
    const out = wizardCommit(src);
    assert.equal(lastEntryTime(out), 6000, 'the session is still 6000 ms long');
    const row = timeline(out).find(r => r.time === 2000);
    assert.equal(row.duty, null, 'and the `-` is still a `-`');
});

// ---- (3) a trailing rest / dark segment sets the session's end time --------

test('(3) a trailing segment that only holds darkness keeps its duration', () => {
    // The author built 2 s lit + 4 s dark. The dark segment's row restates a
    // brightness that is already 0, so it reads as a redundant hold — but it is
    // the last entry, and the last entry is where the firmware ends the session
    // (config_parser.c:2111-2124). Deleting it compiled a 6000 ms session to
    // 2000 ms.
    const { text, totalEnd } = compile(session([2000, [light(80)]], [4000, [light(0)]]));
    assert.equal(totalEnd, 6000);
    assert.equal(lastEntryTime(text), 6000, 'the compiled timeline is as long as the session');
    assert.equal(timeline(text).find(r => r.time === 6000).bright.value, 0);
});

test('(3) a trailing segment with no layers at all keeps its duration', () => {
    // A pure rest emits nothing of its own: the previous layer's terminal
    // off-row sits at the rest's START, so without a session-end anchor the
    // compiled timeline ends there and the rest never happens.
    const { text, totalEnd } = compile(session([2000, [light(80)]], [4000, []]));
    assert.equal(totalEnd, 6000);
    assert.equal(lastEntryTime(text), 6000, 'the rest is held open by an entry at its end');
    assert.equal(timeline(text).find(r => r.time === 6000).bright.value, 0,
        'and it is the off state that is restated, so nothing moves');
});

test('(3) the end-time exception re-admits one row, not one per channel', () => {
    // The end time is a property of the FILE, not of a channel, so one entry at
    // it is enough. Keeping every channel's redundant hold there instead spent
    // 1-3 no-op entries on 9 of the shipped library sessions whose end time was
    // never at risk, against a hard cap of 100 (CONFIG_PARSER_MAX_ENTRIES,
    // config_parser.c:540-542).
    const src = [
        'A 0 200 0 80 0 1', 'A 0 300 0 80 0 2',
        'A 4000 200 0 80 0 1',   // ch1: a pure hold
        'A 4000 300 0 0 0 2',    // ch2: a real change, and it pins t=4000
    ].join('\n') + '\n';
    const out = timeline(wizardCommit(src));
    assert.equal(out.filter(r => r.time === 4000).length, 1,
        "ch2's real row already carries the end time, so ch1's hold stays dropped");
    assert.equal(out.filter(r => r.time === 4000)[0].channel, 2);
    assert.equal(lastEntryTime(serialize({ rows: out })), 4000);
});

test('(3) a redundant hold that is NOT the last entry is still dropped', () => {
    // coalesce() still has to earn its keep: the fixes above must not turn into
    // "never delete anything", or MAX_ENTRIES becomes reachable from an
    // ordinary session.
    const src = ['A 0 200 0 80 0 1',
                 'A 2500 200 0 80 0 1',   // pure echo of t=0
                 'A 5000 200 0 0 0 1'].join('\n') + '\n';
    assert.deepEqual(timeline(wizardCommit(src)).map(r => r.time), [0, 5000]);
});

// ---- all three defects at once ---------------------------------------------

// A row's state: which channel it addresses plus the bytes it writes, with the
// timestamp (and any note) removed. This is rowIdentity()'s notion of "the same
// row", which is what licenses coalesce() to delete one.
function states(text) {
    return new Set(parse(text).doc.rows
        .filter(r => r.kind === 'led' || r.kind === 'audio')
        .map(r => (r.kind === 'audio' ? 'a' + r.channel : 'l' + r.mask) + '|' +
            serialize({ rows: [Object.assign({}, r, { time: 0, inlineComment: '' })] }).trim()));
}

test('every state the author authored is still in the compile', () => {
    // The invariant the three fixes share: equal serialized bytes is the only
    // defensible licence to delete a row, because that is exactly when the file
    // — and therefore the firmware — cannot tell the difference. So every
    // DISTINCT state the author wrote has to come back out, on the same channel.
    //
    // One fixture, all three defects: ramps, an in-place pulse, `-` cells, a v2
    // phase/env pair, the v3 depth column, three channels and a trailing dark
    // boundary. The pulse columns are restated on every row of a channel the way
    // ledc_format.md tells authors to for positional columns, so the layer-wide
    // pulse the compiler re-emits matches what was authored.
    const src = [
        '0 >10 50 >0 255 100 0 15 1 0',
        'A 0 240 0 >0 0 1 0 0 - - - - - 66',
        'A 0 0 0 >0 0 3 0 5 20',
        '10000 10 50 ~25:30:10000 - - - 15 2 90',
        'A 10000 240 0 55 0 1 0 0 - - - - - 66',
        '60000 10 50 0 - - - 15 2 90',
        'A 60000 240 0 0 0 1 0 0 - - - - - 66',
        'A 60000 0 0 0 0 3 0 5 20',
    ].join('\n') + '\n';
    const first = wizardCommit(src);
    const got = states(first);
    const missing = [...states(src)].filter(s => !got.has(s));
    assert.deepEqual(missing, [], 'states the Wizard dropped');
    // Idempotent: a second commit must not keep shaving rows off.
    assert.equal(wizardCommit(first), first, 'a Wizard commit is idempotent');
    // Every line the compiler emitted re-parses, and the timeline ends where the
    // author's last entry does.
    assert.equal(parse(first).doc.rows.filter(r => r.kind === 'raw').length, 0,
        'every compiled line is still legal `.ledc`');
    assert.equal(lastEntryTime(first), 60000, 'the session kept its length');
});
