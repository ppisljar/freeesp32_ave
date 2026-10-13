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

// ---------------------------------------------------------------------------
// INLINE NOTES — the trailing `# why` on an `led`/`audio` row.
//
// The last silent-data-loss path in the generator, and a DIFFERENT problem from
// everything above. `led`/`audio` rows are modelled: the views rebuild them from
// keyframes/layers, so the row cannot ride through whole the way an `S` row can
// — only the note can, and it then has to be re-attached to the rebuilt row.
// Before views/passthrough.js grew `collectInlineNotes`/`reattachInlineNotes`,
// every note on every row was deleted by the first Lanes or Wizard commit:
// measured on the fixture below, 6 of 6 notes gone, no warning.
//
// That is worse than it sounds, because inline annotation is this project's
// documented house style — docs/sessions/authoring.html asks every row of a
// good session to carry one — so a single dragged point erased essentially all
// of a session's documentation while leaving the timeline intact and plausible.
// ---------------------------------------------------------------------------

// Four DISTINCT identities at t=0 and two more at t=5000. The t=0 cluster is
// the point: a note is keyed on (kind, time, channel|mask), and four rows that
// share a timestamp are exactly where a too-loose key would smear notes across
// rows. Each note names the row it belongs to so a mis-attachment is legible in
// the assertion failure rather than being a silent off-by-one.
const ANNOTATED = [
    '0 10 50 80 255 0 0 1     # led mask 1 at t=0',
    '0 8 50 60 0 0 255 2      # led mask 2 at t=0',
    'A 0 200 0 80 0 1         # audio ch 1 at t=0',
    'A 0 210 0 80 0 2         # audio ch 2 at t=0',
    '5000 12 50 80 0 255 0 1  # led mask 1 at t=5000',
    'A 5000 205 0 80 0 1      # audio ch 1 at t=5000',
].join('\n') + '\n';

// identity -> note, for every modelled row that carries one. Mirrors the key
// the implementation uses (null channel/mask normalized to 0) so the test
// checks ATTACHMENT, not just survival of the text somewhere in the file.
function notesByIdentity(doc) {
    const out = {};
    for (const r of doc.rows) {
        if (r.kind !== 'led' && r.kind !== 'audio') continue;
        if (!r.inlineComment) continue;
        const key = r.kind === 'audio'
            ? 'a:' + r.time + ':' + (r.channel === null || r.channel === undefined ? 0 : r.channel)
            : 'l:' + r.time + ':' + (r.mask === null || r.mask === undefined ? 0 : r.mask);
        // A second note on one identity would be a mis-attachment, so record it
        // rather than overwriting silently.
        out[key] = (out[key] === undefined) ? r.inlineComment : out[key] + ' |AND| ' + r.inlineComment;
    }
    return out;
}

const EXPECTED_NOTES = {
    'l:0:1': 'led mask 1 at t=0',
    'l:0:2': 'led mask 2 at t=0',
    'a:0:1': 'audio ch 1 at t=0',
    'a:0:2': 'audio ch 2 at t=0',
    'l:5000:1': 'led mask 1 at t=5000',
    'a:5000:1': 'audio ch 1 at t=5000',
};

// ---- (g) the round-trip each view used to destroy ---------------------------

test('(g) an inline note survives a view round-trip on the row it annotates', () => {
    const orig = parse(ANNOTATED).doc;
    // Guard the fixture: if parse.js stops filling inlineComment, every
    // assertion below would pass vacuously on an empty-vs-empty comparison.
    assert.deepEqual(notesByIdentity(orig), EXPECTED_NOTES, 'fixture parses six notes');

    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        assert.deepEqual(notesByIdentity(trip(orig, false)), EXPECTED_NOTES,
            label + ': every note back on its own row');
    }
});

test('(g) inline notes survive the text file, not just the in-memory doc', () => {
    // The real failure path is commit -> serialize -> flash/reload -> parse ->
    // commit again, because the model is rebuilt from the FILE on reopen. A fix
    // that only held the note in memory would still lose it on the next reload.
    const orig = parse(ANNOTATED).doc;
    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        const reloaded = parse(serialize(trip(orig, true))).doc;
        assert.deepEqual(notesByIdentity(reloaded), EXPECTED_NOTES,
            label + ': notes are in the written file');
        assert.deepEqual(notesByIdentity(trip(reloaded, true)), EXPECTED_NOTES,
            label + ': and survive the commit after the reload');
    }
});

test('(g) a note edited in the Text view wins over the metadata snapshot', () => {
    // Same "live doc beats the snapshot" rule the carried rows obey. The note
    // is also inside the `# @ave-*` JSON from the previous commit; trusting
    // that copy would silently revert the author's rewording.
    const committed = serialize(laneTrip(parse(ANNOTATED).doc, true));
    assert.ok(committed.indexOf('audio ch 1 at t=0') >= 0, 'snapshot holds the old text');

    const edited = parse(committed.replace('# audio ch 1 at t=0', '# EDITED IN TEXT VIEW')).doc;
    const out = serialize(laneTrip(edited, true));
    const body = out.split('\n').filter(l => l.indexOf('# @ave') !== 0).join('\n');
    assert.ok(body.indexOf('# EDITED IN TEXT VIEW') >= 0, 'the new wording is written');
    assert.equal(body.indexOf('# audio ch 1 at t=0'), -1, 'the old wording is gone');
});

// ---- (h) the case that bites: the annotated row no longer exists ------------

// A redundant hold: identical values restated on the same channel, MID-session.
// Both views fold it away on purpose — the lane view's coalesceRampKeys drops a
// repeated step keyframe, the wizard's coalesce() drops a row that re-holds the
// previous row's exact values. So the note's row genuinely does not exist
// afterwards.
//
// The hold must not be the LAST row. A trailing hold sets the session's end
// time, so the wizard deliberately re-admits it (wizard_compile.js) and the
// note then stays attached — correct behaviour, but it stops exercising the
// orphan path. That is not hypothetical: this fixture originally ended on the
// hold, and the assertion below caught it the moment coalesce() was fixed.
// TRAILING_HOLD_AUDIO pins that behaviour separately.
const FOLDED_LED = [
    '0 10 50 80 255 0 0 1',
    '5000 10 50 80 255 0 0 1  # why we hold here',
    '9000 14 50 80 255 0 0 1',
].join('\n') + '\n';
const FOLDED_AUDIO = [
    'A 0 200 0 80 0 1',
    'A 5000 200 0 80 0 1      # why we hold here',
    'A 9000 300 0 80 0 1',
].join('\n') + '\n';

// The same hold, but terminal — the shape the wizard must KEEP.
const TRAILING_HOLD_AUDIO = [
    'A 0 200 0 80 0 1',
    'A 5000 200 0 80 0 1      # why we hold here',
].join('\n') + '\n';

test('(h) a note whose row the rebuild folds away survives as a whole-line comment', () => {
    // The contract documented in views/passthrough.js: we may lose the note's
    // ATTACHMENT when the row goes, but never the author's words. Dropping the
    // text here would be the same silent deletion this whole module exists to
    // prevent, just one level down.
    for (const src of [FOLDED_LED, FOLDED_AUDIO]) {
        const orig = parse(src).doc;
        for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
            const out = trip(orig, false);

            // The row really is gone — otherwise this test is not exercising
            // the orphan path at all and would pass for the wrong reason.
            assert.equal(out.rows.filter(r => r.time === 5000 &&
                (r.kind === 'led' || r.kind === 'audio')).length, 0,
                label + ': the rebuild folded the annotated row away');

            const kept = out.rows.filter(r => r.kind === 'comment' &&
                String(r.text).indexOf('why we hold here') >= 0);
            assert.equal(kept.length, 1, label + ': the words survive, exactly once');
            assert.equal(kept[0].text, '# why we hold here',
                label + ': kept as a plain whole-line comment');
            // And it survives the file, not just the doc.
            assert.match(serialize(out), /^# why we hold here$/m, label + ': written out');
        }
    }
});

test('(h) a trailing hold keeps its note inline — the wizard must not fold it', () => {
    // The flip side of the fold, and the reason the FOLDED_* fixtures grew a
    // third row. A terminal hold is load-bearing: it sets the session's end
    // time. The wizard's coalesce() used to drop it (and used to drop any row
    // whose only change was a format-v2 pulse field, which was a silent
    // data-loss bug in its own right). Now it keeps it, so the note stays
    // ATTACHED rather than degrading to a comment — strictly better, and worth
    // pinning so a future coalesce change cannot quietly undo it.
    const out = wizardTrip(parse(TRAILING_HOLD_AUDIO).doc, false);
    const row = out.rows.find(r => r.kind === 'audio' && r.time === 5000);
    assert.ok(row, 'the trailing hold survives: it carries the session end time');
    assert.equal(row.inlineComment, 'why we hold here', 'and keeps its note inline');
    assert.equal(out.rows.filter(r => r.kind === 'comment' &&
        String(r.text).indexOf('why we hold here') >= 0).length, 0,
        'so there is no orphan comment for it');
});

test('(h) two rows sharing one identity: no note text is lost, by either route', () => {
    // Two rows CAN share (kind, time, channel) — a legal firmware batch where
    // file order decides what the device sees (find_next_audio_for_bit takes the
    // first later same-channel entry, config_parser.c:2460-2476, and the stable
    // sort at :566-576 preserves file order among equal timestamps).
    //
    // This test used to assert that BOTH views collapsed the pair to one row and
    // orphaned the second note. That collapse was itself a data-loss bug: it
    // deleted a row outright and materialised any `-` sentinel on it into a hard
    // number, which on test2a.ledc turned a 4->12 Hz AM ramp into 4->0 Hz on the
    // device. The views now reach the same guarantee by different routes, so the
    // assertion here is the CONTRACT (no note text is lost) rather than one
    // view's mechanism:
    //   - Lanes keeps both rows, so both notes stay inline.
    //   - the Wizard's coalesce() still folds a byte-identical consecutive
    //     duplicate — which is its legitimate job, not the sentinel bug; it
    //     materialises nothing (verified: the sentinel case round-trips with its
    //     dashes intact) — so the second note orphans to a comment.
    const src = 'A 0 200 0 80 0 1 # first note\nA 0 200 0 80 0 1 # second note\n';
    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        const out = trip(parse(src).doc, false);
        const text = serialize(out);
        for (const note of ['first note', 'second note']) {
            assert.equal((text.split(note).length - 1), 1,
                label + ': "' + note + '" survives exactly once');
        }
        // Whatever survives inline must be attached in source order, never
        // shuffled onto the wrong row.
        const inline = out.rows.filter(r => r.inlineComment).map(r => r.inlineComment);
        assert.deepEqual(inline, inline.slice().sort(), label + ': notes in source order');
        assert.equal(inline[0], 'first note', label + ': the first note keeps its row');
    }
});

test('(h) a duplicate identity does not materialise a `-` sentinel in either view', () => {
    // The device-visible half of the same bug, and the reason the test above
    // stopped asserting a collapse. test2a.ledc has `A 30000 - - - 12 4`
    // followed by `A 30000 440 -100 45 0 4`; collapsing them rewrote the three
    // sentinels to 440 / -100 / 45, so the mod sweep from `A 20000 294 0 50 >4 4`
    // retargeted from 12 Hz to 0 Hz — a 4->12 Hz AM ramp became 4->0 Hz.
    const src = 'A 20000 294 0 50 >4 4\nA 30000 - - - 12 4\nA 30000 440 -100 45 0 4\n';
    const dashes = (t) => (t.match(/(^| )-( |$)/g) || []).length;
    for (const [label, trip] of [['lane', laneTrip], ['wizard', wizardTrip]]) {
        const out = serialize(trip(parse(src).doc, false));
        assert.equal(dashes(out), dashes(src),
            label + ': every `-` survives; a materialised sentinel changes what'
            + ' the device plays');
    }
});

// ---- (i) notes must not breed -----------------------------------------------

test('(i) consecutive commits do not duplicate a note', () => {
    // The mirror of "(b) the view metadata comment is not duplicated". The note
    // is in THREE places after a commit — on the row, inside the `# @ave-*`
    // JSON, and (for an orphan) as a comment row — so the ways to double it are
    // many: re-attach to the row AND emit a comment, or carry the orphan comment
    // as passthrough AND re-orphan the note that produced it.
    const occurrences = (text, needle) => (text.split(needle).length - 1);

    for (const [label, commit] of [['lane', laneCommit], ['wizard', wizCommit]]) {
        // Attached note: present once on the row + once in the metadata JSON.
        let text = commit(ANNOTATED);
        const first = occurrences(text, 'audio ch 1 at t=0');
        // Guard: a count that starts at zero would make every "still N" check
        // below pass trivially on code that simply deletes the note.
        assert.ok(first > 0, label + ': the note is in the first commit at all');
        for (let k = 0; k < 3; k++) {
            text = commit(text);
            assert.equal(occurrences(text, 'audio ch 1 at t=0'), first,
                label + ': attached note count stable across commit ' + (k + 2));
        }
        assert.deepEqual(notesByIdentity(parse(text).doc), EXPECTED_NOTES,
            label + ': and still attached to the right rows');

        // Orphaned note: the risky one. After commit 1 it is a comment row, so
        // commit 2 must treat it as passthrough and not also re-derive it.
        let ot = commit(FOLDED_AUDIO);
        const ofirst = occurrences(ot, 'why we hold here');
        assert.ok(ofirst > 0, label + ': the orphaned note is in the first commit at all');
        for (let k = 0; k < 3; k++) {
            ot = commit(ot);
            assert.equal(occurrences(ot, 'why we hold here'), ofirst,
                label + ': orphan note count stable across commit ' + (k + 2));
        }
        const bodyLines = ot.split('\n').filter(l => l.indexOf('# @ave') !== 0);
        assert.equal(bodyLines.filter(l => l === '# why we hold here').length, 1,
            label + ': exactly one orphan comment line in the body');
    }
});

// ---- (j) the no-notes path is byte-for-byte unchanged ----------------------

test('(j) a doc with no inline notes serializes exactly as before', () => {
    // The view model is JSON.stringify'd verbatim into the `# @ave-*` comment,
    // so an always-present `inlineNotes: []` would change EVERY session file on
    // disk and break the byte-perfect-reopen contract
    // (test/lane_serialize.test.js "(d) # @ave-lane metadata gives a
    // byte-perfect reopen" deep-compares the restored model). The key must be
    // absent, not empty.
    const plain = parse(PLAIN).doc;

    const lanes = docToLanes(plain);
    assert.equal(Object.prototype.hasOwnProperty.call(lanes, 'inlineNotes'), false,
        'lanes carries no inlineNotes key');
    const { session } = sessionFromDoc(plain);
    assert.equal(Object.prototype.hasOwnProperty.call(session, 'inlineNotes'), false,
        'session carries no inlineNotes key');

    // Same expected bytes as test (e) above — the rows and their order are
    // untouched by the note mechanism when there are no notes.
    assert.equal(serialize(laneTrip(plain, false)),
        'A 0 200 0 80 0 1\n0 10 50 80 255 0 0 1\n' +
        'A 5000 210 0 80 0 1\n5000 12 50 80 0 255 0 1\n',
        'lane output unchanged');
    assert.equal(serialize(wizardTrip(plain, false)),
        '0 10 50 80 255 0 0 1\nA 0 200 0 80 0 1\n' +
        '5000 12 50 80 0 255 0 1\nA 5000 210 0 80 0 1\n',
        'wizard output unchanged');

    // And a doc that HAS notes must drop the key again once they are removed,
    // the same way `passthrough` does — otherwise a stale snapshot note comes
    // back after the user deletes every comment in the Text view.
    const stripped = parse(ANNOTATED.replace(/\s*#.*$/gm, '')).doc;
    assert.equal(Object.prototype.hasOwnProperty.call(docToLanes(stripped), 'inlineNotes'), false,
        'deleting every note drops the lanes key');
    assert.equal(Object.prototype.hasOwnProperty.call(sessionFromDoc(stripped).session, 'inlineNotes'),
        false, 'deleting every note drops the session key');
});
