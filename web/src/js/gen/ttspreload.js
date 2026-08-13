// Bulk TTS preload. Walks the S (speech) rows of every config (device SPIFFS +
// browser-local), synthesizes each UNIQUE phrase for the active engine, and
// lets synthSpeech() cache the result in IndexedDB. Once complete the browser
// can bounce any session's speech fully OFFLINE — the whole point being field
// use on the road (SoftAP / phone hotspot with no reliable internet).
//
// Requires network + the currently-selected engine while preloading. Phrases
// already cached are skipped without a network call (synthSpeech cache-hit).

import { getEngine, synthSpeech, SPEECH_RATE } from './tts.js';
import { parse } from './parse.js';
import { cacheGet } from './bgstore.js';
import { allConfigTexts } from '../configstore.js';

// The effective voice mapping MUST mirror synthSpeech() so our dedup + cache
// existence keys line up exactly with what synthSpeech stores.
export function effVoice(engine, voice) {
    return (!voice || voice === 'default')
        ? (engine === 'google' ? 'en' : 'en-US')
        : voice;
}

// Pure extraction: dedup the unique speech phrases across an array of config
// text strings for a given engine. No IO — unit-testable. Returns
// [{ key, voice, text }] in first-seen order.
export function extractPhrases(engine, texts) {
    const seen = new Map();   // key -> { key, voice, text }
    for (const text of texts) {
        let doc;
        try { doc = parse(text).doc; } catch (e) { continue; }
        for (const r of (doc && doc.rows) || []) {
            if (!r || r.kind !== 'speech') continue;
            const t = r.text && String(r.text).trim();
            if (!t) continue;
            const v = effVoice(engine, r.voice);
            const key = engine + '|' + v + '|r' + SPEECH_RATE + '|' + r.text;
            if (!seen.has(key)) seen.set(key, { key, voice: r.voice || 'default', text: r.text });
        }
    }
    return [...seen.values()];
}

// Gather the unique speech phrases across all configs for the active engine.
// Returns { engine, phrases: [{ key, voice, text }] }.
export async function collectPhrases() {
    const engine = getEngine();
    const configs = await allConfigTexts();
    return { engine, phrases: extractPhrases(engine, configs.map(c => c.text)) };
}

// Preload every phrase. onProgress({ done, total, ok, failed, cached, current }).
// Returns { engine, total, ok, failed, cached, errors }. Processes sequentially
// to avoid hammering / rate-limiting the TTS backend; individual failures are
// collected, not fatal.
export async function preloadAllSpeech(onProgress) {
    const { engine, phrases } = await collectPhrases();
    const total = phrases.length;
    let done = 0, ok = 0, failed = 0, cached = 0;
    const errors = [];
    const emit = (current) => onProgress && onProgress({ done, total, ok, failed, cached, current });
    emit(null);
    for (const p of phrases) {
        try {
            // Skip a network round-trip if it's already cached.
            let hit = null;
            try { hit = await cacheGet(p.key); } catch (e) { /* treat as miss */ }
            if (hit) { cached++; }
            else { await synthSpeech(p.text, { engine, voice: p.voice }); ok++; }
        } catch (e) {
            failed++;
            errors.push({ text: p.text, error: String((e && e.message) || e) });
        }
        done++;
        emit(p.text);
    }
    return { engine, total, ok, failed, cached, errors };
}
