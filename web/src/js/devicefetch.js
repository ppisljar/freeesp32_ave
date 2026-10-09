// The single gate every request to the device goes through.
//
// WHY THIS EXISTS
//
// The device is an ESP32 running esp_http_server with SEVEN sockets, three of
// which httpd reserves for itself. It is not a web server; it is a microcon-
// troller that also answers HTTP, on one task, while generating audio and
// driving LEDs under hard real-time deadlines. Treat it like a browser treats
// a CDN and it falls over.
//
// Chrome will happily open 6 connections per host, and the app has a dozen
// modules that each call fetch() whenever they feel like it, including four
// background pollers. When one endpoint turns slow — /api/state went to 10-20 s
// — the requests pile up until all 6 of Chrome's connections are occupied.
// After that, EVERY request from the page queues behind them indefinitely: the
// UI is dead, no error is raised, nothing times out, and curl from a terminal
// still answers in 40 ms because curl is not inside Chrome's exhausted pool.
// That last detail is what makes the failure so misleading — the device looks
// healthy from everywhere except the page that is strangling it.
//
// Per-call discipline cannot fix this, because the bug is emergent: every
// individual call site is reasonable, and the pile-up only happens when they
// coincide. So the limit lives HERE, in one place that all traffic must cross,
// and the rule is enforced structurally rather than by remembering to be
// careful at 48 call sites.
//
// WHAT IT GUARANTEES
//
//   1. At most MAX_INFLIGHT requests to the device at any instant. Everything
//      else waits in a queue. The page can never consume Chrome's connection
//      pool, so a slow endpoint degrades the UI instead of killing it.
//   2. Identical concurrent GETs share one request (single-flight). Three
//      modules asking for /api/state at the same moment cost the device one
//      response, not three.
//   3. Every request has a deadline. A stalled connection is released instead
//      of being held until the tab closes. An unbounded fetch is a leaked
//      socket, and seven is all we get.
//
// Use deviceFetch() for ALL device traffic. Plain fetch() to /api/... is a bug.
// The narrow exceptions are documented at their call sites: the WebSocket PCM
// push and the OTA upload, which are long-lived by nature, user-initiated, and
// never concurrent with themselves.

// Two, not one: a single slot would make a slow poll block user-initiated
// actions entirely, and two still leaves Chrome four idle connections as
// headroom so the page can never be the thing that exhausts the pool.
const MAX_INFLIGHT = 2;

// Default deadline. Generous enough for a busy httpd mid-session, short enough
// that a hung endpoint frees its slot promptly. Callers that are legitimately
// slow (uploads, OTA) pass their own.
export const DEFAULT_TIMEOUT_MS = 8000;

let inflight = 0;
const waiting = [];               // [{ run }] — FIFO
const dedupe = new Map();         // url -> Promise<Response> for in-flight GETs

function pump() {
    while (inflight < MAX_INFLIGHT && waiting.length) {
        const job = waiting.shift();
        inflight++;
        job.run();
    }
}

function withTimeout(url, options, timeoutMs) {
    if (options.signal) return fetch(url, options);         // caller owns aborting
    if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) {
        return fetch(url, { ...options, signal: AbortSignal.timeout(timeoutMs) });
    }
    const ac = new AbortController();
    const t = setTimeout(() => ac.abort(), timeoutMs);
    return fetch(url, { ...options, signal: ac.signal }).finally(() => clearTimeout(t));
}

// fetch() for the device. Same signature as fetch, plus:
//   timeoutMs  — per-request deadline (default DEFAULT_TIMEOUT_MS)
//   share      — join an identical in-flight request instead of issuing a new
//                one. Defaults to true for GET, false otherwise: sharing a POST
//                or PUT would silently drop a write.
//
// NOTE on shared responses: a Response body can only be read once, so shared
// callers each get r.clone(). Do not rely on identity of the Response object.
export function deviceFetch(url, options = {}) {
    const {
        timeoutMs = DEFAULT_TIMEOUT_MS,
        share,
        gate = true,
        ...rest
    } = options;

    // gate:false — a long-lived, user-initiated, self-serialising transfer (the
    // BG audio push, an OTA image). Holding one of only two slots for the
    // minutes such a request runs would starve every poller and make the UI
    // look frozen, which is the exact failure this module exists to prevent.
    // These still count against Chrome's pool, but there is never more than one
    // of them and the user is watching a progress bar while it runs.
    if (!gate) return withTimeout(url, rest, timeoutMs);

    const method = (rest.method || 'GET').toUpperCase();
    const shareable = share !== undefined ? share : method === 'GET';
    const key = shareable ? method + ' ' + url : null;

    if (key && dedupe.has(key)) {
        return dedupe.get(key).then(r => r.clone());
    }

    const p = new Promise((resolve, reject) => {
        waiting.push({
            run: () => {
                withTimeout(url, rest, timeoutMs)
                    .then(resolve, reject)
                    .finally(() => {
                        inflight--;
                        if (key) dedupe.delete(key);
                        pump();
                    });
            },
        });
        pump();
    });

    if (key) {
        dedupe.set(key, p);
        // A rejection must not leave a poisoned entry behind.
        p.catch(() => dedupe.delete(key));
        return p.then(r => r.clone());
    }
    return p;
}

// Convenience: GET and parse JSON. Throws on a non-2xx so callers do not have
// to remember that fetch resolves for 500s.
export async function deviceJson(url, options = {}) {
    const r = await deviceFetch(url, { cache: 'no-store', ...options });
    if (!r.ok) throw new Error(url + ' -> HTTP ' + r.status);
    return r.json();
}

// For diagnostics / tests: how much traffic is currently gated.
export function deviceFetchStats() {
    return { inflight, queued: waiting.length, shared: dedupe.size, max: MAX_INFLIGHT };
}
