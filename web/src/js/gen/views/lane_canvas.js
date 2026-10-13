// DAW lane canvas — render + pointer gestures (Phase 4).
//
// Draws the lane model on a single <canvas> (no per-keyframe DOM) and turns
// mouse / touch / pen gestures into edits via the callbacks passed in. Renders
// are rAF-batched. Hit targets are >= 44px. Gesture map:
//   tap empty area  -> add keyframe (then inspector)
//   tap keyframe    -> select + inspector
//   drag keyframe   -> move (v always; t only once the pointer travels in x)
//   long-press kf   -> context menu (delete / duplicate / set-curve / to-mod)
//   midpoint handle -> cycle outgoing shape step -> '>' -> '*'
//   ctrl/cmd + wheel-> zoom the time axis about the cursor
//   horiz / shift-wheel -> pan the time axis
//   plain vert wheel-> native vertical scroll of the lane list
//   two-finger drag -> pan the time axis
//   ruler drag      -> scrub the playhead
//
// The canvas is intentionally dependency-light; it calls back into lane.js for
// every model mutation so undo + serialize stay centralised there.

import { AUDIO_CELL_FIELDS, LED_CELL_FIELDS, isPeriodicShape, keyframe }
    from './lane_serialize.js';

// Vertical metrics (CSS px).
const RULER_H = 26;
const LANE_HEAD_H = 22;
const SUB_H = 46;          // per-sublane height (>= 44px touch target band)
const LABEL_W = 96;        // left gutter with field labels
const HIT_R = 22;          // 44px diameter hit radius
const LONG_PRESS_MS = 500;

// Nominal value ranges per field (lo, hi). hi=null => derive from data.
const FIELD_RANGES = {
    pan: [-100, 100], vol: [0, 100], duty: [0, 100], bright: [0, 100],
    r: [0, 255], g: [0, 255], b: [0, 255],
    freq: [0, null], mod: [0, null], freqR: [0, null], wave: [0, 6],
};
const FIELD_LABELS = {
    freq: 'Carrier', pan: 'Pan', vol: 'Volume', mod: 'Iso',
    duty: 'Duty', bright: 'Bright', r: 'R', g: 'G', b: 'B',
    freqR: 'Binaural', wave: 'Wave',
};

// Canvas colours. A <canvas> cannot read CSS custom properties, so these mirror
// the tokens in css/style.css by hand — opaque values throughout, because the
// canvas composites onto its own transparency rather than onto the panel behind
// it, and translucent fills would wash out unpredictably. If you retheme the
// stylesheet, retheme this with it.
//
// EXPORTED (along with niceStep and fmtMs below) so the Simulate view's canvases
// share one mirror of the tokens rather than carrying a second copy that would
// drift out of step at the next retheme. This is the ONE sanctioned exception to
// the no-colour-literals rule (web/README.md), so there should be exactly one of
// these objects in the codebase.
export const PAL = {
    laneHead:   '#161a26',   // lane header band
    laneText:   '#eeecf6',   // --fg
    rulerBg:    '#0e121c',
    rulerEdge:  '#2a3040',
    grid:       '#1a1f2b',   // vertical time gridlines
    dim:        '#7a7791',   // --fg-dim: tick labels, ghost curve
    stripBg:    '#090c14',   // automation strip background
    stripEdge:  '#1d2230',
    gutterBg:   '#10141e',   // field-label gutter
    gutterText: '#aba8c0',   // --fg-mut
    selRing:    '#ffffff',   // selected keyframe ring (was #222, invisible now)
    handleFill: '#0b0e18',   // mid-handle disc, so the coloured ring reads
    step:       '#5fd8e8',   // --cyan
    lin:        '#4ade80',   // --ok
    quad:       '#ffb454',   // --amber
    mod:        '#a98bff',   // --violet: periodic modulation
    modFill:    'rgba(169,139,255,0.14)',
    wave:       '#ff7ac6',   // --magenta: waveType markers
    playhead:   '#ff6b6b',   // --err
};

function shapeColor(shape) {
    if (shape === 'step') return PAL.step;
    if (shape === 'lin') return PAL.lin;
    if (shape === 'quad') return PAL.quad;
    return PAL.mod; // periodic mods
}

// ---- Sublane invariants ----------------------------------------------------
// Every gesture that changes a keyframe's time goes through these, because
// lanesToDoc reads a sublane's `keys` in ARRAY order and stops at the first key
// past the row time it is emitting — lane_serialize.js, emitCellAt:
// `if (keys[i].t <= t) { active = keys[i]; idx = i; } else break;`. An unsorted
// array therefore makes the projection resolve the wrong keyframe: values land
// on foreign timestamps and the moved key's '>'/'*' prefix is dropped — while
// the canvas, which sorts a copy before drawing (drawStrip, below), still shows
// the right picture and validate() sees a well-formed file. steppedAt, which
// backs the freqR / wave strips, breaks out of its walk the same way. The ADD
// path has always re-sorted; a drag that crosses a sibling must too.
// Exported so the invariant can be unit-tested without a pointer event.

// Move one keyframe in place, then restore ascending-t order. The keyframe
// OBJECT is kept (the drag holds a reference to it, and the inspector edits it
// by identity) — only the array order changes.
export function moveKeyframe(keys, kf, t, v) {
    kf.t = t;
    kf.v = v;
    return sortKeyframes(keys, kf);
}

// Add a keyframe and keep the order. Same contract as moveKeyframe.
export function insertKeyframe(keys, kf) {
    keys.push(kf);
    return sortKeyframes(keys, kf);
}

function sortKeyframes(keys, kf) {
    keys.sort((a, b) => a.t - b.t); // in place: lane.js holds this same array
    return kf;
}

// Resolve a timestamp collision, at commit time. One (lane, t) pair is ONE row:
// lane_serialize.js folds each lane's keyframe times into a Set to get its row
// times (laneUnionTimes) and emitCellAt yields a single emission per time,
// so of two keyframes sharing a t the file silently keeps whichever the walk
// reaches last while the canvas goes on drawing both dots. `keeper` — the
// keyframe this gesture moved or added, the one the inspector is about to open
// on — wins; the sibling it landed on goes, so the dots left on screen are
// exactly the rows in the file. Returns the number dropped.
//
// Called on pointerup rather than on every pointermove on purpose: dragging a
// keyframe ACROSS a sibling must not eat it, only dropping it on top does.
export function dropCoincidentKeys(keys, keeper) {
    let dropped = 0;
    for (let i = keys.length - 1; i >= 0; i--) {
        if (keys[i] !== keeper && keys[i].t === keeper.t) {
            keys.splice(i, 1);
            dropped++;
        }
    }
    return dropped;
}

// ---- Keyframe drag: the time axis engages only when the pointer does ------
// A drag the user means as value-only must not be able to consume a sibling.
// The handler used to write `snapTime(xToTime(p.x))` on EVERY pointermove, so
// the dragged keyframe's time was re-derived from the pointer's ABSOLUTE x and
// re-rounded to the snap grid even when the pointer had not moved sideways at
// all — and pointerup then handed that time to dropCoincidentKeys, which
// deletes whatever sibling shares it.
//
// Measured on sessions/library/03_deep_relaxation.ledc — 30 minutes, its last
// AUDIO row `A 1800000 200 0 0 10 1 0 0` (its four `S ... "WAKE UP!"` rows run
// on from 1800000 to 1830000, but speech rows get no lane slot in docToLanes
// and fitToContent walks the lane model, so they do not move the zoom) — right
// after fitToContent on a 1310 px canvas, which is what a 1440 px window
// leaves once body padding (2 x 20), .container (2 x 24 + border), .section
// (2 x 18 + border) and the scroller's border are off it (the generator tab
// runs .container.is-wide, nav.js, so the canvas does get the rest):
// msPerPx = 1642, and the snap grid is snapStep(1642) = niceStep(8 * 1642) =
// 20000 ms. A TWENTY SECOND grid, 12.2 px wide. (The same session on a 390 px
// phone window is a 316 px canvas — the <= 640px media query tightens the
// chrome to 8 + 13 + 13 px of padding plus the three 1 px borders, 37 px a
// side — so 9340 ms/px, where the grid is 120 s.)
// Coarse is the normal case and not an edge case: of the 99 sessions under
// sessions/, 78 open at a 10 s grid or coarser and 55 at exactly 20 s.
//
// Two measured ways that erased a keyframe nobody aimed at:
//   * off-grid keyframes are ordinary — of the 6690 keyframes those 99 sessions
//     parse into, 650 sit off their own session's fit grid (497 of them in the
//     85 files under sessions/library, 153 in the 14 under sessions/test), and
//     41 of those round onto a timestamp a sibling in the same sublane already
//     holds (34 library, 7 test). One of the 41, in
//     sessions/library/13_astral_deep.ledc: `15000 0 50 12 255 80 0 10` and
//     `30000 0 50 ~4:10:12000 255 80 0 10` put LED brightness keys at 15 s and
//     30 s, and that session (50 min) fits at 2736 ms/px with a 30 s grid — so
//     dragging the 15 s dot down to re-dial brightness snapped it 5.5 px onto
//     30000 and the collapse ate the second key outright: the whole
//     `~4:10:12000` breathing LFO gone, not just a value. That session's rows
//     are a test of their own. Enumerated across the shipped sessions at the
//     fitToContent zoom, there are 34 such off-grid-key-onto-sibling pairs: 17
//     on a 5 s grid, 14 on 10 s and 3 on 30 s (the 13_astral_deep brightness
//     pair above, 5.9 px). None on the 20 s grid of the worked example — that
//     zoom has the same risk geometry but no actual collision in any shipped
//     session, so the arithmetic generalises while the step does not;
//   * keyframeAt accepts a grab up to HIT_R = 22 px away from the dot, which
//     at 1642 ms/px is 36 s of time the keyframe teleported through on the
//     first move, because the handler read the pointer's position instead of
//     its travel.
// Hence three rules, in one place so they can be tested without a pointer
// event: the keyframe follows the pointer's TRAVEL (grab offset preserved),
// the time axis stays frozen at the keyframe's original t until that travel
// passes DRAG_X_DEADZONE_PX, and a snap that would land on a sibling the
// pointer is not actually near is refused rather than merged. Dropping one
// keyframe onto another on purpose still works — aim within DROP_AIM_PX of the
// dot you can see.
//
// The third rule is deliberately the narrow one: snapStep targets an ~8 px
// grid at every zoom, so rounding can never displace a key by more than half a
// cell — 6.1 px at the zoom measured above — and the refusal therefore only
// fires in the 4..6.1 px band where the pointer is off the sibling's dot but
// close enough for the grid to pull onto it. It is the belt to the dead zone's
// braces: the dead zone can judge a value-only drag, but nothing else can
// judge a sideways drag that stops just short of a neighbour.

// Snap grid for times, in ms, at a given zoom: a ~8px grid, so it stays usable
// when zoomed in and goes deliberately coarse when zoomed out. The one
// definition of "the grid" — every gesture that rounds a time goes through it,
// so there is a single thing to measure and a single thing to retune.
export function snapStep(msPerPx) { return niceStep(msPerPx * 8); }

export const DRAG_X_DEADZONE_PX = 4;  // 6.6 s of time at the zoom measured above
export const DROP_AIM_PX = 4;         // a dot's drawn radius (drawStrip: 4, 6 selected)

// Snap `raw` onto the grid — unless the cell it rounds into is already held by
// a keyframe the pointer is not actually on. Then keep the unsnapped
// millisecond: both keyframes survive as their own rows, and the dot stays
// under the cursor where the user put it. `mine` is the keyframe being moved
// (null when one is being added), which is never its own neighbour.
function snapAwayFromNeighbour(keys, mine, raw, msPerPx, aim) {
    const step = snapStep(msPerPx);
    const snapped = Math.max(0, Math.round(raw / step) * step);
    const occupant = (keys || []).find(k => k !== mine && k.t === snapped);
    if (!occupant) return snapped;

    // How far the pointer actually is from the dot it would consume. The x term
    // alone is not enough on the ADD path: valToY spreads a field over 30 px, so
    // a tap can sit in the same ~6 px half-cell in x while being 22+ px away in
    // VALUE — visibly nowhere near the dot, and outside the HIT_R = 22 px circle
    // that would otherwise have routed it to select. Measured before this was
    // 2D: at 1642 ms/px a tap 2 px across and 22.8 px up from a dot still ate
    // it, and on an exact x hit the old `snapped !== raw` guard could never fire
    // at all, so any vertical distance was ignored.
    //
    // `aim` is supplied by the add path, which knows the pointer's y. The drag
    // path passes none and stays x-only on purpose: there the keyframe being
    // moved is under the cursor by construction, and those three rules are
    // pinned by tests.
    const dxPx = Math.abs(snapped - raw) / msPerPx;
    const dyPx = (aim && typeof aim.yPx === 'number' && typeof aim.yOf === 'function')
        ? Math.abs(aim.yPx - aim.yOf(occupant)) : 0;
    if (Math.hypot(dxPx, dyPx) > DROP_AIM_PX) return raw;
    return snapped;
}

// The time one pointermove of a keyframe drag should write. `dxPx` is the
// pointer's travel in x since pointerdown (NOT its position), `startT` the
// keyframe's time when the drag began.
export function dragTime(keys, kf, startT, dxPx, msPerPx) {
    if (Math.abs(dxPx) <= DRAG_X_DEADZONE_PX) return startT; // value-only drag
    return snapAwayFromNeighbour(
        keys, kf, Math.max(0, Math.round(startT + dxPx * msPerPx)), msPerPx);
}

// Apply one pointermove to the model: same contract as moveKeyframe (the
// keyframe object is kept, keys[] stays sorted), with the time axis rules
// above. No collision is resolved here — see commitKeyframeDrag.
export function dragKeyframe(keys, kf, startT, dxPx, msPerPx, v) {
    return moveKeyframe(keys, kf, dragTime(keys, kf, startT, dxPx, msPerPx), v);
}

// Finish a keyframe drag (pointerup). A drag that left the keyframe's time
// where it found it was a value edit, so it may consume nothing — otherwise a
// vertical drag at 20 s/grid spends a sibling for a volume change. Returns the
// number of keyframes dropped.
export function commitKeyframeDrag(keys, kf, startT) {
    if (kf.t === startT) return 0;
    return dropCoincidentKeys(keys, kf);
}

// ---- Tap-to-add: the same hazard reached from the other gesture -----------
// The add path only runs when the tap missed every dot, because keyframeAt
// routes a tap within HIT_R = 22 px of one to SELECT instead — but that radius
// is measured in 2D while the snap moves only the time. So a tap a few px to
// the side of a dot and a long way DOWN the strip misses it on screen and
// lands on its timestamp in the model, where the add path's own
// dropCoincidentKeys call (deliberate: one (lane, t) is one row) deletes it.
// Measured on the 46 px strip: valToY spreads a field over SUB_H - 2 * 8 =
// 30 px, so a tap can be up to 30 px off in value while staying inside the
// 6.1 px half-cell in x, and past 21 px of that it is outside the 22 px circle
// — on a 0..100 field (vol, bright, duty) a tap 70 units away in value from
// the keyframe it silently replaced. Hence the same refusal as the drag: a
// snapped time may only consume a neighbour the pointer is within DROP_AIM_PX
// of, measured as the real 2D distance to that dot. Aim at the dot and the
// replace still happens, which is what makes a deliberate "re-dial this row"
// tap work.
export function addTime(keys, tRaw, msPerPx, aim) {
    return snapAwayFromNeighbour(keys, null, Math.max(0, Math.round(tRaw)), msPerPx, aim);
}

// ---- Mid-handle hit targets ------------------------------------------------
// The shape-cycle handle is painted only where the outgoing segment is wide
// enough to hold it, so its hit target has to live and die with the painted
// frame: this cache is reset at the top of every render and refilled by
// drawMidHandle as it draws. Deliberately NOT stored on the keyframe, for two
// reasons:
//   * a handle that stopped being painted — zoomed out, panned away, following
//     keyframe deleted — must stop being clickable. onPointerDown tests the
//     mid handle BEFORE keyframeAt and tap-to-add, so one stale coordinate
//     turns an ordinary click on blank canvas into an invisible curve change
//     (step -> '>' -> '*') that rewrites the file;
//   * the lane model is JSON-stringified verbatim into the `# @ave-lane`
//     metadata comment (lane_serialize.js lanesToDoc, the LANE_META_PREFIX
//     header row), so anything parked on a keyframe becomes file bytes — the
//     saved session would otherwise depend on the viewport width and zoom
//     level at the moment of the save.
export const MID_HANDLE_MIN_SEG_PX = 30; // narrower segment => no handle drawn
export const MID_HANDLE_HIT_PX = 10;     // hit radius around the 6px disc

export function createMidHandleCache() {
    let pts = new Map(); // keyframe object -> { x, y } as last painted
    return {
        // A render starts by forgetting the previous frame's targets.
        reset() { pts = new Map(); },
        add(kf, x, y) { pts.set(kf, { x, y }); },
        // The keyframe of `keys` whose painted handle covers (x, y), or null.
        // Scoped to one sublane's keys so a handle in another strip that
        // happens to sit at the same pixel cannot answer for it.
        hit(keys, x, y) {
            for (const k of (keys || [])) {
                const p = pts.get(k);
                if (!p) continue;
                const d = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y);
                if (d <= MID_HANDLE_HIT_PX * MID_HANDLE_HIT_PX) return k;
            }
            return null;
        },
    };
}

// Sessions saved by an earlier build carry `_mid` pixel coordinates inside
// their `# @ave-lane` metadata, from when the handle cache above lived on the
// keyframe. Drop them on the way through so a reopened session stops
// re-serializing someone's old viewport; nothing reads them any more. Returns
// the number removed. One pass over a model the firmware caps at
// CONFIG_PARSER_MAX_ENTRIES rows (model.js MAX_ENTRIES), so it is cheap enough
// to run per frame rather than needing an import hook the canvas does not have.
export function stripTransientKeyState(lanes) {
    let n = 0;
    for (const lane of ((lanes && lanes.lanes) || [])) {
        for (const f in (lane.sub || {})) {
            for (const k of ((lane.sub[f] && lane.sub[f].keys) || [])) {
                if (k._mid !== undefined) { delete k._mid; n++; }
            }
        }
    }
    return n;
}

export function createLaneCanvas(canvas, cb) {
    const ctx2d = canvas.getContext('2d');

    // Viewport: time window [t0, t0 + span] mapped onto the plot area.
    let t0 = 0;            // left edge time (ms)
    let msPerPx = 20;      // zoom (ms per CSS px)
    let dpr = window.devicePixelRatio || 1;

    // Layout cache rebuilt each render: list of sublane strips with their rects.
    let strips = [];       // { lane, field, kind, x, y, w, h, lo, hi }
    let plotX = LABEL_W, plotW = 100, totalH = 200;

    let selected = null;   // { laneKey, field, t }
    let rafPending = false;
    const midHandles = createMidHandleCache(); // this frame's handle hit targets

    function model() { return cb.getModel(); }

    // ---- Coordinate transforms --------------------------------------------
    function timeToX(t) { return plotX + (t - t0) / msPerPx; }
    function xToTime(x) { return t0 + (x - plotX) * msPerPx; }
    function valToY(strip, v) {
        const { lo, hi, y, h } = strip;
        const pad = 8;
        const range = (hi - lo) || 1;
        const frac = (v - lo) / range;
        return y + (h - pad) - frac * (h - 2 * pad);
    }
    function yToVal(strip, yy) {
        const { lo, hi, y, h } = strip;
        const pad = 8;
        const range = (hi - lo) || 1;
        const frac = ((y + (h - pad)) - yy) / (h - 2 * pad);
        return lo + frac * range;
    }

    function fieldRange(field, keys) {
        const r = FIELD_RANGES[field] || [0, null];
        let lo = r[0], hi = r[1];
        if (hi === null) {
            hi = 1;
            for (const k of (keys || [])) {
                hi = Math.max(hi, k.v, (k.lfoEnd !== undefined ? k.lfoEnd : k.v));
            }
            hi = Math.ceil(hi * 1.2) || 1;
        }
        return [lo, hi];
    }

    // Visible cell sublanes for a lane (color may be combined or split).
    function laneFields(lane) {
        if (lane.kind === 'audio') {
            const fs = AUDIO_CELL_FIELDS.slice();
            if (lane.sub && lane.sub.freqR) fs.push('freqR');
            if (lane.sub && lane.sub.wave) fs.push('wave');
            return fs;
        }
        const fs = ['freq', 'duty', 'bright'];
        if (lane.colorSplit) fs.push('r', 'g', 'b');
        else fs.push('r'); // combined color shown on the R strip as a proxy
        return fs;
    }

    // ---- Layout ------------------------------------------------------------
    function layout() {
        const cssW = canvas.clientWidth || 600;
        plotX = LABEL_W;
        plotW = Math.max(50, cssW - LABEL_W - 8);
        strips = [];
        let y = RULER_H;
        for (const lane of model().lanes) {
            y += LANE_HEAD_H;
            if (lane.collapsed) continue;
            for (const field of laneFields(lane)) {
                const keys = (lane.sub[field] && lane.sub[field].keys) || [];
                const [lo, hi] = fieldRange(field, keys);
                strips.push({ lane, field, kind: lane.kind, x: plotX, y,
                              w: plotW, h: SUB_H, lo, hi });
                y += SUB_H;
            }
        }
        totalH = y + 8;
    }

    // ---- Rendering ---------------------------------------------------------
    function scheduleRender() {
        if (rafPending) return;
        rafPending = true;
        requestAnimationFrame(() => { rafPending = false; render(); });
    }

    function render() {
        layout();
        // The handles are hit-testable only where this frame paints them, and
        // a model that came back from an old file may carry their predecessor.
        midHandles.reset();
        stripTransientKeyState(model());
        const cssW = canvas.clientWidth || 600;
        dpr = window.devicePixelRatio || 1;
        canvas.width = Math.round(cssW * dpr);
        canvas.height = Math.round(totalH * dpr);
        canvas.style.height = totalH + 'px';
        ctx2d.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx2d.clearRect(0, 0, cssW, totalH);

        ctx2d.font = '11px system-ui, sans-serif';
        ctx2d.textBaseline = 'middle';

        drawRuler(cssW);

        // Lane headers + sublanes.
        let y = RULER_H;
        for (const lane of model().lanes) {
            ctx2d.fillStyle = PAL.laneHead;
            ctx2d.fillRect(0, y, cssW, LANE_HEAD_H);
            ctx2d.fillStyle = PAL.laneText;
            ctx2d.fillText((lane.collapsed ? '▸ ' : '▾ ') + lane.name,
                           6, y + LANE_HEAD_H / 2);
            y += LANE_HEAD_H;
            if (lane.collapsed) continue;
            y += laneFields(lane).length * SUB_H;
        }
        for (const s of strips) drawStrip(s);

        drawPlayhead(cssW);
    }

    function drawRuler(cssW) {
        ctx2d.fillStyle = PAL.rulerBg;
        ctx2d.fillRect(0, 0, cssW, RULER_H);
        ctx2d.strokeStyle = PAL.rulerEdge;
        ctx2d.beginPath(); ctx2d.moveTo(0, RULER_H - 0.5); ctx2d.lineTo(cssW, RULER_H - 0.5); ctx2d.stroke();
        // Nice tick spacing in ms.
        const targetPx = 80;
        const rawMs = targetPx * msPerPx;
        const step = niceStep(rawMs);
        const startT = Math.ceil(t0 / step) * step;
        ctx2d.fillStyle = PAL.dim;
        ctx2d.strokeStyle = PAL.grid;
        for (let t = startT; ; t += step) {
            const x = timeToX(t);
            if (x > cssW) break;
            if (x < plotX) continue;
            ctx2d.beginPath(); ctx2d.moveTo(x, 0); ctx2d.lineTo(x, totalH); ctx2d.stroke();
            ctx2d.fillStyle = PAL.dim;
            ctx2d.fillText(fmtMs(t), x + 3, RULER_H / 2);
        }
    }

    function drawStrip(s) {
        // background + label gutter
        ctx2d.fillStyle = PAL.stripBg;
        ctx2d.fillRect(s.x, s.y, s.w, s.h);
        ctx2d.strokeStyle = PAL.stripEdge;
        ctx2d.strokeRect(s.x, s.y + 0.5, s.w, s.h);
        ctx2d.fillStyle = PAL.gutterBg;
        ctx2d.fillRect(0, s.y, LABEL_W, s.h);
        ctx2d.fillStyle = PAL.gutterText;
        ctx2d.fillText(FIELD_LABELS[s.field] || s.field, 6, s.y + s.h / 2);

        const sub = s.lane.sub[s.field];
        if (s.field === 'wave') { drawWaveMarkers(s, sub); return; }
        if (s.field === 'freqR') { drawGhost(s, sub); return; }
        if (!sub || !sub.keys.length) return;

        const keys = sub.keys.slice().sort((a, b) => a.t - b.t);
        // segments
        for (let i = 0; i < keys.length; i++) {
            const k = keys[i];
            const next = keys[i + 1];
            const x1 = timeToX(k.t), y1 = valToY(s, k.v);
            ctx2d.strokeStyle = shapeColor(k.shape);
            ctx2d.lineWidth = 2;
            if (isPeriodicShape(k.shape)) {
                drawLfoBand(s, k, next);
            } else if (next) {
                const x2 = timeToX(next.t), y2 = valToY(s, next.v);
                ctx2d.beginPath();
                if (k.shape === 'step') {
                    ctx2d.moveTo(x1, y1); ctx2d.lineTo(x2, y1); ctx2d.lineTo(x2, y2);
                } else if (k.shape === 'lin') {
                    ctx2d.moveTo(x1, y1); ctx2d.lineTo(x2, y2);
                } else { // quad — sample the ease curve
                    ctx2d.moveTo(x1, y1);
                    const N = 16;
                    for (let j = 1; j <= N; j++) {
                        const p = j / N;
                        const tt = p < 0.5 ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p);
                        ctx2d.lineTo(x1 + (x2 - x1) * p, y1 + (y2 - y1) * tt);
                    }
                }
                ctx2d.stroke();
                if (k.approx) drawApproxBadge((x1 + x2) / 2, (y1 + y2) / 2);
                drawMidHandle(s, k, x1, x2, y1, y2);
            } else {
                // trailing keyframe — hold flat to the right edge
                ctx2d.beginPath(); ctx2d.moveTo(x1, y1); ctx2d.lineTo(s.x + s.w, y1); ctx2d.stroke();
            }
        }
        ctx2d.lineWidth = 1;
        // keyframe dots
        for (const k of keys) {
            const x = timeToX(k.t), yy = valToY(s, k.v);
            const sel = selected && selected.laneKey === s.lane.key &&
                        selected.field === s.field && selected.t === k.t;
            ctx2d.beginPath();
            ctx2d.arc(x, yy, sel ? 6 : 4, 0, Math.PI * 2);
            ctx2d.fillStyle = shapeColor(k.shape);
            ctx2d.fill();
            if (sel) { ctx2d.strokeStyle = PAL.selRing; ctx2d.lineWidth = 2; ctx2d.stroke(); ctx2d.lineWidth = 1; }
        }
    }

    function drawLfoBand(s, k, next) {
        const x1 = timeToX(k.t);
        const x2 = next ? timeToX(next.t) : s.x + s.w;
        const yStart = valToY(s, k.v);
        const yEnd = valToY(s, (k.lfoEnd !== undefined && k.lfoEnd !== null) ? k.lfoEnd : k.v);
        const top = Math.min(yStart, yEnd), bot = Math.max(yStart, yEnd);
        ctx2d.fillStyle = PAL.modFill;
        ctx2d.fillRect(x1, top, x2 - x1, bot - top || 2);
        // tiled waveshape
        const period = (k.lfoPeriodMs && k.lfoPeriodMs > 0) ? k.lfoPeriodMs : 1000;
        const pxPer = period / msPerPx;
        ctx2d.strokeStyle = PAL.mod;
        ctx2d.lineWidth = 1.5;
        ctx2d.beginPath();
        const mid = (top + bot) / 2, amp = (bot - top) / 2 || 4;
        let started = false;
        for (let x = x1; x <= x2; x += 2) {
            const ph = ((x - x1) / Math.max(1, pxPer)) * Math.PI * 2;
            const yy = mid - Math.sin(ph) * amp;
            if (!started) { ctx2d.moveTo(x, yy); started = true; } else ctx2d.lineTo(x, yy);
        }
        ctx2d.stroke();
        ctx2d.lineWidth = 1;
        // "≈ N cycles" readout
        if (x2 - x1 > 40) {
            const cycles = (next ? (next.t - k.t) : (xToTime(s.x + s.w) - k.t)) / period;
            ctx2d.fillStyle = PAL.mod;
            ctx2d.fillText('≈ ' + cycles.toFixed(1) + ' cyc', x1 + 4, top - 2 < s.y ? top + 8 : top - 2);
        }
    }

    function drawMidHandle(s, k, x1, x2, y1, y2) {
        if (x2 - x1 < MID_HANDLE_MIN_SEG_PX) return; // not painted => not clickable
        const mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
        midHandles.add(k, mx, my);
        ctx2d.fillStyle = PAL.handleFill;
        ctx2d.strokeStyle = shapeColor(k.shape);
        ctx2d.beginPath(); ctx2d.arc(mx, my, 6, 0, Math.PI * 2); ctx2d.fill(); ctx2d.stroke();
        ctx2d.fillStyle = shapeColor(k.shape);
        const glyph = k.shape === 'step' ? '→' : (k.shape === 'lin' ? '/' : '²');
        ctx2d.fillText(glyph, mx - 3, my);
    }

    function drawApproxBadge(x, y) {
        ctx2d.fillStyle = PAL.quad;
        ctx2d.fillText('≈', x - 3, y - 10);
    }

    function drawGhost(s, sub) {
        if (!sub || !sub.keys.length) return;
        const keys = sub.keys.slice().sort((a, b) => a.t - b.t);
        ctx2d.strokeStyle = PAL.dim;
        ctx2d.setLineDash([4, 3]);
        ctx2d.beginPath();
        for (let i = 0; i < keys.length; i++) {
            const x = timeToX(keys[i].t), yy = valToY(s, keys[i].v);
            if (i === 0) ctx2d.moveTo(x, yy);
            else { const py = valToY(s, keys[i - 1].v); ctx2d.lineTo(x, py); ctx2d.lineTo(x, yy); }
        }
        ctx2d.stroke();
        ctx2d.setLineDash([]);
        for (const k of keys) {
            ctx2d.beginPath(); ctx2d.arc(timeToX(k.t), valToY(s, k.v), 3, 0, Math.PI * 2);
            ctx2d.fillStyle = PAL.dim; ctx2d.fill();
        }
    }

    function drawWaveMarkers(s, sub) {
        if (!sub || !sub.keys.length) return;
        const WAVE = ['sin', 'sqr', 'tri', 'saw', 'wht', 'pnk', 'brn'];
        for (const k of sub.keys.slice().sort((a, b) => a.t - b.t)) {
            const x = timeToX(k.t);
            ctx2d.strokeStyle = PAL.wave;
            ctx2d.beginPath(); ctx2d.moveTo(x, s.y + 4); ctx2d.lineTo(x, s.y + s.h - 4); ctx2d.stroke();
            ctx2d.fillStyle = PAL.wave;
            ctx2d.fillText(WAVE[k.v] || String(k.v), x + 3, s.y + s.h / 2);
        }
    }

    function drawPlayhead(cssW) {
        const ms = cb.getPlayheadMs && cb.getPlayheadMs();
        if (ms === null || ms === undefined) return;
        const x = timeToX(ms);
        if (x < plotX || x > cssW) return;
        ctx2d.strokeStyle = PAL.playhead;
        ctx2d.lineWidth = 1.5;
        ctx2d.beginPath(); ctx2d.moveTo(x, 0); ctx2d.lineTo(x, totalH); ctx2d.stroke();
        ctx2d.lineWidth = 1;
    }

    // ---- Hit testing -------------------------------------------------------
    function stripAt(x, y) {
        for (const s of strips) if (y >= s.y && y < s.y + s.h) return s;
        return null;
    }
    function keyframeAt(s, x, y) {
        const sub = s.lane.sub[s.field];
        if (!sub) return null;
        let best = null, bestD = HIT_R * HIT_R;
        for (const k of sub.keys) {
            const kx = timeToX(k.t), ky = valToY(s, k.v);
            const d = (kx - x) * (kx - x) + (ky - y) * (ky - y);
            if (d <= bestD) { bestD = d; best = k; }
        }
        return best;
    }
    function midHandleAt(s, x, y) {
        const sub = s.lane.sub[s.field];
        return sub ? midHandles.hit(sub.keys, x, y) : null;
    }
    function laneHeaderAt(y) {
        let yy = RULER_H;
        for (const lane of model().lanes) {
            if (y >= yy && y < yy + LANE_HEAD_H) return lane;
            yy += LANE_HEAD_H;
            if (!lane.collapsed) yy += laneFields(lane).length * SUB_H;
        }
        return null;
    }

    // ---- Value snapping ----------------------------------------------------
    // Times are snapped by dragTime / addTime, which own the grid and the rule
    // about not letting it eat a neighbour; nothing here rounds a time.
    function snapVal(field, v) {
        const r = FIELD_RANGES[field] || [0, null];
        const lo = r[0], hi = r[1];
        let nv = Math.round(v * 10) / 10;
        if (lo !== null) nv = Math.max(lo, nv);
        if (hi !== null && hi !== undefined) nv = Math.min(hi, nv);
        return nv;
    }

    // ---- Pointer gestures --------------------------------------------------
    const pointers = new Map();
    let drag = null;        // { strip, kf, moved }
    let longTimer = null;
    let pinch = null;       // { d0, t0c }

    function localPt(e) {
        const r = canvas.getBoundingClientRect();
        return { x: e.clientX - r.left, y: e.clientY - r.top };
    }

    function onPointerDown(e) {
        canvas.setPointerCapture && canvas.setPointerCapture(e.pointerId);
        const p = localPt(e);
        pointers.set(e.pointerId, p);

        if (pointers.size === 2) {
            const pts = Array.from(pointers.values());
            pinch = { d0: Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y),
                      ms0: msPerPx,
                      cx: (pts[0].x + pts[1].x) / 2,
                      t0c: t0, panX0: (pts[0].x + pts[1].x) / 2 };
            clearLong();
            drag = null;
            return;
        }

        // Ruler scrub.
        if (p.y < RULER_H) {
            cb.setPlayheadMs && cb.setPlayheadMs(Math.max(0, xToTime(p.x)));
            drag = { ruler: true };
            scheduleRender();
            return;
        }

        // Lane header toggle.
        const header = laneHeaderAt(p.y);
        if (header && p.x < plotX) {
            cb.beginEdit();
            header.collapsed = !header.collapsed;
            cb.commit();
            scheduleRender();
            return;
        }

        const s = stripAt(p.x, p.y);
        if (!s || p.x < plotX) return;

        // Midpoint shape-cycle handle.
        const mid = midHandleAt(s, p.x, p.y);
        if (mid) {
            cb.beginEdit();
            mid.shape = mid.shape === 'step' ? 'lin' : (mid.shape === 'lin' ? 'quad' : 'step');
            cb.commit();
            scheduleRender();
            return;
        }

        const kf = keyframeAt(s, p.x, p.y);
        if (kf) {
            selected = { laneKey: s.lane.key, field: s.field, t: kf.t };
            // `keys` rides along: the move has to re-sort the array the
            // keyframe lives in, not just the keyframe (see moveKeyframe).
            // `downX` is what the time axis moves relative to: the drag
            // follows the pointer's travel, so grabbing the dot off-centre
            // (keyframeAt reaches HIT_R = 22 px) does not teleport it.
            drag = { strip: s, kf, keys: s.lane.sub[s.field].keys,
                     moved: false, startT: kf.t, downX: p.x };
            // Long-press -> context menu.
            longTimer = setTimeout(() => {
                longTimer = null;
                if (drag && !drag.moved) {
                    openContextMenu(s, kf, e.clientX, e.clientY);
                    drag = null;
                }
            }, LONG_PRESS_MS);
            scheduleRender();
        } else {
            // Tap on empty area inside a strip -> remember for add-on-up.
            drag = { strip: s, addAt: p, moved: false };
        }
    }

    function onPointerMove(e) {
        const p = localPt(e);
        if (pointers.has(e.pointerId)) pointers.set(e.pointerId, p);

        if (pinch && pointers.size === 2) {
            const pts = Array.from(pointers.values());
            const d = Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y);
            const cx = (pts[0].x + pts[1].x) / 2;
            const tAtC = pinch.t0c + (pinch.cx - plotX) * pinch.ms0;
            if (d > 4) msPerPx = Math.max(1, Math.min(2000, pinch.ms0 * pinch.d0 / d));
            t0 = Math.max(0, tAtC - (cx - plotX) * msPerPx);
            scheduleRender();
            return;
        }

        if (!drag) return;

        if (drag.ruler) { cb.setPlayheadMs && cb.setPlayheadMs(Math.max(0, xToTime(p.x))); scheduleRender(); return; }

        if (drag.kf) {
            if (!drag.moved) {
                // Any movement at all starts the edit, because the VALUE
                // follows the pointer from the first move; the threshold that
                // matters is on the time axis, inside dragTime.
                drag.moved = true; clearLong(); cb.beginEdit();
            }
            dragKeyframe(drag.keys, drag.kf, drag.startT, p.x - drag.downX, msPerPx,
                         snapVal(drag.strip.field, yToVal(drag.strip, p.y)));
            selected = { laneKey: drag.strip.lane.key, field: drag.strip.field, t: drag.kf.t };
            scheduleRender();
        } else if (drag.addAt) {
            // movement before pointerup cancels the add (treat as nothing)
            if (Math.hypot(p.x - drag.addAt.x, p.y - drag.addAt.y) > 6) drag.moved = true;
        }
    }

    function onPointerUp(e) {
        clearLong();
        const p = localPt(e);
        pointers.delete(e.pointerId);
        canvas.releasePointerCapture && canvas.releasePointerCapture(e.pointerId);
        if (pointers.size < 2) pinch = null;

        if (!drag) return;
        const d = drag; drag = null;

        if (d.ruler) return;

        if (d.kf) {
            if (d.moved) {
                // Dropped on top of a sibling? Only one of them can be a row
                // — but a drag that never changed the time consumes nothing.
                if (commitKeyframeDrag(d.keys, d.kf, d.startT)) scheduleRender();
                cb.commit();
            } else {
                openInspectorFor(d.strip, d.kf, e.clientX, e.clientY); // tap = inspect
            }
            return;
        }
        if (d.addAt && !d.moved) {
            // tap empty -> add a keyframe at this t/v
            const s = d.strip;
            const v = snapVal(s.field, yToVal(s, p.y));
            cb.beginEdit();
            if (!s.lane.sub[s.field]) s.lane.sub[s.field] = { keys: [] };
            const keys = s.lane.sub[s.field].keys;
            // Hand the refusal the pointer's y and this strip's value->y
            // mapping, so "is the tap near the dot it would replace?" is the
            // distance the user can actually see, not just the x component.
            const t = addTime(keys, xToTime(p.x), msPerPx,
                { yPx: p.y, yOf: (k) => valToY(s, k.v) });
            const kf = insertKeyframe(keys, keyframe(t, v, 'step'));
            // Tapping ONTO a timestamp this sublane already uses replaces that
            // keyframe — the row can only carry the value just tapped in. The
            // grid no longer pulls a tap onto a timestamp it missed on screen
            // (addTime, above), so this fires only where the pointer really is.
            dropCoincidentKeys(keys, kf);
            cb.commit();
            selected = { laneKey: s.lane.key, field: s.field, t };
            scheduleRender();
            openInspectorFor(s, kf, e.clientX, e.clientY);
        }
    }

    function clearLong() { if (longTimer) { clearTimeout(longTimer); longTimer = null; } }

    function onWheel(e) {
        // Ctrl/Cmd + vertical wheel => zoom the time axis about the cursor.
        if (e.ctrlKey || e.metaKey) {
            e.preventDefault();
            const p = localPt(e);
            const tAt = xToTime(p.x);
            const factor = e.deltaY > 0 ? 1.15 : 1 / 1.15;
            msPerPx = Math.max(1, Math.min(2000, msPerPx * factor));
            t0 = Math.max(0, tAt - (p.x - plotX) * msPerPx);
            scheduleRender();
            return;
        }
        // Horizontal wheel (trackpad) or shift+wheel (mouse) => pan the time axis.
        // The plot has no native horizontal overflow, so we move t0 ourselves.
        const horiz = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX
                    : (e.shiftKey ? e.deltaY : 0);
        if (horiz !== 0) {
            e.preventDefault();
            t0 = Math.max(0, t0 + horiz * msPerPx);
            scheduleRender();
            return;
        }
        // Plain vertical wheel => let .gen-lane-scroll scroll natively (no preventDefault).
    }

    // ---- Inspector + context menu -----------------------------------------
    function openInspectorFor(s, kf, clientX, clientY) {
        cb.openInspector(s.lane, s.field, kf, { x: clientX, y: clientY },
            () => { scheduleRender(); });
    }
    function openContextMenu(s, kf, clientX, clientY) {
        cb.openContextMenu(s.lane, s.field, kf, { x: clientX, y: clientY },
            () => { scheduleRender(); });
    }

    // ---- Wiring ------------------------------------------------------------
    canvas.style.touchAction = 'none';
    canvas.addEventListener('pointerdown', onPointerDown);
    canvas.addEventListener('pointermove', onPointerMove);
    canvas.addEventListener('pointerup', onPointerUp);
    canvas.addEventListener('pointercancel', onPointerUp);
    canvas.addEventListener('wheel', onWheel, { passive: false });

    function fitToContent() {
        // Choose an initial zoom that fits the whole session in view.
        let maxT = 1000;
        for (const lane of model().lanes) {
            for (const f in lane.sub) {
                for (const k of (lane.sub[f].keys || [])) maxT = Math.max(maxT, k.t);
            }
        }
        const cssW = canvas.clientWidth || 600;
        msPerPx = Math.max(1, (maxT * 1.1) / Math.max(50, cssW - LABEL_W - 8));
        t0 = 0;
    }

    function destroy() {
        canvas.removeEventListener('pointerdown', onPointerDown);
        canvas.removeEventListener('pointermove', onPointerMove);
        canvas.removeEventListener('pointerup', onPointerUp);
        canvas.removeEventListener('pointercancel', onPointerUp);
        canvas.removeEventListener('wheel', onWheel);
    }

    return {
        render: scheduleRender,
        renderNow: render,
        fitToContent,
        setSelected(sel) { selected = sel; scheduleRender(); },
        destroy,
    };
}

// ---- small format helpers --------------------------------------------------
export function niceStep(ms) {
    const steps = [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000,
                   20000, 30000, 60000, 120000, 300000, 600000];
    for (const s of steps) if (s >= ms) return s;
    return steps[steps.length - 1];
}
export function fmtMs(ms) {
    if (ms >= 60000) {
        const m = Math.floor(ms / 60000), s = Math.round((ms % 60000) / 1000);
        return m + ':' + String(s).padStart(2, '0');
    }
    if (ms >= 1000) return (ms / 1000) + 's';
    return ms + 'ms';
}
