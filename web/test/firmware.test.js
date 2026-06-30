// Unit tests for the pure helpers in firmware.js (no DOM, no network).

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { bufToHex, looksLikeUpdater, buildUpdateUrl } from '../src/js/firmware.js';

test('bufToHex: lowercase, zero-padded hex of bytes', () => {
    assert.equal(bufToHex(new Uint8Array([0x00, 0x0f, 0xa0, 0xff])), '000fa0ff');
    assert.equal(bufToHex(new Uint8Array([])), '');
    // Accepts an ArrayBuffer too.
    const u = new Uint8Array([1, 2, 3]);
    assert.equal(bufToHex(u.buffer), '010203');
});

test('looksLikeUpdater: distinguishes updater text from main-app HTML', () => {
    assert.equal(looksLikeUpdater('OTA updater ready (running: ota_1 @ 0x210000)'), true);
    assert.equal(looksLikeUpdater('ota updater READY blah'), true);
    assert.equal(looksLikeUpdater('<!DOCTYPE html><html><head><title>ESP32'), false);
    assert.equal(looksLikeUpdater(''), false);
    assert.equal(looksLikeUpdater(null), false);
    assert.equal(looksLikeUpdater(undefined), false);
});

test('buildUpdateUrl: app target has no part, data targets do; hash always present', () => {
    assert.equal(buildUpdateUrl('', 'app', 'deadbeef'), '/update?sha256=deadbeef');
    assert.equal(buildUpdateUrl('', 'storage', 'abc'), '/update?part=storage&sha256=abc');
    assert.equal(buildUpdateUrl('', 'cfgfs', 'abc'), '/update?part=cfgfs&sha256=abc');
    // Works with an absolute base for the SoftAP recovery copy.
    assert.equal(buildUpdateUrl('http://192.168.4.1', 'app', 'ff'),
        'http://192.168.4.1/update?sha256=ff');
    assert.equal(buildUpdateUrl('http://192.168.4.1', 'storage', 'ff'),
        'http://192.168.4.1/update?part=storage&sha256=ff');
});
