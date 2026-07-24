// Unit tests for the browser session logger buffer (no DOM; window is undefined
// under Node so the global error listeners are simply skipped).
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    startClientLog, stopClientLog, getClientLogText, clog, clogI, clogW, clogE, isClientLogging,
} from '../src/js/clientlog.js';

test('clientlog: records only while active, and resets on start', () => {
    // Before start: not capturing; a call is mirrored to console but not buffered.
    clogI('pre', 'ignored before start');
    startClientLog();
    assert.equal(isClientLogging(), true);
    clogI('ws', 'open');
    clogW('ws', 'ring low');
    clogE('ws', 'connection error');
    const txt = getClientLogText();
    assert.match(txt, /client log started/);
    assert.match(txt, /ws: open/);
    assert.match(txt, /WARN ws: ring low/);
    assert.match(txt, /ERR  ws: connection error/);
    assert.doesNotMatch(txt, /ignored before start/);

    // A fresh start clears the buffer.
    startClientLog();
    assert.doesNotMatch(getClientLogText(), /connection error/);
    stopClientLog();
    assert.equal(isClientLogging(), false);
});

test('clientlog: serializes Errors and objects without throwing', () => {
    startClientLog();
    clog('error', 'bounce', new Error('TTS boom'));
    clog('info', 'http', { status: 500 });
    const txt = getClientLogText();
    assert.match(txt, /Error: TTS boom/);
    assert.match(txt, /"status":500/);
    stopClientLog();
});
