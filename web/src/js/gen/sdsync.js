// Sync session audio to the device's SD card, so sessions can run with no
// browser attached (plans/sd_offline_sessions_plan.md).
//
// The card holds a LIBRARY, not per-session mixes:
//     /sdcard/speech/sp_<hash>.wav   one file per unique phrase
//     /sdcard/bg/<name>.wav          shared background tracks
// Phrases are reused across sessions, so adding a session is nearly free and
// syncing is incremental — we upload only what is missing.
//
// The device resolves an `S` row to a filename by hashing the phrase itself,
// which is why speechFilename() below MUST stay byte-identical to
// speech_player_filename() in main/speech_player.c. A divergence is silent:
// the device would look for files the browser never uploaded, and you would
// simply get no speech. test/sdsync.test.js pins both to the same values.

import { synthSpeech, getEngine, SPEECH_RATE } from './tts.js';
import { collectPhrases } from './ttspreload.js';
import { audioBufferToWav16 } from './bgaudio.js';
import { clogI, clogW, clogE } from '../clientlog.js';

import { deviceFetch } from '../devicefetch.js';
// FNV-1a 32-bit. Must match main/speech_player.c exactly.
// `| 0` then `>>> 0` keeps JS in 32-bit unsigned space; Math.imul does the
// 32-bit multiply C does natively (a plain `*` loses precision past 2^53).
export function fnv1a32(str) {
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) {
        h ^= str.charCodeAt(i) & 0xff;
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h >>> 0;
}

// "speech/sp_xxxxxxxx.wav" for a phrase. Mirrors speech_player_filename().
// NOTE: hashes the raw bytes of `voice|text` as the firmware sees them. Only
// ASCII is exercised today; non-ASCII would need both sides to agree on an
// encoding, so keep phrases ASCII until that is settled.
export function speechFilename(voice, text) {
    const v = (voice && String(voice).trim() && voice !== 'default') ? voice : 'default';
    const key = v + '|' + (text || '');
    return 'speech/sp_' + fnv1a32(key).toString(16).padStart(8, '0') + '.wav';
}

// ---- device file list ------------------------------------------------------

export async function listCard() {
    const r = await deviceFetch('/api/sd', { cache: 'no-store' });
    if (!r.ok) throw new Error('/api/sd HTTP ' + r.status);
    return r.json();   // { mounted, files:[{name,size}], total_bytes, free_bytes, card }
}

async function uploadFile(name, blob) {
    // A phrase WAV is a few hundred KB over wifi to a microcontroller writing
    // it to an SD card — comfortably past deviceFetch's default deadline.
    const r = await deviceFetch('/api/sd/' + name, {
        method: 'PUT', body: blob, timeoutMs: 60000,
    });
    if (!r.ok) throw new Error('upload ' + name + ' failed: HTTP ' + r.status);
    return r.json();
}

// ---- the sync ---------------------------------------------------------------

// Upload every speech phrase the library needs that the card does not already
// have. Returns { mounted, total, uploaded, skipped, failed, errors }.
//
// onProgress({ done, total, uploaded, skipped, current }) is called per phrase.
export async function syncSpeechToCard(onProgress) {
    const card = await listCard();
    if (!card.mounted) {
        return { mounted: false, total: 0, uploaded: 0, skipped: 0, failed: 0, errors: [] };
    }

    const have = new Set((card.files || []).map(f => f.name));
    const { engine, phrases } = await collectPhrases();
    clogI('sdsync', `card has ${have.size} file(s); library needs ${phrases.length} phrase(s)`);

    let uploaded = 0, skipped = 0, failed = 0, done = 0;
    const errors = [];
    const emit = (current) => onProgress && onProgress({
        done, total: phrases.length, uploaded, skipped, current,
    });
    emit(null);

    for (const p of phrases) {
        const name = speechFilename(p.voice, p.text);
        if (have.has(name)) { skipped++; done++; emit(p.text); continue; }
        try {
            // synthSpeech serves from the browser's IndexedDB phrase cache when
            // it can, so a preloaded library syncs without re-hitting the TTS
            // engine at all.
            const buf = await synthSpeech(p.text, { engine, voice: p.voice });
            const wav = audioBufferToWav16(buf);
            await uploadFile(name, new Blob([wav], { type: 'audio/wav' }));
            have.add(name);
            uploaded++;
        } catch (e) {
            failed++;
            errors.push({ text: p.text, name, error: String((e && e.message) || e) });
            clogW('sdsync', 'failed ' + name + ': ' + e);
        }
        done++;
        emit(p.text);
    }

    clogI('sdsync', `done: ${uploaded} uploaded, ${skipped} already present, ${failed} failed`);
    return { mounted: true, total: phrases.length, uploaded, skipped, failed, errors };
}

// True when every phrase this session needs is already on the card, i.e. the
// session can play with no browser attached. Used by play.js to decide whether
// to skip the bounce-and-stream path entirely.
export async function sessionIsOffline(doc) {
    const rows = (doc && doc.rows) || [];
    const speech = rows.filter(r => r.kind === 'speech' && r.text && r.text.trim());
    if (!speech.length) return true;            // nothing to fetch

    let card;
    try { card = await listCard(); } catch (e) { return false; }
    if (!card.mounted) return false;

    const have = new Set((card.files || []).map(f => f.name));
    return speech.every(r => have.has(speechFilename(r.voice, r.text)));
}
