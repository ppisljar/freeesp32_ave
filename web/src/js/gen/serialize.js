// Lossless `.led`/`.ledc` serializer.
//
// serialize(doc) -> text. Produces the canonical form that satisfies the
// Bug-avoidance contract by construction:
//   #1 LED channel is a real OR'd 8-bit mask, never a counter (model.mask).
//   #2 pan is emitted ×1 in -100..+100 (never ×127).
//   #3 wave_type emits a freq_r placeholder when the row does not set freq_r
//      (`0` = "mono" for an editor-built row, `-` = "leave it alone" when the
//      source line's freq_r token was absent — see serializeAudio).
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
// against -0 / NaN / ±Infinity which would otherwise corrupt a re-parse.
//
// ±Infinity is reachable from a real file: parse.js keeps whatever parseFloat()
// returned, and an overflowing literal (`A 0 1e400 ...`) overflows to Infinity.
// Writing that back as '0' turned the author's MAXIMUM into a MINIMUM — a
// beyond-Nyquist carrier became DC, a `1e400` volume became silence — which is
// the one direction a numeric guard must never fail in.
//
// We emit the largest finite double rather than clamping to a field range,
// because fmt() is shared by every column and the firmware's ranges differ per
// column (ledc_format.md "Column reference"); there is no single range to clamp
// to here, and the author's literal is already gone by the time we see the row
// (it would take a parse.js change to preserve it verbatim). The largest finite
// double keeps both magnitude and sign, atof() reads it back as a finite
// number, and the direction is right: a maximum stays a maximum.
//
// What the device does with it then varies BY COLUMN, so this is not a promise
// of safety. Audio freq and amp are clamped and handle +inf correctly
// (s_clamp_freq / s_clamp_amp, audio_generator.c:465-476), and LED R/G/B go
// through clamp_u8_field (config_parser.c:1628/:1637) — but LED duty and
// brightness take a bare `(uint8_t)` cast with no clamp at all
// (config_parser.c:1605/:1615), so an out-of-range value there is a
// float->uint8_t conversion, not a clamp. That is pre-existing and unchanged by
// this function: any file literally containing `0 10 50 300 ...` hits the same
// cast. Byte-stable:
// String(Number.MAX_VALUE) re-parses to Number.MAX_VALUE exactly.
// NaN keeps returning '0': it has no sign and no magnitude to preserve.
function fmt(n) {
    if (n === undefined || n === null || Number.isNaN(n)) return '0';
    if (n === 0) return '0'; // normalizes -0
    if (!Number.isFinite(n)) return String(n > 0 ? Number.MAX_VALUE : -Number.MAX_VALUE);
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
                                     cellTok(row.phase), cellTok(row.attack),
                                     jitterStr(row.jitter), cellTok(row.depth)]);
    const hasNew = newTrail.length > 0;

    // Minimal trailing tokens with the positional-placeholder rule. When any new
    // pulse field is present, the carrier optionals (channel/freqR/waveType) must
    // be emitted too so the new fields land at the right positions.
    const emitWave = row.waveType !== null && row.waveType !== undefined; // bug #3/#6
    // A `>`/`*` prefix on freq_r only survives on the parsed cell that rides
    // alongside the plain number (model.js freqRCell), so prefer it when set.
    const freqRRamp = !!(row.freqRCell && row.freqRCell.interp &&
                         row.freqRCell.interp !== 'none');
    // Does this row SET freq_r? For a PARSED row the present bit is
    // authoritative (parse.js:165), because an absent token and an explicit `0`
    // both store freqR = 0 yet mean OPPOSITE things to the engine: a clear
    // AUD_SET_FREQR leaves the live right carrier alone (the guard at
    // config_parser.c:1182) while a literal 0 "collapses the channel back to
    // mono" (the comment at config_parser.c:1181). Deciding from the VALUE
    // alone deleted the explicit `0` that ledc_format.md:794 tells authors to
    // write — "Restate freqR (0 for mono) on any entry that moves an audio
    // carrier" — so a beat the author ENDED kept beating.
    // Rows built by the editors leave freqRPresent undefined; for those the
    // value still decides, so a freqR: 0 editor row keeps serializing without
    // the token (bug #4's minimal-trailing-token rule).
    const freqRSet = (row.freqRPresent === undefined)
        ? ((row.freqR && row.freqR > 0) || freqRRamp)                     // bug #4
        : !!row.freqRPresent;
    const emitFreqR = emitWave || freqRSet || hasNew; // wave_type / new fields force the freq_r slot
    const emitChannel = emitFreqR || (row.channel !== null && row.channel !== undefined);

    // Placeholder for a forced-but-unset freq_r slot. `-` only when the SOURCE
    // line said "leave it alone" (absent token or `-`, both freqRPresent ===
    // false): writing `0` there would silently go mono. An editor-built row has
    // no present bit, and its freqR number is authoritative, so 0 there really
    // does mean mono — which is what contract #3/#6 expect for a noise layer.
    const freqRFill = (row.freqRPresent === false) ? '-' : fmt(0);

    if (emitChannel) parts.push(fmt(row.channel === null || row.channel === undefined ? 0 : row.channel));
    if (emitFreqR)   parts.push(!freqRSet ? freqRFill
                                : (freqRRamp ? cellStr(row.freqRCell) : fmt(row.freqR)));
    if (emitWave)      parts.push(fmt(row.waveType));
    // waveType placeholder so duty aligns. MUST be `-` (= leave the waveform
    // unchanged, config_parser.c:1845 skips the token on a dash), never `0`:
    // `0` is not a placeholder in this column, it is the SINE waveform, so a
    // channel running noise or square silently became a sine tone. Since the
    // format is positional, `-` is the only way to say "change just the duty
    // and keep the timbre" — see ledc_format.md's "One rule, two spellings".
    else if (hasNew)   parts.push('-');

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
    const line = 'S ' + fmt(row.time) + ' ' + voice + ' ' + fmt(row.volume)
        + ' "' + (row.text || '') + '"';
    // Re-attach the inline comment, as serializeLed / serializeAudio /
    // serializeMarker do. A `#` on an `S` line is only a comment AFTER the
    // closing quote — inside the quotes it is spoken text — and parse.js splits
    // it on exactly that boundary, which is the boundary the firmware uses too
    // (it finds the payload with strchr/strrchr on the quotes,
    // config_parser.c:1743-1756, having already cut at '#', :1523). Without
    // this the parser captured the comment and the first save deleted it.
    return row.inlineComment ? line + ' # ' + row.inlineComment : line;
}

/*
 * `M <time> <LABEL> [mod=...] [key=value ...]`.
 *
 * `mod=` is emitted FIRST when present, matching how the device reads it (a
 * reserved key, consumed into `modality` rather than stored alongside the
 * ordinary pairs) and how the examples in docs/sessions/markers.html read.
 */
function serializeMarker(row) {
    const parts = ['M', fmt(row.time), row.label || ''];
    // `modalityRaw` holds a `mod=` value this build does not recognise. The
    // device warns and infers (config_parser.c:174-177) but keeps running, so
    // an unknown modality is a forward-compatibility case, not an error — and
    // dropping the token here deleted the author's text on the first save.
    const modTok = row.modality || row.modalityRaw;
    if (modTok) parts.push('mod=' + modTok);
    for (const p of (row.kv || [])) parts.push(p.k + '=' + p.v);
    // Re-attach the inline comment exactly as serializeLed / serializeAudio do.
    // parse.js:281 carries it into the model, and a marker's `# why` is often
    // the only record of what the event MEANS, so dropping it here deleted it
    // on the first save from every view. Safe on the device: the firmware
    // strips everything from '#' before tokenizing (config_parser.c:1523), so
    // the comment never reaches parse_marker_line's key=value loop.
    let line = parts.join(' ');
    if (row.inlineComment) line += ' # ' + row.inlineComment;
    return line;
}

export function serialize(doc) {
    if (!doc || !doc.rows) return '';

    // `doc.bg` is the AUTHORITATIVE session background, not a mirror to ignore.
    // parse.js:323 fills it from the last BG row (last-wins, exactly like
    // config_parser.c:490), so for a parsed doc it says the same thing the rows
    // do and nothing changes below. But callers OVERRIDE it without touching
    // the rows: play.js:196 serializes `{...doc, bg: {url:'push://session',…}}`
    // so the firmware's prime gate arms (config_parser.c:766 keys the deferred
    // t=0 start off a push:// url) and browser-pushed narration stays
    // sample-aligned to t=0. That override used to be dropped on the floor —
    // the device got the file's original BG, or none, and the speech drifted.
    //
    // Honour it by writing it into the LAST bg row rather than adding a line,
    // so there is never a duplicate BG (harmless to the firmware, which is
    // last-wins, but it would show up in every save and download). A url-less
    // descriptor is ignored: `BG  0 0` would not even re-parse (parse.js:208).
    const override = (doc.bg && doc.bg.url) ? doc.bg : null;
    let lastBg = -1;
    if (override) {
        for (let i = 0; i < doc.rows.length; i++) if (doc.rows[i].kind === 'bg') lastBg = i;
    }

    const out = [];
    for (let i = 0; i < doc.rows.length; i++) {
        const row = doc.rows[i];
        switch (row.kind) {
            case 'blank':   out.push(''); break;
            case 'comment': out.push(row.text); break;
            case 'raw':     out.push(row.text); break;
            case 'led':     out.push(serializeLed(row)); break;
            case 'audio':   out.push(serializeAudio(row)); break;
            case 'bg':      out.push(serializeBg(i === lastBg ? override : row.bg)); break;
            case 'speech':  out.push(serializeSpeech(row)); break;
            case 'marker':  out.push(serializeMarker(row)); break;
            default: break;
        }
    }
    // An override on a doc with no BG row of its own needs a line of its own.
    // Appended, not prepended: the firmware reads BG from anywhere in the file
    // (it is session-level, consumed after the whole parse), while prepending
    // would shift every line number the text view maps diagnostics onto.
    if (override && lastBg < 0) out.push(serializeBg(override));

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
