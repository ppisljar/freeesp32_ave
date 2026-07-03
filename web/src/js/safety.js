// Epilepsy / photosensitivity safety opt-in (browser-local).
//
// Flicker sessions drive rhythmic light + sound that can, in rare cases,
// provoke seizures in photosensitive people. Before the FIRST time this
// browser plays a session that actually flickers an LED, we show a one-time
// contraindication notice and require an explicit acknowledgement. The
// acceptance is stored in localStorage and never shown again (until reset
// from the Settings page).
//
// This is purely browser-side: the firmware-level photosensitivity clamp
// (a `safety_mode` device setting) was cut, so there is intentionally NO
// /api/settings wiring here. If a firmware clamp is added later, this module
// is the natural place to also surface/toggle it — but today it is a no-op on
// the device.
//
// The pure logic (does a doc flicker? / is acceptance stored?) is exported so
// it can be unit-tested without a DOM; the modal glue is not unit-tested.

import { chooseModal } from './util.js';

// localStorage key — follows the existing `ave:` convention (see gen/tts.js).
export const SAFETY_KEY = 'ave:safetyAccepted';

// The stored value written on acceptance (a version tag, so the notice can be
// re-shown in future if the wording materially changes by bumping this).
export const SAFETY_VERSION = '1';

// Experiential (not medical) contraindication copy shown in the opt-in modal.
export const SAFETY_TITLE = 'Before you begin';
export const SAFETY_TEXT =
    'This session uses flickering light and rhythmic sound to guide your ' +
    'experience.\n\n' +
    'Please do not use it if you have a history of seizures or ' +
    'photosensitive epilepsy. Take care if you are pregnant, or if you wear a ' +
    'pacemaker or other implant. Never use it while driving or operating ' +
    'machinery — sit or lie down somewhere safe.\n\n' +
    'Stop straight away if you feel dizzy, disoriented, or unwell.\n\n' +
    'By continuing you confirm you have read this and choose to proceed.';

// True iff an LED row in the doc actually flickers (freq value or its
// modulation endpoint is > 0). A pure step to 0 Hz or an audio-only / static
// LED session does not count as flicker.
export function ledRowFlickers(row) {
    if (!row || row.kind !== 'led') return false;
    const f = row.freq;
    if (f == null) return false;
    const value = (typeof f === 'object') ? f.value : f;
    const modEnd = (typeof f === 'object') ? f.modEnd : null;
    if (typeof value === 'number' && value > 0) return true;
    if (typeof modEnd === 'number' && modEnd > 0) return true;
    return false;
}

// True iff the parsed doc contains any flickering LED row.
export function docHasFlicker(doc) {
    if (!doc || !Array.isArray(doc.rows)) return false;
    return doc.rows.some(ledRowFlickers);
}

// Small storage shim so the pure predicates are testable with a fake store and
// degrade gracefully if localStorage is unavailable (private mode, etc.).
function defaultStore() {
    try { return (typeof localStorage !== 'undefined') ? localStorage : null; }
    catch (e) { return null; }
}

export function isSafetyAccepted(store) {
    store = (store === undefined) ? defaultStore() : store;
    if (!store) return false;
    try { return store.getItem(SAFETY_KEY) === SAFETY_VERSION; }
    catch (e) { return false; }
}

export function setSafetyAccepted(store) {
    store = (store === undefined) ? defaultStore() : store;
    if (!store) return;
    try { store.setItem(SAFETY_KEY, SAFETY_VERSION); } catch (e) { /* ignore */ }
}

export function resetSafetyAccepted(store) {
    store = (store === undefined) ? defaultStore() : store;
    if (!store) return;
    try { store.removeItem(SAFETY_KEY); } catch (e) { /* ignore */ }
}

// Gate a play: resolve `true` if playback may proceed, `false` if the user
// declined the safety notice. Audio-only / non-flicker sessions and already-
// acknowledged browsers pass through with no prompt.
export async function ensureSafetyAccepted(doc) {
    if (!docHasFlicker(doc)) return true;
    if (isSafetyAccepted()) return true;
    const choice = await chooseModal(
        SAFETY_TITLE,
        [{ label: 'I understand — continue', value: 'accept',
           desc: 'Store this acknowledgement in this browser and start.' }],
        SAFETY_TEXT,
    );
    if (choice === 'accept') {
        setSafetyAccepted();
        return true;
    }
    return false;   // Cancel / Esc / backdrop → do not start playback.
}
