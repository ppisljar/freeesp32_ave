// Lane-model handling of the `-` sentinel, and the one-click interior-LFO snap.
//
// Two defects, both of which lost data the author wrote:
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
// Covers both, plus the four curated library sessions as named regressions.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';
import {
    docToLanes, lanesToDoc, reconstructLanes, snapToBandBoundaries, keyframe,
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
