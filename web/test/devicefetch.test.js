// The gate that keeps the page from strangling the device. These tests exist
// because the failure it prevents is invisible in normal use and catastrophic
// when it happens: once the page holds all of Chrome's 6 per-host connections,
// every request queues forever, with no error and no timeout. See devicefetch.js.

import { test } from 'node:test';
import assert from 'node:assert/strict';

// Fresh module per test — the gate holds process-wide state.
async function freshGate() {
    const mod = await import('../src/js/devicefetch.js?t=' + Math.random());
    return mod;
}

function deferredFetch() {
    const calls = [];
    globalThis.fetch = (url, opts = {}) => new Promise((resolve, reject) => {
        calls.push({ url, opts, resolve, reject });
    });
    return calls;
}

test('never exceeds MAX_INFLIGHT concurrent requests to the device', async () => {
    const { deviceFetch, deviceFetchStats } = await freshGate();
    const calls = deferredFetch();

    // Ten distinct URLs so nothing is deduped — the pure concurrency case.
    const ps = [];
    for (let i = 0; i < 10; i++) ps.push(deviceFetch('/api/x' + i));
    await new Promise(r => setImmediate(r));

    const max = deviceFetchStats().max;
    assert.equal(calls.length, max, `only ${max} should reach the network`);
    assert.equal(deviceFetchStats().queued, 10 - max);

    // Completing one admits exactly one more — never a burst.
    calls[0].resolve(new Response('ok'));
    await new Promise(r => setImmediate(r));
    assert.equal(calls.length, max + 1);

    // Drain: each completion admits another queued job, which calls the stub
    // again — so keep flushing until the queue stops producing new work.
    for (let guard = 0; guard < 50 && calls.length; guard++) {
        const pending = calls.splice(0);
        pending.forEach(c => c.resolve(new Response('ok')));
        await new Promise(r => setImmediate(r));
    }
    await Promise.allSettled(ps);
    assert.equal(deviceFetchStats().queued, 0, 'queue drains fully');
});

test('identical concurrent GETs cost the device one request', async () => {
    const { deviceFetch } = await freshGate();
    const calls = deferredFetch();

    const a = deviceFetch('/api/state');
    const b = deviceFetch('/api/state');
    const c = deviceFetch('/api/state');
    await new Promise(r => setImmediate(r));
    assert.equal(calls.length, 1, 'three callers, one request');

    calls[0].resolve(new Response('{"v":1}'));
    const bodies = await Promise.all([a, b, c].map(p => p.then(r => r.text())));
    // Each caller gets its own readable body, not a consumed one.
    assert.deepEqual(bodies, ['{"v":1}', '{"v":1}', '{"v":1}']);
});

test('writes are never shared — that would silently drop one', async () => {
    const { deviceFetch } = await freshGate();
    const calls = deferredFetch();

    deviceFetch('/api/sd/a.wav', { method: 'PUT', body: '1' });
    deviceFetch('/api/sd/a.wav', { method: 'PUT', body: '2' });
    await new Promise(r => setImmediate(r));
    assert.equal(calls.length, 2, 'both PUTs must reach the device');
});

test('a failed request does not poison the dedupe entry', async () => {
    const { deviceFetch } = await freshGate();
    const calls = deferredFetch();

    const p = deviceFetch('/api/state');
    await new Promise(r => setImmediate(r));
    calls[0].reject(new Error('boom'));
    await assert.rejects(p);
    await new Promise(r => setImmediate(r));

    // The next caller must get a real attempt, not the cached rejection.
    deviceFetch('/api/state');
    await new Promise(r => setImmediate(r));
    assert.equal(calls.length, 2);
});

test('gate:false bypasses the queue so a long upload cannot starve polling', async () => {
    const { deviceFetch, deviceFetchStats } = await freshGate();
    const calls = deferredFetch();

    deviceFetch('/api/bg-stream', { method: 'POST', body: 'x', gate: false });
    await new Promise(r => setImmediate(r));
    assert.equal(calls.length, 1);
    assert.equal(deviceFetchStats().inflight, 0, 'holds no gate slot');
});

test('a slow request releases its slot on timeout instead of leaking a socket', async () => {
    const { deviceFetch } = await freshGate();
    let gotSignal = null;
    globalThis.fetch = (url, opts = {}) => { gotSignal = opts.signal; return new Promise(() => {}); };

    deviceFetch('/api/state', { timeoutMs: 20 });
    await new Promise(r => setImmediate(r));
    assert.ok(gotSignal, 'a deadline must be attached');
    await new Promise(r => setTimeout(r, 60));
    assert.equal(gotSignal.aborted, true, 'request aborted at its deadline');
});
