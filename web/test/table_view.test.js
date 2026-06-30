// Unit tests for the Table view's pure helpers (mask<->chips, time parse/format)
// and the compound-cell label/glyph helpers. These touch no DOM so they run in
// plain Node, mirroring test/text_view.test.js.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    chipsToMask, maskToChips, parseTimeInput, formatTimeMs,
} from '../src/js/gen/views/table.js';
import {
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
