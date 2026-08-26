// Unit tests for the Table view's pure helpers (mask<->chips, time parse/format)
// and the compound-cell label/glyph helpers. These touch no DOM so they run in
// plain Node, mirroring test/text_view.test.js.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    chipsToMask, maskToChips, parseTimeInput, formatTimeMs, computeTimeGroups, orderRowsForDisplay, computeChannelGroups, computeRowGroups,
} from '../src/js/gen/views/table.js';
import {
    cellTargetHint,
    cellInlineGlyph, cellLabel,
} from '../src/js/gen/views/cell.js';
import { cell } from '../src/js/gen/model.js';

// ---- mask <-> chips --------------------------------------------------------

test('chipsToMask: bit 0 => channel 1 => mask 1', () => {
    const chips = [true, false, false, false, false, false, false, false];
    assert.equal(chipsToMask(chips), 1);
});

test('chipsToMask: all 8 chips => 0xFF', () => {
    assert.equal(chipsToMask(new Array(8).fill(true)), 0xFF);
});

test('chipsToMask: channels 1+4 => mask 9 (bits 0 and 3)', () => {
    const chips = [true, false, false, true, false, false, false, false];
    assert.equal(chipsToMask(chips), 9);
});

test('chipsToMask: legacy four (0x0F) and never a counter', () => {
    const chips = [true, true, true, true, false, false, false, false];
    const m = chipsToMask(chips);
    assert.equal(m, 0x0F);
    assert.notEqual(m, 4, 'must be an OR\'d bitmask, not a count of selected chips');
});

test('maskToChips: round-trips with chipsToMask for every 8-bit value', () => {
    for (let m = 0; m <= 0xFF; m++) {
        assert.equal(chipsToMask(maskToChips(m)), m);
    }
});

test('maskToChips: 145 (0b10010001) => channels 1,5,8', () => {
    assert.deepEqual(maskToChips(145), [true, false, false, false, true, false, false, true]);
});

// ---- time parse / format ---------------------------------------------------

test('parseTimeInput: plain integer ms', () => {
    assert.equal(parseTimeInput('5000'), 5000);
    assert.equal(parseTimeInput('0'), 0);
});

test('parseTimeInput: mm:ss.mmm', () => {
    assert.equal(parseTimeInput('1:05.432'), 65432);
    assert.equal(parseTimeInput('0:30'), 30000);
    assert.equal(parseTimeInput('2:00.000'), 120000);
});

test('parseTimeInput: leading-colon shorthand and fractional padding', () => {
    assert.equal(parseTimeInput(':05'), 5000);
    assert.equal(parseTimeInput('0:00.5'), 500);   // .5 => 500 ms
    assert.equal(parseTimeInput('0:00.05'), 50);   // .05 => 50 ms
});

test('parseTimeInput: invalid -> null', () => {
    assert.equal(parseTimeInput(''), null);
    assert.equal(parseTimeInput('abc'), null);
    assert.equal(parseTimeInput('1:99'), null);    // seconds > 59
    assert.equal(parseTimeInput(null), null);
});

test('formatTimeMs: formats with padded ss + mmm', () => {
    assert.equal(formatTimeMs(0), '0:00.000');
    assert.equal(formatTimeMs(65432), '1:05.432');
    assert.equal(formatTimeMs(500), '0:00.500');
});

test('format/parse round-trip across sample times', () => {
    for (const ms of [0, 1, 500, 5000, 65432, 120000, 599999]) {
        assert.equal(parseTimeInput(formatTimeMs(ms)), ms);
    }
});

// ---- compound-cell label / glyph -------------------------------------------

test('cellInlineGlyph: per interp kind', () => {
    assert.equal(cellInlineGlyph(cell(5, 'none')), '');
    assert.equal(cellInlineGlyph(cell(5, 'lin')), '→');
    assert.equal(cellInlineGlyph(cell(5, 'quad')), 'x²');
    assert.equal(cellInlineGlyph(cell(5, 'sine', 10, 1000)), '∿');
    assert.equal(cellInlineGlyph(cell(5, 'tri', 10, 1000)), '∿');
});

test('cellLabel: step / ramp / periodic', () => {
    assert.equal(cellLabel(cell(12, 'none')), '12');
    assert.equal(cellLabel(cell(12, 'lin')), '→ 12');
    assert.equal(cellLabel(cell(8, 'quad')), 'x² 8');
    assert.equal(cellLabel(cell(10, 'sine', 20, 500)), '10∿20');
});

test('cellLabel: periodic with null modEnd falls back to start value', () => {
    assert.equal(cellLabel({ value: 40, interp: 'sine', modEnd: null, modPeriodMs: null }), '40∿40');
});

// ---- time grouping (thicker divider between instants) ----------------------

const led    = t => ({ kind: 'led',    time: t });
const audio  = t => ({ kind: 'audio',  time: t });
const speech = t => ({ kind: 'speech', time: t });
const cmt    = () => ({ kind: 'comment', text: '# x' });
const blank  = () => ({ kind: 'blank' });

test('computeTimeGroups: rows sharing a timestamp form one group', () => {
    assert.deepEqual(
        computeTimeGroups([audio(3000), led(3000), speech(3000), audio(60000), led(60000)]),
        [1, 1, 1, 2, 2]);
});

test('computeTimeGroups: a comment heads the group that follows it', () => {
    // The comment must land in group 2 with the rows it introduces, not in
    // group 1 with the rows above it.
    assert.deepEqual(
        computeTimeGroups([audio(3000), led(3000), cmt(), audio(60000), led(60000)]),
        [1, 1, 2, 2, 2]);
});

test('computeTimeGroups: leading untimed rows join the first real group', () => {
    assert.deepEqual(computeTimeGroups([cmt(), blank(), led(0), led(0)]), [1, 1, 1, 1]);
});

test('computeTimeGroups: trailing untimed rows stay with the last group', () => {
    assert.deepEqual(computeTimeGroups([led(0), led(500), cmt(), blank()]), [1, 2, 2, 2]);
});

test('computeTimeGroups: a repeated timestamp after another does not split', () => {
    assert.deepEqual(computeTimeGroups([led(0), audio(0), led(0)]), [1, 1, 1]);
});

test('computeTimeGroups: an all-untimed document still yields one group', () => {
    assert.deepEqual(computeTimeGroups([cmt(), cmt()]), [1, 1]);
});

test('computeTimeGroups: returns an array parallel to rows', () => {
    const rows = [led(0), cmt(), audio(10)];
    assert.equal(computeTimeGroups(rows).length, rows.length);
});

// ---- display ordering (S, LED, audio within one instant) -------------------

const kindsInOrder = rows => orderRowsForDisplay(rows).map(i => rows[i].kind);

test('orderRowsForDisplay: within one instant it is speech, LED, audio', () => {
    assert.deepEqual(kindsInOrder([audio(3000), led(3000), speech(3000)]),
                     ['speech', 'led', 'audio']);
});

test('orderRowsForDisplay: already-ordered rows are left alone', () => {
    assert.deepEqual(kindsInOrder([speech(0), led(0), audio(0)]),
                     ['speech', 'led', 'audio']);
});

test('orderRowsForDisplay: each instant is sorted independently, groups run in time order', () => {
    assert.deepEqual(
        kindsInOrder([audio(3000), speech(3000), audio(60000), led(60000)]),
        ['speech', 'audio', 'led', 'audio']);
});

test('orderRowsForDisplay: a comment stays at the head of the block it introduces', () => {
    assert.deepEqual(kindsInOrder([cmt(), audio(0), speech(0), led(0)]),
                     ['comment', 'speech', 'led', 'audio']);
});

test('orderRowsForDisplay: two rows of one kind keep their file order', () => {
    const rows = [led(0), led(0)];
    rows[0].tag = 'first'; rows[1].tag = 'second';
    const out = orderRowsForDisplay(rows).map(i => rows[i].tag);
    assert.deepEqual(out, ['first', 'second']);
});

test('orderRowsForDisplay: returns every index exactly once', () => {
    const rows = [audio(0), cmt(), led(0), speech(5), blank(), audio(5)];
    const out = orderRowsForDisplay(rows);
    assert.deepEqual([...out].sort((a, b) => a - b), [0, 1, 2, 3, 4, 5]);
});

test('orderRowsForDisplay: does not mutate the rows array', () => {
    const rows = [audio(0), speech(0), led(0)];
    const before = rows.map(r => r.kind);
    orderRowsForDisplay(rows);
    assert.deepEqual(rows.map(r => r.kind), before);
});

// ---- ramp destination sub-label -------------------------------------------

test('cellTargetHint: a linear ramp reports where it lands', () => {
    assert.equal(cellTargetHint({ value: 10, interp: 'lin' }, 40), '\u21b3 40');
});

test('cellTargetHint: a quadratic ramp reports it too', () => {
    assert.equal(cellTargetHint({ value: 10, interp: 'quad' }, 40), '\u21b3 40');
});

test('cellTargetHint: a ramp with no later entry says it holds', () => {
    // Silence here would read as "no ramp" even though the cell is coloured as one.
    assert.equal(cellTargetHint({ value: 10, interp: 'lin' }, null), '\u21b3 holds');
    assert.equal(cellTargetHint({ value: 10, interp: 'lin' }, undefined), '\u21b3 holds');
});

test('cellTargetHint: a step value has no destination', () => {
    assert.equal(cellTargetHint({ value: 10, interp: 'none' }, 40), '');
});

test('cellTargetHint: a periodic mod gets none — the trigger already shows start-end', () => {
    assert.equal(cellTargetHint({ value: 10, interp: 'sine', modEnd: 20 }, 40), '');
});

test('cellTargetHint: a zero target is reported, not treated as absent', () => {
    assert.equal(cellTargetHint({ value: 10, interp: 'lin' }, 0), '\u21b3 0');
});

test('cellTargetHint: tolerates a missing cell', () => {
    assert.equal(cellTargetHint(null, 40), '');
});

// ---- grouping by timestamp value, not by adjacency -------------------------
// Sessions are commonly written channel-by-channel, so one instant's rows are
// scattered through the file. Grouping on consecutive runs missed that and left
// 35 of the 77 library sessions with every row in a group of its own.

test('computeTimeGroups: rows at one time group together even when far apart', () => {
    assert.deepEqual(
        computeTimeGroups([audio(0), audio(1000), led(0), led(1000)]),
        [1, 2, 1, 2]);
});

test('computeTimeGroups: groups are numbered in ascending time, not file order', () => {
    assert.deepEqual(computeTimeGroups([led(2000), led(0), led(1000)]), [3, 1, 2]);
});

test('orderRowsForDisplay: an audio section then an LED section interleaves by time', () => {
    const rows = [audio(0), audio(1000), led(0), led(1000)];
    assert.deepEqual(orderRowsForDisplay(rows).map(i => rows[i].kind + '@' + rows[i].time),
                     ['led@0', 'audio@0', 'led@1000', 'audio@1000']);
});

test('orderRowsForDisplay: chronological even when the file is not', () => {
    const rows = [led(2000), audio(0), led(0)];
    assert.deepEqual(orderRowsForDisplay(rows).map(i => rows[i].kind + '@' + rows[i].time),
                     ['led@0', 'audio@0', 'led@2000']);
});

test('computeTimeGroups: a comment before a late-file row heads that row\'s time block', () => {
    // cmt sits before led(0); led(0) belongs to the t=0 block, so the comment does too.
    assert.deepEqual(computeTimeGroups([audio(0), audio(1000), cmt(), led(0)]), [1, 2, 1, 1]);
});

// ---- channel sort ----------------------------------------------------------

const ledM  = (t, mask) => ({ kind: 'led',   time: t, mask });
const audC  = (t, ch)   => ({ kind: 'audio', time: t, channel: ch });

test('computeChannelGroups: speech first, then LED masks, then audio channels', () => {
    const rows = [audC(0, 2), ledM(0, 4), speech(0), audC(0, 1), ledM(0, 1)];
    // speech=1, led mask1=2, led mask4=3, audio ch1=4, audio ch2=5
    assert.deepEqual(computeChannelGroups(rows), [5, 3, 1, 4, 2]);
});

test('computeChannelGroups: LED rows key on the exact mask, not the individual channels', () => {
    // mask 3 (ch1+2) is its own lane — the row cannot belong to two groups.
    const rows = [ledM(0, 1), ledM(0, 3), ledM(0, 1)];
    assert.deepEqual(computeChannelGroups(rows), [1, 2, 1]);
});

test('computeChannelGroups: rows with no channel land in a trailing group', () => {
    const rows = [ledM(0, 1), { kind: 'bg' }, cmt()];
    const g = computeChannelGroups(rows);
    assert.equal(g[0], 1);
    assert.equal(g[1], g[2]);
    assert.ok(g[1] > g[0]);
});

test('orderRowsForDisplay: channel mode gathers a lane and sorts it in time', () => {
    const rows = [audC(1000, 1), ledM(0, 1), audC(0, 1), ledM(1000, 1)];
    assert.deepEqual(
        orderRowsForDisplay(rows, 'channel').map(i => rows[i].kind + '@' + rows[i].time),
        ['led@0', 'led@1000', 'audio@0', 'audio@1000']);
});

test('orderRowsForDisplay: speech leads in channel mode', () => {
    const rows = [audC(0, 1), ledM(0, 1), speech(500)];
    assert.equal(orderRowsForDisplay(rows, 'channel').map(i => rows[i].kind)[0], 'speech');
});

test('orderRowsForDisplay: time mode is unchanged by the new argument', () => {
    const rows = [audC(0, 1), ledM(0, 1), speech(0)];
    assert.deepEqual(orderRowsForDisplay(rows), orderRowsForDisplay(rows, 'time'));
});

test('computeRowGroups: dispatches on mode', () => {
    const rows = [ledM(0, 1), audC(500, 1)];
    assert.deepEqual(computeRowGroups(rows, 'time'), computeTimeGroups(rows));
    assert.deepEqual(computeRowGroups(rows, 'channel'), computeChannelGroups(rows));
});
