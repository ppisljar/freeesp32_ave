// Session log capture.
//
// The device keeps only the last 32 KB of ESP_LOGx output in a WRAPPING PSRAM
// ring (see main/diagnostics.c: DIAG_LOG_RING_BYTES) — so a long session's early
// lines are overwritten on the device and lost. To preserve the WHOLE session
// log we poll GET /api/logs on a background timer (independent of the
// Diagnostics tab, so it runs no matter where the user is in the app) and stitch
// the overlapping ring snapshots into one growing buffer, then persist it to
// IndexedDB keyed by the session's report filename so it can be viewed later.

import { startClientLog, stopClientLog, getClientLogText } from './clientlog.js';

import { startPolling } from './poll.js';
import { deviceFetch } from './devicefetch.js';
const POLL_MS = 3000;
const DB_NAME = 'ave-logs';
const DB_VERSION = 1;
const STORE = 'sessionlogs';
const KEEP_MAX = 100;            // evict oldest logs beyond this many records

// ---- overlap stitching (pure, unit-tested) --------------------------------

// Length of the longest suffix of `a` that is also a prefix of `b`. Computed via
// the KMP prefix function over "b-prefix \x00 a-tail" (both capped to the min
// length; \x00 never appears in text logs so it's a safe separator).
export function overlapLen(a, b) {
    const m = Math.min(a.length, b.length);
    if (m === 0) return 0;
    const s = b.slice(0, m) + '\x00' + a.slice(a.length - m);
    const f = new Int32Array(s.length);
    for (let i = 1; i < s.length; i++) {
        let j = f[i - 1];
        while (j > 0 && s[i] !== s[j]) j = f[j - 1];
        if (s[i] === s[j]) j++;
        f[i] = j;
    }
    return f[s.length - 1];
}

// Append the genuinely-new tail of ring snapshot `snap` onto accumulated `acc`.
// `snap` is a suffix of the same stream `acc` is a prefix of, so its head
// overlaps acc's tail; only the part past the overlap is new.
export function mergeLogSnapshots(acc, snap) {
    if (!acc) return snap || '';
    if (!snap) return acc;
    const ov = overlapLen(acc, snap);
    if (ov === 0) {
        // No shared boundary: the ring wrapped past what we last saw (>32 KB
        // emitted between polls) or the log was cleared. Note the gap, keep both.
        return acc + '\n[…log ring wrapped — some device lines were lost…]\n' + snap;
    }
    return acc + snap.slice(ov);
}

// ---- capture lifecycle ----------------------------------------------------
let s_timer = null;
let s_acc = '';
let s_active = false;

function stopTimer() {
    if (s_timer) { s_timer(); s_timer = null; }   // s_timer is startPolling's stop()
    s_active = false;
}

async function pollOnce() {
    try {
        const r = await deviceFetch('/api/logs', { cache: 'no-store' });
        const txt = r.ok ? await r.text() : '';
        if (txt) s_acc = mergeLogSnapshots(s_acc, txt);
    } catch (e) { /* transient (busy httpd / network) — retry next tick */ }
}

// Begin capturing. Resets any prior in-flight buffer (e.g. a replay that never
// produced a report). Safe to call repeatedly.
export function startLogCapture() {
    stopTimer();
    s_acc = '';
    s_active = true;
    startClientLog();                        // browser-side log for the same window
    // startPolling runs it immediately and never overlaps — see poll.js.
    s_timer = startPolling(pollOnce, POLL_MS);
}

export function isCapturing() { return s_active; }

// Stop capturing and persist the accumulated log under `reportName`. Takes one
// final sample so the tail between the last tick and stop isn't dropped.
// Returns the stored byte length (0 if nothing captured or no name).
export async function finalizeLogCapture(reportName) {
    if (s_active) await pollOnce();
    stopTimer();
    const deviceText = s_acc;
    s_acc = '';
    const browserText = getClientLogText();
    stopClientLog();
    if (!reportName || (!deviceText && !browserText)) return 0;
    try {
        await putLog(reportName, deviceText, browserText);
        await evictOld();
    } catch (e) { /* IndexedDB unavailable — nothing to persist to */ }
    return deviceText.length + browserText.length;
}

// Stop capturing and throw away the buffers (no report to link to).
export function cancelLogCapture() { stopTimer(); s_acc = ''; stopClientLog(); }

// ---- IndexedDB persistence ------------------------------------------------
let _db = null;
function openDB() {
    if (_db) return _db;
    _db = new Promise((resolve, reject) => {
        if (typeof indexedDB === 'undefined') { reject(new Error('IndexedDB unavailable')); return; }
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = () => {
            const db = req.result;
            if (!db.objectStoreNames.contains(STORE)) {
                const os = db.createObjectStore(STORE, { keyPath: 'name' });
                os.createIndex('createdAt', 'createdAt');
            }
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error || new Error('indexedDB.open failed'));
    });
    return _db;
}

function putLog(name, deviceText, browserText) {
    return openDB().then(db => new Promise((resolve, reject) => {
        const tx = db.transaction(STORE, 'readwrite');
        tx.objectStore(STORE).put({
            name, createdAt: Date.now(),
            deviceText,  deviceBytes:  (deviceText  || '').length,
            browserText, browserBytes: (browserText || '').length,
        });
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    }));
}

// Look up the captured logs for a report name →
// { name, deviceText, deviceBytes, browserText, browserBytes } or null.
export function getStoredLog(name) {
    if (!name) return Promise.resolve(null);
    return openDB().then(db => new Promise((resolve, reject) => {
        const req = db.transaction(STORE, 'readonly').objectStore(STORE).get(name);
        req.onsuccess = () => resolve(req.result || null);
        req.onerror = () => reject(req.error);
    })).catch(() => null);
}

export function deleteStoredLog(name) {
    if (!name) return Promise.resolve();
    return openDB().then(db => new Promise((resolve) => {
        const tx = db.transaction(STORE, 'readwrite');
        tx.objectStore(STORE).delete(name);
        tx.oncomplete = () => resolve();
        tx.onerror = () => resolve();
    })).catch(() => {});
}

// Keep only the newest KEEP_MAX logs (oldest by createdAt evicted).
function evictOld() {
    return openDB().then(db => new Promise((resolve) => {
        const keys = [];
        const os = db.transaction(STORE, 'readonly').objectStore(STORE);
        os.index('createdAt').openCursor().onsuccess = (e) => {
            const cur = e.target.result;
            if (cur) { keys.push(cur.primaryKey); cur.continue(); return; }
            const excess = keys.length - KEEP_MAX;
            if (excess > 0) {
                const del = db.transaction(STORE, 'readwrite').objectStore(STORE);
                for (let i = 0; i < excess; i++) del.delete(keys[i]);
            }
            resolve();
        };
    })).catch(() => {});
}
