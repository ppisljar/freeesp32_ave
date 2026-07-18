// Tests for the pure helpers of the BG WebSocket raw-PCM push
// (bg_websocket_pcm_push_plan.md). The stateful pushBgWs needs a real
// WebSocket + DOM, so only the pure pieces are unit-tested here.

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { wsHandshakeMsg, wsUrl, WS_CHUNK_BYTES } from '../src/js/gen/transport.js';

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
