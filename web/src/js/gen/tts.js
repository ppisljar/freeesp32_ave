// Text-to-speech for `S` speech rows (bg_browser_push_plan.md).
//
// Two engines, both free / no API key, selectable per call via opts.engine:
//
//   'puter'  — Puter.js (https://js.puter.com/v2/): puter.ai.txt2speech() gives
//              AWS Polly neural voices entirely in the browser (no CORS, no
//              device round-trip). Loaded lazily from CDN on first use. NOTE:
//              Puter uses a "user pays" model — deployed apps may prompt the end
//              user to sign into Puter the first time. Needs internet in the
//              BROWSER (not the device).
//   'google' — the device proxies Google Translate TTS at GET /api/tts?tl=&q=
//              (same-origin, ESP32 does the outbound HTTPS GET). Google caps
//              each request at ~200 chars, so text is split into word-boundary
//              chunks. Needs internet on the DEVICE (station mode).
//
// Either way the result is decoded + conformed to 44100/stereo and (for
// multi-chunk text) concatenated into one AudioBuffer ready to mix into the
// bounce. previewSpeech() uses the browser's built-in speechSynthesis for a
// quick local audition (its audio can't be captured, so it's preview-only).

import { decodeFile, audioBufferToWav16, wavBlob, DEVICE_SAMPLE_RATE, DEVICE_CHANNELS } from './bgaudio.js';
import { cacheGet, cachePut } from './bgstore.js';

const CHUNK_CHARS = 180;      // Google Translate ~200-char per-request cap
const PUTER_CHUNK_CHARS = 2500; // Polly txt2speech ~3000-char cap

// Selected TTS engine, persisted so the Background-audio tab owns the setting
// but any caller (bounce, Play) can read it. 'puter' | 'google'.
const ENGINE_KEY = 'ave:ttsEngine';
export function getEngine() {
    try { return localStorage.getItem(ENGINE_KEY) || 'puter'; } catch (e) { return 'puter'; }
}
export function setEngine(e) {
    try { localStorage.setItem(ENGINE_KEY, e === 'google' ? 'google' : 'puter'); } catch (e2) { /* ignore */ }
}

// Split text into <=CHUNK_CHARS pieces, preferring to break on whitespace so
// words aren't cut mid-syllable. A single over-long word is hard-split.
export function chunkText(text, maxLen = CHUNK_CHARS) {
    const out = [];
    let s = String(text || '').trim();
    while (s.length > maxLen) {
        let cut = s.lastIndexOf(' ', maxLen);
        if (cut <= 0) cut = maxLen;           // no space — hard split
        out.push(s.slice(0, cut).trim());
        s = s.slice(cut).trim();
    }
    if (s.length) out.push(s);
    return out;
}

// Fetch + decode + conform one text chunk to a 44100/stereo AudioBuffer.
async function fetchChunk(chunk, voice) {
    const q = '?tl=' + encodeURIComponent(voice || 'en') + '&q=' + encodeURIComponent(chunk);
    const r = await fetch('/api/tts' + q);
    if (!r.ok) throw new Error('TTS HTTP ' + r.status);
    const ab = await r.arrayBuffer();
    return decodeFile(ab);   // decodeAudioData(mp3) -> conform to 44100/stereo
}

// Concatenate 44100/stereo AudioBuffers into one (via an OfflineAudioContext
// only used as an AudioBuffer factory — no rendering needed).
function concatBuffers(buffers) {
    const total = buffers.reduce((n, b) => n + b.length, 0);
    const octx = new OfflineAudioContext(DEVICE_CHANNELS, Math.max(1, total), DEVICE_SAMPLE_RATE);
    const out = octx.createBuffer(DEVICE_CHANNELS, Math.max(1, total), DEVICE_SAMPLE_RATE);
    for (let c = 0; c < DEVICE_CHANNELS; c++) {
        const dst = out.getChannelData(c);
        let off = 0;
        for (const b of buffers) {
            dst.set(b.getChannelData(Math.min(c, b.numberOfChannels - 1)), off);
            off += b.length;
        }
    }
    return out;
}

// ---- Puter.js engine (browser-only, AWS Polly) -----------------------------

let _puterLoading = null;
function ensurePuter() {
    if (typeof window !== 'undefined' && window.puter && window.puter.ai) return Promise.resolve();
    if (_puterLoading) return _puterLoading;
    _puterLoading = new Promise((resolve, reject) => {
        if (typeof document === 'undefined') { reject(new Error('no DOM for Puter')); return; }
        const s = document.createElement('script');
        s.src = 'https://js.puter.com/v2/';
        s.async = true;
        s.onload = () => {
            // window.puter may take a tick to initialise.
            let tries = 0;
            const poll = () => {
                if (window.puter && window.puter.ai) return resolve();
                if (++tries > 100) return reject(new Error('Puter.js loaded but puter.ai missing'));
                setTimeout(poll, 50);
            };
            poll();
        };
        s.onerror = () => reject(new Error('failed to load Puter.js from CDN'));
        document.head.appendChild(s);
    });
    return _puterLoading;
}

async function synthViaPuter(text, voice) {
    await ensurePuter();
    const chunks = chunkText(text, PUTER_CHUNK_CHARS);
    const buffers = [];
    for (const c of chunks) {
        // puter.ai.txt2speech(text, language) resolves to an HTMLAudioElement
        // whose .src is the (blob/data) MP3 we can fetch + decode.
        const audio = await window.puter.ai.txt2speech(c, voice || 'en-US');
        const src = audio && (audio.src || (audio.audio && audio.audio.src));
        if (!src) throw new Error('Puter txt2speech returned no audio src');
        const resp = await fetch(src);
        const ab = await resp.arrayBuffer();
        buffers.push(await decodeFile(ab));
    }
    return buffers.length === 1 ? buffers[0] : concatBuffers(buffers);
}

async function synthViaGoogle(text, voice) {
    const chunks = chunkText(text, CHUNK_CHARS);
    const buffers = [];
    for (const c of chunks) buffers.push(await fetchChunk(c, voice)); // sequential = ordered
    return buffers.length === 1 ? buffers[0] : concatBuffers(buffers);
}

// Synthesize speech -> one 44100/stereo AudioBuffer. Dispatches on opts.engine
// ('puter' default | 'google'). Results are cached (hidden LRU) keyed by
// engine|voice|text so repeat plays of a session don't re-hit the TTS engine.
export async function synthSpeech(text, opts = {}) {
    if (!text || !String(text).trim()) throw new Error('empty speech text');
    const engine = opts.engine || 'puter';
    // The `S`-row voice defaults to the sentinel 'default' (speechRow()); it is
    // NOT a real voice/language, so map it (and any empty value) to the engine's
    // actual default. Passing 'default' straight to Polly/Google fails their
    // language-code enum ("Value 'default' at 'languageCode' failed…").
    const rawVoice = opts.voice;
    const voice = (!rawVoice || rawVoice === 'default')
        ? (engine === 'google' ? 'en' : 'en-US')
        : rawVoice;
    const key = engine + '|' + voice + '|' + text;

    // Cache hit → decode the stored WAV.
    try {
        const cached = await cacheGet(key);
        if (cached) return decodeFile(cached);
    } catch (e) { /* cache is best-effort */ }

    const buf = (engine === 'google') ? await synthViaGoogle(text, voice)
                                      : await synthViaPuter(text, voice);
    // Store for reuse (best-effort; don't fail synth on cache write errors).
    try { await cachePut(key, wavBlob(audioBufferToWav16(buf)), Math.round(buf.duration * 1000)); }
    catch (e) { /* ignore */ }
    return buf;
}

// Preview a line aloud using the browser's built-in speechSynthesis. This does
// NOT feed the mix (its audio can't be captured) — audition only. `voice` is a
// BCP-47 language hint; we pick the first matching installed voice if any.
export function previewSpeech(text, voice) {
    if (typeof speechSynthesis === 'undefined') return false;
    speechSynthesis.cancel();
    const u = new SpeechSynthesisUtterance(String(text || ''));
    if (voice && voice !== 'default') {
        u.lang = voice;
        const match = speechSynthesis.getVoices().find(v => v.lang && v.lang.toLowerCase().indexOf(voice.toLowerCase()) === 0);
        if (match) u.voice = match;
    }
    speechSynthesis.speak(u);
    return true;
}
