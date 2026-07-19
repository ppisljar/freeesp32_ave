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

test('byConfigName: _RGB variants group with their base session, in number order', () => {
    // Device (spiffs) returns files in scrambled filesystem order; the dropdown
    // must group each session's variants together and order them by number. In a
    // plain name sort '_' precedes '.', so the _RGB variant lands just before its
    // base — that's fine, what matters is they're adjacent and number-ordered.
    const names = [
        '02_power_nap_RGB.ledc', '10_lucid_presleep.ledc', '01_sleep_onset.ledc',
        '02_power_nap.ledc', '01_sleep_onset_RGB.ledc', '10_lucid_presleep_RGB.ledc',
    ];
    assert.deepEqual(names.slice().sort(byConfigName), [
        '01_sleep_onset_RGB.ledc', '01_sleep_onset.ledc',
        '02_power_nap_RGB.ledc', '02_power_nap.ledc',
        '10_lucid_presleep_RGB.ledc', '10_lucid_presleep.ledc',
    ]);
});
