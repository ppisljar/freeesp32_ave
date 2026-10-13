// Lanes/Wizard must not delete the rows they cannot model.
//
// Both views project the shared doc into their own model (lanes with keyframe
// sublanes, segments with layers) and REBUILD the doc from that model on every
// commit. Before views/passthrough.js was wired in, committing a single dragged
// point silently dropped every `S` speech row, every `M` marker row, every
// ordinary comment, and every `raw` row the parser had kept verbatim — a
// session carrying narration lost its narration, with no warning and no undo
// beyond the view's own stack.
//
// The promise these tests lock in is the one views/passthrough.js documents:
// nothing is lost; a row carrying a TIME comes back at that time; an untimed
// row comes back attached to the block it preceded; and a doc with no
// unmodellable rows is emitted exactly as it was before the mechanism existed.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { serialize } from '../src/js/gen/serialize.js';
import {
    lanesToDoc, docToLanes, LANE_META_PREFIX,
} from '../src/js/gen/views/lane_serialize.js';
import {
    compileSession, sessionFromDoc, WIZARD_META_PREFIX,
} from '../src/js/gen/views/wizard_compile.js';

// Every row kind the parser can produce, in one file:
//   comment, blank, raw (unparseable — kept verbatim), bg, marker, speech,
//   led, audio. Only led/audio/bg are modelled by the views; the rest have to
//   ride through as passthrough.
const ALL_KINDS = [
    '# intro comment',
    '',
    'BG sdcard://bg.wav 0 60',
    'M 0 START',
    'S 0 default 80 "hello"',
    '0 10 50 80 255 0 0 1',
    'A 0 200 0 80 0 1',
    '# mid comment',
    'M 5000 MID',
    '5000 12 50 80 0 255 0 1',
    'A 5000 210 0 80 0 1',
    'this is garbage !!',          // raw: the firmware would reject it, we keep it
    '# trailing',
].join('\n') + '\n';

// A doc with nothing to carry — the normal path, which must be untouched.
const PLAIN = [
    '0 10 50 80 255 0 0 1',
    'A 0 200 0 80 0 1',
    '5000 12 50 80 0 255 0 1',
    'A 5000 210 0 80 0 1',
].join('\n') + '\n';

function countKinds(doc) {
    const n = {};
    for (const r of doc.rows) n[r.kind] = (n[r.kind] || 0) + 1;
    return n;
}

// The views regenerate their own `# @ave-*` metadata comment, so it is never
// part of the user's comment count.
function userComments(doc, prefix) {
    return doc.rows.filter(r => r.kind === 'comment' &&
        String(r.text || '').replace(/^#+\s*/, '').indexOf(prefix) !== 0);
}
function metaComments(doc, prefix) {
    return doc.rows.filter(r => r.kind === 'comment' &&
        String(r.text || '').replace(/^#+\s*/, '').indexOf(prefix) === 0);
}

// One commit through each view: doc -> view model -> doc.
function laneTrip(doc, withMeta) {
    return lanesToDoc(docToLanes(doc), { withMeta: !!withMeta }).doc;
}
function wizardTrip(doc, withMeta) {
    return compileSession(sessionFromDoc(doc).session, { withMeta: !!withMeta }).doc;
}

// ---- (a) the one that matters: nothing is lost -----------------------------

test('(a) a lane commit loses no row kind', () => {
    const orig = parse(ALL_KINDS).doc;
    const before = countKinds(orig);
    // Guard the fixture itself: if parse.js stops producing one of these kinds
    // the test would silently stop covering it.
    assert.deepEqual(Object.keys(before).sort(),
        ['audio', 'bg', 'blank', 'comment', 'led', 'marker', 'raw', 'speech'],
        'fixture exercises every row kind');

    const out = laneTrip(orig, false);
    assert.deepEqual(countKinds(out), before, 'per-kind row counts unchanged');
});

test('(a) a wizard commit loses no row kind', () => {
    const orig = parse(ALL_KINDS).doc;
    const out = wizardTrip(orig, false);
    assert.deepEqual(countKinds(out), countKinds(orig), 'per-kind row counts unchanged');
});

test('(a) carried rows survive the text file, not just the in-memory doc', () => {
    // The real failure path is commit -> serialize -> flash/reload -> parse, so
    // assert the carried rows come back through actual `.led` text.
    const orig = parse(ALL_KINDS).doc;
    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        const reparsed = parse(serialize(trip(orig, true))).doc;
        const n = countKinds(reparsed);
        // +1 comment: the view's own metadata comment is written into the file.
        assert.equal(n.comment, countKinds(orig).comment + 1, label + ': comments + meta');
        for (const k of ['blank', 'raw', 'bg', 'marker', 'speech', 'led', 'audio']) {
            assert.equal(n[k], countKinds(orig)[k], label + ': ' + k + ' count after reparse');
        }
    }
});

// ---- (b) the metadata comment must not breed --------------------------------

test('(b) the view metadata comment is not duplicated by repeated commits', () => {
    const orig = parse(ALL_KINDS).doc;

    // Two full commits with metadata on. collectPassthrough excludes the view's
    // own metadata comment; if it did not, every round-trip would carry the
    // previous copy and the file would grow without bound.
    const lane2 = laneTrip(parse(serialize(laneTrip(orig, true))).doc, true);
    assert.equal(metaComments(lane2, LANE_META_PREFIX).length, 1, 'one @ave-lane comment');
    assert.equal(userComments(lane2, LANE_META_PREFIX).length, 3, 'three user comments');

    const wiz2 = wizardTrip(parse(serialize(wizardTrip(orig, true))).doc, true);
    assert.equal(metaComments(wiz2, WIZARD_META_PREFIX).length, 1, 'one @ave-wizard comment');
    assert.equal(userComments(wiz2, WIZARD_META_PREFIX).length, 3, 'three user comments');
});

// ---- (c) position: timed rows keep their time ------------------------------

test('(c) a timed carried row comes back at its own time', () => {
    const orig = parse(ALL_KINDS).doc;
    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        const out = trip(orig, false);
        const markers = out.rows.filter(r => r.kind === 'marker');
        assert.deepEqual(markers.map(r => r.time), [0, 5000], label + ': marker times');
        assert.deepEqual(markers.map(r => r.label), ['START', 'MID'], label + ': marker labels');

        const speech = out.rows.filter(r => r.kind === 'speech');
        assert.equal(speech.length, 1, label + ': one speech row');
        assert.equal(speech[0].time, 0, label + ': speech stays at t=0');
        assert.equal(speech[0].text, 'hello', label + ': speech text intact');
    }
});

test('(c) a timed carried row sits with its own block, not at the end', () => {
    const orig = parse(ALL_KINDS).doc;
    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        const rows = trip(orig, false).rows;
        const mid = rows.findIndex(r => r.kind === 'marker' && r.time === 5000);
        const firstT5000 = rows.findIndex(r => r.time === 5000 &&
            (r.kind === 'led' || r.kind === 'audio'));
        const lastT0 = rows.reduce((acc, r, i) =>
            (r.time === 0 && (r.kind === 'led' || r.kind === 'audio')) ? i : acc, -1);
        assert.ok(mid > lastT0, label + ': M 5000 after the t=0 block');
        assert.ok(mid < firstT5000, label + ': M 5000 before the t=5000 rows it marks');
    }
});

// ---- (d) position: untimed rows keep their block ---------------------------

test('(d) a comment stays above the block it preceded', () => {
    const orig = parse(ALL_KINDS).doc;
    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        const rows = trip(orig, false).rows;
        const at = i => String(rows[i].text || '');
        const mid = rows.findIndex(r => r.kind === 'comment' && r.text === '# mid comment');
        const firstT5000 = rows.findIndex(r => r.time === 5000);
        const lastT0 = rows.reduce((acc, r, i) =>
            (r.time === 0 && (r.kind === 'led' || r.kind === 'audio')) ? i : acc, -1);
        assert.ok(mid > lastT0, label + ': "# mid comment" is past the t=0 block');
        assert.ok(mid < firstT5000,
            label + ': "# mid comment" introduces the t=5000 block it preceded');

        // The intro comment + blank anchor to the first block, so they land
        // directly under the header (BG is the only header row here).
        const intro = rows.findIndex(r => r.kind === 'comment' && r.text === '# intro comment');
        assert.equal(rows[0].kind, 'bg', label + ': BG stays pinned to the header');
        assert.equal(intro, 1, label + ': intro comment is the first non-header row');
        assert.equal(rows[2].kind, 'blank', label + ': the blank line keeps its place');

        // A trailing comment with no block after it anchors to null = end of
        // file, which must not silently fold it back to the top.
        assert.equal(at(rows.length - 1), '# trailing', label + ': trailing comment last');
        assert.equal(rows[rows.length - 2].kind, 'raw', label + ': trailing raw second-to-last');
    }
});

// ---- (e) the normal path is byte-for-byte unchanged ------------------------

test('(e) a doc with nothing to carry serializes exactly as before', () => {
    const plain = parse(PLAIN).doc;

    // No `passthrough` key is attached when there is nothing to carry, so the
    // metadata JSON — which is written verbatim into the file — is identical to
    // what the views emitted before this mechanism existed.
    const lanes = docToLanes(plain);
    assert.equal(Object.prototype.hasOwnProperty.call(lanes, 'passthrough'), false,
        'lanes carries no passthrough key');
    const { session } = sessionFromDoc(plain);
    assert.equal(Object.prototype.hasOwnProperty.call(session, 'passthrough'), false,
        'session carries no passthrough key');

    // And the emitted rows are the same rows in the same order (audio sorts
    // before led within a timestamp in the lane view; that predates this change).
    assert.equal(serialize(laneTrip(plain, false)),
        'A 0 200 0 80 0 1\n0 10 50 80 255 0 0 1\n' +
        'A 5000 210 0 80 0 1\n5000 12 50 80 0 255 0 1\n',
        'lane output unchanged');
    assert.equal(serialize(wizardTrip(plain, false)),
        '0 10 50 80 255 0 0 1\nA 0 200 0 80 0 1\n' +
        '5000 12 50 80 0 255 0 1\nA 5000 210 0 80 0 1\n',
        'wizard output unchanged');
});

// ---- (f) the live doc wins over the metadata snapshot ----------------------

test('(f) passthrough is recomputed from the live doc, never trusted from metadata', () => {
    // Commit once so the file carries a metadata comment whose JSON holds a
    // snapshot of the passthrough rows...
    const committed = serialize(laneTrip(parse(ALL_KINDS).doc, true));
    assert.ok(committed.indexOf(LANE_META_PREFIX) >= 0, 'metadata written');

    // ...then delete the markers in the Text view, the way a user would, and
    // commit again. Reading the snapshot instead of the doc would resurrect
    // them (and, symmetrically, would drop rows added since).
    const edited = parse(committed.split('\n')
        .filter(l => l.indexOf('M ') !== 0).join('\n')).doc;
    assert.equal(countKinds(edited).marker, undefined, 'markers gone from the live doc');

    const out = laneTrip(edited, true);
    assert.equal(countKinds(out).marker, undefined, 'deleted markers stay deleted');
    assert.equal(countKinds(out).speech, 1, 'the speech row the user kept is still there');

    // Same for the wizard.
    const wCommitted = serialize(wizardTrip(parse(ALL_KINDS).doc, true));
    const wEdited = parse(wCommitted.split('\n')
        .filter(l => l.indexOf('M ') !== 0).join('\n')).doc;
    const wOut = wizardTrip(wEdited, true);
    assert.equal(countKinds(wOut).marker, undefined, 'deleted markers stay deleted');
    assert.equal(countKinds(wOut).speech, 1, 'speech preserved');
});

// ---------------------------------------------------------------------------
// Regressions found by adversarially verifying the first cut of this feature.
// All three are the SAME bug class the mechanism exists to prevent, and none
// of the tests above caught any of them: they each exercise one view at a
// time, and two of these only appear when the two views are used in sequence.
// ---------------------------------------------------------------------------

// Round-trip helpers that mirror what the views actually do on commit.
const laneCommit = (text) => serialize(lanesToDoc(docToLanes(parse(text).doc), { withMeta: true }).doc);
const wizCommit = (text) => serialize(compileSession(sessionFromDoc(parse(text).doc).session, { withMeta: true }).doc);

const SMALL = [
    '0 10 50 80 255 0 0 1   # ramp-in',
    'A 0 200 0 80 0 1       # carrier',
    'S 3000 hello there',
    'M 4000 label=eyes-closed',
    '',
].join('\n');

test('passthrough: alternating Lanes/Wizard commits do not grow the file', () => {
    // Each view excludes only its OWN metadata prefix => it carries the
    // sibling's `# @ave-*` comment as a passthrough row nested inside its own
    // JSON, and the sibling then nests that larger comment inside its own.
    // Measured before the fix: 1012 -> 27503 bytes over six switches, ~1.8x
    // each, unbounded. The firmware mallocs the whole file into ESP32 heap
    // (config_parser.c:401) and truncates any line over 256 chars
    // (config_parser.c:422), so unbounded growth is a device failure.
    let text = SMALL;
    const sizes = [];
    for (let k = 0; k < 8; k++) {
        text = (k % 2 === 0) ? laneCommit(text) : wizCommit(text);
        sizes.push(text.length);
    }
    // Settles into a 2-cycle (whichever view's metadata is present), never grows.
    const ceiling = Math.max(sizes[0], sizes[1]);
    for (const n of sizes) {
        assert.ok(n <= ceiling, `file grew: ${sizes.join(' -> ')}`);
    }
    // And no view's metadata is ever nested inside another's.
    assert.equal((text.match(/@ave-lane/g) || []).length +
                 (text.match(/@ave-wizard/g) || []).length, 1,
        'exactly one view metadata comment survives a commit');
});

test('passthrough: an edit in one view survives a switch to the other', () => {
    // The flip side of the same root cause. A surviving stale sibling snapshot
    // makes sessionFromDoc report imported=false and prefer that snapshot --
    // which predates the Lanes edit -- over reconstructing from the rows that
    // carry it. Before the fix the 42 below was silently reverted to 10.
    const authored = wizCommit(SMALL);
    const lanes = docToLanes(parse(authored).doc);
    let edited = false;
    for (const ln of (lanes.lanes || [])) {
        for (const key of Object.keys(ln.sub || {})) {
            const keys = ln.sub[key] && ln.sub[key].keys;
            if (keys && keys.length) { keys[0].v = 42; edited = true; break; }
        }
        if (edited) break;
    }
    assert.ok(edited, 'fixture must expose at least one keyframe to edit');

    const afterLanes = serialize(lanesToDoc(lanes, { withMeta: true }).doc);
    const bodyOf = (t) => t.split('\n').filter(l => !l.trimStart().startsWith('#')).join('\n');
    assert.match(bodyOf(afterLanes), /(^|\s)42(\s|$)/, 'lane commit wrote the edit');

    const reopened = sessionFromDoc(parse(afterLanes).doc);
    assert.equal(reopened.imported, true,
        'wizard must reconstruct structurally, not restore a pre-edit snapshot');
    assert.match(bodyOf(serialize(compileSession(reopened.session, { withMeta: true }).doc)),
        /(^|\s)42(\s|$)/, 'the edit survives the view switch');
});

test('passthrough: a BG row added after the snapshot is not deleted', () => {
    // `bg` is in MODELLED, so collectPassthrough skips it -- the view is meant
    // to re-emit it from its model. But a snapshot written before the user
    // added the BG row has no bg key, so the row was skipped as "modelled" and
    // then never emitted. syncPassthrough adopts it from the live doc instead.
    const PLAIN = '0 10 50 80 255 0 0 1\nA 0 200 0 80 0 1\n';
    for (const commit of [laneCommit, wizCommit]) {
        const committed = commit(PLAIN);                 // metadata, no bg key
        const withBg = committed.replace(/\n(0 10)/, '\nBG sdcard://rain.wav -10 40\n$1');
        assert.match(withBg, /^BG /m, 'fixture has a BG row');
        assert.match(commit(withBg), /^BG /m, 'BG row survives the next commit');
    }
});
