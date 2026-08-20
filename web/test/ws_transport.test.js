// Tests for the BG WebSocket raw-PCM push (bg_websocket_pcm_push_plan.md):
// the pure helpers, plus pushBgWs's connect/settle behaviour driven through a
// stub WebSocket.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { wsHandshakeMsg, wsUrl, WS_CHUNK_BYTES, WS_OPEN_TIMEOUT_MS,
         pushBgWs } from '../src/js/gen/transport.js';

test('wsHandshakeMsg encodes the device format contract', () => {
    const m = JSON.parse(wsHandshakeMsg(-40, 75));
    assert.equal(m.pan, -40);
    assert.equal(m.loudness, 75);
    assert.equal(m.rate, 44100);
    assert.equal(m.bits, 16);
    assert.equal(m.ch, 2);
});

test('wsHandshakeMsg defaults are centered / half loudness', () => {
    const m = JSON.parse(wsHandshakeMsg());
    assert.equal(m.pan, 0);
    assert.equal(m.loudness, 50);
});

test('wsUrl is same-origin ws:// over http and wss:// over https', () => {
    assert.equal(wsUrl({ protocol: 'http:', host: '192.168.4.1' }), 'ws://192.168.4.1/api/bg-ws');
    assert.equal(wsUrl({ protocol: 'https:', host: 'device.local' }), 'wss://device.local/api/bg-ws');
});

test('WS_CHUNK_BYTES stays within the device BG_WS_RECV_BYTES (16384)', () => {
    assert.ok(WS_CHUNK_BYTES <= 16384, 'chunk must fit one device WS recv buffer');
});

// ---- pushBgWs: connect timeout / settle-once ------------------------------
// Stub socket: never opens unless the test calls open(), records close() calls.
class StubWS {
    static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
    constructor(url) { this.url = url; this.readyState = 0; this.bufferedAmount = 0; this.sent = []; this.closes = []; StubWS.last = this; }
    send(d) { this.sent.push(d); }
    close(code, reason) { this.closes.push({ code, reason }); this.readyState = 3; if (this.onclose) this.onclose({ code, reason: reason || '', wasClean: code === 1000 }); }
    open() { this.readyState = 1; if (this.onopen) this.onopen(); }
}

function withStubWS(fn) {
    const prev = globalThis.WebSocket;
    globalThis.WebSocket = StubWS;
    return Promise.resolve().then(fn).finally(() => { globalThis.WebSocket = prev; });
}

test('WS_OPEN_TIMEOUT_MS is far below the browser\'s own ~240 s socket timeout', () => {
    assert.ok(WS_OPEN_TIMEOUT_MS <= 30000, 'must fail fast enough to be noticed mid-session');
});

test('pushBgWs: a handshake that never completes rejects instead of hanging', () => withStubWS(async () => {
    const err = await pushBgWs(new Uint8Array(64), { openTimeoutMs: 30 }).then(
        () => null, e => e);
    assert.ok(err, 'must reject');
    // The message has to tell the user what they lost and what to do about it.
    assert.match(err.message, /WITHOUT the spoken lines/);
    assert.match(err.message, /out of sockets/);
    // And the dead socket is closed rather than left dangling.
    assert.equal(StubWS.last.closes.length, 1);
    assert.equal(StubWS.last.closes[0].code, 4000);
}));

test('pushBgWs: no PCM is sent when the socket never opens', () => withStubWS(async () => {
    await pushBgWs(new Uint8Array(64), { openTimeoutMs: 30 }).catch(() => {});
    assert.deepEqual(StubWS.last.sent, []);
}));

test('pushBgWs: an opened socket sends the handshake + PCM and resolves on EOS close',
     () => withStubWS(async () => {
    const pcm = new Uint8Array(1000);
    const p = pushBgWs(pcm, { pan: -20, loudness: 80, openTimeoutMs: 5000 });
    StubWS.last.open();                       // handshake completes in time
    const res = await p;
    assert.deepEqual(res, { ok: true });
    assert.equal(JSON.parse(StubWS.last.sent[0]).loudness, 80);   // handshake first
    assert.equal(StubWS.last.sent[1].length, 1000);               // then the PCM
    assert.equal(StubWS.last.closes[0].code, 1000);               // clean EOS close
}));

test('pushBgWs: the open timeout does not fire after a successful connect',
     () => withStubWS(async () => {
    const p = pushBgWs(new Uint8Array(8), { openTimeoutMs: 20 });
    StubWS.last.open();
    await p;
    await new Promise(r => setTimeout(r, 60));   // past the timeout deadline
    assert.equal(StubWS.last.closes.length, 1, 'no second close from a stale timer');
}));
