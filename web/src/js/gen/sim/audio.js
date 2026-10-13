// Audio synthesis engine, ported for simulation — a STREAMING renderer.
//
// THE one audio engine in the browser: the Simulate view plays it live and the
// bounce renders with it offline, so a preview and a WAV cannot disagree.
//
// It renders a BLOCK at a time with seek, rather than a whole session into one
// pair of Float32Arrays — 30 minutes of stereo floats is 635 MB — by keeping
// per-channel state between calls.
//
// It replaced an earlier bounce-only engine (`gen/synth.js`, now reduced to the
// mixing helpers in `gen/mix.js`) that had drifted from
// main/audio_generator.c in fifteen documented ways. The three that changed
// what you HEARD:
//
//   1. MIX NORMALISATION IS A FIXED 1/16, NOT 1/nActive.
//      audio_generator.c:875 `float new_target = 1.0f / NUM_AUDIO_CHANNELS` —
//      constant, deliberately, so that sixteen same-panned channels at full
//      amplitude sum to exactly ±1 "regardless of how many channels are active".
//      The old engine divided by the active count instead, rendering a
//      two-channel session ~18 dB hot and a one-channel session ~24 dB hot.
//      Three measured device reference points this reproduces exactly:
//        one centre-panned sine at vol 100 → int16 peak 1448 (−27.1 dBFS)
//        a binaural channel (pan bypassed) → −24.1 dBFS
//        pink noise at vol 100            → −43.3 dBFS
//      That is a quiet master by design; the UI's monitor trim exists for it.
//
//   2. THE ENVELOPE SYSTEM. The old engine had only `1 + 0.1*sin(modPhase)`.
//      The device has five envelopes, a depth column with env-dependent
//      defaults, and duty/attack/phase columns. A file saying `env 0` with a
//      depth rendered as a 10 % tremolo instead of a full-depth gate —
//      precisely the bug the firmware fixed (config_parser.c:2720:
//      "every 'isochronic' gate measured ~5.8% depth: a tremolo, not a pulse").
//
//   3. NOISE IS SCALED BY 0.5. AUDIO_NOISE_LEVEL_SCALE (audio_generator.c:86)
//      applies to waves 4/5/6 and deliberately NOT to wave 7; without it a
//      noise bed sits 6 dB above the tones beside it.
//
// Also fixed here: the gate phase re-anchors to the entry's timestamp rather
// than free-running (a measured ~48 ms offset before the firmware fix); freqR
// carries forward and can ramp; beat jitter exists; the channel default is 0;
// amplitude is clamped to 1.0 and frequency to Nyquist while pan deliberately
// is NOT (|pan| > 1 really does invert the left output on hardware).
//
// WHAT IS IDEALISED
//
//   - Per-sample rather than per-buffer evaluation of the mod slots and the
//     iso/freqR sweeps. The device steps those in a 23.2195 ms staircase; here
//     they are smooth. Core freq/amp/pan/mod sweeps step per sample on both.
//   - No start-latency model. The device pre-advances each phase by
//     −(increment × 2048) and then skips forward by however late the arming
//     actually was (0..1024 frames, effectively random per run), so absolute
//     carrier phase is not reproducible even on the device. Rendered here as
//     late_us = 0.
//   - Math.sin instead of the 4096-entry LUT with 20-bit interpolation (≈
//     −93 dB difference).
//   - Jitter LFO phase is keyed to session time; the device keys it to the
//     absolute wall clock, so its phase differs on every run.
//
// Pure: no Web Audio, no DOM. The transport in views/sim.js owns the
// AudioContext and calls renderBlock(). Unit-tested in web/test/sim_audio.test.js.

import { SAMPLE_RATE, NYQUIST, NUM_AUDIO_CHANNELS } from '../model.js';
import { evalCell, activeIndexAt, AUDIO_DEFAULTS } from './resolve.js';

const Q32 = 4294967296;
const Q32_PER_HZ = Q32 / SAMPLE_RATE;      // audio_generator.c:68
const TWO_PI = Math.PI * 2;

// audio_generator.c:875 — fixed headroom divisor, never 1/n_active.
export const MIX_GAIN = 1 / NUM_AUDIO_CHANNELS;
// audio_generator.c:86
export const NOISE_LEVEL_SCALE = 0.5;
// AUDIO_AMP_RAMP_SAMPLES — the 5 ms de-click ramp, 220 samples at 44.1 kHz.
export const AMP_RAMP_SAMPLES = 220;

// audio_generator.c:138
export const ISO_ENV_SQUARE = 0;
export const ISO_ENV_SINE = 1;
export const ISO_ENV_TRIANGLE = 2;
export const ISO_ENV_TRAPEZOID = 3;
export const ISO_ENV_TREMOLO = 4;

// config_parser.c:2669 — params.mod_depth is hardcoded, and is the fallback
// whenever no entry has ever set an explicit depth.
export const LEGACY_MOD_DEPTH = 0.1;

// ---- Waveforms -------------------------------------------------------------

// The EEG-contour single-cycle table (audio_generator.c eeg_lut): the same
// harmonic coefficients, DC-removed and peak-normalised, read with the
// firmware's top-10-bits index / low-22-bits interpolation.
const EEG_HARM_AMP = [1.00, 0.50, 0.33, 0.22, 0.14, 0.09];
const EEG_HARM_PHASE = [0.00, 0.50, 1.20, -0.70, 0.90, 2.10];
const EEG_LUT_SIZE = 1024;
const eegLut = (() => {
    const t = new Float32Array(EEG_LUT_SIZE);
    let mean = 0;
    for (let i = 0; i < EEG_LUT_SIZE; i++) {
        const theta = TWO_PI * i / EEG_LUT_SIZE;
        let v = 0;
        for (let h = 0; h < EEG_HARM_AMP.length; h++) {
            v += EEG_HARM_AMP[h] * Math.sin((h + 1) * theta + EEG_HARM_PHASE[h]);
        }
        t[i] = v; mean += v;
    }
    mean /= EEG_LUT_SIZE;
    let peak = 1e-9;
    for (let i = 0; i < EEG_LUT_SIZE; i++) {
        t[i] -= mean;
        const m = Math.abs(t[i]);
        if (m > peak) peak = m;
    }
    for (let i = 0; i < EEG_LUT_SIZE; i++) t[i] /= peak;
    return t;
})();

function eegSample(phase) {
    const p = phase >>> 0;
    const idx = (p >>> 22) & (EEG_LUT_SIZE - 1);
    const idx2 = (idx + 1) & (EEG_LUT_SIZE - 1);
    const frac = (p & 0x3FFFFF) / 0x400000;
    const a = eegLut[idx];
    return a + frac * (eegLut[idx2] - a);
}

// Carrier sample from a Q32 phase. Waves 4/5/6 are noise and never reach here.
export function waveform(wave, phase) {
    switch (wave) {
        case 1: return (phase >>> 0) < 0x80000000 ? 1 : -1;            // square
        case 2: {                                                      // triangle
            let f = phase >>> 0;
            if (f >= 0x80000000) f = (0xFFFFFFFF - f) >>> 0;
            return f * (2 / 0x7FFFFFFF) - 1;
        }
        case 3: return (phase | 0) / 0x80000000;                       // sawtooth
        case 7: return eegSample(phase);                               // EEG contour
        default: return Math.sin(TWO_PI * (phase >>> 0) / Q32);        // sine
    }
}

export function isNoiseWave(w) { return w >= 4 && w <= 6; }

// ---- Envelopes -------------------------------------------------------------

// audio_generator.c:163 — trapezoid gate. `duty` and `attackFrac` are fractions
// of the mod cycle; the attack is applied symmetrically to rise AND fall and is
// clamped to half the on-time, so it degenerates to a triangle rather than
// overlapping itself.
export function isoTrapezoid(ph, duty, attackFrac) {
    if (ph >= duty) return 0;
    let a = attackFrac;
    if (a > duty * 0.5) a = duty * 0.5;
    if (a <= 0) return 1;
    if (ph < a) return ph / a;
    if (ph > duty - a) return (duty - ph) / a;
    return 1;
}

/*
 * audio_generator.c:179 — the unipolar gate for env 0..3, g in [0,1].
 *
 * Note which envelopes read `duty`: SQUARE and TRAPEZOID do, SINE and TRIANGLE
 * do not. A duty column next to env 1 or env 2 is a silent no-op, and env 2's
 * on-fraction is always exactly 50 %.
 *
 * SINE is a TRUE raised sine. It used to be 4*ph*(1-ph), a parabola whose
 * derivative discontinuity at the cycle boundary measured h2/h1 = 0.251 and
 * h3/h1 = 0.112 — so an 8 Hz "alpha" gate was also driving 16 and 24 Hz, which
 * for entrainment is not a cosmetic difference.
 */
export function isoGate(ph, env, duty, attackFrac) {
    switch (env) {
        case ISO_ENV_SQUARE: return ph < duty ? 1 : 0;
        case ISO_ENV_TRIANGLE: return ph < 0.5 ? 2 * ph : 2 - 2 * ph;
        case ISO_ENV_TRAPEZOID: return isoTrapezoid(ph, duty, attackFrac);
        default: return 0.5 - 0.5 * Math.cos(TWO_PI * ph);             // SINE
    }
}

/*
 * The sticky iso state per entry.
 *
 * env/duty/attack/depth are NOT per-entry values — they are channel state that
 * an entry may or may not write (config_parser.c:2715 passes a "leave alone"
 * sentinel for every field the line omits). Three rules that are each easy to
 * miss:
 *
 *   - DEPTH's default is env-dependent. An explicit column wins; otherwise an
 *     entry that SELECTS an envelope also picks that envelope's natural depth,
 *     100 % for the unipolar gates and 10 % for the tremolo
 *     (config_parser.c:2729). An entry that touches neither leaves the
 *     channel's depth alone, and a channel that has never been given one falls
 *     back to params.mod_depth = 0.1.
 *   - `duty 0` IS A NO-OP. set_iso_channel writes duty only when `duty_pct > 0`
 *     (audio_generator.c:236), so a literal 0 leaves the previous duty standing.
 *     attack 0 and depth 0 are both written, because their gates are >= 0.
 *   - env outside 0..4 folds to 4 (audio_generator.c:235). The LED side folds
 *     out-of-range env to 0 instead — do not share a decoder between them.
 */
export function annotateIso(entries) {
    let env = AUDIO_DEFAULTS.env;          // 4 (tremolo)
    let duty = AUDIO_DEFAULTS.duty / 100;  // 0.5
    let attack = AUDIO_DEFAULTS.attack;    // 3 ms
    let depth = -1;                        // -1 = never set → LEGACY_MOD_DEPTH

    for (const e of entries) {
        const raw = e.raw;
        const hasEnv = raw.env !== undefined && raw.env !== null;
        const hasDuty = raw.duty !== undefined && raw.duty !== null;
        const hasAttack = raw.attack !== undefined && raw.attack !== null;
        const hasDepth = raw.depth !== undefined && raw.depth !== null;

        if (hasEnv) {
            const v = Math.trunc(raw.env);
            env = (v >= 0 && v <= ISO_ENV_TREMOLO) ? v : ISO_ENV_TREMOLO;
        }
        if (hasDuty) {
            const pct = raw.duty.value;
            if (pct > 0) duty = Math.min(100, pct) / 100;   // 0 leaves it alone
        }
        if (hasAttack) {
            const ms = raw.attack.value;
            if (ms >= 0) attack = ms;
        }
        if (hasDepth) {
            const pct = raw.depth.value;
            if (pct >= 0) depth = Math.min(100, pct) / 100;
        } else if (hasEnv) {
            depth = (env <= ISO_ENV_TRAPEZOID) ? 1.0 : 0.1;
        }

        e.iso = {
            env,
            duty,
            attack,
            depth: depth < 0 ? LEGACY_MOD_DEPTH : depth,
            depthExplicit: depth >= 0,
        };
    }
    return entries;
}

// ---- Beat jitter -----------------------------------------------------------

/*
 * audio_generator.c:245 — anti-habituation detune of the RIGHT carrier only.
 *
 * GLOBAL, not per-channel: the last audio line in the file that carries the
 * jitter column sets it for the whole session. Gated on freqR > 0, so it only
 * ever moves a binaural beat.
 */
export function sessionBeatJitter(doc) {
    let spec = null;
    for (const row of (doc && doc.rows) || []) {
        if (row.kind !== 'audio') continue;
        if (row.jitter === undefined || row.jitter === null) continue;
        spec = {
            amp: row.jitter.amp || 0,
            periodMs: (row.jitter.period === undefined || row.jitter.period === null)
                ? 45000 : row.jitter.period,
        };
    }
    return spec;
}

export function beatJitterHz(spec, tMs) {
    if (!spec || !spec.amp || !spec.periodMs) return 0;
    let ph = (tMs % spec.periodMs) / spec.periodMs;
    if (ph < 0) ph += 1;
    return spec.amp * Math.sin(TWO_PI * ph);
}

// ---- Noise -----------------------------------------------------------------

// xorshift 11/7/17, returning [-1,1) and the new state.
function whiteStep(state) {
    let s = state >>> 0;
    s ^= s >>> 11; s >>>= 0;
    s ^= (s << 7) >>> 0; s >>>= 0;
    s ^= s >>> 17; s >>>= 0;
    return [(s | 0) / 0x80000000, s];
}

// audio_generator.c:572 — the seeds every channel starts from. Fixed, not
// per-channel: the device gives each channel the same pair, and the L/R streams
// are decorrelated by the two different constants rather than by the channel
// index. Using a per-channel seed here would make two noise beds sound
// different from the device's.
export const NOISE_SEED_L = 0xABCD1234;
export const NOISE_SEED_R = 0x12345678;

function makeNoiseState(seed) {
    return { s: seed >>> 0, pb0: 0, pb1: 0, pb2: 0, brown: 0 };
}

/*
 * Pick between a latched value and a continuously animated one.
 *
 * The iso fields (duty/attack/phase/depth) are LATCHED at dispatch through
 * set_iso_channel, which is where the "duty 0 is a no-op" gate lives. A `>`,
 * `*` or LFO prefix on one of them additionally installs a sweep or mod on
 * AUDIO_PARAM_ISO_*, which then animates the latched value. So: a prefixed cell
 * is evaluated continuously, a bare one reads the sticky latch — and a duty
 * ramp passing through 0 really does reach 0, because the sweep path has no
 * such gate.
 */
function animOr(c, tgt, entryTime, tMs, sticky) {
    if (!c) return sticky;
    const k = c.interp;
    if (!k || k === 'none') return sticky;
    return evalCell(c, tgt ? tgt.target : null, entryTime, tgt ? tgt.targetMs : null, tMs);
}

/*
 * Per-entry field evaluators, built once and cached on the entry.
 *
 * The per-sample loop below runs 44 100 times per channel per second, and in a
 * typical session most cells are plain step values that cannot change for the
 * whole entry — so calling evalCell for every field on every sample spends
 * nearly all its time re-deriving constants. Hoisting the constant fields out
 * turned a session that saturated the main thread (rAF starved, the tab
 * unresponsive) into one that renders comfortably in real time.
 *
 * `k` is null for a constant field, in which case `v` is the value; otherwise
 * `k` carries the cell and its ramp target and the loop evaluates it.
 */
function evaluators(e) {
    if (e._ev) return e._ev;
    const ev = {};
    for (const f of ['freq', 'pan', 'vol', 'mod', 'freqR',
                     'duty', 'phase', 'attack', 'depth']) {
        const c = e.cells[f];
        if (!c) { ev[f] = { k: null, v: 0, cell: null }; continue; }
        const kind = c.interp;
        if (!kind || kind === 'none') {
            ev[f] = { k: null, v: c.value || 0, cell: c };
        } else {
            const t = e.t[f] || { target: null, targetMs: null };
            ev[f] = { k: kind, v: 0, cell: c, target: t.target, targetMs: t.targetMs };
        }
    }
    e._ev = ev;
    return ev;
}

function evalAt(slot, entryTime, tMs) {
    return slot.k === null
        ? slot.v
        : evalCell(slot.cell, slot.target, entryTime, slot.targetMs, tMs);
}

/*
 * Device-accurate float -> int16.
 *
 * audio_test.c:216 does `(int16_t)(clamped * 32767.0f)` — a C cast, so it
 * TRUNCATES toward zero. gen/bgaudio.js's encodeWav16 rounds against 32768
 * instead, which differs by up to one LSB and uses a different rounding rule.
 * Used by the level tests, which assert against measured device peaks.
 */
export function toInt16Device(x) {
    const c = x > 1 ? 1 : (x < -1 ? -1 : x);
    return Math.trunc(c * 32767);
}

// One noise sample of the requested colour, advancing the chain.
// The device ALWAYS advances both the left and right chains even for a mono
// channel (it computes raw_r and discards it in the panner), so both are
// stepped here too — otherwise the two engines' LFSR states diverge the moment
// a channel becomes binaural.
function noiseStep(ns, wave) {
    let w;
    [w, ns.s] = whiteStep(ns.s);
    if (wave === 5) {                        // pink, Kellet 3-pole
        ns.pb0 = 0.99886 * ns.pb0 + w * 0.0555179;
        ns.pb1 = 0.99332 * ns.pb1 + w * 0.0750759;
        ns.pb2 = 0.96900 * ns.pb2 + w * 0.1538520;
        return (ns.pb0 + ns.pb1 + ns.pb2 + w * 0.5362) * 0.11;
    }
    if (wave === 6) {                        // brown, leaky integrator
        ns.brown = 0.998 * ns.brown + w * 0.02;
        return ns.brown * 3.5;
    }
    return w;                                // white
}

// ---- The renderer ----------------------------------------------------------

const CORE = ['freq', 'pan', 'vol', 'mod'];

/*
 * createRenderer(channels, opts)
 *
 *   channels  Map<channelIndex, entries[]> from resolveAudioChannels()
 *   opts      { sampleRate, masterGain, beatJitter }
 *
 * Returns { renderBlock(outL, outR, startSample, count), seek(sample),
 *           channelStates(), sampleRate }.
 *
 * renderBlock OVERWRITES outL/outR for [0, count). startSample must be the
 * sample index the block begins at; calling it with a non-contiguous
 * startSample without seek() first will render from stale phase.
 */
export function createRenderer(channels, opts = {}) {
    const sr = opts.sampleRate || SAMPLE_RATE;
    const masterGain = (opts.masterGain === undefined || opts.masterGain === null)
        ? 1.0 : opts.masterGain;
    const jitterSpec = opts.beatJitter || null;

    // One state object per channel. Seeded per channel so two noise beds are
    // decorrelated, as they are on the device (each channel has its own state).
    const states = [];
    for (const [ch, entries] of channels) {
        if (!entries || !entries.length) continue;
        annotateIso(entries);
        states.push({
            ch,
            entries,
            startMs: entries[0].time,
            startSample: Math.floor((entries[0].time / 1000) * sr),
            phaseL: 0, phaseR: 0, phaseMod: 0,
            nsL: makeNoiseState(NOISE_SEED_L),
            nsR: makeNoiseState(NOISE_SEED_R),
            ei: 0,
            // De-click: the device arms a 220-sample linear amplitude ramp at
            // channel start and at every stepped amplitude change where the
            // delta exceeds 1e-4 and no amp sweep is running. Tracked as a
            // target/current pair so a stepped vol edit glides here too.
            ampCurrent: 0, ampRampLeft: 0, ampStep: 0, ampTarget: 0,
            lastAmpTarget: null,
        });
    }
    states.sort((a, b) => a.ch - b.ch);

    function resetChannel(st, sample) {
        const tMs = (sample * 1000) / sr;
        st.ei = Math.max(0, activeIndexAt(st.entries, tMs));
        st.nsL = makeNoiseState(NOISE_SEED_L);
        st.nsR = makeNoiseState(NOISE_SEED_R);
        st.ampCurrent = 0; st.ampRampLeft = 0; st.ampStep = 0;
        st.lastAmpTarget = null;

        // Carrier phase pre-advance (audio_generator.c:552): the device starts
        // each accumulator at -(increment x 2048) so it reaches 0 at the sample
        // the DMA pipeline actually emits first. Absolute carrier phase is not
        // reproducible anyway — the device then skips forward by however late
        // the arming happened to be, 0..1024 frames, effectively random per run
        // — but the pre-advance is what sets the INITIAL RELATIVE phase of a
        // monaural pair, and 2048 frames of an 8 Hz detune is 0.37 of a beat
        // cycle. Rendered here with late_us = 0.
        {
            const e0 = st.entries[st.ei];
            const f = e0 && e0.cells.freq ? (e0.cells.freq.value || 0) : 0;
            const fr = e0 && e0.cells.freqR ? (e0.cells.freqR.value || 0) : 0;
            st.phaseL = (-(((f * Q32_PER_HZ) >>> 0) * 2048)) >>> 0;
            st.phaseR = (-((((fr > 0 ? fr : f) * Q32_PER_HZ) >>> 0) * 2048)) >>> 0;
        }
        // The gate phase is defined by the entry's position on the timeline, so
        // it can be reconstructed exactly at any seek point rather than needing
        // the history. This is the device's own re-anchor formula
        // (audio_generator.c:1115): cycles = elapsed_seconds * current_mod_freq,
        // fractional part only.
        const e = st.entries[st.ei];
        if (e) {
            const t = e.t.mod || { target: null, targetMs: null };
            const modHz = evalCell(e.cells.mod, t.target, e.time, t.targetMs, tMs);
            if (modHz > 0) {
                const cycles = ((tMs - e.time) / 1000) * modHz;
                st.phaseMod = ((cycles - Math.floor(cycles)) * Q32) >>> 0;
            } else {
                st.phaseMod = 0;
            }
        } else {
            st.phaseMod = 0;
        }
    }

    // Pre-clamp level statistics, accumulated across renderBlock calls.
    const stats = { peak: 0, clipped: 0, samples: 0 };

    function seek(sample) {
        for (const st of states) resetChannel(st, sample);
    }

    seek(0);

    function renderBlock(outL, outR, startSample, count) {
        outL.fill(0, 0, count);
        outR.fill(0, 0, count);

        for (const st of states) {
            const entries = st.entries;
            let n = Math.max(0, st.startSample - startSample);   // skip before start

            // Walk the block one ENTRY-RUN at a time rather than one sample at a
            // time. Everything that can only change at an entry boundary -- the
            // waveform, whether the channel is binaural, the latched iso state,
            // the per-field evaluators -- is then hoisted out of the inner loop,
            // which is the difference between rendering comfortably in real time
            // and saturating the main thread.
            while (n < count) {
                const i = startSample + n;
                const tMs = (i * 1000) / sr;

                let crossed = false;
                while (st.ei + 1 < entries.length && entries[st.ei + 1].time <= tMs) {
                    st.ei++; crossed = true;
                }
                const e = entries[st.ei];
                if (crossed) st.phaseMod = 0;             // frac(0 * f) = 0

                const ev = evaluators(e);
                const iso = e.iso;

                // freqR is an absolute right-ear carrier. The binaural trigger is
                // the CONFIGURED pair, not the live one: a frequency sweep on a
                // mono channel must not be allowed to trip it.
                const freqRCfg = e.cells.freqR ? (e.cells.freqR.value || 0) : 0;
                const freqCfg = e.cells.freq ? (e.cells.freq.value || 0) : 0;
                const binaural = freqRCfg > 0 && freqRCfg !== freqCfg;

                let wave = Math.trunc(e.cells.wave ? e.cells.wave.value : 0);
                if (!(wave >= 0 && wave <= 7)) wave = 0;
                const noise = isNoiseWave(wave);
                const carrierScale = noise ? NOISE_LEVEL_SCALE : 1.0;

                // While an amplitude SWEEP is running the value already moves per
                // sample, so the device snaps instead of arming a de-click ramp.
                const ampSweeping = ev.vol.k === 'lin' || ev.vol.k === 'quad';

                // Arm the de-click ramp ONCE per run, which is where the device
                // arms it too (at the pending-params latch, not per sample).
                {
                    const v = evalAt(ev.vol, e.time, tMs) / 100;
                    const target = v < 0 ? 0 : (v > 1 ? 1 : v);
                    if (st.lastAmpTarget === null) {
                        st.ampTarget = target;
                        st.ampCurrent = 0;
                        st.ampStep = target / AMP_RAMP_SAMPLES;
                        st.ampRampLeft = AMP_RAMP_SAMPLES;
                        st.lastAmpTarget = target;
                    } else if (crossed && !ampSweeping &&
                               Math.abs(target - st.lastAmpTarget) > 1e-4) {
                        st.ampTarget = target;
                        st.ampStep = (target - st.ampCurrent) / AMP_RAMP_SAMPLES;
                        st.ampRampLeft = AMP_RAMP_SAMPLES;
                        st.lastAmpTarget = target;
                    }
                }

                // This run ends at the next entry, or at the end of the block.
                const nextTime = entries[st.ei + 1] ? entries[st.ei + 1].time : Infinity;
                const nextSampleIdx = (nextTime === Infinity)
                    ? Infinity : Math.ceil((nextTime / 1000) * sr);
                const runEnd = Math.min(count, Math.max(n + 1, nextSampleIdx - startSample));

                for (; n < runEnd; n++) {
                    const si = startSample + n;
                    const t = (si * 1000) / sr;

                    const freqRaw = evalAt(ev.freq, e.time, t);
                    const panRaw = evalAt(ev.pan, e.time, t);
                    const volRaw = evalAt(ev.vol, e.time, t);
                    const modHz = evalAt(ev.mod, e.time, t);

                    // Clamped exactly where the device clamps: amplitude to 1.0
                    // and frequency to Nyquist at every _locked entry point, and
                    // pan NOT at all -- |pan| > 1 runs the equal-power law past
                    // 90 degrees and really does invert the left output's polarity.
                    const freq = freqRaw < 0 ? 0 : (freqRaw > NYQUIST ? NYQUIST : freqRaw);
                    const vt = volRaw / 100;
                    const ampTarget = vt < 0 ? 0 : (vt > 1 ? 1 : vt);
                    const pan = panRaw / 100;

                    let freqRight = freq;
                    if (binaural) {
                        freqRight = ev.freqR.cell ? evalAt(ev.freqR, e.time, t) : freqRCfg;
                        if (jitterSpec) freqRight += beatJitterHz(jitterSpec, t);
                        if (freqRight < 0) freqRight = 0;
                        else if (freqRight > NYQUIST) freqRight = NYQUIST;
                    }

                    // Step the de-click ramp; once it has finished, follow the
                    // live value so an amplitude SWEEP moves per sample.
                    if (st.ampRampLeft > 0) {
                        st.ampCurrent += st.ampStep;
                        st.ampRampLeft--;
                        if (st.ampRampLeft === 0) st.ampCurrent = st.ampTarget;
                    } else {
                        st.ampCurrent = ampTarget;
                        st.lastAmpTarget = ampTarget;
                    }
                    const amp = st.ampCurrent;

                    // Oscillator. Both noise chains always advance.
                    let rawL, rawR;
                    if (noise) {
                        rawL = noiseStep(st.nsL, wave);
                        const r = noiseStep(st.nsR, wave);
                        rawR = binaural ? r : rawL;
                    } else {
                        rawL = waveform(wave, st.phaseL);
                        rawR = binaural ? waveform(wave, st.phaseR) : rawL;
                    }

                    let sL = amp * carrierScale * rawL;
                    let sR = amp * carrierScale * rawR;

                    // Envelope. mod_phase advances ONLY inside this branch, so the
                    // gate freezes while mod is 0 rather than running on.
                    if (modHz > 0) {
                        const dRaw = ev.depth.cell && ev.depth.k !== null
                            ? evalAt(ev.depth, e.time, t) : iso.depth * 100;
                        const depth = (dRaw < 0 ? 0 : (dRaw > 100 ? 100 : dRaw)) / 100;
                        const phaseDeg = ev.phase.cell && ev.phase.k !== null
                            ? evalAt(ev.phase, e.time, t) : 0;
                        const mph = phaseDeg
                            ? (st.phaseMod + ((((((phaseDeg % 360) + 360) % 360) / 360) * Q32) >>> 0)) >>> 0
                            : st.phaseMod;
                        if (iso.env === ISO_ENV_TREMOLO) {
                            const m = 1 + depth * Math.sin(TWO_PI * mph / Q32);
                            sL *= m; sR *= m;
                        } else {
                            const duRaw = ev.duty.cell && ev.duty.k !== null
                                ? evalAt(ev.duty, e.time, t) : iso.duty * 100;
                            const duty = (duRaw < 0 ? 0 : (duRaw > 100 ? 100 : duRaw)) / 100;
                            const atRaw = ev.attack.cell && ev.attack.k !== null
                                ? evalAt(ev.attack, e.time, t) : iso.attack;
                            const attackMs = atRaw < 0 ? 0 : atRaw;
                            const g = isoGate(mph / Q32, iso.env, duty, attackMs * modHz * 0.001);
                            const m = (1 - depth) + depth * g;
                            sL *= m; sR *= m;
                        }
                        st.phaseMod = (st.phaseMod + ((modHz * Q32_PER_HZ) >>> 0)) >>> 0;
                    }

                    // Pan. For a non-binaural channel the device feeds the panner
                    // sample_l ONLY -- both outputs derive from it and raw_r is
                    // discarded, which is why the right-ear stream only reaches
                    // the output on a configured binaural channel.
                    if (binaural) {
                        outL[n] += sL * MIX_GAIN;
                        outR[n] += sR * MIX_GAIN;
                    } else {
                        const angle = (pan + 1) * (Math.PI / 4);
                        outL[n] += sL * Math.cos(angle) * MIX_GAIN;
                        outR[n] += sL * Math.sin(angle) * MIX_GAIN;
                    }

                    // BOTH accumulators always advance (audio_generator.c:1703),
                    // even on a mono channel where the right stream is discarded.
                    st.phaseL = (st.phaseL + ((freq * Q32_PER_HZ) >>> 0)) >>> 0;
                    st.phaseR = (st.phaseR + ((freqRight * Q32_PER_HZ) >>> 0)) >>> 0;
                }
            }
        }

        // Master gain then hard clamp, as audio_test.c:201 does after the BG and
        // speech post-mix stages.
        //
        // The PRE-clamp peak is recorded on the way through, because that is the
        // only place the information exists: once the clamp has run, a session
        // that overdrove the mix is indistinguishable from one that just touched
        // full scale. The device clamps in exactly the same place and loses it in
        // exactly the same way — which is why a simulator is the only thing that
        // can tell you a session clips.
        let peak = stats.peak;
        let clipped = stats.clipped;
        for (let n = 0; n < count; n++) {
            const l = outL[n] * masterGain;
            const r = outR[n] * masterGain;
            const al = l < 0 ? -l : l;
            const ar = r < 0 ? -r : r;
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
            if (al > 1) clipped++;
            if (ar > 1) clipped++;
            outL[n] = l > 1 ? 1 : (l < -1 ? -1 : l);
            outR[n] = r > 1 ? 1 : (r < -1 ? -1 : r);
        }
        stats.peak = peak;
        stats.clipped = clipped;
        stats.samples += count;
    }

    // What every channel is doing at tMs — for the timeline inspector. Pure
    // read; does not disturb the render state.
    function channelStates(tMs) {
        const out = [];
        for (const st of states) {
            const i = activeIndexAt(st.entries, tMs);
            if (i < 0) continue;
            const e = st.entries[i];
            const tgt = e.t;
            const freq = Math.max(0, Math.min(NYQUIST,
                evalCell(e.cells.freq, tgt.freq.target, e.time, tgt.freq.targetMs, tMs)));
            const vol = evalCell(e.cells.vol, tgt.vol.target, e.time, tgt.vol.targetMs, tMs);
            const pan = evalCell(e.cells.pan, tgt.pan.target, e.time, tgt.pan.targetMs, tMs);
            const mod = evalCell(e.cells.mod, tgt.mod.target, e.time, tgt.mod.targetMs, tMs);
            const freqRCfg = e.cells.freqR ? (e.cells.freqR.value || 0) : 0;
            const freqCfg = e.cells.freq ? (e.cells.freq.value || 0) : 0;
            const binaural = freqRCfg > 0 && freqRCfg !== freqCfg;
            let freqR = binaural
                ? evalCell(e.cells.freqR, e.t.freqR.target, e.time, e.t.freqR.targetMs, tMs)
                : null;
            if (binaural && jitterSpec) freqR += beatJitterHz(jitterSpec, tMs);
            out.push({
                ch: st.ch,
                entryTime: e.time,
                freq, pan, vol, mod,
                wave: Math.trunc(e.cells.wave ? e.cells.wave.value : 0),
                binaural,
                freqR,
                beatHz: binaural ? Math.abs(freqR - freq) : null,
                iso: e.iso,
            });
        }
        return out;
    }

    return {
        renderBlock, seek, channelStates,
        sampleRate: sr,
        channelCount: states.length,
        // Pre-clamp peak and the number of clipped samples since the last
        // resetStats(). `peak > 1` means the session overdrives the mix.
        stats: () => ({ ...stats }),
        resetStats: () => { stats.peak = 0; stats.clipped = 0; stats.samples = 0; },
    };
}

/*
 * Whole-session convenience render, used by gen/bounce.js.
 *
 * Renders in blocks internally, so a long session costs no more memory than the
 * output buffers themselves. Note the LEVEL: the device's fixed 1/16 divisor
 * makes this ~18-24 dB quieter than the pre-2026-10 bounce, which was hot.
 */
export function renderSessionDevice(channels, opts = {}) {
    const sr = opts.sampleRate || SAMPLE_RATE;
    const totalMs = Math.max(1, opts.totalMs || 0);
    const N = Math.max(1, Math.ceil((totalMs / 1000) * sr));
    const left = new Float32Array(N);
    const right = new Float32Array(N);
    const r = createRenderer(channels, opts);
    const BLOCK = 8192;
    const bl = new Float32Array(BLOCK);
    const br = new Float32Array(BLOCK);
    for (let off = 0; off < N; off += BLOCK) {
        const n = Math.min(BLOCK, N - off);
        r.renderBlock(bl, br, off, n);
        left.set(bl.subarray(0, n), off);
        right.set(br.subarray(0, n), off);
    }
    return { left, right, sampleRate: sr };
}
