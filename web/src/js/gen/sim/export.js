// Exports from the simulator: a WAV you can listen to, and a CSV/JSON you can
// line up against a capture.
//
// The CSV is the one that earns its place in THIS project. There is a
// measurement rig (docs/hardware/measurements.html) and a stimulus-marker bus
// whose whole purpose is making the device's output comparable with an external
// recording — so a prediction in the same shape as that recording is directly
// checkable, rather than something you have to eyeball against a screenshot.
//
// Sampling rate for the LED rows is the STRIP's real frame period, not a round
// number: one 74-pixel WS2812 frame takes 2.41 ms to clock out and refreshes
// serialise behind it, so that is the fastest the hardware can change a pixel.
// Sampling faster would invent detail the device cannot produce; sampling on a
// round 1 ms or 10 ms grid would be a quantity with no physical meaning.
//
// Pure: no DOM, no Blob, no download. The view wraps these. Unit-tested in
// web/test/sim_export.test.js.

import { SAMPLE_RATE, WAVE_TYPES } from '../model.js';
import { createRenderer, renderSessionDevice, sessionBeatJitter, toInt16Device } from './audio.js';
import { resolveAudioChannels, resolveLedChannels, resolveMarkers, sessionEndMs } from './resolve.js';
import { ledStateAt, stripFrameUs, markerPipelineLagUs, LED_BRIGHTNESS_Q8_MAX } from './led.js';

/*
 * Canonical 16-bit PCM WAV.
 *
 * Deliberately NOT gen/bgaudio.js's encodeWav16: that one rounds against 32768,
 * while the device casts `(int16_t)(clamped * 32767.0f)`, which TRUNCATES toward
 * zero. The difference is a single LSB, but this file exists to be compared
 * against hardware, and a known-wrong rounding rule in a reference export is the
 * kind of thing that costs somebody an afternoon.
 */
export function encodeWavDevice(left, right, sampleRate = SAMPLE_RATE) {
    const frames = left.length;
    const bytes = 44 + frames * 4;
    const buf = new ArrayBuffer(bytes);
    const dv = new DataView(buf);
    const u8 = new Uint8Array(buf);
    const ascii = (off, str) => { for (let i = 0; i < str.length; i++) u8[off + i] = str.charCodeAt(i); };

    ascii(0, 'RIFF');
    dv.setUint32(4, bytes - 8, true);
    ascii(8, 'WAVE');
    ascii(12, 'fmt ');
    dv.setUint32(16, 16, true);          // PCM fmt chunk size
    dv.setUint16(20, 1, true);           // PCM
    dv.setUint16(22, 2, true);           // stereo
    dv.setUint32(24, sampleRate, true);
    dv.setUint32(28, sampleRate * 4, true);  // byte rate
    dv.setUint16(32, 4, true);           // block align
    dv.setUint16(34, 16, true);          // bits
    ascii(36, 'data');
    dv.setUint32(40, frames * 4, true);

    let off = 44;
    for (let i = 0; i < frames; i++) {
        dv.setInt16(off, toInt16Device(left[i]), true); off += 2;
        dv.setInt16(off, toInt16Device(right[i]), true); off += 2;
    }
    return u8;
}

// Render the whole session and encode it. `tailMs` matches the bounce's pad so
// a channel left at volume does not stop mid-note.
export function exportWav(doc, { tailMs = 2000, minMs = 1000 } = {}) {
    const { channels, maxTimeMs } = resolveAudioChannels(doc);
    const totalMs = Math.max(minMs, maxTimeMs + tailMs);
    const { left, right } = renderSessionDevice(channels, {
        totalMs, beatJitter: sessionBeatJitter(doc),
    });
    return { bytes: encodeWavDevice(left, right), durationMs: totalMs };
}

// ---- tabular ---------------------------------------------------------------

/*
 * Sample the resolved timeline on a fixed grid.
 *
 * Returns { stepMs, columns, rows } where each row is a flat array lining up
 * with `columns`. Columns are built from the channels the session actually
 * uses, so a two-zone session does not emit six empty ones.
 */
export function sampleTimeline(doc, { stepMs, endMs } = {}) {
    const step = stepMs || stripFrameUs() / 1000;
    const { channels: audioCh } = resolveAudioChannels(doc);
    const { channels: ledCh } = resolveLedChannels(doc);
    const end = (endMs === undefined || endMs === null) ? sessionEndMs(doc) : endMs;

    const audioKeys = [...audioCh.keys()].sort((a, b) => a - b);
    const ledKeys = [...ledCh.keys()].sort((a, b) => a - b);

    const columns = ['t_ms'];
    for (const ch of audioKeys) {
        columns.push('a' + ch + '_hz', 'a' + ch + '_hz_r', 'a' + ch + '_beat_hz',
                     'a' + ch + '_vol', 'a' + ch + '_pan', 'a' + ch + '_mod_hz',
                     'a' + ch + '_wave');
    }
    for (const bit of ledKeys) {
        const n = bit + 1;
        columns.push('led' + n + '_hz', 'led' + n + '_level', 'led' + n + '_r',
                     'led' + n + '_g', 'led' + n + '_b', 'led' + n + '_duty',
                     'led' + n + '_phase_deg');
    }

    const renderer = createRenderer(audioCh, { beatJitter: sessionBeatJitter(doc) });
    const rows = [];
    for (let t = 0; t <= end; t += step) {
        const row = [round3(t)];
        const states = renderer.channelStates(t);
        const byCh = new Map(states.map(s => [s.ch, s]));
        for (const ch of audioKeys) {
            const s = byCh.get(ch);
            if (!s) { row.push('', '', '', '', '', '', ''); continue; }
            row.push(round3(s.freq),
                     s.binaural ? round3(s.freqR) : '',
                     s.beatHz === null ? '' : round3(s.beatHz),
                     round3(s.vol), round3(s.pan), round3(s.mod),
                     WAVE_TYPES[s.wave] || s.wave);
        }
        for (const bit of ledKeys) {
            const st = ledStateAt(ledCh.get(bit), t);
            if (!st || !st.active) { row.push('', '', '', '', '', '', ''); continue; }
            row.push(st.dc ? 0 : round3(st.freq),
                     round3(st.levelQ8 / LED_BRIGHTNESS_Q8_MAX),
                     st.px.r, st.px.g, st.px.b,
                     st.dc ? '' : st.duty,
                     st.dc ? '' : st.phase);
        }
        rows.push(row);
    }
    return { stepMs: step, columns, rows, endMs: end };
}

function round3(v) {
    if (typeof v !== 'number' || !Number.isFinite(v)) return v;
    return Math.round(v * 1000) / 1000;
}

function csvCell(v) {
    const s = String(v === null || v === undefined ? '' : v);
    return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

/*
 * CSV with a provenance header.
 *
 * The `#` lines matter as much as the data: without the sample rate, the pixel
 * count the LED lag was derived from, and the fact that onsets are PREDICTED
 * rather than measured, a row of numbers lined up against a capture can be
 * misread as a measurement. The firmware's own JSONL marker sink carries the
 * same kind of header for the same reason (marker.c:632).
 */
export function exportCsv(doc, opts = {}) {
    const t = sampleTimeline(doc, opts);
    const markers = resolveMarkers(doc, markerPipelineLagUs);
    const out = [];
    out.push('# freeesp32_ave browser simulator — PREDICTED output, not measured');
    out.push('# step_ms=' + round3(t.stepMs)
           + ' (one WS2812 strip frame; the fastest any pixel can change)');
    out.push('# audio_sample_rate=' + SAMPLE_RATE);
    out.push('# led_level is 0..1 of full brightness; r,g,b are the 8-bit bytes'
           + ' the strip would receive (no gamma anywhere, by design)');
    for (const m of markers) {
        out.push('# marker t_ms=' + m.timeMs + ' label=' + m.label
               + ' modality=' + m.modality + (m.inferred ? '(inferred)' : '')
               + ' predicted_onset_ms=' + round3(m.onsetMs)
               + (m.kv.length ? ' ' + m.kv.map(p => p.k + '=' + p.v).join(' ') : ''));
    }
    out.push(t.columns.join(','));
    for (const row of t.rows) out.push(row.map(csvCell).join(','));
    return out.join('\n') + '\n';
}

// Same content as JSON, for anything that would rather not parse CSV.
export function exportJson(doc, opts = {}) {
    const t = sampleTimeline(doc, opts);
    return JSON.stringify({
        source: 'freeesp32_ave browser simulator',
        note: 'PREDICTED output, not measured',
        step_ms: round3(t.stepMs),
        audio_sample_rate: SAMPLE_RATE,
        end_ms: t.endMs,
        markers: resolveMarkers(doc, markerPipelineLagUs).map(m => ({
            t_ms: m.timeMs, label: m.label, modality: m.modality,
            inferred: m.inferred, predicted_onset_ms: round3(m.onsetMs),
            kv: m.kv,
        })),
        columns: t.columns,
        rows: t.rows,
    }, null, 1);
}
