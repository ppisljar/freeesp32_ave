// Lossless `.led`/`.ledc` parser.
//
// A faithful mirror of main/config_parser.c (parse_line / parse_led_line /
// parse_audio_line / parse_bg_line / parse_value_with_interpolation /
// parse_mod_extras), EXTENDED to capture everything the firmware understands so
// the model round-trips: the 5 modulation prefixes with start:end:period_ms,
// audio freq_r (token 7) and wave_type (token 8), channel-less audio lines,
// BG lines, per-channel RGB interp, mask==0 rejection, and source order /
// comments / blank lines.
//
// parse(text) -> { doc, diagnostics: [{ line, severity, msg }] }
// Unparsable content lines become `raw` rows (held verbatim + flagged) so the
// rest of the file still round-trips.

import {
    emptyDoc, cell, blankRow, commentRow, rawRow, ledRow, audioRow, bgRow, bg,
    speechRow,
    GLYPH_TO_INTERP, isModInterp, DEFAULT_MOD_PERIOD_MS, NYQUIST, WAVE_COUNT,
} from './model.js';

// Parse one numeric token that may carry an interpolation prefix.
// Mirrors parse_value_with_interpolation + parse_mod_extras. Returns a Cell.
//   >v / *v            → ramp (value = atof(rest))
//   ^/~///\/_ start:end:period → periodic mod (value=start, modEnd, modPeriodMs)
//   v                  → bare step
// parseFloat() stops at ':' exactly like atof(), so value = the start number.
export function parseCell(token) {
    if (token === undefined || token === null || token === '') {
        return cell(0);
    }
    if (token === '-') return null;   // `-` sentinel = leave unchanged
    const ch = token[0];
    const interp = GLYPH_TO_INTERP[ch];
    if (!interp) {
        // No prefix → bare step value.
        return cell(parseFloat(token) || 0);
    }
    const body = token.slice(1);
    const value = parseFloat(body) || 0;
    if (!isModInterp(interp)) {
        // Ramp (linear / quadratic): no end/period.
        return cell(value, interp);
    }
    // Periodic modulation: extract end + period (parse_mod_extras semantics).
    let modEnd = value;            // default end = start (degenerate)
    let modPeriodMs = DEFAULT_MOD_PERIOD_MS;
    const c1 = body.indexOf(':');
    if (c1 >= 0) {
        const afterEnd = body.slice(c1 + 1);
        modEnd = parseFloat(afterEnd) || 0;
        const c2 = afterEnd.indexOf(':');
        if (c2 >= 0) {
            modPeriodMs = parseFloat(afterEnd.slice(c2 + 1)) || 0;
        }
    }
    return cell(value, interp, modEnd, modPeriodMs);
}

// Split a content line into its field text and a trailing inline comment.
// Mirrors config_parser.c: everything from the first '#' to EOL is comment.
// Returns { fields, comment } where comment is the trimmed text after '#'
// (without the '#'), or '' if none.
function splitInlineComment(line) {
    const h = line.indexOf('#');
    if (h < 0) return { fields: line, comment: '' };
    return { fields: line.slice(0, h), comment: line.slice(h + 1).trim() };
}

function tokenize(s) {
    const t = s.trim();
    if (t === '') return [];
    return t.split(/[ \t]+/);
}

// --- Optional / sentinel-aware field helpers (format v2) --------------------
// undefined = token absent (field omitted); null = `-` (leave unchanged).
function tokAt(tokens, i) { return i < tokens.length ? tokens[i] : undefined; }
function optCell(tokens, i) { const t = tokAt(tokens, i); return t === undefined ? undefined : parseCell(t); }
function optInt(tokens, i) {
    const t = tokAt(tokens, i);
    if (t === undefined) return undefined;
    if (t === '-') return null;
    const n = parseInt(t, 10);
    return Number.isFinite(n) ? n : 0;
}
function parseMask(t) { return t === '-' ? null : (parseInt(t, 10) || 0); }
function optJitter(tokens, i) {
    const t = tokAt(tokens, i);
    if (t === undefined) return undefined;
    if (t === '-') return null;
    const parts = String(t).split(':');
    const amp = parseFloat(parts[0]) || 0;
    const period = parts.length > 1 ? (parseFloat(parts[1]) || 0) : undefined;
    return { amp, period };
}

// Parse the tokens of an LED line: 5 (legacy) or 8..12 (canonical + optional
// env/phase/attack/jitter). Mirrors parse_led_line. Returns { row } or { error }.
function parseLed(tokens, comment) {
    if (tokens.length !== 5 && (tokens.length < 8 || tokens.length > 12)) {
        return { error: 'LED line has ' + tokens.length +
                 ' tokens; expected 5 (legacy) or 8..12 (time freq duty bright R G B mask [env phase attack jitter])' };
    }
    const time = parseInt(tokens[0], 10) || 0;
    const freq = parseCell(tokens[1]);
    const duty = parseCell(tokens[2]);
    const bright = parseCell(tokens[3]);
    let r, g, b, mask, legacy5;
    if (tokens.length >= 8) {
        r = parseCell(tokens[4]);
        g = parseCell(tokens[5]);
        b = parseCell(tokens[6]);
        mask = parseMask(tokens[7]);
        legacy5 = false;
    } else {
        // Legacy 5-field: default RGB = full white, no color interp.
        r = cell(255); g = cell(255); b = cell(255);
        mask = parseMask(tokens[4]);
        legacy5 = true;
    }
    // mask == 0 is rejected by the firmware (null = `-` unchanged is allowed).
    if (mask === 0) {
        return { error: 'LED line has channel_mask=0 (rejected by firmware)' };
    }
    // Optional pulse fields (canonical lines only): env phase attack jitter.
    const env    = optInt(tokens, 8);
    const phase  = optCell(tokens, 9);
    const attack = optCell(tokens, 10);
    const jitter = optJitter(tokens, 11);
    return { row: ledRow({ time, freq, duty, bright, r, g, b, mask, legacy5,
                           env, phase, attack, jitter, inlineComment: comment }) };
}

// Parse the tokens AFTER the leading 'A' of an audio line: 5..13 tokens.
// Mirrors parse_audio_line. Returns { row } or { error }.
function parseAudio(tokens, comment) {
    if (tokens.length < 5) {
        return { error: 'Audio line needs at least 5 tokens (time freq pan volume mod)' };
    }
    const time = parseInt(tokens[0], 10) || 0;
    const freq = parseCell(tokens[1]);
    const pan = parseCell(tokens[2]);
    const vol = parseCell(tokens[3]);
    const mod = parseCell(tokens[4]);

    // channel (token 6): present → literal int; absent / `-` → null (firmware default).
    let channel = null;
    if (tokens.length >= 6 && tokens[5] !== '-') channel = parseInt(tokens[5], 10) || 0;

    // freq_r (token 7): present → atof, validated (>0 && <= Nyquist) else 0; `-` → 0.
    let freqR = 0;
    if (tokens.length >= 7 && tokens[6] !== '-') {
        const fr = parseFloat(tokens[6]) || 0;
        freqR = (fr > 0 && fr <= NYQUIST) ? fr : 0;
    }

    // wave_type (token 8): present → int 0..6 (else 0); absent / `-` → null.
    let waveType = null;
    if (tokens.length >= 8 && tokens[7] !== '-') {
        const wt = parseInt(tokens[7], 10);
        waveType = (Number.isFinite(wt) && wt >= 0 && wt < WAVE_COUNT) ? wt : 0;
    }

    // Optional pulse fields (format v2): duty env phase attack jitter.
    const duty   = optCell(tokens, 8);
    const env    = optInt(tokens, 9);
    const phase  = optCell(tokens, 10);
    const attack = optCell(tokens, 11);
    const jitter = optJitter(tokens, 12);

    return { row: audioRow({ time, freq, pan, vol, mod, channel, freqR, waveType,
                             duty, env, phase, attack, jitter, inlineComment: comment }) };
}

// Parse the tokens AFTER the leading 'BG' keyword (url pan loudness).
// Mirrors parse_bg_line. Returns { bg } or { error }. pan/loudness kept RAW.
function parseBg(tokens) {
    if (tokens.length < 3) {
        return { error: 'BG line needs 3 tokens (url pan loudness)' };
    }
    const url = tokens[0];
    const validScheme = url.indexOf('http://') === 0 ||
                        url.indexOf('https://') === 0 ||
                        url.indexOf('sdcard://') === 0 ||
                        url.indexOf('push://') === 0;
    if (!validScheme) {
        return { error: 'BG url scheme must be http://, https://, sdcard://, or push://' };
    }
    const pan = parseFloat(tokens[1]) || 0;
    const loudness = parseFloat(tokens[2]) || 0;
    return { bg: bg(url, pan, loudness) };
}

// Parse the text AFTER the leading 'S' keyword: `<time> <voice> <volume> "text"`.
// The text is a quoted string (may contain spaces and '#'); we take everything
// between the FIRST and LAST double-quote so embedded quotes survive. If there
// are no quotes, the 4th token onward is treated as the (unquoted) text.
// Returns { row } or { error }.
function parseSpeech(rest) {
    const q1 = rest.indexOf('"');
    let head, text;
    if (q1 >= 0) {
        const q2 = rest.lastIndexOf('"');
        head = rest.slice(0, q1);
        text = (q2 > q1) ? rest.slice(q1 + 1, q2) : rest.slice(q1 + 1);
    } else {
        head = rest;
        text = '';
    }
    const htokens = tokenize(head);
    if (htokens.length < 3) {
        return { error: 'S line needs: S <time> <voice> <volume> "text"' };
    }
    if (q1 < 0) {
        // No quotes — reconstruct text from the 4th token onward.
        text = htokens.slice(3).join(' ');
    }
    const time = parseInt(htokens[0], 10) || 0;
    const voice = htokens[1];
    const volume = parseFloat(htokens[2]) || 0;
    return { row: speechRow({ time, voice, volume, text }) };
}

export function parse(text) {
    const doc = emptyDoc();
    const diagnostics = [];
    if (text === undefined || text === null) return { doc, diagnostics };

    const lines = String(text).split('\n');
    // A trailing newline produces a final empty element; drop it so a file
    // ending in '\n' does not gain a spurious blank row (serialize re-adds it).
    if (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();

    for (let i = 0; i < lines.length; i++) {
        const lineNo = i + 1;
        let line = lines[i];
        // Strip a trailing CR (CRLF files).
        if (line.length > 0 && line[line.length - 1] === '\r') line = line.slice(0, -1);

        const trimmedLead = line.replace(/^[ \t]+/, '');

        // Blank line.
        if (trimmedLead === '') {
            doc.rows.push(blankRow());
            continue;
        }

        // Whole-line comment (first non-ws char is '#') — held verbatim.
        if (trimmedLead[0] === '#') {
            doc.rows.push(commentRow(line));
            continue;
        }

        // BG line: "BG" (case-insensitive) followed by ws or end-of-line.
        if ((trimmedLead[0] === 'B' || trimmedLead[0] === 'b') &&
            (trimmedLead[1] === 'G' || trimmedLead[1] === 'g') &&
            (trimmedLead.length === 2 || /\s/.test(trimmedLead[2]))) {
            const { fields } = splitInlineComment(line);
            const bgTokens = tokenize(fields).slice(1); // drop the "BG" keyword
            const res = parseBg(bgTokens);
            if (res.bg) {
                doc.rows.push(bgRow(res.bg));
                doc.bg = res.bg; // last-wins
            } else {
                doc.rows.push(rawRow(line, res.error));
                diagnostics.push({ line: lineNo, severity: 'error', msg: res.error });
            }
            continue;
        }

        // Speech line: "S" followed by ws. Handled BEFORE inline-comment
        // splitting because the quoted text may legitimately contain '#'.
        if ((trimmedLead[0] === 'S' || trimmedLead[0] === 's') &&
            (trimmedLead.length === 1 || /\s/.test(trimmedLead[1]))) {
            const rest = trimmedLead.replace(/^[Ss][ \t]*/, '');
            const res = parseSpeech(rest);
            if (res.row) {
                doc.rows.push(res.row);
            } else {
                doc.rows.push(rawRow(line, res.error));
                diagnostics.push({ line: lineNo, severity: 'error', msg: res.error });
            }
            continue;
        }

        // Content line: LED or audio. Strip inline comment first.
        const { fields, comment } = splitInlineComment(line);
        const tokens = tokenize(fields);
        if (tokens.length === 0) {
            // Line was only a comment after leading text? (e.g. "   # x" already
            // handled above) — treat as a comment row verbatim.
            doc.rows.push(commentRow(line));
            continue;
        }

        let res;
        if (tokens[0][0] === 'A' || tokens[0][0] === 'a') {
            res = parseAudio(tokens.slice(1), comment);
        } else {
            res = parseLed(tokens, comment);
        }
        if (res.row) {
            doc.rows.push(res.row);
        } else {
            doc.rows.push(rawRow(line, res.error));
            diagnostics.push({ line: lineNo, severity: 'error', msg: res.error });
        }
    }

    return { doc, diagnostics };
}
