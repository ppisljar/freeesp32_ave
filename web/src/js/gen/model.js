// Generator shared core model.
//
// One in-memory representation of a `.led`/`.ledc` timeline that round-trips
// losslessly to text via gen/parse.js + gen/serialize.js. Every Generator view
// (Text/Table/Lane/Wizard, later phases) is a projection of this single model.
//
// The structures here mirror the AUTHORITATIVE firmware grammar in
// main/config_parser.c. When anything is ambiguous, the C parser wins.

// ---- Engine constants (mirror the firmware) --------------------------------
export const NUM_AUDIO_CHANNELS = 16; // config_audio_entry channel range / apply_patch reject >= 16
export const NUM_LED_CHANNELS   = 8;  // led channel_mask is a uint8 (bits 0..7)
export const MAX_ENTRIES        = 100; // CONFIG_PARSER_MAX_ENTRIES
export const MAX_BATCH_SIZE     = 50;  // same-timestamp batch cap
export const SAMPLE_RATE        = 44100; // AUDIO_GEN_SAMPLE_RATE
export const NYQUIST            = SAMPLE_RATE / 2; // 22050

// Waveform types — index = the integer emitted in the 8th audio token.
// 0 sine, 1 square, 2 triangle, 3 sawtooth, 4 white, 5 pink, 6 brown, 7 eeg.
export const WAVE_TYPES = ['sine', 'square', 'triangle', 'sawtooth', 'white', 'pink', 'brown', 'eeg'];
export const WAVE_COUNT = WAVE_TYPES.length; // 8 (AUDIO_WAVE_COUNT)
export const NOISE_WAVE_TYPES = [4, 5, 6];   // white / pink / brown (eeg=7 is a carrier, NOT noise)

// Interpolation kinds used inside a Cell.
//   none → bare step value
//   lin  → '>' linear ramp (target = next same-channel/field entry)
//   quad → '*' quadratic ease (target = next same-channel/field entry)
//   tri/sine/sawup/sawdn/sq → self-contained periodic modulation
//                              (carry modEnd + modPeriodMs)
export const INTERP_KINDS = ['none', 'lin', 'quad', 'tri', 'sine', 'sawup', 'sawdn', 'sq'];

// Periodic-modulation kinds (the ones that carry modEnd + modPeriodMs).
export const MOD_INTERP_KINDS = ['tri', 'sine', 'sawup', 'sawdn', 'sq'];

// interp kind <-> firmware prefix glyph. 'none' has no glyph.
export const INTERP_GLYPHS = {
    none:  '',
    lin:   '>',
    quad:  '*',
    tri:   '^',
    sine:  '~',
    sawup: '/',
    sawdn: '\\',
    sq:    '_',
};

// glyph -> interp kind (reverse of INTERP_GLYPHS, excluding 'none').
export const GLYPH_TO_INTERP = {
    '>':  'lin',
    '*':  'quad',
    '^':  'tri',
    '~':  'sine',
    '/':  'sawup',
    '\\': 'sawdn',
    '_':  'sq',
};

// Default modulation period (ms) when a `:period` token is omitted — matches
// parse_mod_extras() in config_parser.c.
export const DEFAULT_MOD_PERIOD_MS = 1000;

// ---- Predicates ------------------------------------------------------------

// True iff the interp kind is a periodic modulation (vs. step/ramp).
export function isModInterp(interp) {
    return MOD_INTERP_KINDS.indexOf(interp) >= 0;
}

// ---- Constructors ----------------------------------------------------------

// One compound "value + interpolation" cell. Used for every interpolatable
// field (LED freq/duty/bright/R/G/B and audio freq/pan/vol/mod).
//   value       step value, ramp start, or modulation start
//   interp      one of INTERP_KINDS
//   modEnd      periodic mods only — the other extreme of the oscillation
//   modPeriodMs periodic mods only — full-cycle time in ms
export function cell(value, interp, modEnd, modPeriodMs) {
    return {
        value: (value === undefined || value === null) ? 0 : value,
        interp: interp || 'none',
        modEnd: (modEnd === undefined) ? null : modEnd,
        modPeriodMs: (modPeriodMs === undefined) ? null : modPeriodMs,
    };
}

// Row constructors. Each row is discriminated by .kind.
export function blankRow() {
    return { kind: 'blank' };
}
export function commentRow(text) {
    return { kind: 'comment', text: text || '' };
}
export function rawRow(text, error) {
    return { kind: 'raw', text: text || '', error: error || '' };
}
// A `BG <url> <pan> <loudness>` line. Kept as its own row so its source
// position survives a round-trip; doc.bg mirrors the LAST bg row (last-wins,
// matching the firmware) for consumers that just need the active descriptor.
export function bgRow(bgData) {
    return { kind: 'bg', bg: bgData };
}
// A `S <time> <voice> <volume> "<text>"` speech line (bg_browser_push_plan.md).
// Browser-only: TTS is synthesized in the browser and mixed into the bounced
// WAV; these rows are FILTERED OUT before anything is sent to the device (the
// firmware cannot parse `S`). Kept in the model + serialized so a .ledc that
// contains speech round-trips losslessly in the editor.
export function speechRow(opts) {
    opts = opts || {};
    return {
        kind: 'speech',
        time: opts.time || 0,
        voice: (opts.voice === undefined || opts.voice === null) ? 'default' : opts.voice,
        volume: (opts.volume === undefined || opts.volume === null) ? 80 : opts.volume, // 0..100
        text: opts.text || '',
    };
}
export function ledRow(opts) {
    opts = opts || {};
    return {
        kind: 'led',
        time: opts.time || 0,
        // `=== undefined` (not `||`) so an explicit null (`-` = leave unchanged) survives.
        freq:   (opts.freq   === undefined) ? cell(0)   : opts.freq,
        duty:   (opts.duty   === undefined) ? cell(0)   : opts.duty,
        bright: (opts.bright === undefined) ? cell(0)   : opts.bright,
        r:      (opts.r      === undefined) ? cell(255) : opts.r,
        g:      (opts.g      === undefined) ? cell(255) : opts.g,
        b:      (opts.b      === undefined) ? cell(255) : opts.b,
        mask:   (opts.mask === undefined) ? 1 : opts.mask, // 1..255
        // NEW pulse fields (format v2). undefined = absent (omit, use default);
        // null = `-` (leave unchanged). env: enum int; phase/attack: compound cells;
        // jitter: {amp, period} | number. See ledc_format.md.
        env:    opts.env,      // 0 square 1 sine 2 tri 3 trapezoid 4 tremolo; LED default 0
        phase:  opts.phase,    // compound cell, degrees
        attack: opts.attack,   // compound cell, ms
        jitter: opts.jitter,   // {amp, period} | number(amp) | null(-) | undefined(off)
        legacy5: !!opts.legacy5, // true only if imported as 5-field & unedited
        inlineComment: opts.inlineComment || '',
    };
}
export function audioRow(opts) {
    opts = opts || {};
    return {
        kind: 'audio',
        time: opts.time || 0,
        // `=== undefined` (not `||`) so an explicit null (`-` = leave unchanged) survives.
        freq: (opts.freq === undefined) ? cell(0) : opts.freq,
        pan:  (opts.pan  === undefined) ? cell(0) : opts.pan,
        vol:  (opts.vol  === undefined) ? cell(0) : opts.vol,
        mod:  (opts.mod  === undefined) ? cell(0) : opts.mod,
        channel:  (opts.channel  === undefined) ? null : opts.channel,  // 1..16; null => omit token
        freqR:    (opts.freqR    === undefined) ? 0    : opts.freqR,    // 0 => mono / omit token
        waveType: (opts.waveType === undefined) ? null : opts.waveType, // 0..6; null => omit token
        // NEW pulse fields (format v2) — same conventions as ledRow. audio env default 4.
        duty:   opts.duty,     // compound cell, %
        env:    opts.env,      // 0..4; audio default 4 (bipolar sine tremolo = legacy)
        phase:  opts.phase,    // compound cell, degrees
        attack: opts.attack,   // compound cell, ms
        jitter: opts.jitter,   // {amp, period} | number | null | undefined
        inlineComment: opts.inlineComment || '',
    };
}

// Session-level background descriptor. Stored on the doc, not as a timeline
// row. pan/loudness held as the RAW line tokens (pan -100..100, loudness
// 0..100) so a round-trip re-emits the same numbers the firmware divides /100.
export function bg(url, pan, loudness) {
    return {
        url: url || '',
        pan: (pan === undefined) ? 0 : pan,
        loudness: (loudness === undefined) ? 0 : loudness,
    };
}

// Empty document: an ordered list of rows + a single optional BG descriptor.
export function emptyDoc() {
    return { rows: [], bg: null };
}
