// Two guards against silent data loss / silent lying in the Generator tab:
//
//   1. Save As must ask before it overwrites an existing config. The Home
//      page's Save As always has (configstore.js:183-185); the Generator's own
//      dialog used to call saveConfigText() straight out, so typing — or just
//      accepting the prefilled name of a file opened from disk — destroyed the
//      session of the same name with no prompt and no undo.
//   2. Every hash in sessions/community.json must be the hash of the session it
//      names, or the Community tab's freshness badge misreports every entry:
//      a stale hash never matches the device's copy, so a session that IS up to
//      date still shows "⬆ update available" forever.
//
// Only the decision is unit-tested for (1) — the modal itself is DOM work
// exercised in the browser, same split as text_view.test.js.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { planSaveAs } from '../src/js/gen/generator.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..');

// ---- (1) Save As overwrite decision ---------------------------------------

const ON_DEVICE = ['01_sleep_onset.ledc', 'mine.ledc'];

test('planSaveAs: an existing name in the destination needs confirmation', () => {
    const plan = planSaveAs({
        name: 'mine.ledc', dest: 'spiffs', names: ON_DEVICE,
        loadedSrc: 'local', loadedName: 'other.ledc',
    });
    assert.equal(plan.action, 'confirm');
    // Wording is configstore's, verbatim — one question, asked one way.
    assert.equal(plan.message, '"mine.ledc" already exists in Device (SPIFFS). Overwrite?');
});

test('planSaveAs: the prefilled-name trap — opened from disk, Save As, accept', () => {
    // The worst path needs no typing. Open-from-disk exists on the standalone
    // build (applyStandaloneChrome's file handler), where Browser (local) is
    // the one writable destination; it sets currentName to the file's name and
    // currentSrc to null, so the dialog prefills a name that may already be in
    // localStorage. Accepting it must still ask — setItem overwrites just as
    // silently as the device PUT does.
    const plan = planSaveAs({
        name: '01_sleep_onset.ledc', dest: 'local', names: ['01_sleep_onset.ledc'],
        loadedSrc: null, loadedName: '01_sleep_onset.ledc',
    });
    assert.equal(plan.action, 'confirm');
    assert.equal(plan.message, '"01_sleep_onset.ledc" already exists in Browser (local). Overwrite?');
});

test('planSaveAs: re-saving over the file you have loaded is not a surprise', () => {
    const plan = planSaveAs({
        name: 'mine.ledc', dest: 'spiffs', names: ON_DEVICE,
        loadedSrc: 'spiffs', loadedName: 'mine.ledc',
    });
    assert.equal(plan.action, 'save');
});

test('planSaveAs: same name, different store, still asks', () => {
    // Loaded from the browser, saving to the device: that is a different file.
    const plan = planSaveAs({
        name: 'mine.ledc', dest: 'spiffs', names: ON_DEVICE,
        loadedSrc: 'local', loadedName: 'mine.ledc',
    });
    assert.equal(plan.action, 'confirm');
});

test('planSaveAs: a name nobody is using saves without a prompt', () => {
    const plan = planSaveAs({
        name: 'brand_new.ledc', dest: 'spiffs', names: ON_DEVICE,
        loadedSrc: null, loadedName: null,
    });
    assert.deepEqual(plan, { action: 'save' });
});

test('planSaveAs: an unlistable destination saves rather than nagging', () => {
    // Device unreachable → fetchConfigGroups returns the group with names:[]
    // (configstore.js:269-270). We cannot tell whether the name is taken, so we
    // behave as configstore does with an empty lastLists[src]: no prompt. The
    // save then fails on its own if the device really is gone.
    for (const names of [undefined, null, []]) {
        const plan = planSaveAs({
            name: 'mine.ledc', dest: 'spiffs', names,
            loadedSrc: null, loadedName: null,
        });
        assert.equal(plan.action, 'save', 'names=' + JSON.stringify(names));
    }
});

test('planSaveAs: an invalid filename is rejected before anything else', () => {
    const plan = planSaveAs({
        name: 'mine.txt', dest: 'spiffs', names: ['mine.txt'],
        loadedSrc: null, loadedName: null,
    });
    assert.equal(plan.action, 'reject');
    assert.match(plan.message, /\.ledc$/);
});

// ---- (2) sessions/community.json freshness ---------------------------------

/*
 * The badge's own hash function, lifted out of community.js at test time.
 *
 * contentHash() is module-private there (community.js:28-34) and community.js
 * cannot be imported here anyway — it reaches for `document` through the
 * generator. Extracting the source beats keeping a second copy of FNV-1a in
 * this file: a copy can drift from the function that actually draws the badge,
 * and this cannot. If the extraction stops matching, the function was renamed
 * or reshaped and this test says so instead of quietly testing nothing.
 */
function badgeContentHash() {
    const src = readFileSync(join(REPO, 'web/src/js/gen/community.js'), 'utf8');
    const m = src.match(/\nfunction contentHash\(text\) \{[\s\S]*?\n\}\n/);
    assert.ok(m, 'contentHash() not found in community.js — update this test to match it');
    return new Function(m[0] + '\nreturn contentHash;')();
}

function communityIndex() {
    return JSON.parse(readFileSync(join(REPO, 'sessions/community.json'), 'utf8'));
}

test('community.json: every hash is the hash of the session it names', () => {
    const idx = communityIndex();
    const contentHash = badgeContentHash();
    assert.ok(idx.sessions.length > 0, 'index lists no sessions');
    const stale = [];
    for (const s of idx.sessions) {
        const text = readFileSync(join(REPO, idx.path, s.file), 'utf8');
        const want = contentHash(text);
        if (s.hash !== want) stale.push(`${s.file}: index ${s.hash} != file ${want}`);
    }
    // A stale hash is not a cosmetic problem: refreshStatus() compares it with
    // the device's copy (community.js:56), so the entry is badged
    // "⬆ update available" no matter how current the device actually is.
    assert.deepEqual(stale, [], 'stale hashes — regenerate sessions/community.json');
});

test('community.json: records when it was generated', () => {
    // Provenance for a file nothing regenerates automatically: without a date
    // there is no way to tell how far behind the library it has drifted.
    const idx = communityIndex();
    assert.equal(typeof idx.generated, 'string', 'community.json carries no "generated" date');
    assert.match(idx.generated, /^\d{4}-\d{2}-\d{2}$/);
    assert.ok(!Number.isNaN(Date.parse(idx.generated)), 'generated is not a real date');
});

// ---- (3) the index is TEXT the Community tab shows, and a complete list ----
//
// renderList() paints `title` as the card heading and `short` as its subtitle
// (community.js renderList), so whatever is in those fields is what a user
// reads. The index that shipped had been generated by cutting each title at the
// first '-': ten of the thirty-six were chopped inside a hyphenated word ('Non'
// for Non-Dual Awareness, 'AM' for AM-Noise Sleep Bed, 'Gateway / Out' for
// Gateway / Out-of-Body Threshold) and three were the '====' rule off the top
// of their header — thirteen unreadable cards out of thirty-six. Two more stop
// at the header's em-dash, which this test cannot see as damage but a reader
// can: 12_astral_focus10 and 13_astral_deep both read just 'Astral'.

// The session's own header comments, as one whitespace-collapsed string.
function headerText(path, file) {
    const out = [];
    for (const line of readFileSync(join(REPO, path, file), 'utf8').split('\n')) {
        if (!line.startsWith('#')) break;
        out.push(line.slice(1).trim());
    }
    return out.join(' ').replace(/\s+/g, ' ');
}

test('community.json: no title is cut off mid-word', () => {
    const idx = communityIndex();
    const bad = [];
    for (const s of idx.sessions) {
        // A title that the header continues with a '-' stopped inside a
        // hyphenated word: the header says "Non-Dual Awareness", the index said
        // "Non". (An em-dash after the title is the header's own separator and
        // is fine, which is why this looks for '-' specifically.)
        if (headerText(idx.path, s.file).includes(s.title + '-')) {
            bad.push(`${s.file}: "${s.title}" stops mid-word`);
        }
        // '====' / '----' is the rule line ABOVE the title in the banner-style
        // headers (09/10/11), never a name.
        if (/[=-]{4,}/.test(s.title)) bad.push(`${s.file}: "${s.title}" is a comment rule`);
        if (s.title.trim().length < 3) bad.push(`${s.file}: "${s.title}" is too short to be a name`);
    }
    assert.deepEqual(bad, [], 'titles the Community tab would render as fragments');
});

test('community.json: every card subtitle is a whole sentence', () => {
    // `short` used to be one physical comment line out of the header, so it
    // ended wherever the author happened to wrap: "Alpha entry (10 Hz), short
    // hold," — a subtitle trailing off in a comma reads as a loading bug.
    const idx = communityIndex();
    const bad = idx.sessions
        .filter(s => !/[.!?]$/.test(s.short || ''))
        .map(s => `${s.file}: ...${(s.short || '').slice(-40)}`);
    assert.deepEqual(bad, [], 'subtitles that stop mid-phrase');
});

test('community.json: every library session is listed, or named in the omission rule', () => {
    // The index is hand-regenerated, so a new session just does not appear —
    // the Community tab is the only place a user can discover one, and 49 of
    // the 85 .ledc files were missing when this test was written. Being absent
    // is allowed, but only on purpose: the file has to SAY so, by name.
    const idx = communityIndex();
    const listed = new Set(idx.sessions.map(s => s.file));
    const policy = String(idx.omitted || '');
    const unexplained = readdirSync(join(REPO, idx.path))
        .filter(f => f.endsWith('.ledc'))
        .filter(f => !listed.has(f) && !policy.includes(f));
    assert.deepEqual(unexplained, [],
        'sessions/library files neither listed nor named in community.json "omitted"');
});

test('community.json: it states what it lists and what it leaves out', () => {
    // JSON has no comments, so the inclusion rule lives in these fields. The
    // test above is only as good as this prose: without it an omission is
    // indistinguishable from an oversight.
    const idx = communityIndex();
    for (const key of ['listed', 'omitted', 'fields']) {
        assert.equal(typeof idx[key], 'string', `community.json carries no "${key}" note`);
        assert.ok(idx[key].length > 80, `"${key}" is too terse to be a stated rule`);
    }
});
