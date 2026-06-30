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
// 0 sine, 1 square, 2 triangle, 3 sawtooth, 4 white, 5 pink, 6 brown.
export const WAVE_TYPES = ['sine', 'square', 'triangle', 'sawtooth', 'white', 'pink', 'brown'];
export const WAVE_COUNT = WAVE_TYPES.length; // 7 (AUDIO_WAVE_COUNT)
export const NOISE_WAVE_TYPES = [4, 5, 6];   // white / pink / brown

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
export function ledRow(opts) {
    opts = opts || {};
    return {
        kind: 'led',
        time: opts.time || 0,
        freq:   opts.freq   || cell(0),
        duty:   opts.duty   || cell(0),
        bright: opts.bright || cell(0),
        r:      opts.r      || cell(255),
        g:      opts.g      || cell(255),
        b:      opts.b      || cell(255),
        mask:   (opts.mask === undefined) ? 1 : opts.mask, // 1..255
        legacy5: !!opts.legacy5, // true only if imported as 5-field & unedited
        inlineComment: opts.inlineComment || '',
    };
}
export function audioRow(opts) {
    opts = opts || {};
    return {
        kind: 'audio',
        time: opts.time || 0,
        freq: opts.freq || cell(0),
        pan:  opts.pan  || cell(0),
        vol:  opts.vol  || cell(0),
        mod:  opts.mod  || cell(0),
        channel:  (opts.channel  === undefined) ? null : opts.channel,  // 1..16; null => omit token
        freqR:    (opts.freqR    === undefined) ? 0    : opts.freqR,    // 0 => mono / omit token
        waveType: (opts.waveType === undefined) ? null : opts.waveType, // 0..6; null => omit token
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
