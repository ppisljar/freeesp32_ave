// Transport helpers — talk to the device's existing HTTP endpoints.
// Reused/adapted from config.js's play/stop logic; no new endpoints.

import { serializeForDevice } from './serialize.js';
import { NUM_AUDIO_CHANNELS, NUM_LED_CHANNELS } from './model.js';

// Serialize the model and POST it to /api/play-config (text/plain). Stops any
// running timeline on the device and starts this one. Speech (`S`) rows are
// stripped by serializeForDevice — the firmware can't parse them (they're
// browser-only, mixed into the bounced WAV).
export function playDoc(doc) {
    const body = serializeForDevice(doc);
    return fetch('/api/play-config', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body,
    }).then(r => r.text());
}

// POST raw .ledc text to /api/play-config (caller has already filtered it, e.g.
// dropped A + S rows for a "bounce all" play where the device only runs LEDs).
export function playConfigText(text) {
    return fetch('/api/play-config', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: text,
    }).then(r => r.text());
}

// Stop all audio + LED (drops the running timeline).
export function stop() {
    return fetch('/api/stop', { method: 'POST' }).then(r => r.text());
}

// Apply a single live-patch line (additive; does not restart the timeline).
// `line` is one .ledc-format line of text.
export function patchLine(line) {
    return fetch('/api/patch-config', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: line,
    }).then(r => r.text());
}

// Snapshot of current per-channel engine state (JSON).
export function getState() {
    return fetch('/api/state').then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
    });
}

// Channel/colour capabilities. Reads caps from /api/state if present, else
// falls back to the compile-time NUM_* constants.
export function getCaps() {
    return getState()
        .then(s => {
            const c = (s && s.caps) || {};
            return {
                num_audio_ch: c.num_audio_ch || NUM_AUDIO_CHANNELS,
                num_led_ch:   c.num_led_ch   || NUM_LED_CHANNELS,
                led_color:    (c.led_color === undefined) ? true : !!c.led_color,
            };
        })
        .catch(() => ({
            num_audio_ch: NUM_AUDIO_CHANNELS,
            num_led_ch:   NUM_LED_CHANNELS,
            led_color:    true,
        }));
}

// ---- BG browser-push (bg_browser_push_plan.md) -----------------------------

// AbortController for the in-flight BG push fetch. A push POST streams the WHOLE
// bounced clip (can be hundreds of MB for a 30-min session), paced by TCP
// backpressure over the entire session. Without a handle to abort it, hitting
// Stop leaves the browser still uploading — so the device keeps receiving/playing
// BG audio after the timeline stopped. stopBg() aborts this.
let s_pushAbort = null;

// Stream a canonical WAV Blob to the device as the active background track.
// The POST body is fixed-length (a Blob), so it works over the device's
// HTTP/1.1 server; TCP flow control paces the upload to playback rate. The
// returned promise resolves when the device has finished consuming the clip
// (natural completion) — which is when a client-driven loop should re-POST.
export function pushBg(wavBlob, { pan = 0, loudness = 50 } = {}) {
    const q = '?pan=' + encodeURIComponent(pan) + '&loudness=' + encodeURIComponent(loudness);
    // Supersede any earlier push and get an abort handle for this one.
    if (s_pushAbort) { try { s_pushAbort.abort(); } catch (e) { /* ignore */ } }
    const ctl = new AbortController();
    s_pushAbort = ctl;
    return fetch('/api/bg-stream' + q, {
        method: 'POST',
        headers: { 'Content-Type': 'audio/wav' },
        body: wavBlob,
        signal: ctl.signal,
    }).then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
    }).catch(err => {
        // A deliberate stopBg()/supersede abort is not an error to surface.
        if (err && err.name === 'AbortError') return { ok: true, aborted: true };
        throw err;
    }).finally(() => { if (s_pushAbort === ctl) s_pushAbort = null; });
}

// Stop the background track: abort the browser's in-flight upload FIRST (so it
// stops sending immediately), then tell the device to tear down the BG player.
// Leaves any running timeline/audio untouched. Best-effort — never rejects.
export function stopBg() {
    if (s_pushAbort) { try { s_pushAbort.abort(); } catch (e) { /* ignore */ } s_pushAbort = null; }
    return fetch('/api/bg-stream?stop=1', { method: 'POST' })
        .then(r => r.json())
        .catch(() => ({ ok: false }));
}
