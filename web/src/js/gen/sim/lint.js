// Simulator-only lints — the problems you can only see once the timeline has
// been RESOLVED, as opposed to the grammar checks in gen/validate.js.
//
// Every finding here is something a source-only validator cannot reach, because
// answering it needs the per-channel carry, the ramp targets, or the engine's
// own defaults:
//
//   RAMP_TO_ZERO      a `>` whose target row writes `-` on that field glides to
//                     0 rather than holding, because the device reads the next
//                     entry's raw struct member out of a memset
//   LED_ROW_DROPPED   an out-of-range row is refused by the install, not
//                     clamped, so the zone keeps its previous state
//   GLOBAL_JITTER     audio `jitter` has no channel argument and the LAST line
//                     carrying it wins for the whole session — a column written
//                     twenty minutes in applies from t=0
//   DUTY_NO_OP        a duty column (or duty modulation) next to env 1 or env 2,
//                     which never read it
//   LED_ENV_FOLDED    env 4 on an LED line, which folds to square rather than
//                     doing anything tremolo-like
//   STRIP_COARSE      a flicker rate the LED strip cannot resolve: one WS2812
//                     frame takes 2.41 ms, so a fast smooth carrier gets only a
//                     handful of distinct levels per cycle on hardware
//   SILENT_CHANNEL    a channel that is configured but can never be heard
//
// Each finding carries `row` (index into doc.rows, so the UI can name the source
// line and scroll to it) and `timeMs` (so it can seek the playhead there).
//
// Pure: no DOM. Unit-tested in web/test/sim_lint.test.js.

import { MAX_LED_FLICKER_HZ } from '../model.js';
import { resolveAudioChannels, resolveLedChannels } from './resolve.js';
import { ledEntryRejection, tickHzFor, stripFrameUs, DEFAULT_CHANNEL_MAP,
         LED_FLICKER_TICK_MAX } from './led.js';
import { annotateIso, ISO_ENV_SINE, ISO_ENV_TRIANGLE, ISO_ENV_TREMOLO } from './audio.js';

// Below this many strip frames per flicker cycle, a smooth carrier visibly
// steps on the hardware while the simulator draws it as a continuous curve.
// Twelve is roughly where the staircase stops being obvious.
const MIN_FRAMES_PER_CYCLE = 12;

function rowIndexOf(doc, row) {
    const i = (doc.rows || []).indexOf(row);
    return i >= 0 ? i : null;
}

function finding(code, severity, msg, doc, row, timeMs) {
    return {
        code, severity, msg,
        row: row ? rowIndexOf(doc, row) : null,
        timeMs: (timeMs === undefined || timeMs === null) ? null : timeMs,
    };
}

export function lintSession(doc) {
    const out = [];
    if (!doc || !doc.rows) return out;

    const { channels: audioCh } = resolveAudioChannels(doc);
    const { channels: ledCh } = resolveLedChannels(doc);

    // ---- ramps that glide to zero ------------------------------------------
    for (const [ch, entries] of audioCh) {
        for (const e of entries) {
            if (!e.rampsIntoDash || !e.rampsIntoDash.length) continue;
            out.push(finding('RAMP_TO_ZERO', 'warn',
                'Audio channel ' + ch + ': the ramp on ' + e.rampsIntoDash.join(' / ')
                + ' targets a later row that writes `-` on that field, so it glides '
                + 'to ZERO instead of holding. The device reads the next entry\'s raw '
                + 'value regardless of whether the row set it. Write the value you '
                + 'want on the target row.',
                doc, e.row, e.time));
        }
    }
    for (const [bit, entries] of ledCh) {
        for (const e of entries) {
            if (!e.rampsIntoDash || !e.rampsIntoDash.length) continue;
            out.push(finding('RAMP_TO_ZERO', 'warn',
                'LED ' + (bit + 1) + ': the ramp on ' + e.rampsIntoDash.join(' / ')
                + ' targets a later row that writes `-` on that field, so it glides '
                + 'to ZERO instead of holding.',
                doc, e.row, e.time));
        }
    }

    // ---- rows the device refuses to install --------------------------------
    const dropped = new Set();
    for (const [bit, entries] of ledCh) {
        for (const e of entries) {
            const why = ledEntryRejection(e);
            if (!why) continue;
            const key = rowIndexOf(doc, e.row) + '|' + why;
            if (dropped.has(key)) continue;
            dropped.add(key);
            out.push(finding('LED_ROW_DROPPED', 'error',
                'This LED row has ' + why + ', so the device REFUSES to install it '
                + '(it returns an error rather than clamping) and LED ' + (bit + 1)
                + ' keeps rendering whatever it was already doing. The ceiling is '
                + MAX_LED_FLICKER_HZ + ' Hz for the rate and 100 for duty and '
                + 'brightness.',
                doc, e.row, e.time));
        }
    }

    // ---- global audio jitter -----------------------------------------------
    // audio_generator.c:245 — the setter takes no channel, and session start AND
    // stop reset it, so the file's last jitter column decides the whole run.
    const jitterRows = (doc.rows || []).filter(
        r => r.kind === 'audio' && r.jitter !== undefined && r.jitter !== null);
    if (jitterRows.length) {
        const last = jitterRows[jitterRows.length - 1];
        if (jitterRows.length > 1) {
            out.push(finding('GLOBAL_JITTER', 'warn',
                jitterRows.length + ' audio rows set the `jitter` column, but beat '
                + 'jitter is a SESSION-WIDE setting with no channel — the last one '
                + 'in the file wins for the entire session, including the part '
                + 'before it. Only the row at ' + last.time + ' ms has any effect '
                + '(amp ' + last.jitter.amp + ').',
                doc, last, last.time));
        } else if (last.time > 0) {
            out.push(finding('GLOBAL_JITTER', 'warn',
                'This row sets `jitter`, which is session-wide rather than '
                + 'per-entry: it applies from t=0, not from ' + last.time + ' ms. '
                + 'The simulator and the device both detune the right carrier for '
                + 'the whole session.',
                doc, last, last.time));
        }
    }

    // ---- duty next to an envelope that ignores it --------------------------
    for (const [ch, entries] of audioCh) {
        annotateIso(entries);
        for (const e of entries) {
            const setsDuty = e.raw.duty !== undefined && e.raw.duty !== null;
            if (!setsDuty) continue;
            const env = e.iso.env;
            if (env === ISO_ENV_SINE || env === ISO_ENV_TRIANGLE) {
                out.push(finding('DUTY_NO_OP', 'warn',
                    'Audio channel ' + ch + ' sets `duty` while the envelope is '
                    + (env === ISO_ENV_SINE ? 'env 1 (sine)' : 'env 2 (triangle)')
                    + ', which never reads it — the column has no effect. '
                    + (env === ISO_ENV_TRIANGLE
                        ? 'A triangle gate is always exactly 50 % on.'
                        : 'A raised-sine gate has no duty parameter.')
                    + ' Use env 0 or env 3 if you want duty to matter.',
                    doc, e.row, e.time));
            }
        }
    }

    // ---- LED env 4 ---------------------------------------------------------
    for (const row of doc.rows) {
        if (row.kind !== 'led') continue;
        if (row.env === undefined || row.env === null) continue;
        if (row.env > 3 || row.env < 0) {
            out.push(finding('LED_ENV_FOLDED', 'warn',
                'LED env ' + row.env + ' is outside the LED range 0..3 and folds to '
                + '0 (square). Only the AUDIO path has env 4 (tremolo); on a light '
                + 'row it means nothing.',
                doc, row, row.time));
        }
    }

    // ---- rates the STRIP cannot resolve ------------------------------------
    // Not the ISR tick: the engine aims for 250 ticks per flicker cycle and
    // reaches its 10 kHz ceiling at exactly 40 Hz, so the tick is fine right up
    // to the documented maximum. The real ceiling is the wire — one 74-pixel
    // WS2812 frame takes 2.41 ms to clock out, and refreshes serialise behind
    // it, so a smooth carrier gets however many frames fit in a cycle.
    const frameMs = stripFrameUs() / 1000;
    for (const [bit, entries] of ledCh) {
        const seen = new Set();
        for (const e of entries) {
            const f = e.cells.freq ? e.cells.freq.value : 0;
            if (!(f > 0)) continue;
            const env = e.cells.env ? Math.trunc(e.cells.env.value) : 0;
            const framesPerCycle = (1000 / f) / frameMs;
            if (framesPerCycle >= MIN_FRAMES_PER_CYCLE) continue;
            const key = Math.round(f * 100) + ':' + env;
            if (seen.has(key)) continue;
            seen.add(key);
            const smooth = env >= 1 && env <= 3;
            out.push(finding('STRIP_COARSE', 'info',
                'LED ' + (bit + 1) + ' at ' + f + ' Hz: a ' + DEFAULT_CHANNEL_MAP.length
                + '-pixel WS2812 frame takes ' + frameMs.toFixed(2) + ' ms to clock '
                + 'out, so the strip can only change about '
                + framesPerCycle.toFixed(0) + ' times per cycle. '
                + (smooth
                    ? 'This carrier is a smooth shape, so the hardware renders it '
                      + 'as those few steps while the simulator draws the ideal '
                      + 'curve — expect it to look coarser on the device.'
                    : 'A square carrier only needs two edges, so this is fine — it '
                      + 'only matters if you switch to a smooth carrier (env 1-3).')
                + ' The flicker timer itself is running at ' + tickHzFor(f) + ' Hz'
                + (tickHzFor(f) >= LED_FLICKER_TICK_MAX ? ', its ceiling' : '') + '.',
                doc, e.row, e.time));
        }
    }

    // ---- channels that can never be heard ----------------------------------
    for (const [ch, entries] of audioCh) {
        let everAudible = false;
        for (const e of entries) {
            const vol = e.cells.vol ? e.cells.vol.value : 0;
            const volEnd = e.cells.vol && e.cells.vol.modEnd !== null
                ? e.cells.vol.modEnd : 0;
            const tgt = e.t.vol ? e.t.vol.target : null;
            if (vol > 0 || volEnd > 0 || (tgt !== null && tgt > 0)) { everAudible = true; break; }
        }
        if (everAudible) continue;
        const wave = entries[0].cells.wave ? entries[0].cells.wave.value : 0;
        const freq = entries[0].cells.freq ? entries[0].cells.freq.value : 0;
        // A noise channel has no pitch, so freq 0 is meaningful there; a tone at
        // freq 0 with vol 0 is just an unused row.
        const isNoise = wave >= 4 && wave <= 6;
        out.push(finding('SILENT_CHANNEL', 'info',
            'Audio channel ' + ch + ' never reaches a volume above 0, so it is '
            + 'configured but silent'
            + (!isNoise && !(freq > 0) ? ' (and has no carrier frequency either)' : '')
            + '.',
            doc, entries[0].row, entries[0].time));
    }

    // Stable order: by time, then by severity so errors lead within an instant.
    const rank = { error: 0, warn: 1, info: 2 };
    out.sort((a, b) => (a.timeMs - b.timeMs) || (rank[a.severity] - rank[b.severity]));
    return out;
}
