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
import { mod } from './field.js';

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

// ---- Monaural --------------------------------------------------------------
// Companion to binaural(): TWO centre-panned channels (pan=0) at `base` and
// `base+beat`. The Δf beat is produced acoustically in the air (the two tones
// sum before they reach the ear), so — unlike binaural, which needs stereo
// separation — a monaural beat works on a single speaker or one ear.
//   opts: { time, base, beat, volume, wave, channel, channelR }
export function monaural(opts) {
    opts = opts || {};
    const time = opts.time || 0;
    const base = (opts.base === undefined) ? 200 : opts.base;
    const beat = (opts.beat === undefined) ? 10 : opts.beat;
    const volume = (opts.volume === undefined) ? 60 : opts.volume;
    const wave = (opts.wave === undefined) ? null : opts.wave;
    const channel = opts.channel || 1;
    const channelR = opts.channelR || 2;
    return [
        audioRow({ time, freq: cell(base), pan: cell(0), vol: cell(volume),
            mod: cell(0), channel, freqR: 0, waveType: wave }),
        audioRow({ time, freq: cell(base + beat), pan: cell(0), vol: cell(volume),
            mod: cell(0), channel: channelR, freqR: 0, waveType: wave }),
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

// ---- Harmonic / octave stack (A5) ------------------------------------------
// N channels sharing ONE beat Δf, each on an octave carrier (100/106, 200/206,
// 400/406 @ Δf6). Upper octaves are attenuated (−3…−6 dB / octave) and the whole
// stack is 1/N auto-scaled so the summed amplitude stays inside headroom (no
// clipping). `harmonicCarriers` is the pure table (shared with the wizard's
// segment builder); `harmonicStack` emits the model rows.
//   opts: { base, beat, count, volume, attenuationDb, subharmonic }
// count = number of octaves (k=0..count-1). subharmonic=true prepends a 20 Hz
// layer (boosts the 40 Hz gamma FFR).
export function harmonicCarriers(opts) {
    opts = opts || {};
    const base = (opts.base === undefined) ? 100 : opts.base;
    const beat = (opts.beat === undefined) ? 6 : opts.beat;
    const count = (opts.count === undefined) ? 3 : opts.count;
    const volume = (opts.volume === undefined) ? 60 : opts.volume;
    // Mid of the −3…−6 dB/octave research range.
    const attenuationDb = (opts.attenuationDb === undefined) ? 4.5 : opts.attenuationDb;
    const list = [];
    if (opts.subharmonic) list.push({ carrier: 20, oct: 0 }); // full-weight sub layer
    for (let k = 0; k < count; k++) list.push({ carrier: base * Math.pow(2, k), oct: k });
    const n = list.length || 1;
    return list.map((it) => {
        const octDb = -attenuationDb * it.oct;           // 0 for base + sub, −n·dB above
        const gain = Math.pow(10, octDb / 20) / n;       // per-octave roll-off × 1/N
        return {
            carrier: it.carrier,
            beat,
            freqR: it.carrier + beat,                    // shared Δf as a mono binaural
            volume: Math.max(1, Math.round(volume * gain)),
        };
    });
}
// Emit the octave stack as mono-binaural audio rows (freq_r = carrier + beat).
//   opts: harmonicCarriers opts + { time, wave, channels }
export function harmonicStack(opts) {
    opts = opts || {};
    const time = opts.time || 0;
    const wave = (opts.wave === undefined) ? null : opts.wave;
    const carriers = harmonicCarriers(opts);
    const rows = [];
    for (let i = 0; i < carriers.length; i++) {
        const c = carriers[i];
        const channel = opts.channels ? opts.channels[i] : (i + 1);
        if (channel > NUM_AUDIO_CHANNELS) break; // cap at 16 channels
        rows.push(audioRow({ time, freq: cell(c.carrier), pan: cell(0), vol: cell(c.volume),
            mod: cell(0), channel, freqR: c.freqR, waveType: wave }));
    }
    return rows;
}

// ---- Breath LFO (A8) + rotating pan ----------------------------------------
// Coherence-breathing pacer: a slow sine swing on brightness (LED) or volume
// (audio). 6 breaths/min = 0.1 Hz = 10 000 ms is the default. Returns a field.js
// `mod` (end = high); the caller sets the field VALUE to `low` (the swing start).
export function breathPeriodMs(bpm) {
    return Math.round(60000 / (bpm || 6));
}
export function breathMod(opts) {
    opts = opts || {};
    const bpm = (opts.bpm === undefined) ? 6 : opts.bpm;
    const high = (opts.high === undefined) ? 100 : opts.high;
    return mod('sine', high, breathPeriodMs(bpm));
}
// Rotating pan / inter-aural drift: a sine LFO that circles the pan −left↔+right
// over 8–30 s (default 15 s). Returns a field.js `mod` (end = right); caller sets
// the pan field VALUE to `left` (default −100).
export function rotatingPanMod(opts) {
    opts = opts || {};
    const periodMs = (opts.periodMs === undefined) ? 15000 : opts.periodMs;
    const right = (opts.right === undefined) ? 100 : opts.right;
    return mod('sine', right, periodMs);
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

// Brainwave binaural-beat presets. `beat` Hz drives R = carrier + beat; `carrier`
// is the research-backed base tone per band (A4): beat perception peaks near a
// ~400 Hz carrier (Δf ≤ ~35 Hz), and ~340 Hz is best for the 40 Hz gamma beat.
// Keep carrier ≤ ~400 Hz and beat ≤ ~35 Hz for strong perception.
export const BRAINWAVE_PRESETS = {
    delta2:  { label: 'Delta 2 Hz',  beat: 2,  carrier: 200, band: 'delta' },
    theta6:  { label: 'Theta 6 Hz',  beat: 6,  carrier: 250, band: 'theta' },
    alpha10: { label: 'Alpha 10 Hz', beat: 10, carrier: 370, band: 'alpha' },
    beta18:  { label: 'Beta 18 Hz',  beat: 18, carrier: 420, band: 'beta' },
    gamma40: { label: 'Gamma 40 Hz', beat: 40, carrier: 340, band: 'gamma' },
};

// LED colour presets (R/G/B 0..255), ordered by SSVEP drive strength (B8): at
// matched luminance, spectral extremes drive far stronger responses — amber/red
// ~8.06 dB, blue/cyan ~6.82 dB, green/lime only ~2.85 dB. Prefer amber/red/blue
// for entrainment drive; green is kept for ambience but is a weak driver.
export const COLOR_PRESETS = {
    warmAmber: { label: 'Warm amber', r: 255, g: 160, b: 32,  ssvep: 8.06 },
    deepRed:   { label: 'Deep red',   r: 255, g: 24,  b: 16,  ssvep: 8.06 },
    calmBlue:  { label: 'Calm blue',  r: 32,  g: 96,  b: 255, ssvep: 6.82 },
    coolCyan:  { label: 'Cool cyan',  r: 32,  g: 220, b: 255, ssvep: 6.82 },
    white:     { label: 'White',      r: 255, g: 255, b: 255 },
    softGreen: { label: 'Soft green (weak drive)', r: 32, g: 220, b: 96, ssvep: 2.85 },
};

// Luminance-complementary invisible-flicker pairs (B1/B8): two colour banks run
// 180° antiphase so mean luminance stays ~flat (no visible flicker) while the
// retina still gets the oscillation. amber↔blue is both the strongest SSVEP pair
// AND luminance-complementary; red↔cyan is the runner-up. Values reference the
// COLOR_PRESETS keys above.
export const FLICKER_PAIRS = {
    amberBlue: { label: 'Amber ↔ Blue', a: 'warmAmber', b: 'calmBlue' },
    redCyan:   { label: 'Red ↔ Cyan',   a: 'deepRed',   b: 'coolCyan' },
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
