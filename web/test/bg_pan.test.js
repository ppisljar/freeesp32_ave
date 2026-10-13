// The background bed's pan law and de-click ramp, pinned to bg_player.c.
//
// Worth asserting because browser and device disagreed here for real. The
// bounce panned the BG clip equal-power (cos/sin), the device pans it LINEAR,
// and at pan 0 that is a full 3.01 dB on BOTH channels — 0.7071 against 1.0.
// It only became audible once sim/audio.js started rendering the synth tones at
// true device level (the fixed 1/16 divisor, audio_generator.c:875): before
// that the whole bounce was wrong together, after it the bed alone was 3 dB
// quiet under the tones.
//
// The firmware's own comments (bg_player.c:110, :1826, :1874) claim this law
// matches audio_generator.c's apply_panning. They are stale — apply_panning
// (audio_generator.c:2304-2315) is equal-power. These tests follow the C code,
// not the C comments.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { bgPanGains, bgRampScale, BG_AMP_RAMP_SAMPLES } from '../src/js/gen/bounce.js';

// The law this file replaced, kept as a named helper so the negative
// assertions below read as "not the old thing" rather than as magic numbers.
function equalPowerGains(pan) {
    const ang = (pan + 1) * (Math.PI / 4);
    return { l: Math.cos(ang), r: Math.sin(ang) };
}

// ---- The linear pan law (bg_player.c:1880-1881) ----------------------------

test('pan 0 is UNITY on both sides, not the equal-power 0.7071', () => {
    // bg_player.c:1880 `(pan <= 0.0f) ? 1.0f : ...` and :1881
    // `(pan >= 0.0f) ? 1.0f : ...` — at exactly 0 BOTH conditions hold, so
    // centre is unity/unity and the bed is NOT attenuated for being centred.
    const g = bgPanGains(0);
    assert.equal(g.l, 1);
    assert.equal(g.r, 1);

    // The whole point of D14: a centred bed used to come out 3.01 dB low.
    assert.equal(equalPowerGains(0).l, Math.cos(Math.PI / 4));
    assert.ok(g.l > equalPowerGains(0).l);
    const dB = 20 * Math.log10(g.l / equalPowerGains(0).l);
    assert.ok(Math.abs(dB - 3.0103) < 0.001, 'expected 3.01 dB, got ' + dB);
});

test('pan -1 is full left and silent right', () => {
    // pan < 0 → left keeps unity, right collapses to 1 + (-1) = 0.
    const g = bgPanGains(-1);
    assert.equal(g.l, 1);
    assert.equal(g.r, 0);
});

test('pan +1 is the mirror: silent left, full right', () => {
    const g = bgPanGains(1);
    assert.equal(g.l, 0);
    assert.equal(g.r, 1);
});

test('midpoints follow the linear formula exactly, near side pegged at unity', () => {
    // 1 - p and 1 + p are exact in binary floating point for these values, so
    // assert.equal is safe and documents the formula literally.
    const half = bgPanGains(0.5);
    assert.equal(half.l, 0.5);          // 1 - 0.5, the far side
    assert.equal(half.r, 1);            // pan >= 0 → near side unmoved

    const quarterL = bgPanGains(-0.25);
    assert.equal(quarterL.l, 1);        // pan <= 0 → near side unmoved
    assert.equal(quarterL.r, 0.75);     // 1 + (-0.25), the far side

    // Equal-power would have moved BOTH sides at every pan position; linear
    // moves exactly one. This is the structural difference, not just a level.
    const ep = equalPowerGains(0.5);
    assert.ok(ep.l !== 1 && ep.r !== 1);
});

test('the near side is unity across the whole sweep — that is the law', () => {
    // Sampled rather than reasoned: whichever side is nearer must read exactly
    // 1.0 everywhere, which is what makes this law lossless at the near ear and
    // is precisely what the equal-power version got wrong.
    for (let p = -1; p <= 1.0001; p += 0.05) {
        const g = bgPanGains(p);
        assert.equal(p <= 0 ? g.l : g.r, 1, 'near side not unity at pan ' + p);
    }
});

test('pan is clamped to [-1, +1] before the law is applied', () => {
    // bg_player's callers clamp first (web_server.c:2274 for the HTTP push,
    // :2521 for the WS handshake), so out-of-range input must saturate rather
    // than invert a channel: 1 - 2 would be -1, i.e. a phase flip on one side.
    assert.deepEqual(bgPanGains(2), bgPanGains(1));
    assert.deepEqual(bgPanGains(-2), bgPanGains(-1));
    assert.equal(bgPanGains(5).l, 0);
    assert.equal(bgPanGains(-5).r, 0);
});

test('loudness multiplies the pan law, with no 1/16 headroom divisor', () => {
    // bg_player.c:1903-1908 — gain (= loudness) first, then pan_l/pan_r. BG is
    // mixed in audio_test.c:176-184, AFTER audio_generator_fill_buffer, so it
    // never passes through audio_generator.c:875's 1/16 divisor the way every
    // synth channel does. A centred bed at loudness 50 therefore peaks at 0.5
    // (-6 dBFS), not 0.5/16.
    const g = bgPanGains(0);
    assert.equal(0.5 * g.l, 0.5);
    assert.equal(0.5 * g.r, 0.5);
    assert.notEqual(0.5 * g.l, 0.5 / 16);
});

// ---- The 220-sample de-click ramp (bg_player.c:159, :1885-1905) ------------

test('the ramp is 220 samples — AUDIO_AMP_RAMP_SAMPLES', () => {
    // bg_player.c:159 duplicates audio_generator.c:81's AUDIO_AMP_RAMP_SAMPLES.
    assert.equal(BG_AMP_RAMP_SAMPLES, 220);
    assert.ok(Math.abs(BG_AMP_RAMP_SAMPLES / 44100 - 0.00499) < 0.0001);
});

test('sample 0 starts at one step, not at silence', () => {
    // bg_player.c:1891-1899 adds loudness_step BEFORE :1901 reads the gain, so
    // the first sample is already 1/220 of target. An `i / 220` port would put
    // a true zero there and be one sample long in every later position.
    assert.equal(bgRampScale(0), 1 / BG_AMP_RAMP_SAMPLES);
    assert.notEqual(bgRampScale(0), 0);
    assert.equal(bgRampScale(1), 2 / BG_AMP_RAMP_SAMPLES);
});

test('the ramp reaches exactly 1 at sample 219 and stays there', () => {
    // bg_player.c:1895-1897: when remaining - 1 == 0 the firmware snaps
    // current_loudness to target_loudness, so the end of the ramp is exact and
    // the gain never drifts above or below unity afterwards.
    assert.equal(bgRampScale(218), 219 / BG_AMP_RAMP_SAMPLES);
    assert.equal(bgRampScale(BG_AMP_RAMP_SAMPLES - 1), 1);
    assert.equal(bgRampScale(BG_AMP_RAMP_SAMPLES), 1);
    assert.equal(bgRampScale(44100), 1);
});

test('the ramp is monotonic and never exceeds unity', () => {
    let prev = 0;
    for (let i = 0; i < BG_AMP_RAMP_SAMPLES + 10; i++) {
        const v = bgRampScale(i);
        assert.ok(v > 0, 'ramp hit zero at ' + i);
        assert.ok(v <= 1, 'ramp overshot unity at ' + i);
        assert.ok(v >= prev, 'ramp went backwards at ' + i);
        prev = v;
    }
});
