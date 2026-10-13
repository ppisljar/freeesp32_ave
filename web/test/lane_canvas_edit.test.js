// Lane-canvas edit tests: the three ways the canvas used to write something
// other than what it drew.
//
//   (1) a drag that crosses a sibling left `keys[]` unsorted, so lanesToDoc
//       resolved the wrong keyframe per row — right picture, wrong file;
//   (2) the mid-handle hit target outlived the handle it belonged to, so a
//       click on blank canvas cycled a curve nobody could see;
//   (3) the handle's pixel centre was cached ON the keyframe, and the keyframe
//       is JSON-stringified into the saved `# @ave-lane` metadata — screen
//       geometry in the user's file.
//
// Plus the fourth case the same gestures could reach: two keyframes on one
// timestamp, which the canvas drew as two dots and the file could only keep as
// one row.
//
//   (5) and the one that cost a keyframe rather than a value: a drag the user
//       meant as value-only still re-derived the time from the pointer and
//       re-snapped it, so at the 20 s grid fitToContent gives a 30-minute
//       session (measured at the head of section (5) below)
//       dropCoincidentKeys deleted the neighbour the snap landed on.
//
// A test marked `[contract, passes pre-fix]` or `[measurement]` is NOT a
// regression test for the section it sits in, and the label is measured rather
// than guessed. Each shipped behaviour was hand-reverted on its own in a copy
// of lane_canvas.js and the whole file re-run against it:
//   (A) the pointermove re-deriving and re-snapping the time from the pointer's
//       POSITION, with the collapse unconditional at pointerup — the bug
//       sections (5) and (6) are about. 9 fail, and every [contract] /
//       [measurement] passes;
//   (B) the handle point parked on the keyframe (`_mid`) with a no-op reset and
//       no _mid scrubber — sections (2) and (3). 7 fail, among them one
//       strengthened and one new test below; both mid-handle [contract]
//       tests pass;
//   (C) the coincident collapse moved back onto every pointermove — 3 fail,
//       one of them strengthened below to make it so;
//   (D) no re-sort in moveKeyframe — section (1). 7 fail;
//   (E) no coincident collapse at all — section (4). 5 fail.
// So a [contract] label means "passes on the revert of the behaviour this
// section is about"; two of them do fail on (C) or (E), and say so. They pin
// the contract of the replacement (or, for [measurement], the zoom figure the
// rules are justified with); the tests that catch each bug sit next to them.
//
// Sections (1) to (5) drive the exported pure helpers and then the real
// lanes -> doc projection, like test/table_view.test.js and
// test/text_view.test.js do. Section (6) adds the small canvas/pointer stub
// the rest of the suite does without, because three of the rules are only
// worth as much as the handler's wiring to them: that onPointerMove hands
// dragTime the pointer's TRAVEL, that tap-to-add goes through addTime, and
// that render()'s cache reset composes with onPointerDown's
// handle-before-keyframe order — the last of which no amount of helper
// arithmetic can reach.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    moveKeyframe, insertKeyframe, dropCoincidentKeys,
    createMidHandleCache, stripTransientKeyState,
    dragTime, dragKeyframe, commitKeyframeDrag, snapStep, addTime,
    createLaneCanvas, DRAG_X_DEADZONE_PX, DROP_AIM_PX,
} from '../src/js/gen/views/lane_canvas.js';
import { lanesToDoc, findLaneMeta, keyframe } from '../src/js/gen/views/lane_serialize.js';
import { serialize } from '../src/js/gen/serialize.js';

// One audio lane whose `freq` sublane is the one under test. `vol` keeps a
// keyframe at t=0 so the lane always has a row there (it is a union time), the
// same shape a real session has.
function laneWith(keys) {
    return {
        version: 1,
        lanes: [{
            kind: 'audio', key: 1, name: 'Audio ch 1', collapsed: false,
            sub: {
                freq: { keys },
                pan: { keys: [] },
                vol: { keys: [keyframe(0, 60, 'step')] },
                mod: { keys: [] },
            },
        }],
    };
}
function freqRows(lanes) {
    return lanesToDoc(lanes, { withMeta: false }).doc.rows
        .filter(r => r.kind === 'audio')
        .map(r => ({ t: r.time, v: r.freq.value, interp: r.freq.interp }));
}

// ---- (1) a drag across a sibling keeps keys[] sorted -----------------------

test('drag across a sibling re-sorts the sublane', () => {
    const keys = [keyframe(0, 100, 'lin'), keyframe(1000, 200, 'lin'),
                  keyframe(2000, 300, 'step')];
    const moved = keys[0];
    moveKeyframe(keys, moved, 1500, 100);   // drag the first key past the second
    assert.deepEqual(keys.map(k => k.t), [1000, 1500, 2000]);
    assert.equal(keys[1], moved, 'the keyframe OBJECT is kept, only reordered');
});

test('drag across a sibling emits each value at its own timestamp', () => {
    // Pre-fix this assigned `t` in place and left keys[] as [1500, 1000, 2000];
    // emitCellAt then read keys[0] for t=1000 (its walk breaks at the first key
    // past the query time) and wrote the dragged key's 100 there, with the
    // t=1500 row getting a ramp-interior 250.
    const keys = [keyframe(0, 100, 'lin'), keyframe(1000, 200, 'lin'),
                  keyframe(2000, 300, 'step')];
    moveKeyframe(keys, keys[0], 1500, 100);
    assert.deepEqual(freqRows(laneWith(keys)), [
        { t: 0, v: 200, interp: 'none' },     // before the first key: held flat
        { t: 1000, v: 200, interp: 'lin' },
        { t: 1500, v: 100, interp: 'lin' },
        { t: 2000, v: 300, interp: 'none' },
    ]);
});

test('drag across a sibling keeps the dragged keyframe\'s glide prefix', () => {
    const keys = [keyframe(0, 100, 'lin'), keyframe(1000, 200, 'quad'),
                  keyframe(4000, 300, 'step')];
    moveKeyframe(keys, keys[0], 2000, 150);
    const text = serialize(lanesToDoc(laneWith(keys), { withMeta: false }).doc);
    assert.equal(text.trim().split('\n').join('|'),
        ['A 0 200 0 60 0 1',      // held flat before the first keyframe
         'A 1000 *200 0 60 0 1',  // the quad start keeps its '*'
         'A 2000 >150 0 60 0 1',  // the dragged keyframe keeps its '>'
         'A 4000 300 0 60 0 1'].join('|'));
});

test('drag across a sibling fixes the stepped strips too (wave markers)', () => {
    // steppedAt backs the wave / freqR strips and breaks out of its walk at the
    // first key past the row time, exactly like emitCellAt.
    const lanes = laneWith([keyframe(0, 300, 'step')]);
    const wave = [keyframe(1000, 1, 'step'), keyframe(2000, 2, 'step')];
    lanes.lanes[0].sub.wave = { keys: wave };
    moveKeyframe(wave, wave[0], 3000, 1);   // drag the 'square' marker to the end
    const waves = lanesToDoc(lanes, { withMeta: false }).doc.rows
        .filter(r => r.kind === 'audio').map(r => [r.time, r.waveType]);
    const at = t => waves.find(w => w[0] === t)[1];
    assert.deepEqual(waves.map(w => w[0]), [0, 2000, 3000], 'one row per time');
    // Pre-fix the walk read the unsorted keys[0] and reported the dragged
    // marker's 1 on the rows it had not reached and the sibling's 2 on the
    // dragged key's own row. What t=0 gets — the earliest marker held back, or
    // nothing, since t=0 predates every wave key — is steppedAt's call in
    // lane_serialize.js, so this test asserts only what the sort decides.
    assert.equal(at(3000), 1, 'the dragged marker reads at its own time');
    assert.equal(at(2000), 2, 'the sibling keeps the marker it set');
    assert.notEqual(at(0), 1, 'and nothing of the dragged marker leaks earlier');
});

test('a dragged lane model round-trips through the @ave-lane metadata sorted', () => {
    // The metadata comment carries the lane model verbatim, so an unsorted
    // array used to survive a reload and keep mis-projecting.
    const keys = [keyframe(0, 100, 'lin'), keyframe(1000, 200, 'lin'),
                  keyframe(2000, 300, 'step')];
    moveKeyframe(keys, keys[0], 1500, 100);
    const doc = lanesToDoc(laneWith(keys), { withMeta: true }).doc;
    const meta = findLaneMeta(doc);
    assert.deepEqual(meta.lanes[0].sub.freq.keys.map(k => k.t), [1000, 1500, 2000]);
});

// ---- (2) the mid-handle hit target dies with the frame ---------------------

test('mid-handle cache [contract, passes pre-fix]: a painted handle is hit-testable', () => {
    // Revert (B)'s cache — `_mid` on the keyframe — answered a hit just as
    // well, so this is the positive half of the pair, not a regression test.
    const kf = keyframe(0, 100, 'lin');
    const cache = createMidHandleCache();
    cache.add(kf, 200, 50);
    assert.equal(cache.hit([kf], 200, 50), kf);
    assert.equal(cache.hit([kf], 206, 54), kf, 'within the 10px hit radius');
    assert.equal(cache.hit([kf], 240, 50), null, 'outside it');
});

test('mid-handle cache: a frame that does not paint the handle leaves nothing to click', () => {
    // What a zoom-out / pan / delete-the-next-keyframe does: render() resets
    // the cache, and drawMidHandle never re-adds a handle it does not paint.
    const kf = keyframe(0, 100, 'lin');
    const cache = createMidHandleCache();
    cache.add(kf, 200, 50);
    cache.reset();
    assert.equal(cache.hit([kf], 200, 50), null,
        'a stale coordinate must not turn a click on blank canvas into an edit');
});

test('mid-handle cache [contract, passes pre-fix]: targets are scoped to the sublane', () => {
    // Also passes on revert (B), the `_mid`-on-the-keyframe build: hit() has
    // always iterated the keys it was handed. Kept as the scoping contract.
    const mine = keyframe(0, 100, 'lin');
    const other = keyframe(0, 100, 'lin'); // another strip, same pixel
    const cache = createMidHandleCache();
    cache.add(other, 200, 50);
    assert.equal(cache.hit([mine], 200, 50), null);
    assert.equal(cache.hit([other], 200, 50), other);
});

test('mid-handle cache: a re-imported keyframe is not the old one', () => {
    // Re-importing from the doc builds fresh keyframe objects; keying the
    // cache on identity means their handles start out unclickable rather than
    // inheriting the previous model's pixels.
    const kf = keyframe(0, 100, 'lin');
    const cache = createMidHandleCache();
    cache.add(kf, 200, 50);
    const reimported = JSON.parse(JSON.stringify(kf));
    assert.deepEqual(reimported, kf);
    assert.equal(cache.hit([reimported], 200, 50), null);
});

// ---- (3) no canvas pixels in the saved file --------------------------------

test('caching a handle adds nothing to the keyframe', () => {
    const kf = keyframe(0, 100, 'lin');
    const before = JSON.stringify(kf);
    createMidHandleCache().add(kf, 337.5, 61.25);
    assert.equal(JSON.stringify(kf), before);
});

test('the @ave-lane metadata holds no screen geometry after a handle is drawn', () => {
    const keys = [keyframe(0, 100, 'lin'), keyframe(4000, 200, 'step')];
    const lanes = laneWith(keys);
    createMidHandleCache().add(keys[0], 337.5, 61.25);
    const doc = lanesToDoc(lanes, { withMeta: true }).doc;
    const meta = doc.rows.find(r => r.kind === 'comment');
    assert.ok(!/_mid|337\.5|61\.25/.test(meta.text), meta.text);
    // Same bytes whatever the viewport was: the model is all that is written.
    assert.equal(meta.text,
        lanesToDoc(laneWith([keyframe(0, 100, 'lin'), keyframe(4000, 200, 'step')]),
                   { withMeta: true }).doc.rows.find(r => r.kind === 'comment').text);
});

test('stripTransientKeyState scrubs _mid out of a session saved by the old build', () => {
    const lanes = laneWith([keyframe(0, 100, 'lin'), keyframe(4000, 200, 'step')]);
    lanes.lanes[0].sub.freq.keys[0]._mid = { x: 337.5, y: 61.25 };
    assert.equal(stripTransientKeyState(lanes), 1);
    assert.equal(stripTransientKeyState(lanes), 0, 'idempotent');
    const meta = lanesToDoc(lanes, { withMeta: true }).doc.rows
        .find(r => r.kind === 'comment');
    assert.ok(!meta.text.includes('_mid'), meta.text);
});

test('stripTransientKeyState tolerates empty / half-built models', () => {
    assert.equal(stripTransientKeyState(null), 0);
    assert.equal(stripTransientKeyState({}), 0);
    assert.equal(stripTransientKeyState({ lanes: [] }), 0);
    assert.equal(stripTransientKeyState({ lanes: [{ kind: 'audio' }] }), 0);
    assert.equal(stripTransientKeyState({ lanes: [{ sub: { freq: {} } }] }), 0);
    // Those five alone pass on revert (B) — a build that scrubs nothing
    // whatsoever — because they only assert "did not throw". So the walk has
    // to be made to clear a real _mid while stepping over a lane with no
    // `sub` and a sublane with no `keys`, which is what fails on (B).
    const half = { lanes: [{ kind: 'audio' }, { sub: { freq: {}, vol: { keys: [
        Object.assign(keyframe(0, 60, 'step'), { _mid: { x: 1, y: 2 } })] } } }] };
    assert.equal(stripTransientKeyState(half), 1);
    assert.equal(half.lanes[1].sub.vol.keys[0]._mid, undefined);
});

// ---- (4) two keyframes on one timestamp -----------------------------------

test('dropping a keyframe onto a sibling leaves one dot and one row', () => {
    // Pre-fix, measured: three dots on the canvas and two rows in the file —
    // keys [[0,100],[1000,200],[1000,70]] projected to `A 0 100 ...` plus
    // `A 1000 70 ...`, the t=4000 row simply gone. WHICH of the two collided
    // values the file keeps follows the array position rather than the gesture:
    // drag the earlier-indexed key onto the later one instead and emitCellAt's
    // walk keeps the sibling's 200 at t=1000 and parks the dragged 70 on the
    // t=0 row the user never touched.
    const keys = [keyframe(0, 100, 'step'), keyframe(1000, 200, 'step'),
                  keyframe(4000, 300, 'step')];
    const moved = keys[2];
    moveKeyframe(keys, moved, 1000, 70);
    assert.equal(dropCoincidentKeys(keys, moved), 1);
    assert.deepEqual(keys.map(k => [k.t, k.v]), [[0, 100], [1000, 70]]);
    assert.equal(keys[1], moved, 'the keyframe the user dragged is the survivor');
    const rows = freqRows(laneWith(keys));
    assert.equal(rows.length, keys.length, 'one row per drawn keyframe');
    assert.deepEqual(rows, [{ t: 0, v: 100, interp: 'none' },
                            { t: 1000, v: 70, interp: 'none' }]);
});

test('dragging ACROSS a sibling does not eat it', () => {
    // The collapse belongs to the commit, not to the pointermove. Strengthened
    // to drive the gesture helpers: calling moveKeyframe and then
    // dropCoincidentKeys by hand, as this test used to, passed on revert (C)
    // too — a build that collapses on every move, which is precisely the
    // behaviour the test is named for. Driving dragKeyframe and asserting the
    // array mid-drag fails on (C), as it should.
    const keys = [keyframe(0, 100, 'step'), keyframe(1000, 200, 'step'),
                  keyframe(4000, 300, 'step')];
    const moved = keys[0];
    const msPerPx = 50;                     // 1000 ms = 20 px, 500 ms grid
    dragKeyframe(keys, moved, 0, 20, msPerPx, 150); // pointer over the sibling
    assert.equal(moved.t, 1000, 'mid-drag: sitting exactly on the sibling');
    assert.equal(keys.length, 3, '... and nothing collapsed while the pointer is down');
    dragKeyframe(keys, moved, 0, 40, msPerPx, 150); // ... and onward
    assert.equal(commitKeyframeDrag(keys, moved, 0), 0);
    assert.deepEqual(keys.map(k => k.t), [1000, 2000, 4000]);
});

test('tap-to-add on an occupied timestamp replaces that keyframe', () => {
    const keys = [keyframe(0, 100, 'step'), keyframe(1000, 200, 'lin')];
    const added = insertKeyframe(keys, keyframe(1000, 20, 'step'));
    assert.equal(dropCoincidentKeys(keys, added), 1);
    assert.deepEqual(keys.map(k => [k.t, k.v]), [[0, 100], [1000, 20]]);
    assert.equal(freqRows(laneWith(keys)).length, 2);
});

test('dropCoincidentKeys keeps the keeper even when it is not the only match', () => {
    // Defensive: a model that arrived already doubled (hand-edited metadata,
    // or an older session saved mid-bug) collapses to the keeper.
    const keeper = keyframe(500, 7, 'step');
    const keys = [keyframe(500, 1, 'step'), keeper, keyframe(500, 9, 'lin')];
    assert.equal(dropCoincidentKeys(keys, keeper), 2);
    assert.deepEqual(keys, [keeper]);
});

// ---- (5) a drag the user meant as value-only ------------------------------
//
// Everything below runs at the zoom a 30-minute session gets from
// fitToContent: sessions/library/03_deep_relaxation.ledc ends at
// `A 1800000 200 0 0 10 1 0 0`, and fitToContent picks
// (maxT * 1.1) / (cssW - LABEL_W - 8) ms per px. As with the rest of the
// suite, the pointer plumbing is not executed here — the handler's job is to
// hand dragTime the pointer's TRAVEL (p.x - drag.downX) rather than its
// position, and these tests drive the helpers it hands it to.
const CANVAS_W = 1440 - 130;  // a 1440px window, less 20px body padding, the
                              // .container's 24px + 1px, the .section's 18px +
                              // 1px and the lane scroller's 1px, both sides
const FIT_MS_PER_PX = (1800000 * 1.1) / (CANVAS_W - 96 - 8); // LABEL_W = 96

test('[measurement] the zoom a 30-minute session fits into has a 20-second grid', () => {
    // Not a regression test — snapStep is unchanged behaviour. It pins the
    // measurement the drag rules are justified with: retune the ~8px target
    // and the figures in lane_canvas.js stop being true here, not in the wild.
    assert.equal(Math.round(FIT_MS_PER_PX), 1642);
    assert.equal(snapStep(FIT_MS_PER_PX), 20000);
    assert.equal(Math.round(snapStep(FIT_MS_PER_PX) / FIT_MS_PER_PX), 12, 'px per cell');
    // The same session on a 390px phone window: still a two-minute grid, and
    // note the chrome is NOT the desktop 130px — style.css's `@media
    // (max-width: 640px)` cuts body padding to 8px, .container to 13px and
    // .section to 13px, so 37px a side and a 316px canvas.
    const phone = (1800000 * 1.1) / (390 - 2 * 37 - 96 - 8);
    assert.equal(Math.round(phone), 9340);
    assert.equal(snapStep(phone), 120000);
});

test('a value-only drag leaves the keyframe\'s time exactly where it was', () => {
    const keys = [keyframe(0, 100, 'step'), keyframe(613000, 50, 'step'),
                  keyframe(620000, 9, 'step')];
    const kf = keys[1];
    // Pre-fix the handler wrote snapTime(xToTime(p.x)) on every pointermove,
    // which for this keyframe is 620000 — the sibling's time — from the very
    // first move, before the user had travelled a pixel sideways.
    for (const dx of [0, 1, -1, DRAG_X_DEADZONE_PX, -DRAG_X_DEADZONE_PX]) {
        assert.equal(dragTime(keys, kf, 613000, dx, FIT_MS_PER_PX), 613000,
                     'dxPx = ' + dx);
    }
    assert.equal(dragTime(keys, kf, 613000, DRAG_X_DEADZONE_PX + 1, FIT_MS_PER_PX),
                 620000, 'a px past the dead zone the time axis engages again');
});

test('a value-only drag does not consume the neighbour 4.3px away', () => {
    // The measured loss, end to end: 613000 and 620000 are 4.3px apart at this
    // zoom, so the user sees one fat dot, drags it down to re-dial the carrier,
    // and the file comes back with one row fewer than the canvas drew.
    const keys = [keyframe(0, 300, 'step'), keyframe(613000, 200, 'step'),
                  keyframe(620000, 100, 'step')];
    const kf = keys[1];
    dragKeyframe(keys, kf, 613000, 0, FIT_MS_PER_PX, 205);  // pointermove, y only
    dragKeyframe(keys, kf, 613000, 2, FIT_MS_PER_PX, 210);  // ... and a 2px wobble
    assert.equal(commitKeyframeDrag(keys, kf, 613000), 0, 'nothing to collapse');
    assert.deepEqual(keys.map(k => [k.t, k.v]),
                     [[0, 300], [613000, 210], [620000, 100]]);
    const text = serialize(lanesToDoc(laneWith(keys), { withMeta: false }).doc);
    assert.equal(text.trim().split('\n').join('|'),
        ['A 0 300 0 60 0 1',
         'A 613000 210 0 60 0 1',              // the value the drag was for
         'A 620000 100 0 60 0 1'].join('|'));  // and the row it was not
});

test('[contract, passes pre-fix] the drag follows travel, not the grab point', () => {
    // keyframeAt accepts a grab up to HIT_R = 22px off the dot, which at this
    // zoom is 36 s of time the keyframe used to teleport through on the first
    // pointermove, because the handler read the pointer's position. The offset
    // is gone from the arithmetic — but `p.x - drag.downX` is in onPointerMove,
    // which this suite cannot execute, so a grab-centred drag writes the same
    // time either way and this only pins the travel contract.
    const keys = [keyframe(0, 100, 'step'), keyframe(613000, 50, 'step')];
    const kf = keys[1];
    dragKeyframe(keys, kf, 613000, 30, FIT_MS_PER_PX, 50);
    assert.equal(kf.t, 660000);
    assert.ok(Math.abs((kf.t - 613000) - 30 * FIT_MS_PER_PX)
              <= snapStep(FIT_MS_PER_PX) / 2, 'within a snap of the 30px travelled');
});

test('[contract, passes pre-fix] a sideways drop onto the sibling you aimed at still merges', () => {
    const keys = [keyframe(0, 100, 'step'), keyframe(613000, 50, 'step'),
                  keyframe(620000, 9, 'step')];
    const kf = keys[1];
    // 5px of travel puts the pointer 0.7px from the sibling's dot: this is the
    // deliberate merge of section (4), and it still works. Revert (A) merged
    // this and much more, so the test cannot fail on the bug section (5) is
    // about — it is here to catch a future over-tightening of the rules above.
    // (It does fail on reverts (C) and (E), which move or remove the collapse:
    // against those it is a real regression test, just not against (A).)
    dragKeyframe(keys, kf, 613000, 5, FIT_MS_PER_PX, 50);
    assert.equal(kf.t, 620000);
    assert.equal(commitKeyframeDrag(keys, kf, 613000), 1);
    assert.deepEqual(keys.map(k => [k.t, k.v]), [[0, 100], [620000, 50]]);
    assert.equal(freqRows(laneWith(keys)).length, 2, 'one row per drawn dot');
});

test('a snap that would land on a sibling the pointer is 5.7px short of is refused', () => {
    const keys = [keyframe(0, 100, 'step'), keyframe(613000, 50, 'step'),
                  keyframe(620000, 9, 'step')];
    const kf = keys[1];
    dragKeyframe(keys, kf, 613000, 10, FIT_MS_PER_PX, 50);
    // 10px of travel is t=629418, which the 20 s grid rounds back onto the
    // sibling's 620000 from 5.7px away — further than DROP_AIM_PX, so the
    // millisecond wins over the grid and both keyframes keep a row.
    assert.ok((629418 - 620000) / FIT_MS_PER_PX > DROP_AIM_PX);
    assert.equal(kf.t, 629418);
    assert.equal(commitKeyframeDrag(keys, kf, 613000), 0);
    assert.deepEqual(keys.map(k => k.t), [0, 620000, 629418]);
    assert.equal(freqRows(laneWith(keys)).length, 3);
});

test('tap-to-add obeys the same refusal, as arithmetic', () => {
    // The harness drives this through the handlers below; here is the rule on
    // its own, because the add path reaches the grid from the other side (an
    // absolute t, no travel and no dead zone to fall back on).
    const keys = [keyframe(0, 100, 'step'), keyframe(620000, 9, 'step')];
    assert.equal(addTime(keys, 629851, FIT_MS_PER_PX), 629851,
                 '6px short of the dot: the millisecond wins over the grid');
    assert.equal(addTime(keys, 621209, FIT_MS_PER_PX), 620000,
                 '0.7px off it: aimed, so the grid finishes the job');
    assert.equal(addTime([], 629851, FIT_MS_PER_PX), 620000,
                 'and with no sibling to protect, the grid always wins');
});

test('a drag that comes back to where it started consumes nothing', () => {
    // Including from a model an older build could save: two keyframes already
    // sharing a timestamp. Collapsing THOSE belongs to the add / merge paths
    // of section (4) — a drag that ended where it began may not spend one.
    const twin = keyframe(1000, 40, 'step');
    const keys = [keyframe(0, 100, 'step'), keyframe(1000, 20, 'step'), twin];
    dragKeyframe(keys, twin, 1000, 40, 50, 45);   // out (50 ms/px: a 500 ms grid)
    assert.equal(twin.t, 3000);
    dragKeyframe(keys, twin, 1000, 0, 50, 45);    // ... and back
    assert.equal(twin.t, 1000);
    assert.equal(commitKeyframeDrag(keys, twin, 1000), 0);
    assert.equal(keys.length, 3, 'the pre-existing double is not this gesture\'s to spend');
});

// Half a snap cell in px — the most the grid can ever displace a key, and so
// the band inside which a snap can reach a dot the pointer is not on.
const HALF_CELL_PX = (msPerPx) => snapStep(msPerPx) / msPerPx / 2;

// 13_astral_deep's LED lane: channel mask 10, `bright` under test, the other
// five fields keyed at t=0 the way the session has them (freq 0, duty 50,
// 255/80/0 amber).
function ledLaneWithBright(keys) {
    return {
        version: 1,
        lanes: [{
            kind: 'led', key: 10, name: 'm10', collapsed: false, colorSplit: false,
            sub: {
                freq: { keys: [keyframe(0, 0, 'step')] },
                duty: { keys: [keyframe(0, 50, 'step')] },
                bright: { keys },
                r: { keys: [keyframe(0, 255, 'step')] },
                g: { keys: [keyframe(0, 80, 'step')] },
                b: { keys: [keyframe(0, 0, 'step')] },
            },
        }],
    };
}

test('the measured loss itself: 13_astral_deep\'s 15 s brightness key keeps the LFO', () => {
    // Not a reconstruction of the arithmetic but the rows from the shipped
    // session the comment in lane_canvas.js cites, at that session's own zoom.
    // sessions/library/13_astral_deep.ledc ends at `3000000 0 50 0 255 80 0 10`
    // (50 min), so fitToContent gives 2736 ms/px and a 30 s grid, and its
    //   15000 0 50 12 255 80 0 10
    //   30000 0 50 ~4:10:12000 255 80 0 10
    // put LED brightness keys 15 s apart — 5.5 px, one fat dot on screen.
    // Pre-fix, pulling the 15 s dot DOWN to re-dial 12% snapped it to 30000
    // (Math.round(0.5) rounds up) and the collapse took the second key with
    // the whole `~4:10:12000` breathing LFO on it.
    const msPerPx = (3000000 * 1.1) / (CANVAS_W - 96 - 8);
    assert.equal(Math.round(msPerPx), 2736);
    assert.equal(snapStep(msPerPx), 30000);
    // 15000 is exactly half of the 30 s cell — 5.5 px — and Math.round takes a
    // .5 upwards, so pre-fix the snap landed squarely on the sibling.
    assert.ok(Math.abs(15000 / msPerPx - HALF_CELL_PX(msPerPx)) < 1e-9,
              'exactly half a cell from the sibling');
    assert.ok(Math.abs(15000 / msPerPx - 5.48) < 0.01, '5.5 px apart on screen');

    const bright = [keyframe(15000, 12, 'step'),
                    keyframe(30000, 4, 'sine', 10, 12000)];
    const lanes = ledLaneWithBright(bright);
    dragKeyframe(bright, bright[0], 15000, 0, msPerPx, 9);   // pointermove, y only
    dragKeyframe(bright, bright[0], 15000, 2, msPerPx, 7);   // ... and a 2px wobble
    assert.equal(commitKeyframeDrag(bright, bright[0], 15000), 0);
    const text = serialize(lanesToDoc(lanes, { withMeta: false }).doc);
    assert.equal(text.trim().split('\n').join('|'),
        ['0 0 50 7 255 80 0 10',              // held flat back to the lane's t=0
         '15000 0 50 7 255 80 0 10',          // the brightness the drag was for
         '30000 0 50 ~4:10:12000 255 80 0 10'].join('|'), 'the LFO row survives');
});

// ---- (6) the same two gestures, driven through the real handlers ----------
//
// The rules above are helpers so they can be tested as arithmetic, but two of
// them are only worth as much as the handler's wiring: that onPointerMove
// hands dragTime the pointer's TRAVEL rather than its position, and that the
// tap-to-add path goes through addTime. Neither is reachable from the helpers,
// and the suite had no DOM, so both were labelled contracts. This harness
// stubs the four browser things createLaneCanvas touches — a 2D context (every
// call a no-op: nothing reads the canvas back), window.devicePixelRatio,
// requestAnimationFrame (inline, so a gesture has rendered before the next
// assertion) and the pointer-event plumbing — which is enough to dispatch a
// real gesture at a real pixel and look at the model afterwards.
globalThis.window = globalThis.window || {};
globalThis.window.devicePixelRatio = 1;
globalThis.requestAnimationFrame = (fn) => { fn(); return 0; };

const CTX_CALLS = ['setTransform', 'clearRect', 'fillRect', 'strokeRect',
    'beginPath', 'moveTo', 'lineTo', 'stroke', 'fill', 'arc', 'fillText',
    'setLineDash'];

// Layout constants mirrored from lane_canvas.js (RULER_H, LANE_HEAD_H, SUB_H,
// LABEL_W, and valToY's 8px pad): to click a dot the harness has to know where
// the strip holding it was painted. `vol` is AUDIO_CELL_FIELDS index 2 and its
// FIELD_RANGES entry is a fixed [0, 100], so its pixels are predictable in a
// way a derived-range field like freq is not.
const RULER_H = 26, LANE_HEAD_H = 22, SUB_H = 46, LABEL_W = 96, PAD = 8;
const VOL_Y = RULER_H + LANE_HEAD_H + 2 * SUB_H;
const volToY = (v) => VOL_Y + (SUB_H - PAD) - (v / 100) * (SUB_H - 2 * PAD);
const tToX = (t) => LABEL_W + t / FIT_MS_PER_PX;

// A lane whose `vol` strip is under test, with a key at 1800000 so
// fitToContent lands on exactly FIT_MS_PER_PX — the 30-minute zoom above.
function volLane(volKeys) {
    const lanes = laneWith([keyframe(0, 200, 'step'), keyframe(1800000, 200, 'step')]);
    lanes.lanes[0].sub.vol.keys = volKeys;
    return lanes;
}

function harness(lanes) {
    const ctx = {};
    for (const m of CTX_CALLS) ctx[m] = () => {};
    const on = new Map();
    const canvas = {
        clientWidth: CANVAS_W, width: 0, height: 0, style: {},
        getContext: () => ctx,
        getBoundingClientRect: () => ({ left: 0, top: 0 }),
        addEventListener: (type, fn) => on.set(type, fn),
        removeEventListener: (type) => on.delete(type),
    };
    const calls = [];
    const api = createLaneCanvas(canvas, {
        getModel: () => lanes,
        beginEdit: () => calls.push('begin'),
        commit: () => calls.push('commit'),
        openInspector: () => calls.push('inspector'),
        openContextMenu: () => calls.push('menu'),
        getPlayheadMs: () => null,
    });
    api.fitToContent();
    api.renderNow();                      // fills the strip layout to hit-test
    const send = (type, x, y) => on.get(type)(
        { pointerId: 1, clientX: x, clientY: y });
    return {
        calls, api,
        down: (x, y) => send('pointerdown', x, y),
        move: (x, y) => send('pointermove', x, y),
        up: (x, y) => send('pointerup', x, y),
    };
}

test('[harness self-check] the fake canvas puts the dots where the tests aim', () => {
    const lanes = volLane([keyframe(0, 60, 'step'), keyframe(620000, 9, 'step')]);
    const h = harness(lanes);
    // Tapping the t=620000 dot dead centre must SELECT it (inspector, no new
    // keyframe) — if this stops holding, the geometry below is aimed at blank
    // canvas and the tests that follow would pass for the wrong reason.
    h.down(tToX(620000), volToY(9));
    h.up(tToX(620000), volToY(9));
    assert.deepEqual(h.calls, ['inspector']);
    assert.equal(lanes.lanes[0].sub.vol.keys.length, 2, 'nothing added');
});

test('a value-only drag through the real handlers keeps both keyframes', () => {
    // The end-to-end form of the loss: grab the 613000 dot, pull it straight
    // UP to re-dial the volume, let go. Pre-fix the pointermove wrote
    // snapTime(xToTime(p.x)) = 620000 — its neighbour 4.3px away — and
    // pointerup collapsed the pair, so the file came back one row short.
    const lanes = volLane([keyframe(0, 60, 'step'), keyframe(613000, 50, 'step'),
                           keyframe(620000, 9, 'step')]);
    const keys = lanes.lanes[0].sub.vol.keys;
    const kf = keys[1];
    const h = harness(lanes);
    h.down(tToX(613000), volToY(50));
    h.move(tToX(613000), volToY(80));
    h.up(tToX(613000), volToY(80));
    assert.equal(kf.t, 613000, 'the time the user did not touch');
    assert.equal(kf.v, 80, 'the value the user dragged for');
    assert.deepEqual(keys.map(k => [k.t, k.v]), [[0, 60], [613000, 80], [620000, 9]]);
    assert.deepEqual(freqRows(lanes).map(r => r.t), [0, 613000, 620000, 1800000]);
});

test('a drag grabbed 20px off the dot moves by travel, not onto the pointer', () => {
    // keyframeAt reaches HIT_R = 22px, so this grab lands on the keyframe —
    // and 20px is 33 s at this zoom, which is what the keyframe used to
    // teleport on the first pointermove when the handler read p.x instead of
    // p.x - downX. Vertical-only move, so the time must not budge at all.
    const lanes = volLane([keyframe(0, 60, 'step'), keyframe(613000, 50, 'step')]);
    const keys = lanes.lanes[0].sub.vol.keys;
    const h = harness(lanes);
    const grabX = tToX(613000) + 20;
    h.down(grabX, volToY(50));
    h.move(grabX, volToY(70));
    h.up(grabX, volToY(70));
    assert.equal(keys[1].t, 613000);
    assert.equal(keys[1].v, 70);
    assert.ok(Math.abs(20 * FIT_MS_PER_PX - 32836) < 1, '20px is 33 s of time');
});

test('tap-to-add below a dot does not let the grid eat the dot it missed', () => {
    // 6px to the right of the t=620000 dot and 23px above it: outside the 22px
    // circle (so this is an add, not a select) but inside the 20 s grid's
    // 6.1px half-cell, so pre-fix the add snapped to 620000 and its
    // dropCoincidentKeys deleted the keyframe the tap had visibly missed.
    const lanes = volLane([keyframe(0, 60, 'step'), keyframe(620000, 9, 'step')]);
    const keys = lanes.lanes[0].sub.vol.keys;
    const h = harness(lanes);
    h.down(tToX(620000) + 6, volToY(85));
    h.up(tToX(620000) + 6, volToY(85));
    assert.deepEqual(h.calls, ['begin', 'commit', 'inspector'], 'the add path ran');
    assert.equal(keys.length, 3, 'the dot 23px below is still there');
    assert.deepEqual(keys.map(k => k.t), [0, 620000, 629851]);
    assert.equal(keys[1].v, 9, 'untouched');
    assert.equal(keys[2].v, 85, 'and the tap landed under the cursor');
});

test('tap-to-add ON a dot still replaces it', () => {
    // The other half of the pair: a tap genuinely ON the dot — within
    // DROP_AIM_PX in BOTH axes — is the deliberate "re-dial this row" gesture,
    // so the grid is allowed to finish the job and the row carries the value.
    const lanes = volLane([keyframe(0, 60, 'step'), keyframe(620000, 9, 'step')]);
    const keys = lanes.lanes[0].sub.vol.keys;
    const h = harness(lanes);
    h.down(tToX(620000) + 2, volToY(9));
    h.up(tToX(620000) + 2, volToY(9));
    assert.deepEqual(keys.map(k => k.t), [0, 620000], 'the dot was replaced, not doubled');
});

test('a tap 2px off in x but far off in VALUE does not eat the dot', () => {
    // The hole the x-only refusal left open, and the reason the refusal is now
    // 2D. valToY spreads a field over 30 px, so a tap can sit inside the ~6 px
    // half-cell in x while being 20+ px away in value — outside the HIT_R = 22
    // px circle that would have routed it to select, i.e. visibly nowhere near
    // the dot. The old rule merged anyway and silently deleted a keyframe the
    // user never aimed at; worse, on an EXACT x hit its `snapped !== raw` guard
    // could not fire at all, so any vertical distance was ignored.
    //
    // NOT covered, and a real limit rather than a bug: a tap on the dot's EXACT
    // x column still replaces it however far off in value it is. There the grid
    // is not pulling the tap anywhere — the pointer is literally on that
    // timestamp, one (lane, t) is one row, and the only alternatives are to
    // invent a 1 ms offset (two rows a millisecond apart) or to ignore the tap.
    // Replacing is the honest resolution. The fix is about the grid MOVING a
    // tap onto a timestamp the user missed on screen.
    const lanes = volLane([keyframe(0, 60, 'step'), keyframe(620000, 9, 'step')]);
    const keys = lanes.lanes[0].sub.vol.keys;
    const h = harness(lanes);
    h.down(tToX(620000) + 2, volToY(85));
    h.up(tToX(620000) + 2, volToY(85));
    assert.equal(keys.length, 3, 'the dot 23px away survives');
    assert.equal(keys[1].v, 9, 'and is untouched');
    assert.equal(keys[2].v, 85, 'the new key landed under the cursor');
});


test('a handle that stops being painted stops being clickable', () => {
    // The assertion the suite used to say it could not make, and the reason
    // section (6) exists at all: render() forgetting the previous frame's
    // handle targets only matters COMPOSED with onPointerDown testing the mid
    // handle before keyframeAt and tap-to-add. On revert (B) — the point
    // parked on the keyframe, reset() a no-op — deleting the keyframe that
    // ENDED the segment left its handle clickable at the old pixel, so an
    // ordinary click on blank canvas cycled the curve of a dot 134px away.
    const lanes = volLane([keyframe(0, 60, 'step'), keyframe(440000, 9, 'step')]);
    const keys = lanes.lanes[0].sub.vol.keys;
    const h = harness(lanes);
    // The 0 -> 440000 segment is 268px wide, well past MID_HANDLE_MIN_SEG_PX =
    // 30, so its handle is painted at the midpoint — x=230, t=220000, which is
    // a whole number of 20 s cells, so the add below cannot land on a cell
    // boundary and depend on which way Math.round breaks the tie. Positive
    // control first: clicking the handle cycles the shape step -> lin ('>').
    const mx = (tToX(0) + tToX(440000)) / 2;
    const my = (volToY(60) + volToY(9)) / 2;
    h.down(mx, my); h.up(mx, my);
    assert.equal(keys[0].shape, 'lin', 'the painted handle cycles the curve');
    assert.deepEqual(h.calls, ['begin', 'commit'], 'and does not open the inspector');

    // Now delete the keyframe that ended the segment. keys[0] is trailing, so
    // drawStrip holds it flat to the right edge and paints no handle for it.
    keys.pop();
    h.calls.length = 0;
    h.api.renderNow();
    h.down(mx, my); h.up(mx, my);
    assert.equal(keys[0].shape, 'lin', 'the same pixel must not cycle it again');
    assert.deepEqual(h.calls, ['begin', 'commit', 'inspector'],
                     'the click reached tap-to-add, as a click on blank canvas should');
    assert.deepEqual(keys.map(k => k.t), [0, 220000], 'it added a keyframe there');
});
