// Silent-audio keep-alive (bg_websocket_pcm_push_plan.md — screen-lock mitigation).
//
// Mobile browsers freeze a page's JS / timers / WebSocket when the phone screen
// locks or the tab goes to the background — UNLESS the page is actively playing
// audio (the OS media-session exemption). Our page does NOT play audio locally
// (the ESP32 does); it only *streams* PCM. So on a lock the WS push pump freezes,
// the device ring starves, and playback clicks/drops.
//
// Fix: while a push is in flight, play a looping *silent* <audio> element. That
// earns the background-audio exemption so the pump keeps running with the screen
// off. Works in an insecure context (plain http on SoftAP) — unlike Wake Lock,
// which needs a secure context we don't have. Must be started from within a user
// gesture (the Play button) for autoplay to allow it.
//
// Caveat: iOS Safari is the strictest; a silent element usually keeps the page
// alive but behaviour varies by OS version. Best-effort — never throws.

let s_audio = null;   // the looping <audio> element
let s_url   = null;   // its object URL (revoked on stop)
let s_refs  = 0;      // ref count so nested start/stop pairs behave

// Build a tiny (~0.05 s) mono 16-bit silent WAV and return an object URL.
function silentWavUrl() {
    const sr = 8000, n = Math.floor(sr * 0.05);
    const buf = new ArrayBuffer(44 + n * 2);
    const dv = new DataView(buf);
    const w = (o, s) => { for (let i = 0; i < s.length; i++) dv.setUint8(o + i, s.charCodeAt(i)); };
    w(0, 'RIFF'); dv.setUint32(4, 36 + n * 2, true); w(8, 'WAVE'); w(12, 'fmt ');
    dv.setUint32(16, 16, true); dv.setUint16(20, 1, true); dv.setUint16(22, 1, true);
    dv.setUint32(24, sr, true); dv.setUint32(28, sr * 2, true); dv.setUint16(32, 2, true);
    dv.setUint16(34, 16, true); w(36, 'data'); dv.setUint32(40, n * 2, true);
    // Sample bytes are already zero → silence.
    return URL.createObjectURL(new Blob([buf], { type: 'audio/wav' }));
}

// Start (ref-counted) the keep-alive. Call from a user gesture. Best-effort:
// returns a promise that always resolves, even if autoplay is blocked.
export function startKeepAlive() {
    s_refs++;
    if (s_audio) return Promise.resolve();
    if (typeof Audio === 'undefined') return Promise.resolve();   // non-browser / SSR
    try {
        s_url = silentWavUrl();
        s_audio = new Audio(s_url);
        s_audio.loop = true;
        // Unmuted + silent content: audible-to-the-OS (grants the exemption) but
        // inaudible to the user. A *muted* element would autoplay freely but iOS
        // may not treat it as a live audio session.
        return Promise.resolve(s_audio.play()).catch(() => { /* autoplay blocked — ignore */ });
    } catch (e) {
        return Promise.resolve();
    }
}

// Stop (ref-counted). Only tears down once the last holder releases.
export function stopKeepAlive() {
    if (s_refs > 0) s_refs--;
    if (s_refs > 0) return;
    if (s_audio) {
        try { s_audio.pause(); s_audio.removeAttribute('src'); s_audio.load(); } catch (e) { /* ignore */ }
        s_audio = null;
    }
    if (s_url) { try { URL.revokeObjectURL(s_url); } catch (e) { /* ignore */ } s_url = null; }
}
