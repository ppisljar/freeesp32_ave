// Shared "play a session" path — used by BOTH the Home Play button and the
// Generator Play button so speech (`S`) rows and push:// BG clips are handled
// identically no matter where Play is pressed.
//
// Behaviour, given a parsed `doc`:
//   - no speech:  run the timeline on the device (A tones + LED; S stripped by
//                 serializeForDevice). http(s):// BG is carried in the device
//                 config and pulled by the firmware; a push:// clip is streamed
//                 from the browser library (pushSessionBg).
//   - has speech: bounce BG + speech to ONE WAV (cached in memory), REWRITE the
//                 BG line to push:// so the device gates the timeline on the
//                 pushed audio (BG sample 0 == session t=0), then stream it.

import { playDoc, pushBg } from './transport.js';
import { bounceSession, pushSessionBg } from './bounce.js';
import { getEngine } from './tts.js';
import { serialize } from './serialize.js';
import { showMessage } from '../util.js';

// In-memory cache of the last bounce, keyed by session content + engine, so
// play/stop/play doesn't re-run the (slow, ~seconds) TTS bounce each time. It
// invalidates automatically when the session serialization changes, and is
// dropped on page reload.
let s_bounceCache = { key: null, blob: null };

// Label for the synthesized session BG on the push:// line. The device only
// uses push:// as a "wait for the browser to stream this" signal; the name is
// cosmetic (kept stable so a saved .ledc round-trips).
const SESSION_BG_NAME = 'session';

function $(id) { return document.getElementById(id); }

function progShow() { const w = $('bounceProg'); if (w) w.style.display = 'block'; progSet(0, 'Preparing…'); }
function progSet(frac, msg) {
    const bar = $('bounceProgBar'), txt = $('bounceProgText');
    if (bar) bar.style.width = Math.max(0, Math.min(100, Math.round((frac || 0) * 100))) + '%';
    if (txt && msg != null) txt.textContent = msg;
}
function progHide() { const w = $('bounceProg'); if (w) w.style.display = 'none'; }

// Bounce BG+speech to a WAV, reusing the in-memory cache when the session is
// unchanged. Shows the progress bar only during a real (cache-miss) bounce.
async function ensureBounce(doc, engine) {
    const key = serialize(doc) + '|' + engine;
    if (s_bounceCache.key === key && s_bounceCache.blob) {
        return s_bounceCache.blob;                 // cache hit — instant replay
    }
    progShow();
    try {
        const res = await bounceSession(doc, {
            scope: 'bgspeech', engine,
            onProgress: (m, f) => progSet(f, m),
        });
        s_bounceCache = { key, blob: res.blob };
        return res.blob;
    } finally {
        progHide();
    }
}

export async function playSession(doc) {
    if (!doc || !doc.rows || !doc.rows.length) {
        showMessage('Nothing to play', 'error');
        return;
    }

    const hasSpeech = doc.rows.some(r => r.kind === 'speech');

    if (!hasSpeech) {
        try {
            const res = await playDoc(doc);            // A + LED on the device
            showMessage(res || 'Playing', 'success');
            const p = pushSessionBg(doc);              // null unless BG is push://
            if (p) p.catch(err => showMessage('Session BG: ' + err.message, 'error'));
        } catch (err) {
            showMessage('Play error: ' + err, 'error');
        }
        return;
    }

    // Speech present: bounce (cached) → rewrite BG to push:// → play → stream.
    // Rewriting to push:// makes the firmware gate the timeline start on the
    // pushed audio priming, so the baked speech stays sample-aligned to t=0.
    try {
        const blob = await ensureBounce(doc, getEngine());
        const docForDevice = {
            ...doc,
            bg: { url: 'push://' + SESSION_BG_NAME, pan: 0, loudness: 100 },
        };
        await playDoc(docForDevice);                   // device: A+LED, armed to wait for the push
        await pushBg(blob, { pan: 0, loudness: 100 });  // fills the ring → device starts synced
        showMessage('Playing (speech merged into background)', 'success');
    } catch (err) {
        progHide();
        showMessage('Play error: ' + (err.message || err), 'error');
    }
}
