// Browser BG clip library (bg_browser_push_plan.md, Phase 2).
//
// IndexedDB store for background-audio clips. localStorage (used by
// configstore.js / reportstore.js) is only kilobytes; audio clips are
// megabytes, so they live in IndexedDB instead. One object store `bgclips`
// keyed by name, each record:
//   { name, createdAt, durationMs, sourceKind, wavBlob }
//     sourceKind: 'noise' | 'drone' | 'file' | 'bounce'
//     wavBlob:    a Blob of canonical 44100/16/stereo WAV (see bgaudio.js)
//
// The metadata-vs-blob split (metaOf) lets the UI list clips cheaply without
// pulling every blob into memory.

const DB_NAME = 'ave-bg';
const DB_VERSION = 1;
const STORE = 'bgclips';

let _dbPromise = null;

function openDB() {
    if (_dbPromise) return _dbPromise;
    _dbPromise = new Promise((resolve, reject) => {
        if (typeof indexedDB === 'undefined') {
            reject(new Error('IndexedDB unavailable'));
            return;
        }
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = () => {
            const db = req.result;
            if (!db.objectStoreNames.contains(STORE)) {
                db.createObjectStore(STORE, { keyPath: 'name' });
            }
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error || new Error('indexedDB.open failed'));
    });
    return _dbPromise;
}

function tx(db, mode) {
    return db.transaction(STORE, mode).objectStore(STORE);
}

// Strip the (large) blob for list views.
export function metaOf(rec) {
    return {
        name: rec.name,
        createdAt: rec.createdAt,
        durationMs: rec.durationMs,
        sourceKind: rec.sourceKind,
        bytes: rec.wavBlob ? rec.wavBlob.size : 0,
    };
}

// List all clips as metadata (no blobs), newest first.
export async function list() {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const out = [];
        const cur = tx(db, 'readonly').openCursor();
        cur.onsuccess = () => {
            const c = cur.result;
            if (c) { out.push(metaOf(c.value)); c.continue(); }
            else { out.sort((a, b) => (b.createdAt || 0) - (a.createdAt || 0)); resolve(out); }
        };
        cur.onerror = () => reject(cur.error);
    });
}

// Get one full record (including wavBlob), or null.
export async function get(name) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const r = tx(db, 'readonly').get(name);
        r.onsuccess = () => resolve(r.result || null);
        r.onerror = () => reject(r.error);
    });
}

// Insert/replace a clip. `rec` needs at least { name, wavBlob }; createdAt and
// missing fields are filled in. Returns the stored record's metadata.
export async function put(rec) {
    if (!rec || !rec.name) throw new Error('clip needs a name');
    if (!rec.wavBlob) throw new Error('clip needs a wavBlob');
    const full = {
        name: rec.name,
        createdAt: rec.createdAt || Date.now(),
        durationMs: rec.durationMs || 0,
        sourceKind: rec.sourceKind || 'file',
        wavBlob: rec.wavBlob,
    };
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const r = tx(db, 'readwrite').put(full);
        r.onsuccess = () => resolve(metaOf(full));
        r.onerror = () => reject(r.error);
    });
}

export async function remove(name) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const r = tx(db, 'readwrite').delete(name);
        r.onsuccess = () => resolve(true);
        r.onerror = () => reject(r.error);
    });
}

export async function exists(name) {
    const db = await openDB();
    return new Promise((resolve, reject) => {
        const r = tx(db, 'readonly').getKey ? tx(db, 'readonly').getKey(name) : null;
        if (r) { r.onsuccess = () => resolve(r.result !== undefined); r.onerror = () => reject(r.error); }
        else { get(name).then(v => resolve(!!v)).catch(reject); }
    });
}
