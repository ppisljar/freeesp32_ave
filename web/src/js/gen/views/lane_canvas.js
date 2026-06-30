// DAW lane canvas — render + pointer gestures (Phase 4).
//
// Draws the lane model on a single <canvas> (no per-keyframe DOM) and turns
// mouse / touch / pen gestures into edits via the callbacks passed in. Renders
// are rAF-batched. Hit targets are >= 44px. Gesture map:
//   tap empty area  -> add keyframe (then inspector)
//   tap keyframe    -> select + inspector
//   drag keyframe   -> move (t snaps to grid, v to nice values)
//   long-press kf   -> context menu (delete / duplicate / set-curve / to-mod)
//   midpoint handle -> cycle outgoing shape step -> '>' -> '*'
//   wheel / pinch   -> zoom the time axis
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

function shapeColor(shape) {
    if (shape === 'step') return '#5b8def';
    if (shape === 'lin') return '#37b24d';
    if (shape === 'quad') return '#f08c00';
    return '#ae3ec9'; // periodic mods
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
            ctx2d.fillStyle = '#eef1f6';
            ctx2d.fillRect(0, y, cssW, LANE_HEAD_H);
            ctx2d.fillStyle = '#333';
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
        ctx2d.fillStyle = '#fafbfc';
        ctx2d.fillRect(0, 0, cssW, RULER_H);
        ctx2d.strokeStyle = '#dde1e7';
        ctx2d.beginPath(); ctx2d.moveTo(0, RULER_H - 0.5); ctx2d.lineTo(cssW, RULER_H - 0.5); ctx2d.stroke();
        // Nice tick spacing in ms.
        const targetPx = 80;
        const rawMs = targetPx * msPerPx;
        const step = niceStep(rawMs);
        const startT = Math.ceil(t0 / step) * step;
        ctx2d.fillStyle = '#888';
        ctx2d.strokeStyle = '#eceff3';
        for (let t = startT; ; t += step) {
            const x = timeToX(t);
            if (x > cssW) break;
            if (x < plotX) continue;
            ctx2d.beginPath(); ctx2d.moveTo(x, 0); ctx2d.lineTo(x, totalH); ctx2d.stroke();
            ctx2d.fillStyle = '#888';
            ctx2d.fillText(fmtMs(t), x + 3, RULER_H / 2);
        }
    }

    function drawStrip(s) {
        // background + label gutter
        ctx2d.fillStyle = '#fff';
        ctx2d.fillRect(s.x, s.y, s.w, s.h);
        ctx2d.strokeStyle = '#f0f2f5';
        ctx2d.strokeRect(s.x, s.y + 0.5, s.w, s.h);
        ctx2d.fillStyle = '#f7f8fa';
        ctx2d.fillRect(0, s.y, LABEL_W, s.h);
        ctx2d.fillStyle = '#666';
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
            if (sel) { ctx2d.strokeStyle = '#222'; ctx2d.lineWidth = 2; ctx2d.stroke(); ctx2d.lineWidth = 1; }
        }
    }

    function drawLfoBand(s, k, next) {
        const x1 = timeToX(k.t);
        const x2 = next ? timeToX(next.t) : s.x + s.w;
        const yStart = valToY(s, k.v);
        const yEnd = valToY(s, (k.lfoEnd !== undefined && k.lfoEnd !== null) ? k.lfoEnd : k.v);
        const top = Math.min(yStart, yEnd), bot = Math.max(yStart, yEnd);
        ctx2d.fillStyle = 'rgba(174,62,201,0.12)';
        ctx2d.fillRect(x1, top, x2 - x1, bot - top || 2);
        // tiled waveshape
        const period = (k.lfoPeriodMs && k.lfoPeriodMs > 0) ? k.lfoPeriodMs : 1000;
        const pxPer = period / msPerPx;
        ctx2d.strokeStyle = '#ae3ec9';
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
            ctx2d.fillStyle = '#ae3ec9';
            ctx2d.fillText('≈ ' + cycles.toFixed(1) + ' cyc', x1 + 4, top - 2 < s.y ? top + 8 : top - 2);
        }
    }

    function drawMidHandle(s, k, x1, x2, y1, y2) {
        if (x2 - x1 < 30) return;
        const mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
        k._mid = { x: mx, y: my };
        ctx2d.fillStyle = '#fff';
        ctx2d.strokeStyle = shapeColor(k.shape);
        ctx2d.beginPath(); ctx2d.arc(mx, my, 6, 0, Math.PI * 2); ctx2d.fill(); ctx2d.stroke();
        ctx2d.fillStyle = shapeColor(k.shape);
        const glyph = k.shape === 'step' ? '→' : (k.shape === 'lin' ? '/' : '²');
        ctx2d.fillText(glyph, mx - 3, my);
    }

    function drawApproxBadge(x, y) {
        ctx2d.fillStyle = '#f08c00';
        ctx2d.fillText('≈', x - 3, y - 10);
    }

    function drawGhost(s, sub) {
        if (!sub || !sub.keys.length) return;
        const keys = sub.keys.slice().sort((a, b) => a.t - b.t);
        ctx2d.strokeStyle = '#888';
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
            ctx2d.fillStyle = '#888'; ctx2d.fill();
        }
    }

    function drawWaveMarkers(s, sub) {
        if (!sub || !sub.keys.length) return;
        const WAVE = ['sin', 'sqr', 'tri', 'saw', 'wht', 'pnk', 'brn'];
        for (const k of sub.keys.slice().sort((a, b) => a.t - b.t)) {
            const x = timeToX(k.t);
            ctx2d.strokeStyle = '#c2255c';
            ctx2d.beginPath(); ctx2d.moveTo(x, s.y + 4); ctx2d.lineTo(x, s.y + s.h - 4); ctx2d.stroke();
            ctx2d.fillStyle = '#c2255c';
            ctx2d.fillText(WAVE[k.v] || String(k.v), x + 3, s.y + s.h / 2);
        }
    }

    function drawPlayhead(cssW) {
        const ms = cb.getPlayheadMs && cb.getPlayheadMs();
        if (ms === null || ms === undefined) return;
        const x = timeToX(ms);
        if (x < plotX || x > cssW) return;
        ctx2d.strokeStyle = '#e03131';
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
        if (!sub) return null;
        for (const k of sub.keys) {
            if (k._mid) {
                const d = (k._mid.x - x) * (k._mid.x - x) + (k._mid.y - y) * (k._mid.y - y);
                if (d <= 100) return k;
            }
        }
        return null;
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

    // ---- Value/time snapping ----------------------------------------------
    function snapTime(t) {
        const step = niceStep(msPerPx * 8); // ~8px grid
        return Math.max(0, Math.round(t / step) * step);
    }
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
            drag = { strip: s, kf, moved: false, startT: kf.t };
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
                const movedEnough = Math.hypot(p.x - timeToX(drag.startT), 0) > 3 || true;
                if (movedEnough) { drag.moved = true; clearLong(); cb.beginEdit(); }
            }
            drag.kf.t = snapTime(xToTime(p.x));
            drag.kf.v = snapVal(drag.strip.field, yToVal(drag.strip, p.y));
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
            if (d.moved) cb.commit();
            else openInspectorFor(d.strip, d.kf, e.clientX, e.clientY); // tap = inspect
            return;
        }
        if (d.addAt && !d.moved) {
            // tap empty -> add a keyframe at this t/v
            const s = d.strip;
            const t = snapTime(xToTime(p.x));
            const v = snapVal(s.field, yToVal(s, p.y));
            cb.beginEdit();
            if (!s.lane.sub[s.field]) s.lane.sub[s.field] = { keys: [] };
            const kf = keyframe(t, v, 'step');
            s.lane.sub[s.field].keys.push(kf);
            s.lane.sub[s.field].keys.sort((a, b) => a.t - b.t);
            cb.commit();
            selected = { laneKey: s.lane.key, field: s.field, t };
            scheduleRender();
            openInspectorFor(s, kf, e.clientX, e.clientY);
        }
    }

    function clearLong() { if (longTimer) { clearTimeout(longTimer); longTimer = null; } }

    function onWheel(e) {
        e.preventDefault();
        const p = localPt(e);
        const tAt = xToTime(p.x);
        const factor = e.deltaY > 0 ? 1.15 : 1 / 1.15;
        msPerPx = Math.max(1, Math.min(2000, msPerPx * factor));
        t0 = Math.max(0, tAt - (p.x - plotX) * msPerPx);
        scheduleRender();
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
function niceStep(ms) {
    const steps = [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000,
                   20000, 30000, 60000, 120000, 300000, 600000];
    for (const s of steps) if (s >= ms) return s;
    return steps[steps.length - 1];
}
function fmtMs(ms) {
    if (ms >= 60000) {
        const m = Math.floor(ms / 60000), s = Math.round((ms % 60000) / 1000);
        return m + ':' + String(s).padStart(2, '0');
    }
    if (ms >= 1000) return (ms / 1000) + 's';
    return ms + 'ms';
}
