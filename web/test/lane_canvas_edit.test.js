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
// These drive the exported pure helpers and then the real lanes -> doc
// projection, like test/table_view.test.js and test/text_view.test.js do: the
// suite has no DOM harness, so the pointer plumbing itself (onPointerDown ->
// onPointerMove -> onPointerUp, and render() filling the handle cache) is not
// executed here. The one assertion that would need real pointer events is
// "clicking blank canvas after zooming out does nothing": the composition of
// render()'s cache reset with onPointerDown's handle-before-keyframe order.
// Its two halves are covered separately below.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    moveKeyframe, insertKeyframe, dropCoincidentKeys,
    createMidHandleCache, stripTransientKeyState,
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
    // t=0 predates every wave key, so steppedAt holds the EARLIEST one (2);
    // pre-fix the walk read the unsorted keys[0] and wrote the dragged marker's
    // 1 at t=0 and t=2000 instead.
    assert.deepEqual(waves, [[0, 2], [2000, 2], [3000, 1]]);
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

test('mid-handle cache: a painted handle is hit-testable', () => {
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

test('mid-handle cache: targets are scoped to the sublane being hit-tested', () => {
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
    assert.equal(stripTransientKeyState({ lanes: [] }), 0);
    assert.equal(stripTransientKeyState({ lanes: [{ kind: 'audio' }] }), 0);
    assert.equal(stripTransientKeyState({ lanes: [{ sub: { freq: {} } }] }), 0);
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
    // dropCoincidentKeys runs on pointerup, so only the landing spot collides.
    const keys = [keyframe(0, 100, 'step'), keyframe(1000, 200, 'step'),
                  keyframe(4000, 300, 'step')];
    const moved = keys[0];
    moveKeyframe(keys, moved, 1000, 150);   // mid-drag, passing over the sibling
    moveKeyframe(keys, moved, 2000, 150);   // ... and onward
    assert.equal(dropCoincidentKeys(keys, moved), 0);
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
