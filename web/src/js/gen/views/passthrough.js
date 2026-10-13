// Carry the rows a structured view cannot model through its round-trip.
//
// THE BUG THIS EXISTS TO FIX
//
// The Lanes and Wizard views do not edit the shared doc in place the way the
// Table view does. They project it into their own model — lanes with keyframe
// sublanes, or segments with layers — and then REBUILD the doc from that model
// on every commit. Anything their model has no slot for simply does not come
// out the other side.
//
// So committing a single dragged point in Lanes used to delete every `S`
// speech row, every `M` marker row, every whole-line comment, and any `raw` row
// the parser had been preserving verbatim because the firmware would reject it.
// Silently, with no warning and no undo beyond the view's own stack. A session
// carrying narration lost its narration.
//
// ALSO fixed here, by a second and deliberately different mechanism: an INLINE
// comment on an `led`/`audio` row (`0 10 50 ... # why`). Those rows ARE
// modelled — the view rebuilds them from keyframes/layers — so the whole row
// cannot ride through; only the NOTE can, to be re-attached to the rebuilt row.
// See "INLINE NOTES" below. Inline annotation is the project's house style
// (docs/sessions/authoring.html), so losing it lost most of a session's
// documentation on the first drag of a point.
//
// WHAT THIS DOES
//
// Lift the unmodellable rows out of the doc before the projection, carry them
// inside the view's metadata comment (which is already written into the file),
// and splice them back when the doc is rebuilt.
//
// POSITION. A view that regenerates every row cannot promise byte-for-byte
// placement, and pretending otherwise would be worse than saying so. The
// promise here is narrower and keepable:
//
//   - nothing is lost;
//   - a row that carries a TIME (speech, marker) comes back at that time;
//   - an untimed row (comment, blank, raw) comes back attached to the block it
//     preceded, which is the same convention the Table view groups by
//     ("untimed rows join the block that FOLLOWS them", views/table.js).
//
// Pure: no DOM. Unit-tested in web/test/view_passthrough.test.js.

import { commentRow } from '../model.js';

// Row kinds a lane/segment model represents itself and will re-emit. Everything
// else has to be carried.
const MODELLED = new Set(['led', 'audio', 'bg']);

// EVERY structured view's metadata marker, owned here rather than by the views,
// so that no view can carry another view's snapshot.
//
// Excluding only the CALLER's own prefix is not enough, and getting this wrong
// is not a cosmetic bug. Lanes would carry the whole `# @ave-wizard {...}`
// comment as a passthrough row nested inside its own `# @ave-lane` JSON, and
// the Wizard would then carry that larger comment inside its own: measured at
// ~1.8x per view switch, 1 KB to 27 KB in six switches, with no bound. The
// firmware truncates any line over CONFIG_PARSER_MAX_LINE_LENGTH = 256
// (config_parser.c:422) and mallocs the entire file into ESP32 heap
// (config_parser.c:401), so the growth is a device failure, not just ugly.
//
// Dropping a sibling's stale snapshot is also the CORRECT behaviour, not a
// lesser evil: that snapshot predates the edits the other view just made, so
// keeping it makes the sibling restore the snapshot and silently revert them.
// Without it the sibling reconstructs structurally from the rows, which carry
// the edits. Pinned by "an edit in one view survives a switch to the other".
//
// A new structured view MUST add its prefix here.
export const LANE_META_PREFIX = '@ave-lane v1 ';
export const WIZARD_META_PREFIX = '@ave-wizard v1 ';
export const VIEW_META_PREFIXES = [LANE_META_PREFIX, WIZARD_META_PREFIX];

// Is this row any structured view's metadata comment?
function isViewMeta(row) {
    if (row.kind !== 'comment') return false;
    const stripped = String(row.text || '').replace(/^#+\s*/, '');
    return VIEW_META_PREFIXES.some(p => stripped.startsWith(p));
}

// Kinds that carry a timestamp of their own.
const TIMED = new Set(['speech', 'marker']);

/*
 * Collect the rows a view will not re-emit.
 *
 * No structured view's metadata comment is ever carried — not the caller's own
 * (it is regenerated on the way out) and not a sibling's (see
 * VIEW_META_PREFIXES above for why both matter).
 *
 * Each entry records `anchor`, the timestamp the row should come back next to:
 * its own for a timed row, and for an untimed row the time of the next timed
 * row that followed it in the source. `null` anchor means "after everything"
 * (a trailing comment with no block after it).
 */
export function collectPassthrough(doc) {
    const rows = (doc && doc.rows) || [];
    const out = [];

    // Time of the next timed row at or after each index, so an untimed row can
    // be anchored to the block it introduces.
    const nextTime = new Array(rows.length).fill(null);
    let seen = null;
    for (let i = rows.length - 1; i >= 0; i--) {
        const r = rows[i];
        if (MODELLED.has(r.kind) || TIMED.has(r.kind)) {
            if (typeof r.time === 'number') seen = r.time;
        }
        nextTime[i] = seen;
    }

    for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        if (MODELLED.has(r.kind)) continue;
        if (isViewMeta(r)) continue;        // any view's metadata — never carried
        out.push({
            i,
            anchor: TIMED.has(r.kind) ? (r.time || 0) : nextTime[i],
            row: r,
        });
    }
    return out;
}

/*
 * Splice carried rows back into a rebuilt row list.
 *
 * `emitted` is what the view produced: [{ time, order, row }] where `order` is
 * its own tie-break within one timestamp (lane index, slot index). `header` is
 * whatever must stay at the very top — the metadata comment and the BG row.
 *
 * Everything merges on (time, rank, sourceOrder):
 *   rank 0  untimed carried rows, so a comment sits ABOVE the block it anchors to
 *   rank 1  timed carried rows (speech, marker)
 *   rank 2  the view's own emitted rows
 * A null anchor sorts last, which puts trailing comments at the end of the file
 * rather than silently at the top.
 */
export function mergePassthrough(header, emitted, passthrough) {
    const END = Number.POSITIVE_INFINITY;
    const items = [];

    for (const e of emitted) {
        items.push({ t: e.time, rank: 2, ord: e.order || 0, row: e.row });
    }
    for (const p of (passthrough || [])) {
        const timed = TIMED.has(p.row.kind);
        items.push({
            t: (p.anchor === null || p.anchor === undefined) ? END : p.anchor,
            rank: timed ? 1 : 0,
            ord: p.i,
            row: p.row,
        });
    }

    items.sort((a, b) => (a.t - b.t) || (a.rank - b.rank) || (a.ord - b.ord));
    return header.concat(items.map(x => x.row));
}

/*
 * Adopt the live doc's BG clip when the view's own snapshot has none.
 *
 * BG is in MODELLED, so collectPassthrough deliberately skips it — the view is
 * supposed to re-emit it from its model. But a snapshot taken before the user
 * added a `BG` row in the Text view has no bg key, so the row was skipped as
 * "modelled" and then never emitted: a silent deletion of exactly the kind this
 * module exists to prevent. Same rule as passthrough — the live doc beats the
 * stale snapshot. Mutates and returns the model for caller convenience.
 */
export function adoptBg(model, doc) {
    if (!model || model.bg) return model;
    const live = doc && (doc.bg || (doc.rows || []).filter(r => r.kind === 'bg').pop());
    const bg = live && (live.bg || live);
    if (bg && bg.url) model.bg = { url: bg.url, pan: bg.pan, loudness: bg.loudness };
    return model;
}

// ---------------------------------------------------------------------------
// INLINE NOTES — the trailing `# why` on an `led`/`audio` row.
// ---------------------------------------------------------------------------
//
// Different problem, different mechanism. `led`/`audio` rows are MODELLED, so
// they must not be carried whole: the view rebuilds them from keyframes/layers
// and a carried copy would duplicate the row. Only the note travels, and it has
// to be RE-ATTACHED to whichever rebuilt row is the same row.
//
// IDENTITY. "The same row" is keyed on (kind, time, channel|mask) — the three
// things a rebuild preserves. Everything else about the row is exactly what the
// view is allowed to change, so keying on a value would drop the note the
// moment the user dragged the point it annotates.
//
//   - `time`   lanesToDoc emits one row per keyframe time (lane_serialize.js:
//              `emitted.push({ time: t, ... })`), compileSession one per
//              segment start (wizard_compile.js: `time: slot.time`).
//   - channel  the audio lane key / the wizard's allocated channel
//              (`channel: lane.key` / `channel: grp.key`).
//   - mask     the LED lane key / the layer's region mask
//              (`mask: lane.key` / `mask: grp.key`).
//
// An absent channel token parses to `null` but reconstructs as lane key `0`
// (reconstructLanes maps null -> 0), so BOTH sides normalize null/undefined to
// 0 or the note would orphan itself on every such row.
//
// COLLISIONS. Two source rows CAN share one identity — same time, same channel
// is a legal firmware batch where the later row wins. Notes are therefore
// consumed IN SOURCE ORDER against the rebuilt rows of that identity in EMIT
// order, one note per row: the nth note on an identity lands on the nth rebuilt
// row of that identity, and never on a row that already took a note. A note can
// never land on a row of a different kind, time or channel/mask.
//
// ORPHANS. The row a note annotated may simply not exist after the rebuild —
// the user deleted it in the Text view, or the Wizard's `coalesce()` folded it
// away as a redundant hold (wizard_compile.js:486 coalesce). Silently dropping the text
// is the exact bug class this module exists to prevent, so the words are kept
// as a WHOLE-LINE comment anchored at the note's time instead. It lands above
// the block it used to annotate (rank 0 in mergePassthrough), so the author
// sees their sentence next to where it belonged rather than losing it. On the
// next commit it is an ordinary `comment` row and rides the passthrough path,
// so it is never re-attached and never duplicated.

// Identity of a modelled row, or null for a row that cannot carry a note.
function inlineNoteKey(row) {
    if (!row) return null;
    const t = Number.isFinite(row.time) ? row.time : 0;
    if (row.kind === 'audio') {
        const ch = (row.channel === null || row.channel === undefined) ? 0 : row.channel;
        return 'a:' + t + ':' + ch;
    }
    if (row.kind === 'led') {
        const mask = (row.mask === null || row.mask === undefined) ? 0 : row.mask;
        return 'l:' + t + ':' + mask;
    }
    return null;
}

/*
 * Lift the inline comments off the doc's modelled rows.
 *
 * Returns [{ i, key, time, text }] in source order — `i` is the source row
 * index, shared with collectPassthrough's `i` so orphans and carried rows
 * tie-break against each other consistently inside one timestamp.
 *
 * Marker rows also parse an inlineComment (parse.js:281) but serializeMarker
 * does not emit one (serialize.js:138), and they ride through whole as
 * passthrough anyway — so they are not collected here.
 */
export function collectInlineNotes(doc) {
    const rows = (doc && doc.rows) || [];
    const out = [];
    for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        if (r.kind !== 'led' && r.kind !== 'audio') continue;
        if (!r.inlineComment) continue;
        const key = inlineNoteKey(r);
        if (key === null) continue;
        out.push({
            i,
            key,
            time: Number.isFinite(r.time) ? r.time : 0,
            text: String(r.inlineComment),
        });
    }
    return out;
}

/*
 * Re-attach carried notes to the rows a view just rebuilt.
 *
 * `emitted` is the view's [{ time, order, row }] list, already sorted the way it
 * will be written. Matching rows are MUTATED (they are freshly built by
 * ledRow/audioRow on this commit, never shared with the live doc).
 *
 * Returns the notes that found no row, in mergePassthrough's entry shape, so
 * the caller appends them to its passthrough list and the ordinary merge places
 * them. Returns [] for the overwhelmingly common no-notes case, which keeps a
 * doc with no inline comments byte-identical to what the views emitted before.
 */
export function reattachInlineNotes(emitted, notes) {
    const orphans = [];
    if (!notes || !notes.length) return orphans;

    // identity -> the rebuilt rows of that identity, in emit order.
    const buckets = new Map();
    for (const e of (emitted || [])) {
        const key = inlineNoteKey(e && e.row);
        if (key === null) continue;
        if (!buckets.has(key)) buckets.set(key, []);
        buckets.get(key).push(e.row);
    }

    // One note per row: a cursor per identity, never rewound.
    const cursor = new Map();
    for (const n of notes) {
        const list = buckets.get(n.key);
        const at = cursor.get(n.key) || 0;
        if (list && at < list.length) {
            list[at].inlineComment = n.text;
            cursor.set(n.key, at + 1);
            continue;
        }
        // No row left at this identity — keep the words as a whole-line comment
        // at the time they were written for, rather than deleting them.
        orphans.push({ i: n.i, anchor: n.time, row: commentRow('# ' + n.text) });
    }
    return orphans;
}

/*
 * The whole "live doc beats the stale snapshot" rule in one call: adopt a BG
 * row the snapshot missed, then recompute the carried set from the doc.
 *
 * Both views run this at import AND at every commit. Commit-time matters
 * because an undo restores a snapshotted model, carried rows and all, which
 * would otherwise reinstate rows the Text view has since deleted.
 */
export function syncPassthrough(model, doc) {
    adoptBg(model, doc);
    const carried = collectPassthrough(doc);
    if (carried.length) model.passthrough = carried;
    // Nothing to carry: drop any stale key so a doc without passthrough rows
    // serializes exactly as it did before this mechanism existed.
    else delete model.passthrough;
    // Same rule, same reason, for the inline notes: recomputed from the live
    // doc so a note the user edited or deleted in the Text view is not restored
    // from the snapshot, and the key is absent (not `[]`) when there is nothing
    // to carry — the model is JSON-stringified verbatim into the file, so an
    // always-present empty key would change every session file on disk.
    const notes = collectInlineNotes(doc);
    if (notes.length) model.inlineNotes = notes;
    else delete model.inlineNotes;
    return model;
}

// How many rows of each kind are being carried — for a view that wants to say
// so, and for tests that want to assert nothing was dropped.
export function passthroughSummary(passthrough) {
    const counts = {};
    for (const p of (passthrough || [])) {
        counts[p.row.kind] = (counts[p.row.kind] || 0) + 1;
    }
    return counts;
}
