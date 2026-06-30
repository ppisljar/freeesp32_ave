// Lossless `.led`/`.ledc` serializer.
//
// serialize(doc) -> text. Produces the canonical form that satisfies the
// Bug-avoidance contract by construction:
//   #1 LED channel is a real OR'd 8-bit mask, never a counter (model.mask).
//   #2 pan is emitted ×1 in -100..+100 (never ×127).
//   #3 wave_type emits a `0` placeholder in the freq_r slot when freq_r==0.
//   #4 freq_r emitted for single-channel binaural.
//   #5 LED lines always 8-field with R G B (unless an untouched legacy import).
//   #6 noise = audio with wave_type 4/5/6 (no carrier dependency).
//   #7 every prefix (> * ^ ~ / \ _) supported; mods emit explicit end+period.
//
// Canonical whitespace is a single space between tokens; comments re-attached.
// Round-trip is exact on the canonical form: serialize(parse(serialize(x))) ===
// serialize(parse(x)); against an aligned source it matches modulo whitespace.

import { INTERP_GLYPHS, isModInterp, DEFAULT_MOD_PERIOD_MS } from './model.js';

// Format a number so String() round-trips it (shortest representation). Guards
// against -0 / NaN / Infinity which would otherwise corrupt a re-parse.
function fmt(n) {
    if (n === undefined || n === null || !Number.isFinite(n)) return '0';
    if (n === 0) return '0'; // normalizes -0
    return String(n);
}

// Render a compound Cell to its prefixed token.
//   none → bare value
//   lin  → '>value'      quad → '*value'
//   periodic → '<glyph>start:end:period'  (always explicit end + period)
export function cellStr(c) {
    if (!c) return '0';
    const glyph = INTERP_GLYPHS[c.interp] || '';
    if (c.interp === 'none') return fmt(c.value);
    if (!isModInterp(c.interp)) return glyph + fmt(c.value); // lin / quad ramp
    // Periodic modulation — emit explicit end + period (bug #7).
    const end = (c.modEnd === undefined || c.modEnd === null) ? c.value : c.modEnd;
    const period = (c.modPeriodMs === undefined || c.modPeriodMs === null)
        ? DEFAULT_MOD_PERIOD_MS : c.modPeriodMs;
    return glyph + fmt(c.value) + ':' + fmt(end) + ':' + fmt(period);
}

function serializeLed(row) {
    const parts = [fmt(row.time), cellStr(row.freq), cellStr(row.duty), cellStr(row.bright)];
    if (!row.legacy5) {
        // 8-field canonical (bug #5): always emit R G B.
        parts.push(cellStr(row.r), cellStr(row.g), cellStr(row.b));
    }
    parts.push(fmt(row.mask)); // real OR'd mask (bug #1)
    let line = parts.join(' ');
    if (row.inlineComment) line += ' # ' + row.inlineComment;
    return line;
}

function serializeAudio(row) {
    // pan emitted ×1 in -100..+100 (bug #2) — the model already stores it that way.
    const parts = ['A', fmt(row.time), cellStr(row.freq), cellStr(row.pan),
                   cellStr(row.vol), cellStr(row.mod)];

    // Minimal trailing tokens with the positional-placeholder rule.
    const emitWave = row.waveType !== null && row.waveType !== undefined; // bug #3/#6
    const freqRPos = (row.freqR && row.freqR > 0);                        // bug #4
    const emitFreqR = emitWave || freqRPos; // wave_type forces the freq_r slot
    const emitChannel = emitFreqR || (row.channel !== null && row.channel !== undefined);

    if (emitChannel) parts.push(fmt(row.channel === null || row.channel === undefined ? 0 : row.channel));
    if (emitFreqR)   parts.push(fmt(freqRPos ? row.freqR : 0)); // 0 placeholder when wave but no freq_r
    if (emitWave)    parts.push(fmt(row.waveType));

    let line = parts.join(' ');
    if (row.inlineComment) line += ' # ' + row.inlineComment;
    return line;
}

function serializeBg(b) {
    // pan / loudness held as raw -100..100 / 0..100 tokens.
    return 'BG ' + b.url + ' ' + fmt(b.pan) + ' ' + fmt(b.loudness);
}

export function serialize(doc) {
    if (!doc || !doc.rows) return '';
    const out = [];
    for (const row of doc.rows) {
        switch (row.kind) {
            case 'blank':   out.push(''); break;
            case 'comment': out.push(row.text); break;
            case 'raw':     out.push(row.text); break;
            case 'led':     out.push(serializeLed(row)); break;
            case 'audio':   out.push(serializeAudio(row)); break;
            case 'bg':      out.push(serializeBg(row.bg)); break;
            default: break;
        }
    }
    // Join with newlines; add a trailing newline so files end cleanly (and so
    // parse(serialize(x)) does not see the last line glued to nothing).
    return out.join('\n') + '\n';
}
