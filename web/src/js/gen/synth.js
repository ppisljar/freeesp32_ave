// Offline session synth (bg_browser_push_plan.md, Phase 3.5).
//
// Reproduces the device's audio path (main/audio_generator.c) sample-by-sample
// so the browser can "bounce" a whole .ledc session to one WAV. Computing the
// float buffer directly (rather than wiring Web Audio nodes) lets us port the
// device arithmetic verbatim:
//   - Q32 phase accumulators:      phase = (phase + inc) >>> 0,  inc = freq*2^32/44100
//   - binaural: LEFT = freq, RIGHT = freqR (ABSOLUTE right-ear Hz; beat = freqR-freq)
//   - isochronic AM: sample *= 1 + 0.1*sin(modPhase)      (fixed 0.1 depth, mod = Hz)
//   - equal-power pan: L = in*cos((pan+1)π/4), R = in*sin(...)   (skipped for binaural)
//   - noise: xorshift LFSR (white) -> Kellet IIR (pink) / leaky integrator (brown)
//   - mix: sum channels, / nActive, * masterGain, hard-clamp [-1,1]
//   - sweeps: '>' linear / '*' quadratic-ease to the NEXT same-channel entry
//   - periodic mod prefixes '^~/\_': oscillate a field value<->modEnd over period
//
// Fidelity is perceptual, not bit-exact (sine uses Math.sin vs the device's 4096
// LUT; the 5 ms de-click amp ramps are approximated by a short fade-in). All
// formulas are cited against audio_generator.c so they can be re-verified.

import { SAMPLE_RATE, cell } from './model.js';

const Q32 = 4294967296;                 // 2^32
const Q32_PER_HZ = Q32 / SAMPLE_RATE;   // matches audio_generator.c:68
const TWO_PI = Math.PI * 2;

// ---- EEG-contour single-cycle table (mirrors audio_generator.c eeg_lut) -----
// Same harmonic coefficients, DC-removed + peak-normalized. 1024-entry, top-10
// bits index / low-22 bits interpolate — matching the firmware Q32 lookup so the
// browser bounce/preview is a perceptual match for wave_type=7.
const EEG_HARM_AMP   = [1.00, 0.50, 0.33, 0.22, 0.14, 0.09];
const EEG_HARM_PHASE = [0.00, 0.50, 1.20, -0.70, 0.90, 2.10];
const EEG_LUT_SIZE = 1024;
const eegLut = (() => {
    const t = new Float32Array(EEG_LUT_SIZE);
    let mean = 0;
    for (let i = 0; i < EEG_LUT_SIZE; i++) {
        const theta = TWO_PI * i / EEG_LUT_SIZE;
        let v = 0;
        for (let h = 0; h < EEG_HARM_AMP.length; h++) v += EEG_HARM_AMP[h] * Math.sin((h + 1) * theta + EEG_HARM_PHASE[h]);
        t[i] = v; mean += v;
    }
    mean /= EEG_LUT_SIZE;
    let peak = 1e-9;
    for (let i = 0; i < EEG_LUT_SIZE; i++) { t[i] -= mean; const m = Math.abs(t[i]); if (m > peak) peak = m; }
    for (let i = 0; i < EEG_LUT_SIZE; i++) t[i] /= peak;
    return t;
})();
function eegSample(phase) {
    const p = phase >>> 0;
    const idx  = (p >>> 22) & (EEG_LUT_SIZE - 1);
    const idx2 = (idx + 1) & (EEG_LUT_SIZE - 1);
    const frac = (p & 0x3FFFFF) / 0x400000;   // low 22 bits
    const a = eegLut[idx];
    return a + frac * (eegLut[idx2] - a);
}

// ---- Waveform from a Q32 phase (audio_generator.c section 2/7) -------------
// wave: 0 sine 1 square 2 triangle 3 saw 7 eeg  (4/5/6 = noise, handled separately)
export function waveform(wave, phase) {
    switch (wave) {
        case 1: return (phase >>> 0) < 0x80000000 ? 1 : -1;                 // square
        case 2: {                                                          // triangle
            let f = phase >>> 0;
            if (f >= 0x80000000) f = (0xFFFFFFFF - f) >>> 0;
            return f * (2 / 0x7FFFFFFF) - 1;
        }
        case 3: return (phase | 0) / 0x80000000;                           // sawtooth (signed/2^31)
        case 7: return eegSample(phase);                                   // EEG-contour carrier
        default: return Math.sin(TWO_PI * (phase >>> 0) / Q32);            // sine (0)
    }
}

// ---- Evaluate a compound cell at time t (sweeps + periodic mod) ------------
// cell/next are model cells {value, interp, modEnd, modPeriodMs}. thisMs/nextMs
// bound the sweep window (nextMs null => no next entry => value holds).
export function evalField(cell, next, thisMs, nextMs, tMs) {
    if (!cell) return 0;
    const base = cell.value || 0;
    const k = cell.interp || 'none';
    if ((k === 'lin' || k === 'quad') && next && nextMs != null && nextMs > thisMs) {
        let p = (tMs - thisMs) / (nextMs - thisMs);
        if (p < 0) p = 0; else if (p > 1) p = 1;
        if (k === 'quad') p = p < 0.5 ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p); // ease-in-out
        return base + ((next.value || 0) - base) * p;
    }
    if (k === 'tri' || k === 'sine' || k === 'sawup' || k === 'sawdn' || k === 'sq') {
        const period = cell.modPeriodMs || 1000;
        const end = (cell.modEnd == null) ? base : cell.modEnd;
        let ph = ((tMs - thisMs) % period) / period;
        if (ph < 0) ph += 1;
        let shape;
        switch (k) {
            case 'tri':   shape = ph < 0.5 ? 2 * ph : 2 - 2 * ph; break;
            case 'sine':  shape = 0.5 - 0.5 * Math.cos(TWO_PI * ph); break;
            case 'sawup': shape = ph; break;
            case 'sawdn': shape = 1 - ph; break;
            default:      shape = ph < 0.5 ? 0 : 1; break;               // square
        }
        return base + (end - base) * shape;
    }
    return base;
}

// ---- Extract per-channel entry lists from a doc ----------------------------
// Returns { channels: Map<ch, entries[]>, maxTimeMs }. Each entry:
//   { time, freq, pan, vol, mod, freqR, wave }  (freq/pan/vol/mod are cells)
export function extractAudioChannels(doc) {
    const channels = new Map();
    let maxTimeMs = 0;
    for (const row of (doc.rows || [])) {
        if (row.kind !== 'audio') continue;
        const ch = (row.channel == null) ? 1 : row.channel;
        if (!channels.has(ch)) channels.set(ch, []);
        channels.get(ch).push({
            time: row.time || 0,
            // Keep raw (may be null = `-` "leave unchanged"); resolved below.
            freq: row.freq, pan: row.pan, vol: row.vol, mod: row.mod,
            freqR: (row.freqR == null) ? 0 : row.freqR,
            wave: row.waveType,   // null = absent/`-` → carried below
        });
        if ((row.time || 0) > maxTimeMs) maxTimeMs = row.time || 0;
    }
    // Resolve the `-` sentinel: a null field carries the previous entry's value
    // for that channel (default at the channel's first entry).
    for (const list of channels.values()) {
        list.sort((a, b) => a.time - b.time);
        const cur = { freq: cell(0), pan: cell(0), vol: cell(0), mod: cell(0), wave: 0 };
        for (const e of list) {
            if (e.freq == null) e.freq = cur.freq; else cur.freq = e.freq;
            if (e.pan  == null) e.pan  = cur.pan;  else cur.pan  = e.pan;
            if (e.vol  == null) e.vol  = cur.vol;  else cur.vol  = e.vol;
            if (e.mod  == null) e.mod  = cur.mod;  else cur.mod  = e.mod;
            if (e.wave == null) e.wave = cur.wave; else cur.wave = e.wave;
        }
    }
    return { channels, maxTimeMs };
}

// White-noise LFSR step (xorshift 11/7/17), returns [-1,1) and new state.
function whiteStep(state) {
    let s = state >>> 0;
    s ^= s >>> 11; s = s >>> 0;
    s ^= (s << 7) >>> 0; s = s >>> 0;
    s ^= s >>> 17; s = s >>> 0;
    return [(s | 0) / 0x80000000, s];
}

// ---- Core render: entries -> {left,right} Float32Array ---------------------
// opts: { totalMs, sampleRate, masterGain }
export function renderSession(channels, opts = {}) {
    const sr = opts.sampleRate || SAMPLE_RATE;
    const totalMs = Math.max(1, opts.totalMs || 0);
    const masterGain = (opts.masterGain == null) ? 1.0 : opts.masterGain;
    const N = Math.max(1, Math.ceil((totalMs / 1000) * sr));
    const left = new Float32Array(N);
    const right = new Float32Array(N);
    const nActive = new Uint16Array(N);
    const fadeSamples = Math.round(0.005 * sr); // 220 @ 44.1k — de-click fade-in

    for (const [, entries] of channels) {
        if (!entries.length) continue;
        const startSample = Math.floor((entries[0].time / 1000) * sr);
        if (startSample >= N) continue;
        for (let i = startSample; i < N; i++) nActive[i] += 1;

        let phaseL = 0, phaseR = 0, phaseMod = 0;         // uint32 accumulators
        let nsL = 0xABCD1234, nsR = 0x12345678;           // white LFSR states
        let pb0 = 0, pb1 = 0, pb2 = 0, pb0r = 0, pb1r = 0, pb2r = 0; // pink IIR
        let brnL = 0, brnR = 0;                           // brown integrators
        let ei = 0;                                       // current entry index

        for (let i = startSample; i < N; i++) {
            const tMs = (i * 1000) / sr;
            while (ei + 1 < entries.length && entries[ei + 1].time <= tMs) ei++;
            const ent = entries[ei];
            const next = entries[ei + 1] || null;
            const nextMs = next ? next.time : null;

            const freq = evalField(ent.freq, next && next.freq, ent.time, nextMs, tMs);
            const panRaw = evalField(ent.pan, next && next.pan, ent.time, nextMs, tMs);
            const volRaw = evalField(ent.vol, next && next.vol, ent.time, nextMs, tMs);
            const modHz = evalField(ent.mod, next && next.mod, ent.time, nextMs, tMs);
            const pan = Math.max(-1, Math.min(1, panRaw / 100));
            let amp = Math.max(0, volRaw / 100);
            // Binaural: right ear tracks freq + (freqR-baseFreq) to preserve the beat.
            const baseFreq = ent.freq ? (ent.freq.value || 0) : 0;
            const binaural = ent.freqR > 0 && ent.freqR !== baseFreq;
            const freqRight = binaural ? (freq + (ent.freqR - baseFreq)) : freq;

            // De-click fade-in over the first ~5 ms of the channel.
            const since = i - startSample;
            if (since < fadeSamples) amp *= since / fadeSamples;

            // Oscillator (noise waves 4/5/6 handled here; eeg=7 is a carrier → waveform()).
            let rawL, rawR;
            if (ent.wave >= 4 && ent.wave <= 6) {
                let w;
                [w, nsL] = whiteStep(nsL);
                if (ent.wave === 5) {         // pink (Kellet)
                    pb0 = 0.99886 * pb0 + w * 0.0555179;
                    pb1 = 0.99332 * pb1 + w * 0.0750759;
                    pb2 = 0.96900 * pb2 + w * 0.1538520;
                    rawL = (pb0 + pb1 + pb2 + w * 0.5362) * 0.11;
                } else if (ent.wave === 6) {  // brown
                    brnL = 0.998 * brnL + w * 0.02; rawL = brnL * 3.5;
                } else {                      // white
                    rawL = w;
                }
                if (binaural) {
                    let w2; [w2, nsR] = whiteStep(nsR);
                    if (ent.wave === 5) {
                        pb0r = 0.99886 * pb0r + w2 * 0.0555179;
                        pb1r = 0.99332 * pb1r + w2 * 0.0750759;
                        pb2r = 0.96900 * pb2r + w2 * 0.1538520;
                        rawR = (pb0r + pb1r + pb2r + w2 * 0.5362) * 0.11;
                    } else if (ent.wave === 6) {
                        brnR = 0.998 * brnR + w2 * 0.02; rawR = brnR * 3.5;
                    } else { rawR = w2; }
                } else { rawR = rawL; }
            } else {
                rawL = waveform(ent.wave, phaseL);
                rawR = binaural ? waveform(ent.wave, phaseR) : rawL;
            }

            let sL = amp * rawL, sR = amp * rawR;

            if (modHz > 0) {
                const m = 1 + 0.1 * Math.sin(TWO_PI * (phaseMod >>> 0) / Q32);
                sL *= m; sR *= m;
                phaseMod = (phaseMod + ((modHz * Q32_PER_HZ) >>> 0)) >>> 0;
            }

            let outL, outR;
            if (binaural) {
                outL = sL; outR = sR;                         // no pan for binaural
            } else {
                const angle = (pan + 1) * (Math.PI / 4);      // equal-power
                outL = sL * Math.cos(angle);
                outR = sR * Math.sin(angle);
            }
            left[i] += outL; right[i] += outR;

            phaseL = (phaseL + ((freq * Q32_PER_HZ) >>> 0)) >>> 0;
            if (binaural) phaseR = (phaseR + ((freqRight * Q32_PER_HZ) >>> 0)) >>> 0;
        }
    }

    // Normalise by active count, apply master gain, hard-clamp.
    for (let i = 0; i < N; i++) {
        const inv = nActive[i] > 0 ? 1 / nActive[i] : 1;
        let l = left[i] * inv * masterGain;
        let r = right[i] * inv * masterGain;
        left[i] = l > 1 ? 1 : (l < -1 ? -1 : l);
        right[i] = r > 1 ? 1 : (r < -1 ? -1 : r);
    }
    return { left, right, sampleRate: sr };
}

// ---- Mixing helpers (pure) for the bounce ----------------------------------

// Allocate zeroed stereo buffers for a given duration.
export function zeroBuffers(totalMs, sr = SAMPLE_RATE) {
    const N = Math.max(1, Math.ceil((totalMs / 1000) * sr));
    return { left: new Float32Array(N), right: new Float32Array(N), sampleRate: sr };
}

// Add src (stereo Float32 channels) into dst at `offsetSamples`, scaled by gain.
// Overruns past dst are truncated. `loopToFill` (BG use) repeats src to cover
// [offsetSamples, dst.length). No clamping here — call clampBuffers() at the end.
export function mixInto(dstL, dstR, srcL, srcR, offsetSamples, gain = 1, loopToFill = false) {
    const N = dstL.length;
    const S = srcL.length;
    if (S === 0) return;
    if (loopToFill) {
        for (let i = Math.max(0, offsetSamples); i < N; i++) {
            const s = (i - offsetSamples) % S;
            dstL[i] += srcL[s] * gain;
            dstR[i] += (srcR ? srcR[s] : srcL[s]) * gain;
        }
    } else {
        for (let s = 0; s < S; s++) {
            const i = offsetSamples + s;
            if (i < 0) continue;
            if (i >= N) break;
            dstL[i] += srcL[s] * gain;
            dstR[i] += (srcR ? srcR[s] : srcL[s]) * gain;
        }
    }
}

// Hard-clamp stereo buffers to [-1,1] in place.
export function clampBuffers(left, right) {
    for (let i = 0; i < left.length; i++) {
        let l = left[i], r = right[i];
        left[i] = l > 1 ? 1 : (l < -1 ? -1 : l);
        right[i] = r > 1 ? 1 : (r < -1 ? -1 : r);
    }
}

// ---- Convenience: doc -> canonical WAV bytes -------------------------------
// opts: { tailMs (extra time after last entry), masterGain, minMs }
export function bounceSessionToWav(doc, opts = {}, encodeWav16) {
    const { channels, maxTimeMs } = extractAudioChannels(doc);
    const tailMs = (opts.tailMs == null) ? 2000 : opts.tailMs;
    const minMs = opts.minMs || 1000;
    const totalMs = Math.max(minMs, maxTimeMs + tailMs);
    const { left, right } = renderSession(channels, {
        totalMs, sampleRate: SAMPLE_RATE, masterGain: opts.masterGain,
    });
    return { wav: encodeWav16([left, right], SAMPLE_RATE), durationMs: totalMs };
}
