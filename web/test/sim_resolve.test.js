// Timeline resolution: the `-` carry, LED mask expansion, the ramp-target
// rules, and the channel-0 default. Each assertion is pinned to the firmware
// behaviour it mirrors, with the citation in the test name.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import {
    resolveAudioChannels, resolveLedChannels, evalCell, easeQuad, modShape,
    activeIndexAt, stateAt, sessionEndMs, AUDIO_DEFAULTS, LED_DEFAULTS,
} from '../src/js/gen/sim/resolve.js';
import { cell } from '../src/js/gen/model.js';

function doc(text) { return parse(text).doc; }

// ---- evalCell --------------------------------------------------------------

test('evalCell: a bare step holds its value', () => {
    const c = cell(42);
    assert.equal(evalCell(c, 99, 0, 1000, 0), 42);
    assert.equal(evalCell(c, 99, 0, 1000, 500), 42);
});

test('evalCell: linear ramp interpolates toward the target', () => {
    const c = cell(100, 'lin');
    assert.equal(evalCell(c, 200, 0, 1000, 0), 100);
    assert.equal(evalCell(c, 200, 0, 1000, 500), 150);
    assert.equal(evalCell(c, 200, 0, 1000, 1000), 200);
});

test('evalCell: ramp clamps outside its window instead of extrapolating', () => {
    const c = cell(100, 'lin');
    assert.equal(evalCell(c, 200, 1000, 2000, 0), 100);
    assert.equal(evalCell(c, 200, 1000, 2000, 9999), 200);
});

test('evalCell: a ramp with no target holds (device installs no sweep)', () => {
    const c = cell(100, 'lin');
    assert.equal(evalCell(c, null, 0, null, 500), 100);
});

test('easeQuad: piecewise ease-in-out, symmetric about the midpoint', () => {
    assert.equal(easeQuad(0), 0);
    assert.equal(easeQuad(0.5), 0.5);
    assert.equal(easeQuad(1), 1);
    assert.equal(easeQuad(0.25), 2 * 0.0625);          // 2p^2
    assert.ok(Math.abs(easeQuad(0.75) - (1 - 2 * 0.0625)) < 1e-12);
});

test('modShape: the five firmware shapes at their quarter points', () => {
    assert.equal(modShape('tri', 0), 0);
    assert.equal(modShape('tri', 0.5), 1);
    assert.equal(modShape('tri', 1), 0);
    assert.ok(Math.abs(modShape('sine', 0) - 0) < 1e-12);
    assert.ok(Math.abs(modShape('sine', 0.5) - 1) < 1e-12);
    assert.ok(Math.abs(modShape('sine', 0.25) - 0.5) < 1e-12);
    assert.equal(modShape('sawup', 0.3), 0.3);
    assert.ok(Math.abs(modShape('sawdn', 0.3) - 0.7) < 1e-12);
    assert.equal(modShape('sq', 0.4), 0);
    assert.equal(modShape('sq', 0.6), 1);
});

test('evalCell: a periodic mod oscillates value<->modEnd over its period', () => {
    const c = cell(10, 'tri', 20, 1000);
    assert.equal(evalCell(c, null, 0, null, 0), 10);
    assert.equal(evalCell(c, null, 0, null, 500), 20);
    assert.equal(evalCell(c, null, 0, null, 1000), 10);
    // Phase origin is the ENTRY's time, not t=0.
    assert.equal(evalCell(c, null, 2000, null, 2500), 20);
});

test('evalCell: period 0 returns the start (the device returns start_q)', () => {
    const c = cell(10, 'sine', 20, 0);
    assert.equal(evalCell(c, null, 0, null, 123), 10);
});

// ---- audio grouping --------------------------------------------------------

test('resolveAudioChannels: a channel-less A line lands on channel 0, not 1', () => {
    // config_parser.c:1816 — `? atoi(tokens[5]) : 0`.
    const { channels } = resolveAudioChannels(doc('A 0 200 0 80 0\n'));
    assert.deepEqual([...channels.keys()], [0]);
});

test('resolveAudioChannels: bare and explicit-0 lines share one channel', () => {
    const { channels } = resolveAudioChannels(doc('A 0 200 0 80 0\nA 1000 210 0 80 0 0\n'));
    assert.deepEqual([...channels.keys()], [0]);
    assert.equal(channels.get(0).length, 2);
});

test('resolveAudioChannels: an out-of-range channel folds into 0', () => {
    const { channels } = resolveAudioChannels(doc('A 0 200 0 80 0 20\n'));
    assert.deepEqual([...channels.keys()], [0]);
});

test('resolveAudioChannels: `-` carries the previous entry value forward', () => {
    const { channels } = resolveAudioChannels(
        doc('A 0 200 -50 80 0 1\nA 1000 - - - - 1\n'));
    const e = channels.get(1);
    assert.equal(e.length, 2);
    assert.equal(e[1].cells.freq.value, 200);
    assert.equal(e[1].cells.pan.value, -50);
    assert.equal(e[1].cells.vol.value, 80);
});

test('resolveAudioChannels: `-` at a first entry resolves to the channel default', () => {
    const { channels } = resolveAudioChannels(doc('A 0 - - - - 1\n'));
    const e = channels.get(1)[0];
    assert.equal(e.cells.freq.value, AUDIO_DEFAULTS.freq);
    assert.equal(e.cells.vol.value, AUDIO_DEFAULTS.vol);
});

test('resolveAudioChannels: freqR carries forward (absent means keep the right carrier)', () => {
    // audio_generator.c:1003 — AUD_SET_FREQR clear keeps the live right carrier,
    // so a later entry that moves freq but omits freqR stays BINAURAL.
    const { channels } = resolveAudioChannels(
        doc('A 0 200 0 80 0 1 210\nA 1000 205 0 80 0 1\n'));
    const e = channels.get(1);
    assert.equal(e[1].cells.freqR.value, 210);
});

test('resolveAudioChannels: a ramp into a `-` targets 0, not the held value', () => {
    // The parser memsets the entry (config_parser.c:1781), so the device reads a
    // hard zero out of the next entry regardless of its present bit.
    const { channels } = resolveAudioChannels(
        doc('A 0 >200 0 80 0 1\nA 1000 - 0 80 0 1\n'));
    const e = channels.get(1);
    assert.equal(e[0].t.freq.target, 0);
    assert.deepEqual(e[0].rampsIntoDash, ['freq']);
    assert.equal(evalCell(e[0].cells.freq, e[0].t.freq.target, 0, 1000, 500), 100);
});

test('resolveAudioChannels: a pulse-field ramp holds when nothing later sets it', () => {
    // config_parser.c:2445 find_next_audio_with is present-aware, so an omitted
    // duty downstream means HOLD rather than ramp-to-zero.
    const { channels } = resolveAudioChannels(
        doc('A 0 200 0 80 10 1 0 0 >40\nA 1000 200 0 80 10 1\n'));
    const e = channels.get(1);
    assert.equal(e[0].t.duty.target, null);
    assert.equal(evalCell(e[0].cells.duty, e[0].t.duty.target, 0, null, 500), 40);
});

test('resolveAudioChannels: a pulse-field ramp targets the next entry that sets it', () => {
    const { channels } = resolveAudioChannels(
        doc('A 0 200 0 80 10 1 0 0 >20\nA 1000 200 0 80 10 1\nA 2000 200 0 80 10 1 0 0 60\n'));
    const e = channels.get(1);
    assert.equal(e[0].t.duty.target, 60);
    assert.equal(e[0].t.duty.targetMs, 2000);
});

// ---- LED grouping ----------------------------------------------------------

test('resolveLedChannels: a mask drives every bit it sets, each independently', () => {
    const { channels } = resolveLedChannels(doc('0 10 50 80 255 0 0 3\n'));
    assert.deepEqual([...channels.keys()].sort(), [0, 1]);
});

test('resolveLedChannels: bits in one mask get their own next-entry', () => {
    // config_parser.c:3151 splits a multi-bit mask into per-group sweep installs
    // precisely because the bits can diverge.
    const { channels } = resolveLedChannels(
        doc('0 >10 50 80 255 0 0 3\n1000 20 50 80 255 0 0 1\n5000 40 50 80 255 0 0 2\n'));
    assert.equal(channels.get(0)[0].t.freq.targetMs, 1000);
    assert.equal(channels.get(1)[0].t.freq.targetMs, 5000);
});

test('resolveLedChannels: a mask-0 row is skipped, as the firmware skips it', () => {
    // parse.js turns a literal 0 into a raw row; a `-` becomes null. Either way
    // config_parser.c:1693 drops the line.
    const { channels } = resolveLedChannels(doc('0 10 50 80 255 0 0 -\n'));
    assert.equal(channels.size, 0);
});

test('resolveLedChannels: `-` at a first entry resolves to the channel default', () => {
    const { channels } = resolveLedChannels(doc('0 - - - - - - 1\n'));
    const e = channels.get(0)[0];
    assert.equal(e.cells.duty.value, LED_DEFAULTS.duty);
    assert.equal(e.cells.bright.value, LED_DEFAULTS.bright);
    assert.equal(e.cells.r.value, LED_DEFAULTS.r);
});

test('resolveLedChannels: a legacy 5-field row defaults to full white', () => {
    const { channels } = resolveLedChannels(doc('0 10 50 80 1\n'));
    const e = channels.get(0)[0];
    assert.equal(e.cells.r.value, 255);
    assert.equal(e.cells.g.value, 255);
    assert.equal(e.cells.b.value, 255);
});

// ---- lookup ----------------------------------------------------------------

test('activeIndexAt: -1 before the first entry, last-wins on a shared timestamp', () => {
    const { channels } = resolveLedChannels(
        doc('1000 10 50 80 255 0 0 1\n1000 20 50 80 0 255 0 1\n3000 30 50 80 0 0 255 1\n'));
    const e = channels.get(0);
    assert.equal(activeIndexAt(e, 0), -1);
    assert.equal(activeIndexAt(e, 1000), 1);          // the later of the pair
    assert.equal(activeIndexAt(e, 2999), 1);
    assert.equal(activeIndexAt(e, 3000), 2);
});

test('stateAt: returns null before a channel has started', () => {
    const { channels } = resolveAudioChannels(doc('A 5000 200 0 80 0 1\n'));
    assert.equal(stateAt(channels.get(1), 0, ['freq']), null);
    assert.equal(stateAt(channels.get(1), 5000, ['freq']).freq, 200);
});

test('sessionEndMs: the last timeline timestamp across all row kinds', () => {
    assert.equal(sessionEndMs(doc('0 10 50 80 255 0 0 1\nA 7000 200 0 80 0 1\n')), 7000);
    assert.equal(sessionEndMs(doc('')), 0);
});
