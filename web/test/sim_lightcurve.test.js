// The measured light/drive curve — a DISPLAY mapping only.
//
// The whole point of this file is the last test: whatever curve the view draws
// with, the pixel bytes this project computes must be untouched. They are what
// the firmware puts on the wire, what the inspector reports and what the CSV
// export carries, and a display convenience that quietly rewrote them would be
// much worse than no display convenience at all.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { resolveLedChannels } from '../src/js/gen/sim/resolve.js';
import {
    MEASURED_LIGHT_CURVE, lightFraction, measuredLightByte, measuredLightPixel,
    ledStateAt,
} from '../src/js/gen/sim/led.js';
import { lintSession } from '../src/js/gen/sim/lint.js';

test('lightFraction reproduces the measured table at its measured points', () => {
    // docs/sessions/photic.html#bright, reproduced by sessions/test t03.
    assert.equal(lightFraction(12), 0.68);
    assert.equal(lightFraction(24), 0.77);
    assert.equal(lightFraction(36), 0.88);
    assert.equal(lightFraction(54), 0.935);   // the 48-60 band's midpoint
    assert.equal(lightFraction(102), 1.00);
    assert.equal(lightFraction(255), 1.00);
});

test('lightFraction interpolates between points and is monotonic', () => {
    const mid = lightFraction(18);
    assert.ok(mid > 0.68 && mid < 0.77, 'got ' + mid);
    let prev = -1;
    for (let d = 0; d <= 255; d++) {
        const f = lightFraction(d);
        assert.ok(f >= prev - 1e-12, 'fraction dipped at drive ' + d);
        prev = f;
    }
});

test('below the measured floor the fraction is held, not invented', () => {
    // Nothing was measured under drive 12, so it stays flat there rather than
    // extrapolating into a region nobody looked at.
    assert.equal(lightFraction(0), 0.68);
    assert.equal(lightFraction(5), 0.68);
});

test('measuredLightByte anchors at both ends and compresses the dim end', () => {
    assert.equal(measuredLightByte(0), 0);
    assert.equal(measuredLightByte(255), 255);
    assert.equal(measuredLightByte(102), 102, 'linear from ~100 up');
    // 12 * 0.68 = 8.16 -> 8. A linear preview would have drawn 12.
    assert.equal(measuredLightByte(12), 8);
    assert.equal(measuredLightByte(24), Math.round(24 * 0.77));   // 18
    assert.equal(measuredLightByte(36), Math.round(36 * 0.88));   // 32
});

test('measuredLightByte never exceeds the drive it was given', () => {
    for (let d = 0; d <= 255; d++) {
        assert.ok(measuredLightByte(d) <= d, 'drive ' + d + ' gained light');
    }
});

test('measuredLightByte clamps out-of-range input', () => {
    assert.equal(measuredLightByte(-5), 0);
    assert.equal(measuredLightByte(900), 255);
});

test('measuredLightPixel maps each component independently', () => {
    assert.deepEqual(measuredLightPixel({ r: 255, g: 12, b: 0 }),
                     { r: 255, g: 8, b: 0 });
});

test('the curve is DISPLAY ONLY — device pixel bytes are untouched', () => {
    // The computed state must be identical whether or not anything chooses to
    // draw it through the curve. measuredLightPixel returns a NEW object and
    // must not mutate its input.
    const ch = resolveLedChannels(parse('0 10 50 20 255 180 84 1\n').doc).channels;
    const st = ledStateAt(ch.get(0), 0);
    const before = { ...st.px };
    const drawn = measuredLightPixel(st.px);
    assert.deepEqual(st.px, before, 'the device bytes were mutated');
    assert.notDeepEqual(drawn, before, 'the display mapping should differ here');
    // And a second read of the same instant still gives the device bytes.
    assert.deepEqual(ledStateAt(ch.get(0), 0).px, before);
});

// ---- the lint it enables ---------------------------------------------------

const lint = (t) => lintSession(parse(t).doc).filter(f => f.code === 'DIM_NONLINEAR');

test('DIM_NONLINEAR flags a breath that never leaves the non-linear region', () => {
    // 17_nondual_awareness breathes ~8:22 on blue 200 — effective drive 16..44.
    const f = lint('0 10 50 ~8:22:12000 0 0 200 1\n');
    assert.equal(f.length, 1);
    assert.equal(f[0].severity, 'warn');
    assert.match(f[0].msg, /drive 16 and 44/);
    assert.match(f[0].msg, /distorted in SHAPE/);
    assert.match(f[0].msg, /change of variable/);
});

test('DIM_NONLINEAR stays quiet when the breath reaches the linear region', () => {
    // The documented fix: same light, higher drive, room to modulate.
    assert.deepEqual(lint('0 10 50 ~30:90:12000 0 0 255 1\n'), []);
});

test('DIM_NONLINEAR stays quiet for a steady brightness', () => {
    assert.deepEqual(lint('0 10 50 20 0 0 200 1\n'), []);
});

test('DIM_NONLINEAR stays quiet when the swing is too small to distort', () => {
    // A couple of per cent across the swing is not worth a warning.
    assert.deepEqual(lint('0 10 50 ~40:44:12000 0 0 200 1\n'), []);
});

test('MEASURED_LIGHT_CURVE is ordered and bounded', () => {
    let prev = 0;
    for (const p of MEASURED_LIGHT_CURVE) {
        assert.ok(p.drive > prev, 'drive points must ascend');
        assert.ok(p.fraction > 0 && p.fraction <= 1);
        prev = p.drive;
    }
    assert.equal(MEASURED_LIGHT_CURVE[MEASURED_LIGHT_CURVE.length - 1].drive, 255);
});
