// Lane-model handling of the `-` sentinel, and the one-click interior-LFO snap.
//
// Five defects, all of which lost data the author wrote. (1) and (2) are the
// original pair; (3)-(5) were found by an adversarial verifier afterwards and
// are documented at their own section headers further down:
//
//   (3) two rows at one (lane, time) collapsed into one, materialising the
//       sentinels of the row that was deleted — and re-pointing the sweeps that
//       targeted it;
//   (4) coalesceRampKeys could delete the keyframe sitting at a held time,
//       after which the hold won and the row went back to `-`;
//   (5) the freq_r present bit (and the wave_type token beside it) was dropped,
//       so "leave the right carrier alone" came back as "go mono".
//
// Still open, deliberately, and visible in two tests below as a comment rather
// than an assertion: a row that re-states its lane's previous values exactly is
// coalesced away entirely, which for a TRAILING row shortens the session. The
// wizard re-admits those (wizard_compile.js); the lane view does not.
//
// The original pair:
//
// (1) CRASH. A lone `-` parses to a null Cell (parse.js parseCell: "`-` sentinel
//     = leave unchanged"), and reconstructLanes used to feed every core cell
//     straight to cellToKeyframe -> interpToShape(c.interp) -> TypeError. Any
//     session that uses the documented sentinel and has no `# @ave-lane`
//     snapshot therefore threw inside docToLanes, which lane.js calls from
//     show() with no try/catch after the previous view is already hidden: a
//     blank generator panel and a console error with nothing in the UI.
//     11 of the 99 shipped sessions crashed, four of them curated library ones.
//
//     The repair is NOT a null guard that invents a value. A `-` leaves the
//     field's present bit clear (config_parser.c:1598-1600 LED, :1777-1800
//     audio) and the execute path substitutes the channel's LIVE value (LED:
//     :2994-3014, audio: :2645-2650) — whatever the device had actually reached.
//     The editor cannot compute that, so the sentinel gets no keyframe at all:
//     its TIME rides in the sublane's `hold` list and is projected back as `-`.
//
// (2) DESTRUCTIVE SNAP. snapToBandBoundaries moved each conflicting keyframe to
//     the nearer band boundary with no collision check, so two keyframes in one
//     band collapsed onto one timestamp (and lanesToDoc emits one row per lane
//     per time, so one of them vanished), and a band spanning the session made
//     t=0 the nearest boundary for everything — dragging keyframes onto the
//     opening entry and overwriting it.
//
// Covers all five, plus the four curated library sessions, test2a.ledc and
// t14_batch_cap.ledc as named regressions.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';
import {
    docToLanes, lanesToDoc, reconstructLanes, snapToBandBoundaries, keyframe,
    coalesceRampKeys,
} from '../src/js/gen/views/lane_serialize.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const SESSIONS = join(HERE, '..', '..', 'sessions');

// The exact pair of lines that crashed the Lanes view: an opening entry, then a
// row that bumps the flicker rate and leaves duty / brightness / RGB alone.
const SENTINEL_PAIR = '0 10 50 80 255 255 255 1\n2000 14 - - - - - 1\n';

// A lane projection commit, as lane.js does it (lanesToDoc after docToLanes).
function laneCommit(text, opts) {
    return serialize(lanesToDoc(docToLanes(parse(text).doc), opts || { withMeta: false }).doc);
}

// Every timeline entry line, inline comment stripped, sorted. Byte identity is
// the wrong comparison for a lane round-trip: lanesToDoc deliberately re-orders
// rows by (time, lane index), so a file that interleaves LED and audio at one
// timestamp legitimately comes back in a different order. What must never
// change is WHICH entries exist.
function entryMultiset(text) {
    const m = new Map();
    for (const line of text.split('\n')) {
        const l = line.replace(/#.*$/, '').trim();
        if (!l || /^[SM] /.test(l) || /^BG /.test(l)) continue;
        m.set(l, (m.get(l) || 0) + 1);
    }
    return m;
}

function assertSameEntries(before, after, label) {
    const b = entryMultiset(before), a = entryMultiset(after);
    for (const k of new Set([...b.keys(), ...a.keys()])) {
        assert.equal(a.get(k) || 0, b.get(k) || 0,
            label + ': entry count changed for ' + JSON.stringify(k) +
            ' (' + (b.get(k) || 0) + ' -> ' + (a.get(k) || 0) + ')');
    }
}

function ledcFiles(dir, out = []) {
    for (const e of readdirSync(dir)) {
        const p = join(dir, e);
        if (statSync(p).isDirectory()) ledcFiles(p, out);
        else if (e.endsWith('.ledc')) out.push(p);
    }
    return out;
}

// ---- (1) the `-` sentinel --------------------------------------------------

test('sentinel: docToLanes does not throw on a lone `-`', () => {
    // Pre-fix this threw TypeError: Cannot read properties of null
    // (reading 'interp') out of cellToKeyframe.
    const lanes = docToLanes(parse(SENTINEL_PAIR).doc);
    assert.equal(lanes.lanes.length, 1);
    assert.equal(lanes.lanes[0].kind, 'led');
});

test('sentinel: a held field gets a hold time, never an invented keyframe', () => {
    const lane = reconstructLanes(parse(SENTINEL_PAIR).doc).lanes[0];
    // freq states a value on both rows, so it keyframes both.
    assert.deepEqual(lane.sub.freq.keys.map(k => [k.t, k.v]), [[0, 10], [2000, 14]]);
    // duty/bright/RGB are `-` on the second row: the time is held, and there is
    // NO keyframe at 2000 — materialising one would hard-code a number the
    // author never wrote (the Wizard's separate confirmed bug).
    for (const f of ['duty', 'bright', 'r', 'g', 'b']) {
        assert.deepEqual(lane.sub[f].hold, [2000], f + ' should hold t=2000');
        assert.equal(lane.sub[f].keys.some(k => k.t === 2000), false,
            f + ' must not materialise a keyframe at the held time');
    }
});

test('sentinel: lanesToDoc emits `-` back, byte for byte', () => {
    assert.equal(laneCommit(SENTINEL_PAIR), SENTINEL_PAIR);
});

test('sentinel: `hold` is absent when a session uses no sentinel', () => {
    // The `# @ave-lane` snapshot is compared byte-wise by the save path, so a
    // session with no `-` must serialize exactly as it did before the hold
    // mechanism existed.
    const lane = reconstructLanes(parse('0 10 50 80 255 255 255 1\n').doc).lanes[0];
    for (const f of ['freq', 'duty', 'bright', 'r', 'g', 'b']) {
        assert.equal(Object.prototype.hasOwnProperty.call(lane.sub[f], 'hold'), false,
            f + ' should carry no `hold` key');
    }
});

test('sentinel: a row whose every core field is `-` survives the projection', () => {
    // The row carries only a pulse field (jitter). Its time exists in no
    // sublane's `keys` at all, so laneUnionTimes has to pick it up from `hold`
    // or the entry disappears from the file.
    const text = '0 10 50 80 255 255 255 1\n5000 - - - - - - 1 0 0 0 7\n';
    assert.equal(laneCommit(text), text);
});

test('sentinel: a real keyframe at a held time wins over the hold', () => {
    // Dropping a keyframe onto a held row is the user deliberately replacing
    // the `-` with a value; the edit must survive the next projection.
    const lanes = docToLanes(parse(SENTINEL_PAIR).doc);
    lanes.lanes[0].sub.bright.keys.push(keyframe(2000, 33, 'step'));
    const out = serialize(lanesToDoc(lanes, { withMeta: false }).doc);
    assert.equal(out, '0 10 50 80 255 255 255 1\n2000 14 - 33 - - - 1\n');
});

test('sentinel: audio `-` round-trips on every core field', () => {
    // Audio takes the same path through buildCellSublane; `-` on pan/vol/mod
    // clears their present bits at config_parser.c:1783-1800.
    const text = 'A 0 240 0 50 0 1\nA 2000 - - - - 1\n';
    assert.equal(laneCommit(text), text);
});

// ---- the shipped sessions --------------------------------------------------

test('sessions: all 99 shipped sessions import into lanes without throwing', () => {
    const files = ledcFiles(SESSIONS);
    assert.ok(files.length >= 99, 'found ' + files.length + ' sessions');
    const threw = [];
    for (const f of files) {
        try { docToLanes(parse(readFileSync(f, 'utf8')).doc); }
        catch (e) { threw.push(f.replace(SESSIONS + '/', '') + ': ' + e.message); }
    }
    // Pre-fix: 11 of these threw, blanking the generator panel on open.
    assert.deepEqual(threw, []);
});

// The four curated library sessions the crash took out. All four drive two LED
// banks in antiphase and write `- - -` for R/G/B to leave the bank colour
// alone, so they are exactly the case the sentinel exists for.
const CURATED = [
    '21_gateway_focus12_SINE.ledc',
    '21_gateway_focus12_TRAP.ledc',
    '25_gateway_journey_SINE.ledc',
    '25_gateway_journey_TRAP.ledc',
];

for (const name of CURATED) {
    test('sessions: ' + name + ' round-trips through the lane model', () => {
        const text = readFileSync(join(SESSIONS, 'library', name), 'utf8');
        const before = serialize(parse(text).doc);
        assert.ok(/(^|\s)-(\s|$)/m.test(before), name + ' should use the `-` sentinel');
        const after = laneCommit(text);
        assertSameEntries(before, after, name);
        // And specifically: no `-` was replaced by a number.
        const dashes = (s) => (s.match(/(?<=\s)-(?=\s|$)/gm) || []).length;
        assert.equal(dashes(after), dashes(before),
            name + ': the number of `-` tokens changed');
    });
}

// ---- (2) snapToBandBoundaries ---------------------------------------------

// One LED lane with sensible defaults, overridden per test.
function ledLanes(sub) {
    return {
        version: 1,
        lanes: [{
            kind: 'led', key: 1, name: 'LED ch 1', collapsed: false, colorSplit: false,
            sub: Object.assign({
                freq:   { keys: [keyframe(0, 10, 'step')] },
                duty:   { keys: [keyframe(0, 50, 'step')] },
                bright: { keys: [keyframe(0, 20, 'step')] },
                r: { keys: [keyframe(0, 255, 'step')] },
                g: { keys: [keyframe(0, 255, 'step')] },
                b: { keys: [keyframe(0, 255, 'step')] },
            }, sub),
        }],
    };
}

// Every (field, time) pair of a lane, so a collapse is visible as a duplicate.
function keyTimes(lane, field) {
    return (lane.sub[field].keys || []).map(k => k.t);
}

test('snap: two keyframes in one band are never collapsed onto one timestamp', () => {
    // Case (a). bright runs an LFO over [1000, 9000); freq has keyframes at
    // 3000 and 4000, both in the band's first half, so both used to pick t=1000
    // and the 3000 one vanished from the file.
    const before = ledLanes({
        freq: { keys: [keyframe(0, 10, 'step'), keyframe(3000, 40, 'step'),
                       keyframe(4000, 60, 'step')] },
        bright: { keys: [keyframe(1000, 20, 'sine', 60, 2000), keyframe(9000, 0, 'step')] },
    });
    const res = snapToBandBoundaries(before);
    const lane = res.lanes.lanes[0];
    const times = keyTimes(lane, 'freq');
    assert.equal(new Set(times).size, times.length,
        'two freq keyframes share a timestamp: ' + times.join(','));
    // All three authored freq values still reach the file.
    assert.deepEqual(lane.sub.freq.keys.map(k => k.v).sort((x, y) => x - y), [10, 40, 60]);
    // The one it could not place is reported rather than forced.
    assert.equal(res.snapped.length, 1);
    assert.deepEqual(res.skipped, [{ laneKey: 1, field: 'freq', t: 4000, reason: 'occupied' }]);
    assert.ok(res.warnings.some(w => /left alone/.test(w)), 'expected a warning line');
});

test('snap: a band that starts at 0 does not drag keyframes onto the opening entry', () => {
    // Case (b). bright runs an LFO over [0, 600000) — most of the session — so
    // t=0 was the nearest boundary for a keyframe at 1500 ms, and the move
    // overwrote the freq the opening entry set.
    const before = ledLanes({
        freq: { keys: [keyframe(0, 10, 'step'), keyframe(1500, 40, 'step')] },
        bright: { keys: [keyframe(0, 20, 'sine', 60, 10000), keyframe(600000, 0, 'step')] },
    });
    const res = snapToBandBoundaries(before);
    const lane = res.lanes.lanes[0];
    assert.deepEqual(lane.sub.freq.keys.map(k => [k.t, k.v]), [[0, 10], [1500, 40]],
        'the opening entry and the conflicting keyframe must both survive unchanged');
    assert.deepEqual(res.snapped, []);
    assert.deepEqual(res.skipped, [{ laneKey: 1, field: 'freq', t: 1500, reason: 'occupied' }]);

    // And the projected file still contains both entries.
    const rows = lanesToDoc(res.lanes, { withMeta: false }).doc.rows
        .filter(r => r.kind === 'led');
    assert.deepEqual(rows.map(r => r.time), [0, 1500, 600000]);
    assert.equal(rows[0].freq.value, 10);
});

test('snap: a keyframe is never pushed past the band midpoint', () => {
    // Refusing to destroy data is not enough on its own: with t=0 occupied the
    // only other boundary of a 0..600000 band is 600000, and relocating an
    // event from 1500 ms to ten minutes in is a different session, not a fix.
    // Only the nearer boundary is ever a destination.
    const before = ledLanes({
        freq: { keys: [keyframe(0, 10, 'step'), keyframe(1500, 40, 'step')] },
        bright: { keys: [keyframe(0, 20, 'sine', 60, 10000), keyframe(600000, 0, 'step')] },
    });
    const res = snapToBandBoundaries(before);
    for (const m of res.snapped) {
        assert.ok(Math.abs(m.to - m.from) <= 300000,
            'moved ' + m.from + ' -> ' + m.to + ', past the band midpoint');
    }
    assert.deepEqual(res.snapped, []);
});

test('snap: a free nearer boundary is still snapped (the fix still fixes)', () => {
    const before = ledLanes({
        freq: { keys: [keyframe(0, 10, 'step'), keyframe(1200, 40, 'step')] },
        bright: { keys: [keyframe(1000, 20, 'sine', 60, 2000), keyframe(9000, 0, 'step')] },
    });
    const res = snapToBandBoundaries(before);
    assert.deepEqual(res.snapped, [{ laneKey: 1, field: 'freq', from: 1200, to: 1000 }]);
    assert.deepEqual(res.skipped, []);
    assert.deepEqual(res.warnings, []);
    assert.deepEqual(keyTimes(res.lanes.lanes[0], 'freq'), [0, 1000]);
});

test('snap: a `-` on the boundary row is not overwritten with a number', () => {
    // The destination row leaves freq unchanged. Landing a keyframe there would
    // replace the sentinel with a value — the materialisation it exists to
    // prevent — so the move is refused.
    const before = ledLanes({
        freq: { keys: [keyframe(0, 10, 'step'), keyframe(1200, 40, 'step')], hold: [1000] },
        bright: { keys: [keyframe(1000, 20, 'sine', 60, 2000), keyframe(9000, 0, 'step')] },
    });
    const res = snapToBandBoundaries(before);
    assert.deepEqual(res.snapped, []);
    assert.deepEqual(res.skipped, [{ laneKey: 1, field: 'freq', t: 1200, reason: 'held' }]);
    assert.ok(res.warnings.some(w => /leaves that field unchanged/.test(w)));
});

test('snap: returns a new lanes object and never mutates the input', () => {
    const before = ledLanes({
        freq: { keys: [keyframe(0, 10, 'step'), keyframe(1200, 40, 'step')] },
        bright: { keys: [keyframe(1000, 20, 'sine', 60, 2000), keyframe(9000, 0, 'step')] },
    });
    const snapshot = JSON.stringify(before);
    const res = snapToBandBoundaries(before);
    assert.equal(JSON.stringify(before), snapshot, 'input was mutated');
    assert.notEqual(res.lanes, before);
    // The caller needs all four keys to report what happened (lane.js).
    assert.deepEqual(Object.keys(res).sort(), ['lanes', 'skipped', 'snapped', 'warnings']);
});

// ---------------------------------------------------------------------------
// The snap button's call-site contract.
//
// snapToBandBoundaries grew a report wrapper ({ lanes, snapped, skipped,
// warnings }) so it could REFUSE a destructive move instead of silently
// destroying an entry. Its only caller, lane.js, kept doing
// `lanes = snapToBandBoundaries(lanes)` — assigning the report to the model —
// so the next commit threw `TypeError: laneList.forEach is not a function`
// before `applyingEdit = true`, leaving the model corrupted, the doc
// un-updated and no repaint. Silent data loss traded for a hard crash, and no
// test noticed because every test called the pure function directly and never
// fed its result onward.
//
// This pins the shape AND the handoff. lane.js is DOM-bound, so what is
// exercised here is the contract it depends on, not the click itself.
// ---------------------------------------------------------------------------

test('snapToBandBoundaries returns a report whose .lanes is consumable by lanesToDoc', () => {
    const doc = parse([
        '0 10 50 80 255 0 0 1',
        '1500 20 50 80 255 0 0 1',
        '3000 ^30 50 80 255 0 0 1',
        '5000 40 50 80 255 0 0 1',
    ].join('\n') + '\n').doc;

    const res = snapToBandBoundaries(docToLanes(doc));

    // The shape the caller must destructure.
    assert.ok(res && typeof res === 'object', 'returns an object');
    for (const k of ['lanes', 'snapped', 'skipped', 'warnings']) {
        assert.ok(k in res, 'report carries .' + k);
    }
    assert.ok(Array.isArray(res.lanes.lanes), '.lanes is a lane MODEL, not the report');
    assert.ok(Array.isArray(res.snapped) && Array.isArray(res.skipped), 'snapped/skipped are arrays');
    assert.ok(Array.isArray(res.warnings), 'warnings is an array');

    // The handoff that crashed: the model goes straight into lanesToDoc.
    const out = lanesToDoc(res.lanes, { withMeta: true });
    assert.ok(Array.isArray(out.doc.rows) && out.doc.rows.length > 0,
        'the snapped model still projects to a doc');

    // Passing the REPORT where the model belongs must fail loudly rather than
    // emit a silently empty session — this is what the crash actually was.
    assert.throws(() => lanesToDoc(res, { withMeta: true }),
        'handing lanesToDoc the report is a programming error, not a no-op');
});

test('a refused snap reports instead of destroying the blocked keyframe', () => {
    // Two interior points that would land on the same boundary. The projection
    // emits one row per (lane, time), so moving both would delete one.
    const doc = parse([
        '0 ^40 50 80 255 0 0 1',
        '1000 10 50 80 255 0 0 1',
        '2000 20 50 80 255 0 0 1',
        '9000 30 50 80 255 0 0 1',
    ].join('\n') + '\n').doc;

    const before = docToLanes(doc);
    const countKeys = (m) => (m.lanes || []).reduce((n, ln) =>
        n + Object.keys(ln.sub || {}).reduce((q, f) =>
            q + ((ln.sub[f] && ln.sub[f].keys) || []).length, 0), 0);

    const res = snapToBandBoundaries(before);

    // Whatever it decides, it must never end up with fewer keyframes than it
    // started with: a snap moves points, it does not consume them.
    assert.equal(countKeys(res.lanes), countKeys(before),
        'no keyframe is destroyed by snapping');
    // And every refusal is accounted for in words the strip can show.
    assert.equal(res.skipped.length > 0, res.warnings.length > 0,
        'a skipped move is always reported');
});

test('lane.js consumes the snap report, and does not assign it to the model', () => {
    // The contract test above pins the shape; this pins the one CALLER, which
    // is where the crash actually lived. lane.js is DOM-bound and cannot be
    // imported here, so this reads its source — the same tactic
    // save_as.test.js uses for community.js's private contentHash().
    const src = readFileSync(join(HERE, '..', 'src', 'js', 'gen', 'views', 'lane.js'), 'utf8');
    assert.match(src, /snapToBandBoundaries\s*\(/, 'lane.js still offers the snap fix');
    assert.doesNotMatch(src, /\blanes\s*=\s*snapToBandBoundaries\s*\(/,
        'lane.js assigns the snap REPORT where a lane model belongs — the next '
        + 'commit throws "laneList.forEach is not a function"');
    assert.match(src, /\.snapped\b/,
        'lane.js must check .snapped so a fully-refused snap is not a silent no-op');
});

// ===========================================================================
// (3) THE FOURTH WAY A `-` BECAME A NUMBER: two rows at one (lane, time).
// ===========================================================================
//
// The three mechanisms above all PRESERVE the sentinel. This one materialised
// it, which is the harm the whole `hold` mechanism exists to prevent.
//
// The lane projection emits exactly one row per (lane, time) — laneUnionTimes is
// a SET of times and emitCellAt resolves one value per time. That is a real
// constraint, not a bug. But a `.ledc` file may legally put two rows on one
// channel at one timestamp, and reconstructLanes used to drop them into the same
// lane, where the second row's keyframes simply overwrote the first row's. In
// sessions/library/test2a.ledc that deleted line 54, `A 30000 - - - 12 4`, and
// materialised its three sentinels as 440 / -100 / 45 from line 64,
// `A 30000 440 -100 45 0 4`.
//
// DEVICE-VISIBLE, and not only through the sentinels. The row COUNT is
// load-bearing: find_next_audio_for_bit returns the FIRST later same-channel
// entry (config_parser.c:2471) as a sweep's target, and the parse-time insertion
// sort breaks ties with `<=` so equal timestamps keep file order
// (config_parser.c:572). So `A 20000 294 0 50 >4 4` aimed its mod ramp at mod 12
// and, with that row gone, aimed at mod 0 — a 10 s 4->12 Hz AM ramp became
// 4->0 Hz. No field-by-field merge can repair that: the device needs TWO
// entries. The group is therefore SPLIT across lanes that share a key, which
// keeps both rows, in order.

// A ch4 mod ramp whose target row holds freq/pan/vol with `-`, plus the second
// ch4 row that shares its timestamp. The shape of test2a.ledc lines 53/54/64.
const OVERLAP_PAIR = [
    'A 20000 294 0 50 >4 4',
    'A 30000 - - - 12 4',
    'A 30000 440 -100 45 0 4',
].join('\n') + '\n';

test('overlap: two rows at one (lane, time) both survive, sentinels and all', () => {
    // Pre-fix the middle line vanished and its `- - -` came back as 440/-100/45.
    assert.equal(laneCommit(OVERLAP_PAIR), OVERLAP_PAIR);
});

test('overlap: the sweep target stays the FIRST row at the shared timestamp', () => {
    // What the firmware actually reads (config_parser.c:2471): the first later
    // same-channel entry. Order among equal timestamps must be file order, so
    // the `-`/mod-12 row has to come back BEFORE the 440 Hz row or the ramp
    // silently re-targets mod 0.
    const out = laneCommit(OVERLAP_PAIR).trim().split('\n');
    assert.deepEqual(out, ['A 20000 294 0 50 >4 4',
                           'A 30000 - - - 12 4',
                           'A 30000 440 -100 45 0 4']);
    const rows = lanesToDoc(docToLanes(parse(OVERLAP_PAIR).doc), { withMeta: false })
        .doc.rows.filter(r => r.kind === 'audio' && r.time === 30000);
    assert.equal(rows.length, 2, 'both entries reach the timeline');
    assert.equal(rows[0].mod.value, 12, 'the ramp target is the row that sets mod 12');
    assert.equal(rows[0].freq, null, 'and it still leaves freq unchanged');
});

test('overlap: the extra row becomes a lane of its own, sharing the key', () => {
    const lanes = reconstructLanes(parse(OVERLAP_PAIR).doc);
    assert.equal(lanes.lanes.length, 2, 'one ordinary lane plus one overlap lane');
    assert.deepEqual(lanes.lanes.map(l => l.key), [4, 4], 'both are channel 4');
    // Slot order is file order, so the primary lane holds the FIRST row of each
    // timestamp and projects before the overlap lane.
    assert.deepEqual(lanes.lanes[0].sub.mod.keys.map(k => [k.t, k.v]),
                     [[20000, 4], [30000, 12]]);
    assert.deepEqual(lanes.lanes[0].sub.freq.hold, [30000],
                     'the sentinel is still a hold, not a value');
    assert.deepEqual(lanes.lanes[1].sub.freq.keys.map(k => [k.t, k.v]), [[30000, 440]]);
    assert.ok(/overlap/.test(lanes.lanes[1].name),
              'the overlap lane is named apart: ' + lanes.lanes[1].name);
});

test('overlap: a lane group with no shared timestamp is untouched', () => {
    // The split must be invisible to the overwhelmingly common case, or every
    // session's `# @ave-lane` snapshot changes shape.
    const lanes = reconstructLanes(parse('A 0 200 0 80 0 1\nA 5000 300 0 80 0 1\n').doc);
    assert.equal(lanes.lanes.length, 1);
    assert.equal(lanes.lanes[0].name, 'Audio ch 1');
});

test('overlap: the projection reports it instead of hiding it', () => {
    const res = lanesToDoc(docToLanes(parse(OVERLAP_PAIR).doc), { withMeta: false });
    assert.ok(res.warnings.some(w => /two or more rows on one channel\/mask/.test(w)),
        'expected an overlap warning, got ' + JSON.stringify(res.warnings));
});

test('sessions: test2a.ledc keeps all 51 of its `-` tokens', () => {
    // The named regression. Measured on the file as shipped: 51 `-` tokens, of
    // which three (line 54's freq/pan/vol) used to become 440 / -100 / 45.
    const text = readFileSync(join(SESSIONS, 'library', 'test2a.ledc'), 'utf8');
    const before = serialize(parse(text).doc);
    const after = laneCommit(text);
    const dashes = (s) => (s.match(/(?<=\s)-(?=\s|$)/gm) || []).length;
    assert.equal(dashes(before), 51, 'the fixture still has 51 sentinels');
    assert.equal(dashes(after), 51, 'a lane round-trip materialised one');
    assertSameEntries(before, after, 'test2a.ledc');
    // And the two ch4 rows at t=30000 are still two rows, in that order.
    const ch4 = after.split('\n').map(l => l.replace(/\s*#.*$/, ''))
        .filter(l => /^A 30000 .*\s4$/.test(l));
    assert.deepEqual(ch4, ['A 30000 - - - 12 4', 'A 30000 440 -100 45 0 4']);
});

test('sessions: t14_batch_cap.ledc keeps all 54 entries at t=1000', () => {
    // The pathological case, and the reason the split cannot be capped: this
    // fixture exists to trip the firmware's 50-per-timestamp batch cap
    // (config_parser.c:572 keeps the order the cap then truncates), so dropping
    // the duplicates would delete the very thing under test. 54 entries share
    // t=1000; 49 of them are byte-identical on mask 3.
    const text = readFileSync(join(SESSIONS, 'test', 't14_batch_cap.ledc'), 'utf8');
    const before = serialize(parse(text).doc);
    const after = laneCommit(text);
    const at1000 = (s) => s.split('\n').filter(l => /^1000 /.test(l)).sort();
    assert.equal(at1000(before).length, 54, 'the fixture still batches 54 entries');
    // Line for line, not just the count: the 49 identical mask-3 rows and the
    // four mask-12 rows that the cap drops must all still be there.
    assert.deepEqual(at1000(after), at1000(before));
    // NOT assertSameEntries: this file also loses its trailing
    // `13000 -1 50 0 0 0 0 15` row to the redundant-constant-step coalesce,
    // which is a different (and still open) mechanism — see the module header.
});

// ===========================================================================
// (4) coalesceRampKeys MUST NOT DELETE THE KEYFRAME AT A HELD TIME.
// ===========================================================================
//
// isHeldAt documents the precedence: "a real keyframe at exactly the same time
// always wins over the hold". coalesceRampKeys can repeal that by deletion. Its
// redundant-constant-step rule drops a step keyframe whose value the previous
// one already holds — and if that keyframe was the one outvoting a hold, the
// hold starts winning again and the row is re-emitted as `-`. Every other drop
// in that function is value-preserving (the value stays implied by the
// surrounding curve); this one hands the field back to whatever the device had
// reached, which is the materialisation-by-deletion the sentinel forbids.
//
// Tested at the function level because that is where the invariant lives. The
// reconstruction path can no longer reach it — a single row contributes either a
// key or a hold for a field, and the one way to get both at one time was two
// rows at one (lane, time), which the split above now separates into different
// lanes. So this is the guard on the exported contract, not a live repro.

test('coalesce: a keyframe at a held time is never dropped as redundant', () => {
    const keys = [keyframe(0, 50, 'step'), keyframe(2000, 50, 'step')];
    // Without `hold` the second keyframe IS redundant and is dropped (the
    // behaviour view_passthrough.test.js relies on for its orphan-note case).
    assert.deepEqual(coalesceRampKeys(keys).map(k => k.t), [0]);
    // With a `-` at 2000 it must stay, or isHeldAt flips and the row goes back
    // to `-`.
    assert.deepEqual(coalesceRampKeys(keys, [2000]).map(k => k.t), [0, 2000]);
});

test('coalesce: a held time also survives the on-the-curve ramp rule', () => {
    // The other drop rule, same harm: 1000 lies exactly on the 0->2000 line, so
    // it is removed as a resampling artefact — unless a `-` sits there.
    const keys = [keyframe(0, 0, 'lin'), keyframe(1000, 50, 'lin'),
                  keyframe(2000, 100, 'step')];
    assert.deepEqual(coalesceRampKeys(keys).map(k => k.t), [0, 2000]);
    assert.deepEqual(coalesceRampKeys(keys, [1000]).map(k => k.t), [0, 1000, 2000]);
});

test('coalesce: the keyframe-wins-over-hold rule survives a projection', () => {
    // A forward guard, not a repro: this passes on the pre-fix code too, because
    // the committed row states a value and so re-imports as a keyframe with no
    // hold beside it. It is here to catch the end-to-end regression if either
    // half of the rule is ever changed — the user drops a value onto a held row
    // (the existing test above), commits, and the value must still be there
    // after the next import rebuilds the sublanes through the coalesce.
    const lanes = docToLanes(parse(SENTINEL_PAIR).doc);
    lanes.lanes[0].sub.bright.keys.push(keyframe(2000, 80, 'step'));
    // 80 is exactly what t=0 sets, so the new keyframe is "redundant" by value.
    const once = serialize(lanesToDoc(lanes, { withMeta: false }).doc);
    assert.equal(once, '0 10 50 80 255 255 255 1\n2000 14 - 80 - - - 1\n');
    assert.equal(laneCommit(once), once, 'the value survives the next round-trip');
});

// ===========================================================================
// (5) THE freq_r PRESENT BIT, AND THE WAVE TOKEN NEXT TO IT.
// ===========================================================================
//
// Both columns are "absent / `-` => leave the live value alone" fields: freq_r
// is written only under AUD_SET_FREQR (config_parser.c:1182, set at :1834) and
// wave_type only under has_wave_type (:1174, set at :1849). An explicit value is
// a real write — freq_r 0 collapses the channel to mono, wave_type 0 selects
// sine — so absent and 0 are opposite instructions that both store 0.
//
// serialize.js was fixed to honour `row.freqRPresent`, but lanesToDoc rebuilt
// its rows through audioRow() without it, so a Lanes commit walked straight past
// that fix. The lane model now encodes ABSENCE rather than inferring it from the
// value: no sublane at all, a time before the first keyframe, or a `hold` time —
// the same vocabulary the cell sublanes already use for `-`, so the
// `# @ave-lane` comment gains no new key.

test('freqR: an explicit `-` in the freq_r slot comes back as `-`', () => {
    // The named case. Without the present bit the serializer has to guess from
    // the value and guesses "0 means mono" (serialize.js:142), so this line came
    // back as `A 10000 200 0 80 0 1 0 1` and killed a live binaural pair.
    const text = 'A 10000 200 0 80 0 1 - 1\n';
    assert.equal(laneCommit(text), text);
});

test('freqR: an explicit 0 (the mono collapse) is not deleted', () => {
    // The opposite error. ledc_format.md tells authors to "restate freqR (0 for
    // mono) on any entry that moves an audio carrier", and gating the sublane on
    // a positive value dropped exactly those rows' token — so a beat the author
    // ENDED kept beating.
    const text = 'A 0 200 0 80 0 1 210\nA 5000 220 0 80 0 1 0\n';
    assert.equal(laneCommit(text), text);
});

test('freqR: no token is invented on a row that wrote none', () => {
    // The backfill that compounded it: the stepped sublanes held their FIRST
    // value backwards in time, so a row before the first freq_r / wave_type row
    // was given one. For wave_type that is not a placeholder — `0` is the SINE
    // waveform — so a channel running square or noise was silently reset.
    const text = 'A 0 3000 0 70 0 1\nA 600 0 0 0 0 1\nA 1000 200 0 70 0 1 0 1\n';
    assert.equal(laneCommit(text), text);
    const lane = reconstructLanes(parse(text).doc).lanes[0];
    assert.deepEqual(lane.sub.wave.keys.map(k => [k.t, k.v]), [[1000, 1]],
        'only the row that wrote the token gets a keyframe');
    assert.equal(Object.prototype.hasOwnProperty.call(lane.sub.wave, 'hold'), false,
        'absences BEFORE the first write need no hold entry');
});

test('freqR: a row that stops writing the token mid-lane is held, not back-filled', () => {
    // Here the absence comes AFTER a write, so run-length encoding alone cannot
    // tell it from "the run continues" — it needs a `hold` time. Pre-fix the run
    // continued and the row re-stated 210 Hz and wave 1.
    const text = 'A 0 200 0 80 0 1 210 1\nA 5000 220 0 80 0 1 - -\n';
    const lane = reconstructLanes(parse(text).doc).lanes[0];
    assert.deepEqual(lane.sub.freqR.hold, [5000]);
    assert.deepEqual(lane.sub.wave.hold, [5000]);
    // With nothing after them to hold their position, the two `-` placeholders
    // are simply omitted — the firmware reads an absent token and a `-` the same
    // way (config_parser.c:1827 freq_r, :1845 wave_type), and the serializer's
    // minimal-trailing-token rule prefers the shorter spelling.
    assert.equal(laneCommit(text), 'A 0 200 0 80 0 1 210 1\nA 5000 220 0 80 0 1\n');
    // Give them a later column to align and the exact tokens come back.
    const held = 'A 0 200 0 80 0 1 210 1\nA 5000 220 0 80 0 1 - - 40\n';
    assert.equal(laneCommit(held), held);
});

test('freqR: a `>` glide on freq_r is not flattened to a step', () => {
    // freq_r is a compound cell in format v3 and the firmware wires a real
    // FREQ_R sweep off the prefix (config_parser.c:2937). The lane model holds
    // it as a plain number, so the prefix only survives on the cell that rides
    // alongside (model.js freqRCell) — unset, the glide became a step and the
    // beat stopped moving.
    const text = 'A 25000 500 0 70 0 3 >250 1\nA 35000 500 0 70 0 3 300 1\n';
    assert.equal(laneCommit(text), text);
});

test('freqR: an unchanged run still collapses to one keyframe', () => {
    // The dedupe is what keeps the `# @ave-lane` snapshot small — measured over
    // the 99 shipped sessions, 1449 rows that write freq_r reduce to 379
    // keyframes and 1433 that write wave_type to 367 — so it must survive the
    // presence work.
    const text = 'A 0 200 0 80 0 1 210 1\nA 5000 220 0 80 0 1 210 1\n'
               + 'A 9000 240 0 80 0 1 210 1\n';
    assert.equal(laneCommit(text), text);
    const lane = reconstructLanes(parse(text).doc).lanes[0];
    assert.deepEqual(lane.sub.freqR.keys.map(k => k.t), [0]);
    assert.deepEqual(lane.sub.wave.keys.map(k => k.t), [0]);
});

test('freqR: a lane that never mentions freq_r grows no sublane', () => {
    // Same stability rule as `hold` above: a session that does not use the
    // column must serialize its snapshot exactly as it did before.
    const lane = reconstructLanes(parse('A 0 200 0 80 0 1\n').doc).lanes[0];
    assert.equal(lane.sub.freqR, undefined);
    assert.equal(lane.sub.wave, undefined);
});
