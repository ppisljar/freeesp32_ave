// Config dropdown ordering: natural + case-insensitive by name.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { byConfigName } from '../src/js/gen/../configstore.js';

test('byConfigName: numeric prefixes order naturally (09 before 10)', () => {
    const names = ['10_gateway', '02_power_nap', '09_lucid', '1_intro'];
    assert.deepEqual(
        names.slice().sort(byConfigName),
        ['1_intro', '02_power_nap', '09_lucid', '10_gateway'],
    );
});

test('byConfigName: case-insensitive', () => {
    const names = ['Zebra', 'apple', 'Banana'];
    assert.deepEqual(names.slice().sort(byConfigName), ['apple', 'Banana', 'Zebra']);
});
