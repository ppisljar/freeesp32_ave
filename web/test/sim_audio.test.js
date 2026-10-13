// The simulation audio path, pinned against the firmware.
//
// The level assertions are the important ones: they are MEASURED DEVICE
// VALUES, not values read back out of this implementation. A centre-panned
// sine at vol 100 peaks at int16 1448 on hardware; a binaural channel (pan
// bypassed) at -24.1 dBFS; pink noise at vol 100 at -43.3 dBFS. Those three
// numbers are what catch the mix-divisor and noise-scale divergences, which
// between them put gen/synth.js up to 24 dB hot.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parse } from '../src/js/gen/parse.js';
import { resolveAudioChannels } from '../src/js/gen/sim/resolve.js';
import {
    createRenderer, renderSessionDevice, waveform, isoGate, isoTrapezoid,
    annotateIso, sessionBeatJitter, beatJitterHz, toInt16Device,
    MIX_GAIN, NOISE_LEVEL_SCALE, AMP_RAMP_SAMPLES, LEGACY_MOD_DEPTH,
    ISO_ENV_SQUARE, ISO_ENV_SINE, ISO_ENV_TRIANGLE, ISO_ENV_TRAPEZOID, ISO_ENV_TREMOLO,
} from '../src/js/gen/sim/audio.js';
import { SAMPLE_RATE } from '../src/js/gen/model.js';

function render(text, ms) {
    const doc = parse(text).doc;
    const { channels } = resolveAudioChannels(doc);
    return renderSessionDevice(channels, {
        totalMs: ms, beatJitter: sessionBeatJitter(doc),
    });
}

function peak(a, from = 0) {
    let p = 0;
    for (let i = from; i < a.length; i++) { const v = Math.abs(a[i]); if (v > p) p = v; }
    return p;
}
function rms(a, from = 0) {
    let s = 0, n = 0;
    for (let i = from; i < a.length; i++) { s += a[i] * a[i]; n++; }
    return Math.sqrt(s / n);
}
const dB = (x) => 20 * Math.log10(x);

// Skip the first 220-sample de-click ramp before measuring steady state.
const SETTLED = AMP_RAMP_SAMPLES + 10;

// ---- Device level references (D1, D2) --------------------------------------

test('a centre-panned sine at vol 100 peaks at int16 1448, as measured on hardware', () => {
    // 1.0 amplitude x cos(pi/4) pan x 1/16 mix = 0.044194 -> trunc(x*32767) = 1448.
    const { left, right } = render('A 0 200 0 100 0 1\n', 500);
    const pl = peak(left, SETTLED);
    assert.equal(toInt16Device(pl), 1448);
    assert.ok(Math.abs(dB(pl) - (-27.1)) < 0.1, 'expected -27.1 dBFS, got ' + dB(pl));
    // Equal-power pan at centre is symmetric.
    assert.ok(Math.abs(peak(right, SETTLED) - pl) < 1e-6);
});

test('the mix divisor is a fixed 1/16 — a second channel does not rescale the first', () => {
    // This is the whole point of audio_generator.c:875. gen/synth.js divides by
    // the active count instead, so adding a channel there HALVES the first one.
    const one = peak(render('A 0 200 -100 100 0 1\n', 500).left, SETTLED);
    const two = peak(render('A 0 200 -100 100 0 1\nA 0 900 100 100 0 2\n', 500).left, SETTLED);
    assert.ok(Math.abs(one - two) < 1e-6,
        'hard-left channel changed level when an unrelated hard-right channel was added');
    assert.ok(Math.abs(one - MIX_GAIN) < 1e-6);
});

test('a binaural channel bypasses pan and so reaches -24.1 dBFS', () => {
    const { left } = render('A 0 200 0 100 0 1 210\n', 500);
    const pl = peak(left, SETTLED);
    assert.ok(Math.abs(pl - MIX_GAIN) < 1e-6, 'expected full 1/16, got ' + pl);
    assert.ok(Math.abs(dB(pl) - (-24.08)) < 0.1, 'expected -24.1 dBFS, got ' + dB(pl));
});

test('white noise peaks at exactly the 0.5 noise scale through pan and mix', () => {
    // The one noise level that is exactly derivable rather than measured: the
    // LFSR output is uniform over [-1, 1), so over a few seconds its peak
    // approaches full scale, and the chain after it is
    //   0.5 (NOISE_LEVEL_SCALE) x cos(pi/4) (centre pan) x 1/16 (mix) = 0.0220971.
    // That pins the noise scale, the pan law and the mix divisor together.
    //
    // Deliberately NOT asserting the device's measured "pink at vol 100 =
    // -43.3 dBFS": pink noise is 1/f, so its RMS depends on the measurement
    // window length (this chain gives -50.4 dBFS over 3 s and -38.0 dBFS peak),
    // and without knowing the window that figure cannot be reproduced. The pink
    // and brown coefficients are transcribed verbatim from
    // audio_generator.c:1511 and :1541 instead.
    const { left } = render('A 0 0 0 100 0 1 0 4\n', 3000);
    const expected = NOISE_LEVEL_SCALE * Math.cos(Math.PI / 4) * MIX_GAIN;
    const p = peak(left, SETTLED);
    assert.ok(Math.abs(p - expected) / expected < 0.01,
        'white peak ' + p + ' should approach ' + expected);
});

test('the EEG carrier is NOT noise-scaled — wave 7 sits outside the range test', () => {
    // audio_generator.c:1202 reads `wave >= 4 && wave <= 6`, written that way
    // deliberately: wave 7 is a carrier, not a noise colour.
    const eeg = peak(render('A 0 200 0 100 0 1 0 7\n', 1000).left, SETTLED);
    const square = peak(render('A 0 200 0 100 0 1 0 1\n', 1000).left, SETTLED);
    assert.ok(Math.abs(eeg - square) / square < 0.02,
        'eeg=' + eeg + ' should match an unscaled square peak ' + square);
    assert.equal(NOISE_LEVEL_SCALE, 0.5);
});

// ---- Clamps (D9) -----------------------------------------------------------

test('amplitude clamps to 1.0 but pan is deliberately NOT clamped', () => {
    const hot = peak(render('A 0 200 0 500 0 1\n', 500).left, SETTLED);
    assert.ok(Math.abs(hot - MIX_GAIN * Math.cos(Math.PI / 4)) < 1e-6,
        'vol 500 should clamp to amplitude 1.0');

    // pan 200 runs the equal-power law to (pan+1)*pi/4 = 3pi/4, where cos is
    // NEGATIVE — the left output inverts polarity. The device does not clamp
    // this, so neither do we; clamping would hide a real hardware behaviour.
    const { left } = render('A 0 200 200 100 0 1\n', 500);
    let sawNeg = false, sawPos = false;
    for (let i = SETTLED; i < left.length; i++) {
        if (left[i] < -1e-6) sawNeg = true;
        if (left[i] > 1e-6) sawPos = true;
    }
    assert.ok(sawNeg && sawPos, 'pan 200 should still produce a signal on the left');
    // Confirm the inversion: left and right must be in ANTIPHASE at pan 200.
    const { left: l2, right: r2 } = render('A 0 200 200 100 0 1\n', 500);
    let dot = 0;
    for (let i = SETTLED; i < l2.length; i++) dot += l2[i] * r2[i];
    assert.ok(dot < 0, 'pan 200 must invert the left channel relative to the right');
});

// ---- Envelopes (D3) --------------------------------------------------------

test('isoGate: the four unipolar shapes at their defining points', () => {
    // SQUARE honours duty.
    assert.equal(isoGate(0.1, ISO_ENV_SQUARE, 0.5, 0), 1);
    assert.equal(isoGate(0.6, ISO_ENV_SQUARE, 0.5, 0), 0);
    // TRIANGLE ignores duty; on-fraction is always exactly 50 %.
    assert.equal(isoGate(0.5, ISO_ENV_TRIANGLE, 0.1, 0), 1);
    assert.equal(isoGate(0.25, ISO_ENV_TRIANGLE, 0.9, 0), 0.5);
    // SINE is a TRUE raised sine, not a 4p(1-p) parabola. At p=0.25 the
    // parabola gives 0.75; the raised sine gives exactly 0.5.
    assert.ok(Math.abs(isoGate(0.25, ISO_ENV_SINE, 0.5, 0) - 0.5) < 1e-12);
    assert.ok(Math.abs(isoGate(0.5, ISO_ENV_SINE, 0.5, 0) - 1) < 1e-12);
    assert.ok(Math.abs(isoGate(0, ISO_ENV_SINE, 0.5, 0) - 0) < 1e-12);
});

test('isoTrapezoid: rise, hold, fall, off — with the attack clamped to half the on-time', () => {
    // duty 0.5, attack 0.1 of the cycle.
    assert.equal(isoTrapezoid(0, 0.5, 0.1), 0);
    assert.ok(Math.abs(isoTrapezoid(0.05, 0.5, 0.1) - 0.5) < 1e-12);   // mid-rise
    assert.equal(isoTrapezoid(0.25, 0.5, 0.1), 1);                     // hold
    assert.ok(Math.abs(isoTrapezoid(0.45, 0.5, 0.1) - 0.5) < 1e-12);   // mid-fall
    assert.equal(isoTrapezoid(0.5, 0.5, 0.1), 0);                      // off
    assert.equal(isoTrapezoid(0.9, 0.5, 0.1), 0);
    // attack 0 degenerates to a hard square inside duty.
    assert.equal(isoTrapezoid(0.25, 0.5, 0), 1);
    // An attack wider than half the on-time clamps, giving a triangle whose
    // peak is the only full-scale point.
    assert.ok(Math.abs(isoTrapezoid(0.25, 0.5, 0.4) - 1) < 1e-12);
});

test('annotateIso: depth defaults are envelope-dependent', () => {
    // config_parser.c:2729 — an entry that SELECTS an envelope also picks that
    // envelope's natural depth, because a unipolar gate wants to be a discrete
    // pulse while env 4 wants to be a gentle tremolo. Before this, every
    // "isochronic" gate measured ~5.8 % depth: a tremolo, not a pulse.
    const d = (text) => {
        const { channels } = resolveAudioChannels(parse(text).doc);
        return annotateIso(channels.get(1))[0].iso;
    };
    // Neither column: the legacy hardcoded params.mod_depth.
    assert.equal(d('A 0 200 0 80 10 1\n').depth, LEGACY_MOD_DEPTH);
    assert.equal(d('A 0 200 0 80 10 1\n').env, ISO_ENV_TREMOLO);
    // env 0..3 with no depth column: full depth.
    assert.equal(d('A 0 200 0 80 10 1 0 0 50 0\n').depth, 1.0);
    assert.equal(d('A 0 200 0 80 10 1 0 0 50 2\n').depth, 1.0);
    // env 4 with no depth column: the tremolo's 10 %.
    assert.equal(d('A 0 200 0 80 10 1 0 0 50 4\n').depth, 0.1);
    // An explicit depth column always wins.
    assert.equal(d('A 0 200 0 80 10 1 0 0 50 0 0 3 - 40\n').depth, 0.4);
    // depth 0 genuinely means no modulation.
    assert.equal(d('A 0 200 0 80 10 1 0 0 50 0 0 3 - 0\n').depth, 0);
});

test('annotateIso: duty 0 is a no-op but attack 0 is written', () => {
    // audio_generator.c:236 gates duty on `> 0` and attack on `>= 0`.
    const { channels } = resolveAudioChannels(parse(
        'A 0 200 0 80 10 1 0 0 40 0 0 5\nA 1000 200 0 80 10 1 0 0 0 0 0 0\n').doc);
    const e = annotateIso(channels.get(1));
    assert.equal(e[0].iso.duty, 0.4);
    assert.equal(e[0].iso.attack, 5);
    assert.equal(e[1].iso.duty, 0.4, 'duty 0 must leave the previous duty standing');
    assert.equal(e[1].iso.attack, 0, 'attack 0 must be written');
});

test('env 0 at full depth gates all the way to silence, unlike the env 4 tremolo', () => {
    // A 10 Hz square gate at duty 50 must have genuinely silent stretches.
    const gated = render('A 0 200 0 100 10 1 0 0 50 0\n', 1000).left;
    const trem = render('A 0 200 0 100 10 1 0 0 50 4\n', 1000).left;
    const gatedMin = (() => {
        // Minimum peak over 1 ms windows, which finds the OFF half.
        let worst = Infinity;
        const w = Math.round(SAMPLE_RATE / 1000);
        for (let i = SETTLED; i + w < gated.length; i += w) {
            let p = 0;
            for (let k = i; k < i + w; k++) p = Math.max(p, Math.abs(gated[k]));
            worst = Math.min(worst, p);
        }
        return worst;
    })();
    assert.ok(gatedMin < 1e-7, 'a full-depth square gate must reach silence, got ' + gatedMin);
    // The tremolo only dips to 1 - 0.1 = 0.9 of level, so it never goes quiet.
    assert.ok(peak(trem, SETTLED) > 0 && rms(trem, SETTLED) > rms(gated, SETTLED),
        'the 10 % tremolo should carry more energy than a 50 % duty gate');
});

test('the gate phase re-anchors at every entry (D4)', () => {
    // audio_generator.c:1115 — phase = frac((t - t_entry) * mod_freq), so a
    // square gate is ON at the instant of its own entry regardless of history.
    const doc = parse(
        'A 0 200 0 100 7 1 0 0 50 0\nA 1123 200 0 100 11 1 0 0 50 0\n').doc;
    const { channels } = resolveAudioChannels(doc);
    const r = createRenderer(channels, {});
    const n = 64;
    const L = new Float32Array(n), R = new Float32Array(n);
    const at = Math.round((1123 / 1000) * SAMPLE_RATE);
    r.seek(at);
    r.renderBlock(L, R, at, n);
    // Phase 0 is the start of the ON half for a square gate, so the signal must
    // be live immediately at the entry rather than sitting in an OFF stretch
    // inherited from the previous entry's 7 Hz grid.
    assert.ok(peak(L) > 0, 'the gate should be ON at the entry it is anchored to');
});

// ---- Binaural / freqR (D5, D6) --------------------------------------------

test('freqR carried forward keeps a channel binaural when a later row omits it', () => {
    const doc = parse('A 0 200 0 100 0 1 210\nA 2000 204 0 100 0 1\n').doc;
    const { channels } = resolveAudioChannels(doc);
    const r = createRenderer(channels, {});
    const s = r.channelStates(2500)[0];
    assert.equal(s.binaural, true, 'omitting freqR must not collapse to mono');
    assert.equal(s.freqR, 210);
    assert.ok(Math.abs(s.beatHz - 6) < 1e-6, 'the beat should now be 6 Hz, got ' + s.beatHz);
});

test('a ramp on freqR glides the beat', () => {
    const doc = parse('A 0 200 0 100 0 1 >210\nA 2000 200 0 100 0 1 204\n').doc;
    const { channels } = resolveAudioChannels(doc);
    const r = createRenderer(channels, {});
    assert.ok(Math.abs(r.channelStates(0)[0].beatHz - 10) < 1e-6);
    assert.ok(Math.abs(r.channelStates(1000)[0].beatHz - 7) < 1e-6,
        'halfway through the ramp the beat should be 7 Hz');
});

test('beat jitter is global, right-ear only, and gated on freqR', () => {
    const spec = sessionBeatJitter(parse(
        'A 0 200 0 100 0 1 210 0 - - - - 0.5:10000\n' +
        'A 5000 200 0 100 0 2 0 0 - - - - 1.5:20000\n').doc);
    assert.deepEqual(spec, { amp: 1.5, periodMs: 20000 }, 'the LAST jitter column wins');
    assert.equal(beatJitterHz(spec, 0), 0);
    assert.ok(Math.abs(beatJitterHz(spec, 5000) - 1.5) < 1e-9);   // quarter period
    assert.ok(Math.abs(beatJitterHz(null, 1234)) === 0);

    // A mono channel is untouched.
    const doc = parse('A 0 200 0 100 0 1 0 - - - - - 1.5:20000\n').doc;
    const r = createRenderer(resolveAudioChannels(doc).channels,
                             { beatJitter: sessionBeatJitter(doc) });
    const s = r.channelStates(5000)[0];
    assert.equal(s.binaural, false);
    assert.equal(s.freqR, null);
});

// ---- Waveforms -------------------------------------------------------------

test('waveform: Q32 phase quadrants for square, triangle and saw', () => {
    assert.equal(waveform(1, 0), 1);
    assert.equal(waveform(1, 0x80000000), -1);
    // The triangle starts at -1 at phase 0 (f * 2/0x7FFFFFFF - 1).
    assert.ok(Math.abs(waveform(2, 0) - (-1)) < 1e-6);
    assert.ok(Math.abs(waveform(2, 0x80000000) - 1) < 1e-6);
    // Saw reads the phase as SIGNED, so it starts at 0 and jumps at the half.
    assert.ok(Math.abs(waveform(3, 0)) < 1e-9);
    assert.ok(Math.abs(waveform(3, 0x40000000) - 0.5) < 1e-6);
    assert.ok(waveform(3, 0x80000000) < 0);
    // Sine.
    assert.ok(Math.abs(waveform(0, 0x40000000) - 1) < 1e-6);
});

// ---- De-click (D8) ---------------------------------------------------------

test('a stepped volume change glides over 220 samples instead of jumping', () => {
    // Measured on a 0.5 Hz SQUARE carrier, which holds a constant +1 for the
    // first second — so every output sample is exactly
    // amp x cos(pi/4) x 1/16 and the amplitude envelope is directly readable
    // sample by sample. (A 200 Hz carrier hides the ramp: one cycle is 220
    // samples, exactly the ramp length, so any per-cycle measurement sees the
    // whole step at once.)
    const { left } = render('A 0 0.5 0 20 0 1 0 1\nA 500 0.5 0 100 0 1 0 1\n', 900);
    const unit = Math.cos(Math.PI / 4) * MIX_GAIN;
    const at = Math.round(0.5 * SAMPLE_RATE);

    // Before the step: amplitude 0.2.
    assert.ok(Math.abs(left[at - 10] / unit - 0.2) < 1e-6);
    // Mid-ramp, halfway through the 220 samples: roughly halfway to 1.0.
    const mid = left[at + AMP_RAMP_SAMPLES / 2] / unit;
    assert.ok(mid > 0.45 && mid < 0.75, 'expected a mid-ramp amplitude, got ' + mid);
    // Settled right after the ramp.
    assert.ok(Math.abs(left[at + AMP_RAMP_SAMPLES + 5] / unit - 1.0) < 1e-6);

    // And no single sample moves more than one ramp step.
    const step = (1.0 - 0.2) / AMP_RAMP_SAMPLES * unit;
    let maxJump = 0;
    for (let i = at - 50; i < at + AMP_RAMP_SAMPLES + 50; i++) {
        maxJump = Math.max(maxJump, Math.abs(left[i + 1] - left[i]));
    }
    assert.ok(maxJump <= step * 1.02,
        'biggest per-sample move ' + maxJump + ' exceeded one ramp step ' + step);
});

// ---- Streaming equivalence -------------------------------------------------

test('renderBlock is block-size independent for a contiguous render', () => {
    const doc = parse('A 0 200 -30 90 6 1 0 0 50 1\nA 700 300 30 60 9 1\n').doc;
    const { channels } = resolveAudioChannels(doc);
    const N = 4096;

    const a = renderSessionDevice(channels, { totalMs: (N / SAMPLE_RATE) * 1000 });

    const r = createRenderer(channels, {});
    const L = new Float32Array(N), R = new Float32Array(N);
    const bl = new Float32Array(512), br = new Float32Array(512);
    for (let off = 0; off < N; off += 512) {
        r.renderBlock(bl, br, off, 512);
        L.set(bl, off); R.set(br, off);
    }
    for (let i = 0; i < N; i++) {
        assert.ok(Math.abs(L[i] - a.left[i]) < 1e-9,
            'left differs at sample ' + i + ': ' + L[i] + ' vs ' + a.left[i]);
    }
});

test('seek reproduces the same signal as playing through to that point', () => {
    // Not sample-exact by construction — carrier phase restarts on a seek, and
    // the device cannot reproduce absolute phase either — so assert on the
    // ENVELOPE, which is what a listener and the gate phase actually depend on.
    const doc = parse('A 0 200 0 100 8 1 0 0 50 0\n').doc;
    const { channels } = resolveAudioChannels(doc);
    const at = Math.round(3.5 * SAMPLE_RATE);
    const n = 4096;

    const full = renderSessionDevice(channels, { totalMs: 4000 });
    const r = createRenderer(channels, {});
    const L = new Float32Array(n), R = new Float32Array(n);
    r.seek(at);
    r.renderBlock(L, R, at, n);

    assert.ok(Math.abs(peak(L) - peak(full.left.subarray(at, at + n))) < 1e-3,
        'a seek should land at the same gate level');
});

test('toInt16Device truncates toward zero, as the C cast does', () => {
    assert.equal(toInt16Device(0), 0);
    assert.equal(toInt16Device(1), 32767);
    assert.equal(toInt16Device(-1), -32767);
    assert.equal(toInt16Device(2), 32767);
    // 0.5 * 32767 = 16383.5 -> truncates to 16383, where a rounding encoder
    // against 32768 would give 16384.
    assert.equal(toInt16Device(0.5), 16383);
});
