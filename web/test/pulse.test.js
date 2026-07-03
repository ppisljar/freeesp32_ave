// Pure tri-state pulse-field helpers shared by the table/lane/wizard editors.
// Convention: '' -> undefined (omit) ; '-' -> null (leave) ; value -> set.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    parsePulseCell, formatPulseCell, parsePulseEnv, formatPulseEnv,
    parsePulseJitter, formatPulseJitter,
} from '../src/js/gen/pulse.js';

test('compound-cell field: tri-state parse', () => {
    assert.equal(parsePulseCell(''), undefined, 'blank -> omit');
    assert.equal(parsePulseCell('   '), undefined, 'whitespace -> omit');
    assert.equal(parsePulseCell('-'), null, 'dash -> leave');
    const c = parsePulseCell('90');
    assert.deepEqual({ value: c.value, interp: c.interp }, { value: 90, interp: 'none' }, 'number -> step cell');
    assert.equal(parsePulseCell('junk').value, 0, 'non-numeric -> 0 cell');
});

test('compound-cell field: format round-trips the tri-state', () => {
    assert.equal(formatPulseCell(undefined), '');
    assert.equal(formatPulseCell(null), '-');
    assert.equal(formatPulseCell(parsePulseCell('12')), '12');
});

test('env field: tri-state parse/format', () => {
    assert.equal(parsePulseEnv(''), undefined);
    assert.equal(parsePulseEnv('-'), null);
    assert.equal(parsePulseEnv('3'), 3);
    assert.equal(formatPulseEnv(undefined), '');
    assert.equal(formatPulseEnv(null), '-');
    assert.equal(formatPulseEnv(4), '4');
});

test('jitter field: amp and amp:period', () => {
    assert.equal(parsePulseJitter(''), undefined);
    assert.equal(parsePulseJitter('-'), null);
    assert.deepEqual(parsePulseJitter('0.2'), { amp: 0.2, period: undefined });
    assert.deepEqual(parsePulseJitter('0.3:30000'), { amp: 0.3, period: 30000 });
    assert.equal(formatPulseJitter(undefined), '');
    assert.equal(formatPulseJitter(null), '-');
    assert.equal(formatPulseJitter({ amp: 0.2 }), '0.2', 'no period -> just amp');
    assert.equal(formatPulseJitter({ amp: 0.3, period: 30000 }), '0.3:30000');
});
