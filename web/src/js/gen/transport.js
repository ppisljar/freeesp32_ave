// Transport helpers — talk to the device's existing HTTP endpoints.
// Reused/adapted from config.js's play/stop logic; no new endpoints.

import { serializeForDevice } from './serialize.js';
import { NUM_AUDIO_CHANNELS, NUM_LED_CHANNELS } from './model.js';
import { clogI, clogW, clogE } from '../clientlog.js';

// Serialize the model and POST it to /api/play-config (text/plain). Stops any
// running timeline on the device and starts this one. Speech (`S`) rows are
// stripped by serializeForDevice — the firmware can't parse them (they're
// browser-only, mixed into the bounced WAV).
export function playDoc(doc) {
    const body = serializeForDevice(doc);
    clogI('http', 'POST /api/play-config (' + body.length + ' bytes)');
    return fetch('/api/play-config', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body,
    }).then(r => {
        if (!r.ok) clogE('http', '/api/play-config → HTTP ' + r.status);
        else clogI('http', '/api/play-config → ' + r.status);
        return r.text();
    }).catch(err => { clogE('http', '/api/play-config failed:', err); throw err; });
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
    clogI('http', 'POST /api/stop');
    return fetch('/api/stop', { method: 'POST' }).then(r => r.text())
        .catch(err => { clogE('http', '/api/stop failed:', err); throw err; });
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
// Active BG WebSocket (raw-PCM push, bg_websocket_pcm_push_plan.md). Only one at
// a time; a new pushBgWs supersedes it, and stopBg/stopBgWs close it.
let s_bgWs = null;

// Throttle-proof pump scheduling. setTimeout/setInterval are throttled to ~1/60s
// in a backgrounded / screen-locked tab, which STALLS the PCM feed and starves
// the device ring (the observed once-a-minute underrun bursts). MessageChannel
// postMessage is NOT subject to that throttling, so while the tab is hidden we
// re-arm the pump through it; while visible we use the cheaper setTimeout. See
// websocket_bg_push_reliability_investigation.md (Regime 2).
const s_pumpChan = (typeof MessageChannel !== 'undefined') ? new MessageChannel() : null;
let s_pumpTask = null;
if (s_pumpChan) {
    s_pumpChan.port1.onmessage = () => { const t = s_pumpTask; s_pumpTask = null; if (t) t(); };
    // Node's MessagePort holds the event loop open, so importing this module
    // would hang `node --test` forever. Browsers have no unref() — no-op there.
    if (typeof s_pumpChan.port1.unref === 'function') s_pumpChan.port1.unref();
}
function armPump(fn, ms) {
    const hidden = (typeof document !== 'undefined') && document.hidden;
    if (hidden && s_pumpChan) { s_pumpTask = fn; s_pumpChan.port2.postMessage(0); }
    else setTimeout(fn, ms);
}

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
    clogI('bg-http', 'POST /api/bg-stream (' + (wavBlob.size || '?') + ' bytes, pan=' + pan + ' loudness=' + loudness + ')');
    return fetch('/api/bg-stream' + q, {
        method: 'POST',
        headers: { 'Content-Type': 'audio/wav' },
        body: wavBlob,
        signal: ctl.signal,
    }).then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        clogI('bg-http', '/api/bg-stream done → ' + r.status);
        return r.json();
    }).catch(err => {
        // A deliberate stopBg()/supersede abort is not an error to surface.
        if (err && err.name === 'AbortError') { clogI('bg-http', '/api/bg-stream aborted (stop/supersede)'); return { ok: true, aborted: true }; }
        clogE('bg-http', '/api/bg-stream failed:', err);
        throw err;
    }).finally(() => { if (s_pushAbort === ctl) s_pushAbort = null; });
}

// ---- BG WebSocket raw-PCM push (bg_websocket_pcm_push_plan.md) --------------

// Max WS binary frame payload — MUST stay <= the device's BG_WS_RECV_BYTES.
export const WS_CHUNK_BYTES = 16384;
// Pause sending while this many bytes sit unsent in the socket (browser-side
// backpressure — the device also paces us via TCP + its ring back-channel).
export const WS_HIGH_WATER = 512 * 1024;
// Give up if the device hasn't completed the WS handshake in this long. Without
// it a device that never accepts the connection (its httpd out of sockets) is
// only noticed when the BROWSER gives up — ~240 s in Chrome — and the session
// meanwhile plays through with no speech and no visible error. The device
// answers in milliseconds when healthy, so 10 s is already generous.
export const WS_OPEN_TIMEOUT_MS = 10000;

// The handshake text frame the device expects before any PCM. Pure → testable.
export function wsHandshakeMsg(pan = 0, loudness = 50) {
    return JSON.stringify({ pan, loudness, rate: 44100, bits: 16, ch: 2 });
}

// ws:// (or wss:// behind TLS) URL for the device's BG WebSocket, same-origin so
// it works in SoftAP mode. Pure → testable with an injected location.
export function wsUrl(loc = (typeof location !== 'undefined' ? location : null)) {
    const proto = (loc && loc.protocol === 'https:') ? 'wss://' : 'ws://';
    const host = loc ? loc.host : 'localhost';
    return proto + host + '/api/bg-ws';
}

// Stream headerless 44.1 kHz / 16-bit / stereo LE PCM to the device over a
// WebSocket. NO on-device decode → no decode artifacts. Sends the handshake,
// then pumps `pcmBytes` (Uint8Array) in WS_CHUNK_BYTES frames, pausing when the
// socket's bufferedAmount is high. Resolves when the clip has been sent and the
// socket closes; rejects on WS error. Supersedes any prior WS push.
export function pushBgWs(pcmBytes, { pan = 0, loudness = 50, onProgress,
                                     openTimeoutMs = WS_OPEN_TIMEOUT_MS } = {}) {
    return new Promise((resolve, reject) => {
        if (s_bgWs) { try { s_bgWs.close(1000, 'supersede'); } catch (e) { /* ignore */ } s_bgWs = null; }
        let ws;
        const url = wsUrl();
        clogI('ws', 'connecting ' + url + ' (' + pcmBytes.length + ' PCM bytes, pan=' + pan + ' loudness=' + loudness + ')');
        try { ws = new WebSocket(url); }
        catch (e) { clogE('ws', 'construct failed:', e); reject(e); return; }
        ws.binaryType = 'arraybuffer';
        s_bgWs = ws;

        let off = 0;
        let sending = false;
        let sentAll = false;
        // The promise settles exactly once: whichever of open-timeout / error /
        // close gets there first wins, and the later ones stay quiet.
        let settled = false;
        let openTimer = null;
        function settle(fn) {
            if (settled) return;
            settled = true;
            if (openTimer !== null) { clearTimeout(openTimer); openTimer = null; }
            if (s_bgWs === ws) s_bgWs = null;
            fn();
        }

        openTimer = setTimeout(() => {
            openTimer = null;
            if (ws.readyState === WebSocket.OPEN) return;      // opened just in time
            const secs = Math.round(openTimeoutMs / 1000);
            clogE('ws', 'handshake timed out after ' + secs + 's (readyState=' + ws.readyState
                      + ') — the device never accepted the connection');
            // Settle FIRST, then close: closing can fire onclose synchronously,
            // and the generic "closed before the clip finished" reason would
            // otherwise win the race and hide why this actually failed.
            settle(() => {
                try { ws.close(4000, 'open timeout'); } catch (e) { /* ignore */ }
                reject(new Error(
                    'No speech: the device did not accept the audio connection within ' + secs
                    + 's. Its web server is likely out of sockets — reboot the device and play again. '
                    + 'The session is running WITHOUT the spoken lines.'));
            });
        }, openTimeoutMs);

        function pump() {
            if (sending || ws.readyState !== WebSocket.OPEN) return;
            sending = true;
            while (off < pcmBytes.length) {
                if (ws.bufferedAmount > WS_HIGH_WATER) {          // let the socket drain
                    sending = false;
                    armPump(pump, 30);   // throttle-proof while backgrounded
                    return;
                }
                const end = Math.min(off + WS_CHUNK_BYTES, pcmBytes.length);
                ws.send(pcmBytes.subarray(off, end));             // view → sends those bytes, no copy
                off = end;
                if (onProgress) onProgress(off / pcmBytes.length);
            }
            sending = false;
            sentAll = true;
            clogI('ws', 'all PCM queued (' + off + ' bytes) — closing on drain');
            // All PCM queued — close once the socket has flushed so the device
            // sees a clean end-of-stream and drains its ring (natural completion).
            (function closeWhenDrained() {
                if (ws.readyState !== WebSocket.OPEN) return;
                if (ws.bufferedAmount > 0) { armPump(closeWhenDrained, 50); return; }
                try { ws.close(1000, 'eos'); } catch (e) { /* ignore */ }
            })();
        }

        ws.onopen = () => {
            if (openTimer !== null) { clearTimeout(openTimer); openTimer = null; }
            clogI('ws', 'open — sending handshake');
            ws.send(wsHandshakeMsg(pan, loudness));
            pump();
        };
        ws.onmessage = (ev) => {
            // Back-channel {consumed, ring_ms} — device ring fill. Used as active
            // flow control: WebSocket message events are NOT throttled in a
            // backgrounded tab, so a low-ring report kicks the pump even when
            // timers are throttled (belt-and-braces with the MessageChannel pump).
            try {
                const m = JSON.parse(ev.data);
                if (m && typeof m.ring_ms === 'number') {
                    if (m.ring_ms < 200) {
                        clogW('ws', 'device ring low: ' + m.ring_ms + ' ms (underrun risk)');
                        if (!sending && !sentAll) armPump(pump, 0);   // feed now
                    }
                    if (onProgress) { /* ring_ms hook for UI */ }
                }
            } catch (e) { /* ignore non-JSON */ }
        };
        ws.onclose = (ev) => {
            settle(() => {
                if (sentAll) { clogI('ws', 'closed cleanly after EOS (code=' + ev.code + ')'); resolve({ ok: true }); }
                else { clogE('ws', 'closed BEFORE clip finished (code=' + ev.code + ' reason="' + (ev.reason || '') + '" clean=' + ev.wasClean + ', sent ' + off + '/' + pcmBytes.length + ' bytes)'); reject(new Error('bg-ws closed before the clip finished sending')); }
            });
        };
        ws.onerror = () => {
            settle(() => {
                clogE('ws', 'connection error (sent ' + off + '/' + pcmBytes.length + ' bytes)');
                reject(new Error('bg-ws connection error'));
            });
        };
    });
}

// Close the active BG WebSocket (if any). Best-effort.
export function stopBgWs() {
    if (s_bgWs) { try { s_bgWs.close(1000, 'stop'); } catch (e) { /* ignore */ } s_bgWs = null; }
}

// Stop the background track: abort the browser's in-flight upload FIRST (so it
// stops sending immediately), then tell the device to tear down the BG player.
// Leaves any running timeline/audio untouched. Best-effort — never rejects.
export function stopBg() {
    if (s_pushAbort) { try { s_pushAbort.abort(); } catch (e) { /* ignore */ } s_pushAbort = null; }
    if (s_bgWs) { try { s_bgWs.close(1000, 'stop'); clogI('ws', 'closed by stopBg'); } catch (e) { /* ignore */ } s_bgWs = null; }
    return fetch('/api/bg-stream?stop=1', { method: 'POST' })
        .then(r => r.json())
        .catch(() => ({ ok: false }));
}
