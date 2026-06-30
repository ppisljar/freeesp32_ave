// Convenience macros (Phase 5) — also usable from the Table/Lane views.
//
// Each macro is PURE and expands into ordinary flat model rows (audioRow /
// ledRow) that the firmware plays directly. They realise the "3 previously
// unrepresentable" authoring conveniences as multi-channel / multi-entry
// timeline entries (no firmware change). The expansions satisfy the
// Bug-avoidance contract by construction:
//   - binaural emits a real freq_r (bug #4) or two hard-panned channels;
//   - pan is ×1 in -100..+100 (bug #2);
//   - noise presets use wave_type 4/5/6, not freq=0 sine (bug #6);
//   - wave_type is carried through (bug #3) and channels are explicit.

import { audioRow, ledRow, cell, NUM_AUDIO_CHANNELS } from './model.js';

// ---- Binaural --------------------------------------------------------------
// One layer -> one freq_r channel (default) OR two hard-panned channels.
// "beat" drives R = base + beat.
//   opts: { time, base, beat, volume, pan, wave, channel, channelR, stereo }
export function binaural(opts) {
    opts = opts || {};
    const time = opts.time || 0;
    const base = (opts.base === undefined) ? 200 : opts.base;
    const beat = (opts.beat === undefined) ? 10 : opts.beat;
    const volume = (opts.volume === undefined) ? 60 : opts.volume;
    const wave = (opts.wave === undefined) ? null : opts.wave;
    const channel = opts.channel || 1;
    if (opts.stereo) {
        const channelR = opts.channelR || 2;
        return [
            audioRow({ time, freq: cell(base), pan: cell(-100), vol: cell(volume),
                mod: cell(0), channel, freqR: 0, waveType: wave }),
            audioRow({ time, freq: cell(base + beat), pan: cell(100), vol: cell(volume),
                mod: cell(0), channel: channelR, freqR: 0, waveType: wave }),
        ];
    }
    const pan = (opts.pan === undefined) ? 0 : opts.pan;
    return [
        audioRow({ time, freq: cell(base), pan: cell(pan), vol: cell(volume),
            mod: cell(0), channel, freqR: base + beat, waveType: wave }),
    ];
}

// ---- Stereo delayed tone ---------------------------------------------------
// Two channels, same carrier, hard-panned; the second entry's time is offset
// later by delayMs (a Haas-style widening).
//   opts: { time, freq, volume, delayMs, channelL, channelR, wave }
export function stereoDelayedTone(opts) {
    opts = opts || {};
    const time = opts.time || 0;
    const freq = (opts.freq === undefined) ? 200 : opts.freq;
    const volume = (opts.volume === undefined) ? 60 : opts.volume;
    const delayMs = (opts.delayMs === undefined) ? 20 : opts.delayMs;
    const channelL = opts.channelL || 1;
    const channelR = opts.channelR || 2;
    const wave = (opts.wave === undefined) ? null : opts.wave;
    return [
        audioRow({ time, freq: cell(freq), pan: cell(-100), vol: cell(volume),
            mod: cell(0), channel: channelL, waveType: wave }),
        audioRow({ time: time + delayMs, freq: cell(freq), pan: cell(100),
            vol: cell(volume), mod: cell(0), channel: channelR, waveType: wave }),
    ];
}

// ---- Harmonic layering -----------------------------------------------------
// One channel per harmonic (octave x2, fifth x1.5, ...), each scaled by
// harmonicVolume^k, capped at NUM_AUDIO_CHANNELS.
//   opts: { time, base, volume, ratios, channels, harmonicVolume, pan, wave }
export function harmonics(opts) {
    opts = opts || {};
    const time = opts.time || 0;
    const base = (opts.base === undefined) ? 200 : opts.base;
    const volume = (opts.volume === undefined) ? 60 : opts.volume;
    const ratios = opts.ratios || [1, 2, 1.5];
    const harmonicVolume = (opts.harmonicVolume === undefined) ? 0.6 : opts.harmonicVolume;
    const pan = (opts.pan === undefined) ? 0 : opts.pan;
    const wave = (opts.wave === undefined) ? null : opts.wave;
    const rows = [];
    for (let i = 0; i < ratios.length; i++) {
        const channel = opts.channels ? opts.channels[i] : (i + 1);
        if (channel > NUM_AUDIO_CHANNELS) break; // cap at 16 channels
        const vol = Math.round(volume * Math.pow(harmonicVolume, i));
        rows.push(audioRow({ time, freq: cell(base * ratios[i]), pan: cell(pan),
            vol: cell(vol), mod: cell(0), channel, waveType: wave }));
    }
    return rows;
}

// ---- Per-tone fades --------------------------------------------------------
// Fade-in: volume ramps 0 -> target over fadeDuration (prefix on the START row,
// animate-on-start). Fade-out: volume ramps from -> 0 into a terminal boundary.
//   opts: { time, duration, target/from, channel, freq, pan, wave, freqR, shape }
export function fadeIn(opts) {
    opts = opts || {};
    const time = opts.time || 0;
    const duration = (opts.duration === undefined) ? 2000 : opts.duration;
    const target = (opts.target === undefined) ? 60 : opts.target;
    const channel = opts.channel || 1;
    const freq = (opts.freq === undefined) ? 200 : opts.freq;
    const pan = (opts.pan === undefined) ? 0 : opts.pan;
    const wave = (opts.wave === undefined) ? null : opts.wave;
    const freqR = opts.freqR || 0;
    const rampInterp = opts.shape === 'quadratic' ? 'quad' : 'lin';
    return [
        audioRow({ time, freq: cell(freq), pan: cell(pan), vol: cell(0, rampInterp),
            mod: cell(0), channel, freqR, waveType: wave }),
        audioRow({ time: time + duration, freq: cell(freq), pan: cell(pan),
            vol: cell(target), mod: cell(0), channel, freqR, waveType: wave }),
    ];
}
export function fadeOut(opts) {
    opts = opts || {};
    const time = opts.time || 0;
    const duration = (opts.duration === undefined) ? 2000 : opts.duration;
    const from = (opts.from === undefined) ? 60 : opts.from;
    const channel = opts.channel || 1;
    const freq = (opts.freq === undefined) ? 200 : opts.freq;
    const pan = (opts.pan === undefined) ? 0 : opts.pan;
    const wave = (opts.wave === undefined) ? null : opts.wave;
    const freqR = opts.freqR || 0;
    const rampInterp = opts.shape === 'quadratic' ? 'quad' : 'lin';
    return [
        audioRow({ time, freq: cell(freq), pan: cell(pan), vol: cell(from, rampInterp),
            mod: cell(0), channel, freqR, waveType: wave }),
        audioRow({ time: time + duration, freq: cell(freq), pan: cell(pan),
            vol: cell(0), mod: cell(0), channel, freqR, waveType: wave }),
    ];
}

// ---- Presets ---------------------------------------------------------------

// Brainwave binaural-beat presets (the "beat" Hz drives R = base + beat).
export const BRAINWAVE_PRESETS = {
    delta2:  { label: 'Delta 2 Hz',  beat: 2,  band: 'delta' },
    theta6:  { label: 'Theta 6 Hz',  beat: 6,  band: 'theta' },
    alpha10: { label: 'Alpha 10 Hz', beat: 10, band: 'alpha' },
    beta18:  { label: 'Beta 18 Hz',  beat: 18, band: 'beta' },
    gamma40: { label: 'Gamma 40 Hz', beat: 40, band: 'gamma' },
};

// LED colour presets (R/G/B 0..255).
export const COLOR_PRESETS = {
    calmBlue:  { label: 'Calm blue',  r: 0,   g: 64,  b: 255 },
    warmAmber: { label: 'Warm amber', r: 255, g: 160, b: 32 },
    softGreen: { label: 'Soft green', r: 32,  g: 220, b: 96 },
    deepRed:   { label: 'Deep red',   r: 200, g: 16,  b: 16 },
    white:     { label: 'White',      r: 255, g: 255, b: 255 },
};

// Noise presets -> wave_type (4 white, 5 pink, 6 brown).
export const NOISE_PRESETS = {
    rain: { label: 'Rain (pink)',  wave: 5 },
    surf: { label: 'Surf (brown)', wave: 6 },
    hiss: { label: 'Hiss (white)', wave: 4 },
};

// Segment + session templates (durations in ms). Layers are wizard-model
// AudioLayer/LightLayer shapes (see wizard_compile.js).
export const SEGMENT_TEMPLATES = {
    intro:  { name: 'Intro',  duration_ms: 60000 },
    deep:   { name: 'Deep',   duration_ms: 600000 },
    wakeup: { name: 'Wake up', duration_ms: 120000 },
};
