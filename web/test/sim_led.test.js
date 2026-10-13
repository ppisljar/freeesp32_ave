// The simulation LED path, pinned to the firmware's integer arithmetic.
//
// The two most valuable assertions here are the ones that would otherwise
// silently disagree with the hardware: the phase column is a LEAD (measured on
// a photodiode at 10 Hz: 0 deg -> 0.00 ms, 90 -> 74.76, 180 -> 49.93,
// 270 -> 24.69), and `freq 0` is a DC branch that ignores duty, env and phase
// rather than a degenerate flicker.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { resolveLedChannels } from '../src/js/gen/sim/resolve.js';
import {
    ledStateAt, carrierLevelQ8, compositePixel, phaseQ16, jitterMilliHz,
    tickHzFor, ledEntryRejection, neohexLayout, channelPixelCounts,
    SINE_Q16, LED_BRIGHTNESS_Q8_MAX, DEFAULT_CHANNEL_MAP,
    LED_CARRIER_SQUARE, LED_CARRIER_SINE, LED_CARRIER_TRIANGLE, LED_CARRIER_TRAPEZOID,
} from '../src/js/gen/sim/led.js';

function chans(text) { return resolveLedChannels(parse(text).doc).channels; }
function at(text, tMs, bit = 0) { return ledStateAt(chans(text).get(bit), tMs); }

// ---- The raised-sine LUT (L4) ----------------------------------------------

test('SINE_Q16 matches the firmware table, including its 65535 peak clamp', () => {
    // led_matrix_example.c:600 — 65536 does not fit a uint16_t, and the first
    // version that stored it wrapped to 0, producing one dim flick per
    // brightness-LFO period at the brightest moment of the breath.
    assert.equal(SINE_Q16[0], 0);
    assert.equal(SINE_Q16[1], 10);
    assert.equal(SINE_Q16[2], 39);
    assert.equal(SINE_Q16[4], 158);
    assert.equal(SINE_Q16[64], 32768);
    assert.equal(SINE_Q16[128], 65535);      // NOT 65536
    assert.equal(SINE_Q16[192], 32768);
    assert.equal(SINE_Q16[256], 0);
    assert.equal(SINE_Q16.length, 257);      // the +1 endpoint, so >>8 needs no clamp
});

// ---- Pixel compositing (L4) ------------------------------------------------

test('compositePixel truncates exactly as the C does', () => {
    // led_strip.c:458 — px = rgb * brightness_q8 / 25600, integer.
    assert.deepEqual(compositePixel(200, 0, 0, 22 * 256), { r: 44, g: 0, b: 0 });
    assert.deepEqual(compositePixel(200, 0, 0, 8 * 256), { r: 16, g: 0, b: 0 });
    assert.deepEqual(compositePixel(255, 255, 255, LED_BRIGHTNESS_Q8_MAX),
                     { r: 255, g: 255, b: 255 });
    assert.deepEqual(compositePixel(255, 255, 255, 0), { r: 0, g: 0, b: 0 });
    // Truncation, not rounding: 100 * 2560/25600 = 10.0, 101 * 2560/25600 = 10.1 -> 10.
    assert.equal(compositePixel(101, 0, 0, 10 * 256).r, 10);
});

// ---- Square carrier + duty -------------------------------------------------

test('the square carrier gates on duty and is the LED default', () => {
    const s = at('0 10 25 100 255 0 0 1\n', 0);
    assert.equal(s.env, LED_CARRIER_SQUARE);
    assert.equal(s.cycleUs, 100000);          // 1e9 / 10000 milliHz
    assert.equal(s.onTimeUs, 25000);          // 100000 * 6400 / 25600
    assert.equal(s.on, true);
    assert.deepEqual(s.px, { r: 255, g: 0, b: 0 });
    // Just inside the ON window, then just outside it.
    assert.equal(at('0 10 25 100 255 0 0 1\n', 24).on, true);
    assert.equal(at('0 10 25 100 255 0 0 1\n', 26).on, false);
    assert.deepEqual(at('0 10 25 100 255 0 0 1\n', 26).px, { r: 0, g: 0, b: 0 });
    // And back on at the start of the next cycle.
    assert.equal(at('0 10 25 100 255 0 0 1\n', 100).on, true);
});

test('brightness scales the pixel but not the gate', () => {
    const s = at('0 10 50 40 200 100 0 1\n', 0);
    assert.equal(s.on, true);
    // 200 * (40*256) / 25600 = 80;  100 * 10240/25600 = 40.
    assert.deepEqual(s.px, { r: 80, g: 40, b: 0 });
});

// ---- Phase is a LEAD (L6) --------------------------------------------------

test('phase is a LEAD: the measured hardware edge table at 10 Hz', () => {
    // Photodiode measurements of the LED-ON edge, 100 ms period:
    //   0 deg -> 0.00 ms, 90 -> 74.76, 180 -> 49.93, 270 -> 24.69.
    // Because the offset is ADDED to elapsed time, the edge arrives EARLIER;
    // reading `phase` as "start this many degrees later" is right only at 0
    // and 180, which is exactly why nobody noticed.
    const edgeMs = (deg) => {
        const text = '0 10 50 100 255 255 255 1 0 ' + deg + '\n';
        const e = chans(text).get(0);
        // Walk one cycle at 0.1 ms resolution and find the OFF -> ON transition.
        let prev = ledStateAt(e, 0).on;
        for (let t = 0.1; t <= 100.0001; t += 0.1) {
            const on = ledStateAt(e, t).on;
            if (on && !prev) return Math.round(t * 100) / 100;
            prev = on;
        }
        return 0;   // already on at t=0
    };
    assert.equal(edgeMs(0), 0);
    assert.ok(Math.abs(edgeMs(90) - 75) < 0.3, 'phase 90 -> ~74.8 ms, got ' + edgeMs(90));
    assert.ok(Math.abs(edgeMs(180) - 50) < 0.3, 'phase 180 -> ~49.9 ms, got ' + edgeMs(180));
    assert.ok(Math.abs(edgeMs(270) - 25) < 0.3, 'phase 270 -> ~24.7 ms, got ' + edgeMs(270));
});

test('a 180-degree pair is in antiphase at every instant', () => {
    const a = chans('0 10 50 100 255 180 84 1\n').get(0);
    const b = chans('0 10 50 100 95 216 232 2 0 180\n').get(1);
    for (let t = 0; t < 100; t += 1) {
        const on = ledStateAt(a, t).on;
        const off = ledStateAt(b, t).on;
        assert.notEqual(on, off, 'both banks agreed at t=' + t + ' ms');
    }
});

test('an env-1 antiphase pair sums to flat luminance — no gamma anywhere (L3)', () => {
    // The carrier shapes are luminance-LINEAR on purpose: two raised sines
    // 180 degrees apart sum to a constant, which is the "invisible" flicker.
    // Applying an sRGB correction to make the CSS look nicer would break it.
    const a = chans('0 10 50 100 255 255 255 1 1 0\n').get(0);
    const b = chans('0 10 50 100 255 255 255 2 1 180\n').get(1);
    let min = Infinity, max = -Infinity;
    for (let t = 0; t < 100; t += 0.5) {
        const sum = ledStateAt(a, t).levelQ8 + ledStateAt(b, t).levelQ8;
        min = Math.min(min, sum); max = Math.max(max, sum);
    }
    // Flat to within the LUT's own 1-LSB clamp at the peak.
    assert.ok(max - min <= 2,
        'antiphase raised sines should sum flat; spread was ' + (max - min));
    assert.ok(Math.abs(min - LED_BRIGHTNESS_Q8_MAX) <= 2);
});

// ---- Smooth carriers -------------------------------------------------------

test('carrierLevelQ8: sine peaks mid-cycle, triangle folds linearly', () => {
    const B = 100 * 256;
    const cyc = 100000;
    assert.equal(carrierLevelQ8(LED_CARRIER_SINE, 0, cyc, B, 0, 0), 0);
    // Half a cycle is the raised sine's peak, at the clamped 65535.
    const peakLvl = carrierLevelQ8(LED_CARRIER_SINE, cyc / 2, cyc, B, 0, 0);
    assert.equal(peakLvl, Math.floor((B * 65535) / 65536));
    // Quarter cycle = half level.
    const q = carrierLevelQ8(LED_CARRIER_SINE, cyc / 4, cyc, B, 0, 0);
    assert.ok(Math.abs(q - B / 2) <= 2);

    assert.equal(carrierLevelQ8(LED_CARRIER_TRIANGLE, 0, cyc, B, 0, 0), 0);
    assert.ok(Math.abs(carrierLevelQ8(LED_CARRIER_TRIANGLE, cyc / 4, cyc, B, 0, 0) - B / 2) <= 2);
    assert.ok(carrierLevelQ8(LED_CARRIER_TRIANGLE, cyc / 2, cyc, B, 0, 0) >= B - 2);
});

test('env 1 and env 2 ignore duty entirely', () => {
    // led_carrier_level_iram reads duty_q16 only in its TRAPEZOID branch.
    const a = at('0 10 10 100 255 255 255 1 1\n', 25);
    const b = at('0 10 90 100 255 255 255 1 1\n', 25);
    assert.equal(a.levelQ8, b.levelQ8, 'a duty column next to env 1 must be a no-op');
    const c = at('0 10 10 100 255 255 255 1 2\n', 25);
    const d = at('0 10 90 100 255 255 255 1 2\n', 25);
    assert.equal(c.levelQ8, d.levelQ8);
});

test('the trapezoid gates on duty with symmetric linear edges', () => {
    const B = 100 * 256;
    const cyc = 100000;
    const dutyQ16 = Math.floor((50 * 256 * 65536) / LED_BRIGHTNESS_Q8_MAX);   // 0.5
    const attackQ16 = Math.floor((3000 * 65536) / cyc);                       // 3 ms
    const lvl = (us) => carrierLevelQ8(LED_CARRIER_TRAPEZOID, us, cyc, B, dutyQ16, attackQ16);
    assert.equal(lvl(0), 0);                       // start of the rise
    assert.ok(Math.abs(lvl(1500) - B / 2) < B * 0.03);   // mid-rise
    assert.ok(lvl(3200) >= B - 2);                 // held
    assert.ok(lvl(25000) >= B - 2);                // still held
    assert.ok(Math.abs(lvl(48500) - B / 2) < B * 0.03);  // mid-fall
    assert.equal(lvl(50000), 0);                   // off beyond duty
    assert.equal(lvl(80000), 0);
});

test('an attack wider than half the on-time clamps to a triangle', () => {
    const B = 100 * 256, cyc = 100000;
    const dutyQ16 = Math.floor(0.5 * 65536);
    const huge = Math.floor(0.9 * 65536);
    // Clamped to duty/2, so the peak lands exactly at the middle of the on-time
    // and nothing is held.
    const lvl = (f) => carrierLevelQ8(LED_CARRIER_TRAPEZOID, f * cyc, cyc, B, dutyQ16, huge);
    assert.ok(lvl(0.25) >= B - 2);
    assert.ok(lvl(0.125) < B * 0.6);
    assert.equal(lvl(0.5), 0);
});

test('env 4 on an LED line folds to SQUARE, the opposite of the audio path', () => {
    // led_matrix_example.c:1746 clamps wave > 3 to SQUARE; the audio side folds
    // out-of-range env to 4. Sharing one decoder would be wrong on these values.
    const s = at('0 10 50 100 255 255 255 1 4\n', 0);
    assert.equal(s.env, LED_CARRIER_SQUARE);
});

// ---- DC channel (L8) -------------------------------------------------------

test('freq 0 is a DC channel: steady on, and duty/env/phase are all ignored', () => {
    // led_matrix_example.c:896 `continue`s before the duty gate, the carrier
    // switch AND the phase offset. 19 shipped sessions render black if this is
    // mishandled, which is why `frequency < 0` (not <= 0) is what stops a mask.
    for (const text of [
        '0 0 50 80 255 0 0 1\n',
        '0 0 5 80 255 0 0 1\n',           // a tiny duty must not dim it
        '0 0 50 80 255 0 0 1 1\n',        // env 1 must not shape it
        '0 0 50 80 255 0 0 1 0 180\n',    // phase must not shift it
    ]) {
        for (const t of [0, 7, 33, 99, 1000]) {
            const s = at(text, t);
            assert.equal(s.dc, true, text.trim());
            assert.equal(s.on, true, text.trim() + ' @ ' + t);
            assert.equal(s.levelQ8, 80 * 256, text.trim() + ' @ ' + t);
            assert.deepEqual(s.px, { r: 204, g: 0, b: 0 });   // 255*20480/25600
        }
    }
});

test('a DC channel still breathes: brightness LFOs and sweeps run every tick', () => {
    // A `~` on brightness is what lets a freq-0 channel fade and breathe.
    const e = chans('0 0 50 ~20:100:4000 255 255 255 1\n').get(0);
    const a = ledStateAt(e, 0).levelQ8;
    const b = ledStateAt(e, 2000).levelQ8;
    assert.equal(a, 20 * 256);
    assert.equal(b, 100 * 256);
    assert.ok(b > a);
});

// ---- Out-of-range rows are dropped (L9) ------------------------------------

test('an out-of-range row is DROPPED, so the channel keeps its previous state', () => {
    // led_matrix_start_flicker_masked (:1593) returns ESP_ERR_INVALID_ARG rather
    // than clamping, and the affected channels keep rendering what they were.
    const e = chans('0 10 50 100 255 0 0 1\n5000 150 50 100 0 255 0 1\n').get(0);
    assert.equal(ledEntryRejection(e[1]) !== null, true);
    const s = ledStateAt(e, 6000);
    assert.equal(s.freq, 10, 'the dropped row must not take effect');
    assert.deepEqual(s.px.g, 0, 'and its colour must not either');
});

test('ledEntryRejection flags freq/duty/brightness over their ceilings', () => {
    const rej = (text) => ledEntryRejection(chans(text).get(0)[0]);
    assert.equal(rej('0 10 50 100 255 0 0 1\n'), null);
    assert.ok(/frequency/.test(rej('0 150 50 100 255 0 0 1\n')));
    assert.ok(/duty/.test(rej('0 10 150 100 255 0 0 1\n')));
    assert.ok(/brightness/.test(rej('0 10 50 150 255 0 0 1\n')));
    // Both ENDS of a ramp are checked (:2345).
    assert.ok(/frequency/.test(rej('0 >10 50 100 255 0 0 1\n5000 150 50 100 255 0 0 1\n')));
});

// ---- Helpers ---------------------------------------------------------------

test('phaseQ16 reproduces the ISR 32-bit rescale', () => {
    assert.equal(phaseQ16(0, 100000), 0);
    assert.equal(phaseQ16(100000, 100000), 65536);
    // Halfway, within the precision the rescale leaves.
    assert.ok(Math.abs(phaseQ16(50000, 100000) - 32768) <= 4);
    assert.equal(phaseQ16(1234, 0), 0);
});

test('tickHzFor: integer Hz x 250, clamped to [1000, 10000]', () => {
    // led_matrix_example.c:1489 — and note the INTEGER division first, so
    // 10.5 Hz gives 10 x 250 = 2500, not 2625.
    assert.equal(tickHzFor(2), 1000);        // below the floor
    assert.equal(tickHzFor(10), 2500);
    assert.equal(tickHzFor(10.5), 2500);
    assert.equal(tickHzFor(40), 10000);
    assert.equal(tickHzFor(80), 10000);      // above the ceiling
});

test('jitterMilliHz: a bipolar triangle that starts and ends at zero', () => {
    const amp = 500, period = 10000000;      // 0.5 Hz over 10 s
    assert.equal(jitterMilliHz(0, amp, period), 0);
    assert.ok(Math.abs(jitterMilliHz(period / 4, amp, period) - amp) <= 1);
    assert.ok(Math.abs(jitterMilliHz(period * 3 / 4, amp, period) + amp) <= 1);
    assert.equal(jitterMilliHz(0, 0, period), 0);
    assert.equal(jitterMilliHz(0, amp, 0), 0);
});

// ---- Physical layout -------------------------------------------------------

test('the shipped channel map is 74 pixels across four mirrored zones', () => {
    assert.equal(DEFAULT_CHANNEL_MAP.length, 74);
    const counts = channelPixelCounts();
    // docs/hardware/index.html:689 — eye A inner 19, eye A outer 18,
    // eye B outer 18, eye B inner 19. The MIRRORING is the easy thing to get
    // wrong: mask 9 is "both inner", mask 6 is "both outer".
    assert.deepEqual(counts, { 0: 19, 1: 18, 2: 18, 3: 19 });
});

test('neohexLayout places 74 pixels over two 37-pixel boards', () => {
    const px = neohexLayout();
    assert.equal(px.length, 74);
    assert.equal(px.filter(p => p.eye === 0).length, 37);
    assert.equal(px.filter(p => p.eye === 1).length, 37);
    // Every pixel keeps the channel the map assigns it.
    for (const p of px) assert.equal(p.channel, DEFAULT_CHANNEL_MAP[p.index]);
    // Seven serpentine rows of widths 4,5,6,7,6,5,4.
    const widths = [4, 5, 6, 7, 6, 5, 4];
    for (let r = 0; r < 7; r++) {
        assert.equal(px.filter(p => p.eye === 0 && p.row === r).length, widths[r]);
    }
});

test('a channel with no entries yields no state at all', () => {
    assert.equal(ledStateAt(undefined, 0), null);
    assert.equal(ledStateAt([], 0), null);
    // And nothing before the first entry.
    assert.equal(at('5000 10 50 100 255 0 0 1\n', 0), null);
});
