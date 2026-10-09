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
    if (c === null) return '-';   // `-` sentinel = leave unchanged (format v2)
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

// --- format v2 pulse-field rendering (undefined = absent, null = `-`) -------
// Render one field to a token string, or undefined if absent.
function envStr(e)  { return e === undefined ? undefined : (e === null ? '-' : fmt(e)); }
function cellTok(c) { return c === undefined ? undefined : cellStr(c); } // cellStr(null)='-'
function jitterStr(j) {
    if (j === undefined) return undefined;
    if (j === null) return '-';
    return (j.period === undefined || j.period === null)
        ? fmt(j.amp) : (fmt(j.amp) + ':' + fmt(j.period));
}
// Given rendered optional fields (undefined | string), drop trailing absent ones;
// absent-in-the-middle becomes `-` so positions stay aligned.
function trailingTokens(rendered) {
    let last = -1;
    for (let i = 0; i < rendered.length; i++) if (rendered[i] !== undefined) last = i;
    if (last < 0) return [];
    const out = [];
    for (let i = 0; i <= last; i++) out.push(rendered[i] === undefined ? '-' : rendered[i]);
    return out;
}

function serializeLed(row) {
    const parts = [fmt(row.time), cellStr(row.freq), cellStr(row.duty), cellStr(row.bright)];
    if (!row.legacy5) {
        // 8-field canonical (bug #5): always emit R G B.
        parts.push(cellStr(row.r), cellStr(row.g), cellStr(row.b));
    }
    parts.push(row.mask === null ? '-' : fmt(row.mask)); // real OR'd mask (bug #1)
    // NEW pulse fields (canonical lines only): env phase attack jitter.
    if (!row.legacy5) {
        parts.push(...trailingTokens([envStr(row.env), cellTok(row.phase),
                                      cellTok(row.attack), jitterStr(row.jitter)]));
    }
    let line = parts.join(' ');
    if (row.inlineComment) line += ' # ' + row.inlineComment;
    return line;
}

function serializeAudio(row) {
    // pan emitted ×1 in -100..+100 (bug #2) — the model already stores it that way.
    const parts = ['A', fmt(row.time), cellStr(row.freq), cellStr(row.pan),
                   cellStr(row.vol), cellStr(row.mod)];

    // NEW pulse fields (format v2): duty env phase attack jitter.
    const newTrail = trailingTokens([cellTok(row.duty), envStr(row.env),
                                     cellTok(row.phase), cellTok(row.attack), jitterStr(row.jitter)]);
    const hasNew = newTrail.length > 0;

    // Minimal trailing tokens with the positional-placeholder rule. When any new
    // pulse field is present, the carrier optionals (channel/freqR/waveType) must
    // be emitted too so the new fields land at the right positions.
    const emitWave = row.waveType !== null && row.waveType !== undefined; // bug #3/#6
    const freqRPos = (row.freqR && row.freqR > 0);                        // bug #4
    const emitFreqR = emitWave || freqRPos || hasNew; // wave_type / new fields force the freq_r slot
    const emitChannel = emitFreqR || (row.channel !== null && row.channel !== undefined);

    if (emitChannel) parts.push(fmt(row.channel === null || row.channel === undefined ? 0 : row.channel));
    if (emitFreqR)   parts.push(fmt(freqRPos ? row.freqR : 0)); // 0 placeholder when wave but no freq_r
    if (emitWave)      parts.push(fmt(row.waveType));
    else if (hasNew)   parts.push(fmt(0)); // waveType placeholder (sine) so duty aligns

    parts.push(...newTrail);

    let line = parts.join(' ');
    if (row.inlineComment) line += ' # ' + row.inlineComment;
    return line;
}

function serializeBg(b) {
    // pan / loudness held as raw -100..100 / 0..100 tokens.
    return 'BG ' + b.url + ' ' + fmt(b.pan) + ' ' + fmt(b.loudness);
}

// `S <time> <voice> <volume> "<text>"`. The text is quoted so it may contain
// spaces; embedded double-quotes survive a round-trip because the parser takes
// everything between the first and last quote. Browser-only (filtered before
// the device — see serializeForDevice).
function serializeSpeech(row) {
    const voice = (row.voice && String(row.voice).trim()) ? String(row.voice).trim() : 'default';
    return 'S ' + fmt(row.time) + ' ' + voice + ' ' + fmt(row.volume) + ' "' + (row.text || '') + '"';
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
            case 'speech':  out.push(serializeSpeech(row)); break;
            default: break;
        }
    }
    // Join with newlines; add a trailing newline so files end cleanly (and so
    // parse(serialize(x)) does not see the last line glued to nothing).
    return out.join('\n') + '\n';
}

// Serialize for the DEVICE.
//
// `S` rows are dropped by DEFAULT because historically the firmware could not
// parse them: anything that was not an `A` line fell through to the LED parser
// and an S row was silently mangled into an LED command. Speech was therefore
// browser-only, mixed into the bounced WAV.
//
// With an SD card the firmware CAN play speech (it resolves each phrase to a
// pre-uploaded file), so pass { keepSpeech: true } when every phrase this
// session needs is already on the card — see gen/sdsync.js sessionIsOffline().
// Getting that wrong in the "keep" direction is harmless on current firmware
// (unknown rows are rejected with a parse warning, not executed), but getting
// it wrong in the "drop" direction just means silent narration.
export function serializeForDevice(doc, { keepSpeech = false } = {}) {
    if (!doc || !doc.rows) return '';
    const rows = keepSpeech ? doc.rows : doc.rows.filter(r => r.kind !== 'speech');
    return serialize({ rows, bg: doc.bg });
}
