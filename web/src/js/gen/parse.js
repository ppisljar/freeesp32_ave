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
    speechRow, markerRow, MARKER_LABEL_RE, MARKER_MAX_KV, MARKER_MODALITIES,
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

    // freq_r (token 7): a COMPOUND CELL in format v3, so a prefix survives a
    // round-trip. Previously parseFloat, which turned '>410' into 0 and rewrote a
    // ramped binaural line to mono on save. `-` → null (leave unchanged).
    // Whether the token was THERE at all. `freqR` itself cannot carry that: an
    // absent token and an explicit `0` both land on 0, but they mean opposite
    // things to the engine — absent/`-` keeps the live right carrier
    // (AUD_SET_FREQR clear, audio_generator.c:1003) while a literal 0 makes the
    // channel mono. Without this flag a row that moves `freq` and omits `freqR`
    // reads as "go mono" and silently kills a binaural beat.
    //
    // Carried as a separate boolean rather than by making `freqR` undefined,
    // because every structured editor and the serializer treat it as a plain
    // number. Absent here (undefined) therefore means "present", which is what
    // an editor-built row wants.
    let freqR = 0, freqRCell;
    const freqRPresent = tokens.length >= 7 && tokens[6] !== '-';
    if (freqRPresent) {
        const c = parseCell(tokens[6]);
        freqR = (c.value > 0 && c.value <= NYQUIST) ? c.value : 0;
        // Keep the cell only when it actually carries a prefix, so an ordinary
        // line serializes byte-identically to before.
        if (c.interp && c.interp !== 'none') {
            freqRCell = cell(freqR, c.interp, c.modEnd, c.modPeriodMs);
        }
    }

    // wave_type (token 8): present → int 0..7 (else 0); absent / `-` → null.
    let waveType = null;
    if (tokens.length >= 8 && tokens[7] !== '-') {
        const wt = parseInt(tokens[7], 10);
        waveType = (Number.isFinite(wt) && wt >= 0 && wt < WAVE_COUNT) ? wt : 0;
    }

    // Optional pulse fields: duty env phase attack jitter, then depth (v3, 14th).
    const duty   = optCell(tokens, 8);
    const env    = optInt(tokens, 9);
    const phase  = optCell(tokens, 10);
    const attack = optCell(tokens, 11);
    const jitter = optJitter(tokens, 12);
    const depth  = optCell(tokens, 13);

    return { row: audioRow({ time, freq, pan, vol, mod, channel, freqR, freqRCell,
                             freqRPresent,
                             waveType, duty, env, phase, attack, jitter, depth,
                             inlineComment: comment }) };
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
//
// WHERE A COMMENT MAY BEGIN ON AN `S` LINE. The quoted text may itself contain
// a '#', so "everything after the first '#'" — the rule splitInlineComment
// applies to every other row kind — is wrong here. The boundary comes from the
// C, which is unambiguous about it:
//   - the payload is parsed from the RAW line, not from the comment-stripped
//     copy: config_parser.c:1556 passes `line`, while the '#' cut at
//     config_parser.c:1523 is made on `line_copy`, a scratch buffer that exists
//     only to produce the tokens the dispatch switches on. So '#' is not a
//     delimiter anywhere parse_speech_line looks.
//   - the text runs from the first '"' at or after the volume
//     (config_parser.c:1743) to the LAST '"' on the line (config_parser.c:1748
//     — `strrchr`, which is why embedded quotes survive), and nothing past that
//     last quote is ever read: config_parser.c:1756 terminates the copy there.
// Hence: inside the quotes a '#' is spoken text; after the closing quote it is
// the only thing that can be a comment.
//
// One consequence reads like a bug and is not. A comment containing a double
// quote MOVES the "last quote", so the device hears
//     S 0 en 80 "hi" # he said "no"
// as the single phrase  hi" # he said "no  . We read it the same way, which is
// both faithful and byte-stable (the first and last quote are re-emitted where
// they stood), so even that spelling round-trips without anyone guessing.
function parseSpeech(rest) {
    const q1 = rest.indexOf('"');
    let head, text, comment = '';
    if (q1 >= 0) {
        const q2 = rest.lastIndexOf('"');
        head = rest.slice(0, q1);
        text = (q2 > q1) ? rest.slice(q1 + 1, q2) : rest.slice(q1 + 1);
        if (q2 > q1) {
            const tail = rest.slice(q2 + 1);
            const h = tail.indexOf('#');
            const before = (h >= 0) ? tail.slice(0, h) : tail;
            if (before.trim() !== '') {
                // Non-comment text after the closing quote. The device ignores
                // it silently (config_parser.c:1756 stops reading there), so it
                // means nothing to the engine — but it is not a comment either
                // and the model has no field to hold it, and deleting an
                // author's bytes on save is the one outcome this parser avoids.
                // Held verbatim as a raw row instead, with the reason attached.
                return { error: 'S line: "' + before.trim() + '" after the closing quote ' +
                                'is ignored by the firmware and cannot be represented; ' +
                                'only a trailing "# comment" may follow the quoted text' };
            }
            if (h >= 0) comment = tail.slice(h + 1).trim();
        }
        // An UNTERMINATED quote (q2 === q1) is rejected outright by the device
        // (config_parser.c:1749). The browser repairs it instead — serializing
        // the row supplies the missing quote — so there is no closing quote to
        // measure a comment from, and a '#' after the lone quote is inside the
        // text the author was still typing. No split in that case.
    } else {
        head = rest;
        text = '';
    }
    const htokens = tokenize(head);
    if (htokens.length < 3) {
        return { error: 'S line needs: S <time> <voice> <volume> "text"' };
    }
    if (q1 < 0) {
        // No quotes — reconstruct text from the 4th token onward. The device
        // rejects such a line outright (config_parser.c:1744), so there is no
        // firmware rule here to mirror and no comment is split off: a '#' is
        // kept as text, and the row serializes back QUOTED, which is what makes
        // it device-legal. Nothing is lost either way.
        text = htokens.slice(3).join(' ');
    }
    const time = parseInt(htokens[0], 10) || 0;
    const voice = htokens[1];
    const volume = parseFloat(htokens[2]) || 0;
    return { row: speechRow({ time, voice, volume, text, inlineComment: comment }) };
}

/*
 * Parse the tokens AFTER the leading `M`: `<time> <LABEL> [key=value ...]`.
 * Mirrors parse_marker_line (config_parser.c:126). Returns { row } or { error }.
 *
 * Three behaviours copied deliberately, because the device is lenient here and
 * a stricter browser parser would reject files the hardware happily runs:
 *   - a token without `=` is WARNED about and skipped, not fatal;
 *   - key=value pairs past the fourth are dropped, not fatal;
 *   - `mod=` is reserved: it sets the modality and is never stored as a kv.
 * Only a missing/!-shaped label is an actual error.
 *
 * Returns { row, warn } — `warn` is a non-fatal diagnostic for something the
 * device logs and carries on past.
 */
function parseMarker(tokens, comment) {
    if (tokens.length < 2) {
        return { error: "M line needs at least '<time> <LABEL>'" };
    }
    const time = parseInt(tokens[0], 10) || 0;
    const label = tokens[1];
    if (!MARKER_LABEL_RE.test(label)) {
        return { error: 'M line: label "' + label + '" must be 1..31 chars of [A-Z0-9_]' };
    }
    const kv = [];
    let modality = null;
    let modalityRaw = '';
    let warn = '';
    for (let i = 2; i < tokens.length; i++) {
        const t = tokens[i];
        const eq = t.indexOf('=');
        if (eq <= 0) continue;                       // no key: device warns + skips
        const k = t.slice(0, eq);
        const v = t.slice(eq + 1);
        if (k === 'mod') {
            if (MARKER_MODALITIES.indexOf(v) >= 0) {
                modality = v;
            } else {
                // An unknown modality is not an error on the device: it warns
                // and falls back to inferring from the batch
                // (config_parser.c:174), leaving the marker in the timeline.
                // Mirroring only the "modality stays unset" half of that threw
                // the token away entirely — neither `modality` nor a kv pair
                // held it — so the author's text was deleted from the file by
                // the next save. Keep the spelling verbatim (model.js
                // modalityRaw) and report it instead of silently editing it.
                modalityRaw = v;
                warn = 'M line ' + label + ': mod="' + v + '" is not one of ' +
                       MARKER_MODALITIES.join('/') + ' — the device warns and infers ' +
                       'the modality from what shares this timestamp. Value kept verbatim.';
            }
            continue;                                // reserved, never a kv
        }
        if (kv.length >= MARKER_MAX_KV) continue;    // device drops the rest
        kv.push({ k, v });
    }
    return { row: markerRow({ time, label, kv, modality, modalityRaw,
                              inlineComment: comment }), warn };
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

        // Marker line: "M" followed by ws. Produces no sound and no light; it
        // exists to announce an experiment event to an external recorder.
        if ((trimmedLead[0] === 'M' || trimmedLead[0] === 'm') &&
            (trimmedLead.length === 1 || /\s/.test(trimmedLead[1]))) {
            const { fields: mf, comment: mc } = splitInlineComment(line);
            const res = parseMarker(tokenize(mf).slice(1), mc);
            if (res.row) {
                doc.rows.push(res.row);
                // Non-fatal: the line still runs on the device, so it is a
                // 'warn' (validate.js's spelling), not an 'error'.
                if (res.warn) diagnostics.push({ line: lineNo, severity: 'warn', msg: res.warn });
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
            // `A1000 240 …` is NOT "an audio row at t=1000". The firmware's
            // audio branch tests only the first BYTE of the token
            // (config_parser.c:1542) and then drops the WHOLE token
            // (config_parser.c:1544 passes `tokens + 1, token_count - 1`), so
            // everything glued to the A is discarded and every remaining field
            // slides one column left: `A1000 240 0 60 0 1` plays at t=240 ms
            // with freq 0, pan 60, vol 0, mod 1. The `M` and `S` branches guard
            // against precisely this by requiring a lone keyword character
            // (config_parser.c:1545 / :1551); the `A` branch does not, so there
            // is no device rejection to mirror either — it accepts the line and
            // misreads it.
            //
            // Neither reading is therefore honest. `A 1000 240 …` is not what
            // the device plays, and `A 240 0 60 …` — what we used to emit,
            // because parseAudio never saw the glued digits — is a DIFFERENT,
            // valid-looking command with the author's 1000 ms silently gone.
            // (Both spellings do produce the same wrong row on the device,
            // which is exactly why the rewrite was invisible.) So hold the
            // bytes and say why, the way every other line the device would not
            // run as written is held. The stale `A1500 binaural 40.0 6.0` in
            // the project CLAUDE.md is from the old keyword grammar, which
            // parse_audio_line no longer implements at all; it does not make
            // the glued form legal.
            if (tokens[0].length > 1) {
                const glued = tokens[0].slice(1);
                const msg = 'Audio prefix "' + tokens[0] + '" must be a lone "A": the ' +
                    'firmware drops the entire first token, so "' + glued + '" is lost ' +
                    'and every field shifts one column left. Write "A ' + glued + ' …".';
                doc.rows.push(rawRow(line, msg));
                diagnostics.push({ line: lineNo, severity: 'error', msg });
                continue;
            }
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
