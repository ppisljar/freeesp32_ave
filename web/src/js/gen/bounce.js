// Session bounce + session-BG push (bg_browser_push_plan.md).
//
// Shared, UI-agnostic orchestration used by the Generator's Bounce button and
// its Play flow (and previously the BG panel). Keeps the "render the timeline to
// one WAV" and "push the session's push:// BG clip" logic in one place.

import * as store from './bgstore.js';
import { synthSpeech } from './tts.js';
import {
    extractAudioChannels, renderSession, zeroBuffers, mixInto, clampBuffers,
} from './synth.js';
import { encodeWav16, wavBlob, decodeFile } from './bgaudio.js';
import { pushBg } from './transport.js';
import { SAMPLE_RATE } from './model.js';
import { clogI, clogW, clogE } from '../clientlog.js';

// Render the session to one WAV.
//   scope: 'all'      → A entrainment mix + speech + BG clip
//          'bgspeech' → speech + BG clip only (A left for the device to synth)
//   engine: TTS engine for S rows ('puter' | 'google')
//   onProgress(msg): optional status callback
// Returns { wav: Uint8Array, blob: Blob, durationMs, name }.
export async function bounceSession(doc, { scope = 'all', engine = 'puter', onProgress } = {}) {
    const log = onProgress || (() => {});
    const rows = (doc && doc.rows) || [];
    const speechRows = rows.filter(r => r.kind === 'speech' && r.text && r.text.trim());
    clogI('bounce', 'start scope=' + scope + ' engine=' + engine + ' rows=' + rows.length + ' speech=' + speechRows.length + (doc.bg && doc.bg.url ? ' bg=' + doc.bg.url : ''));

    // 1. Synthesize each speech line (sequential = ordered). This is the slow
    //    part (one network TTS call per line), so it drives most of the progress
    //    bar: onProgress(msg, frac) — frac in [0,1], synth spans 0..0.8.
    const speeches = [];
    if (speechRows.length) log('Synthesizing ' + speechRows.length + ' speech line(s) via ' + engine + '…', 0.02);
    for (let si = 0; si < speechRows.length; si++) {
        const s = speechRows[si];
        let buf;
        try {
            buf = await synthSpeech(s.text, { voice: s.voice, engine });
        } catch (e) {
            clogE('bounce', 'TTS failed for speech ' + (si + 1) + '/' + speechRows.length + ' @' + s.time + 'ms:', e);
            throw e;
        }
        speeches.push({
            offset: Math.floor((s.time / 1000) * SAMPLE_RATE),
            buf,
            gain: clamp01((s.volume == null ? 80 : s.volume) / 100),
        });
        log('Synthesized speech ' + (si + 1) + '/' + speechRows.length,
            ((si + 1) / speechRows.length) * 0.8);
    }

    // 2. Decode the session BG clip so speech can be mixed onto it.
    //    - push://name  → from the (hidden) library
    //    - http(s)://   → fetched in the browser; CORS failure is a hard error
    //                     with a "download + upload" suggestion (the device can
    //                     still pull it live, but we can't bake speech onto it)
    //    - sdcard://    → can't be fetched by the browser; suggest upload
    let bgBuf = null, bgGain = 1, bgPan = 0, bgNote = null;
    if (doc.bg && doc.bg.url) {
        const url = doc.bg.url;
        bgGain = clamp01((doc.bg.loudness == null ? 50 : doc.bg.loudness) / 100);
        bgPan = clampPan((doc.bg.pan == null ? 0 : doc.bg.pan) / 100);
        if (url.indexOf('push://') === 0) {
            const name = url.slice('push://'.length);
            const rec = await store.get(name);
            if (rec) {
                try { bgBuf = await decodeFile(rec.wavBlob); clogI('bounce', 'BG push://' + name + ' decoded'); }
                catch (e) { bgBuf = null; clogW('bounce', 'BG push://' + name + ' decode failed:', e); }
            }
            else { bgNote = 'BG clip "' + name + '" not in library — not baked'; clogW('bounce', bgNote); }
        } else if (url.indexOf('http://') === 0 || url.indexOf('https://') === 0) {
            log('Fetching BG ' + url + '…', 0.82);
            let ab;
            try {
                const r = await fetch(url);
                if (!r.ok) throw new Error('HTTP ' + r.status);
                ab = await r.arrayBuffer();
            } catch (e) {
                clogE('bounce', 'BG fetch failed (CORS?) ' + url + ':', e);
                throw new Error('Could not fetch BG "' + url + '" from the browser (likely CORS). '
                    + 'Download the file and upload it via the BG row 📁 button, then retry.');
            }
            bgBuf = await decodeFile(ab);
            clogI('bounce', 'BG fetched + decoded (' + ab.byteLength + ' bytes) ' + url);
        } else {
            // sdcard:// or anything else the browser can't retrieve.
            throw new Error('BG "' + url + '" can’t be read by the browser to bake. '
                + 'Upload the file via the BG row 📁 button to include it.');
        }
    }

    // 3. Total duration = max(A window, speech ends) + a trailing pad. Without
    //    the pad the buffer ends the instant the LAST speech line finishes, so
    //    the real-time WS push (and any audio/timeline drift) clips its tail —
    //    the last "S" line gets cut and reads as "never spoken". 2 s of trailing
    //    silence guarantees the final line plays in full with margin.
    const SPEECH_TAIL_MS = 2000;
    const { channels, maxTimeMs } = extractAudioChannels(doc);
    let totalMs = (scope === 'all') ? maxTimeMs + 2000 : 0;
    for (const sp of speeches) {
        const endMs = ((sp.offset + sp.buf.length) / SAMPLE_RATE) * 1000 + SPEECH_TAIL_MS;
        if (endMs > totalMs) totalMs = endMs;
    }
    if (totalMs < 1000) totalMs = 1000;
    log('Rendering ' + Math.round(totalMs / 1000) + 's…', 0.9);
    await tick(); // let any status UI paint before the synchronous render

    // 4. Base: A entrainment mix (scope 'all') or silence.
    let left, right;
    if (scope === 'all') { const r = renderSession(channels, { totalMs }); left = r.left; right = r.right; }
    else { const z = zeroBuffers(totalMs); left = z.left; right = z.right; }

    // 5. Mix BG clip (looped, equal-power panned).
    if (bgBuf) {
        const bl = bgBuf.getChannelData(0);
        const br = bgBuf.numberOfChannels > 1 ? bgBuf.getChannelData(1) : bl;
        const ang = (bgPan + 1) * (Math.PI / 4);
        const gl = bgGain * Math.cos(ang), gr = bgGain * Math.sin(ang);
        const S = bl.length;
        for (let i = 0; i < left.length && S; i++) {
            const s = i % S;
            left[i] += bl[s] * gl;
            right[i] += br[s] * gr;
        }
    }

    // 6. Mix speech (centered) at each line's time.
    for (const sp of speeches) {
        const sl = sp.buf.getChannelData(0);
        const sr = sp.buf.numberOfChannels > 1 ? sp.buf.getChannelData(1) : sl;
        mixInto(left, right, sl, sr, sp.offset, sp.gain, false);
    }

    clampBuffers(left, right);
    const wav = encodeWav16([left, right], SAMPLE_RATE);
    const name = 'session-' + scope + '-' + Math.round(totalMs / 1000) + 's';
    clogI('bounce', 'done: ' + Math.round(totalMs / 1000) + 's WAV, ' + wav.length + ' bytes' + (bgNote ? ' (' + bgNote + ')' : ''));
    return { wav, blob: wavBlob(wav), durationMs: totalMs, name, note: bgNote };
}

// Push the session's push:// BG clip to the device (used by Play when the BG is
// a browser clip and NOT being replaced by a bounce). Returns a promise or null.
export function pushSessionBg(doc) {
    const desc = doc && doc.bg;
    if (!desc || !desc.url || desc.url.indexOf('push://') !== 0) return null;
    const name = desc.url.slice('push://'.length);
    return store.get(name).then(rec => {
        if (!rec) throw new Error('BG clip "' + name + '" not in this browser’s library');
        return pushBg(rec.wavBlob, { pan: desc.pan, loudness: desc.loudness });
    });
}

function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
function clampPan(x) { return x < -1 ? -1 : (x > 1 ? 1 : x); }
function tick() { return new Promise(r => setTimeout(r, 20)); }
