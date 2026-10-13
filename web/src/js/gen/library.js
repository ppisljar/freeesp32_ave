// The published session library on GitHub — fetching only.
//
// A LEAF module on purpose: it imports nothing from the app. The Community
// browser and the config dropdown both want this list, and if the fetch lived
// in either of them the other would have to import it, making
// configstore -> community -> configstore a cycle. ES modules tolerate cycles
// through live bindings, but a bundler can still hand you a temporal-dead-zone
// error on a const that happens to be read during evaluation, and that failure
// mode is both rare and baffling. One small module with no imports cannot
// produce it.
//
// BROWSER-ONLY, no device proxy: raw.githubusercontent.com sends
// `Access-Control-Allow-Origin: *` and the repo is public, so the browser
// fetches cross-origin directly. It needs internet IN THE BROWSER — fine on a
// normal WiFi, not when joined to the device's SoftAP, and not over file://.

export const REPO = 'ppisljar/freeesp32_ave';
export const BRANCH = 'main';
// Fully-qualified `refs/heads/<branch>` form (what GitHub's "Raw" button emits).
const RAW_BASE = `https://raw.githubusercontent.com/${REPO}/refs/heads/${BRANCH}`;
export const INDEX_URL = `${RAW_BASE}/sessions/community.json`;
export const rawUrl = (path, file) => `${RAW_BASE}/${path}/${file}`;

export const DEFAULT_PATH = 'sessions/library';

/*
 * The index, fetched once and shared.
 *
 * Cached as the PROMISE rather than the result, so concurrent callers join one
 * request instead of racing two. A rejection clears the cache, or a retry after
 * the network comes back would keep returning the original failure.
 */
let indexPromise = null;

export function fetchIndex() {
    if (indexPromise) return indexPromise;
    indexPromise = fetch(INDEX_URL, { cache: 'no-cache' })
        .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
        .catch(err => { indexPromise = null; throw err; });
    return indexPromise;
}

// Drop the cache so the next fetch really goes out (the Community Retry button).
export function resetIndex() { indexPromise = null; }

// Raw text of one published session, by file name.
export function fetchSessionText(file, path) {
    return fetchIndex().then(idx =>
        fetch(rawUrl(path || (idx && idx.path) || DEFAULT_PATH, file), { cache: 'no-cache' })
            .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); }));
}
