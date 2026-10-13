// Session bounce + session-BG push (bg_browser_push_plan.md).
//
// Shared, UI-agnostic orchestration used by the Generator's Bounce button and
// its Play flow (and previously the BG panel). Keeps the "render the timeline to
// one WAV" and "push the session's push:// BG clip" logic in one place.

import * as store from './bgstore.js';
import { synthSpeech } from './tts.js';
import { zeroBuffers, mixInto, clampBuffers } from './mix.js';
import { resolveAudioChannels } from './sim/resolve.js';
import { renderSessionDevice, sessionBeatJitter } from './sim/audio.js';
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
    const { channels, maxTimeMs } = resolveAudioChannels(doc);
    let totalMs = (scope === 'all') ? maxTimeMs + 2000 : 0;
    for (const sp of speeches) {
        const endMs = ((sp.offset + sp.buf.length) / SAMPLE_RATE) * 1000 + SPEECH_TAIL_MS;
        if (endMs > totalMs) totalMs = endMs;
    }
    if (totalMs < 1000) totalMs = 1000;
    log('Rendering ' + Math.round(totalMs / 1000) + 's…', 0.9);
    await tick(); // let any status UI paint before the synchronous render

    // 4. Base: A entrainment mix (scope 'all') or silence.
    //
    // Rendered by sim/audio.js, the same engine the Simulate view plays, so a
    // bounce and a preview cannot disagree. Note the LEVEL: that renderer uses
    // the device's fixed 1/16 mix divisor, so a bounced session is ~18-24 dB
    // quieter than the old one — and correct. A single centre-panned tone at
    // vol 100 peaks at -27.1 dBFS, exactly where it measures on the bench.
    let left, right;
    if (scope === 'all') {
        const r = renderSessionDevice(channels, { totalMs, beatJitter: sessionBeatJitter(doc) });
        left = r.left; right = r.right;
    } else {
        const z = zeroBuffers(totalMs); left = z.left; right = z.right;
    }

    // 5. Mix BG clip (looped) with the device's LINEAR pan law — see
    //    bgPanGains below for why this is not the equal-power law it was.
    if (bgBuf) {
        const bl = bgBuf.getChannelData(0);
        const br = bgBuf.numberOfChannels > 1 ? bgBuf.getChannelData(1) : bl;
        const pg = bgPanGains(bgPan);
        // bg_player.c:1903-1908 — loudness first, then the pan law, then
        // accumulate. No 1/16 headroom divisor: BG is mixed in the output task
        // AFTER audio_generator_fill_buffer (audio_test.c:176-184), so unlike
        // every synth channel it never sees audio_generator.c:875's divisor.
        const gl = bgGain * pg.l, gr = bgGain * pg.r;
        // Bake the device's 5 ms fade-in ONLY for an audition bounce. In scope
        // 'bgspeech' this WAV becomes the WS push payload, and
        // bg_player_start_push arms its OWN 220-sample ramp over whatever we
        // send (bg_player.c:1700-1705) — baking it here as well would square
        // the fade. Pan and loudness, by contrast, MUST be baked in BOTH
        // scopes: play.js pushes with pan 0 / loudness 100, which the device
        // turns into pan_l = pan_r = 1.0 and gain 1.0, so its own pan law and
        // loudness contribute nothing to a pushed clip and there is no
        // double-apply to avoid.
        const bakeRamp = (scope === 'all');
        const S = bl.length;
        for (let i = 0; i < left.length && S; i++) {
            const s = i % S;
            const rs = (bakeRamp && i < BG_AMP_RAMP_SAMPLES) ? bgRampScale(i) : 1;
            left[i] += bl[s] * gl * rs;
            right[i] += br[s] * gr * rs;
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

// ---- BG pan law + de-click ramp, ported from bg_player.c -------------------
//
// The device does NOT pan the background bed equal-power. It uses a LINEAR law
// that holds the NEAR side at unity and attenuates only the FAR side —
// bg_player.c:1879-1881:
//
//     const float pan      = s_bg.pan;
//     const float pan_l    = (pan <= 0.0f) ? 1.0f : (1.0f - pan);
//     const float pan_r    = (pan >= 0.0f) ? 1.0f : (1.0f + pan);
//
// This bounce used to use the equal-power law (cos/sin of (pan+1)·π/4), which
// is 3.01 dB down on BOTH channels at pan 0 — 0.7071 where the device gives
// 1.0. That used to hide inside a generally-wrong output level; it does not any
// more, because sim/audio.js renders the synth tones at true device level (the
// fixed 1/16 divisor, audio_generator.c:875), so the 3 dB landed squarely on
// the BG-to-tone balance: the bed played too quiet under the entrainment tones.
//
// FIRMWARE COMMENTS ARE WRONG HERE, THE CODE IS RIGHT. bg_player.c:110, :1826
// and :1874 all claim this law "matches audio_generator.c's apply_panning".
// It does not: apply_panning (audio_generator.c:2304-2315) is equal-power
// cos/sin, read out of the sine LUT, and the synth channels use it both there
// and inline at audio_generator.c:1230-1234. So the device genuinely pans the
// BG bed linearly and the synth channels equal-power — two different laws in
// one mix — and the bounce must mirror both, not pick one. The C wins over the
// C comment.
//
// Pure so test/bg_pan.test.js can pin it without touching an AudioBuffer.
export function bgPanGains(pan) {
    const p = clampPan(pan);
    return {
        l: (p <= 0) ? 1 : (1 - p),
        r: (p >= 0) ? 1 : (1 + p),
    };
}

// The 5 ms de-click fade-in the device arms on every BG session start.
// bg_player.c:159 `#define BG_AMP_RAMP_SAMPLES 220u` (220 samples = 4.99 ms at
// 44.1 kHz), armed at bg_player.c:1388-1392 for URL/SD sessions and :1702-1705
// for pushed ones. The literal is duplicated here for the same reason
// bg_player.c duplicates audio_generator.c's AUDIO_AMP_RAMP_SAMPLES rather than
// including the header: it is one number, and the coupling is not worth it.
export const BG_AMP_RAMP_SAMPLES = 220;

// Ramp scale applied to BG sample `i`, counted from the start of the session.
//
// bg_player.c:1885-1905 adds one step BEFORE reading the gain for the sample:
// current_loudness starts at 0, the loop does `current_loudness +=
// loudness_step` and only then `const float gain = s_bg.current_loudness`. So
// sample 0 already sits at 1/220 of target (NOT at 0), and sample 219 is the
// first at full target — where the firmware also snaps to target_loudness to
// shed float-accumulation drift. Hence (i + 1) / 220, not i / 220.
export function bgRampScale(i) {
    return i >= BG_AMP_RAMP_SAMPLES ? 1 : (i + 1) / BG_AMP_RAMP_SAMPLES;
}

function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
function clampPan(x) { return x < -1 ? -1 : (x > 1 ? 1 : x); }
function tick() { return new Promise(r => setTimeout(r, 20)); }
