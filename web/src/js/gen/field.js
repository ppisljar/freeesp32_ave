// The reusable Field abstraction (Phase 5).
//
// A Field is the Wizard view's per-field intent — a value plus two INDEPENDENT
// decorations that the segment compiler turns into firmware prefixes:
//
//   Field = {
//     value: Number,
//     ramp:  null | { shape:'linear'|'quadratic', window:'whole'|{first_ms} },
//     mod:   null | { wave, end, period_ms },
//   }
//
//   - Transition (ramp) = the `>` / `*` ramp. "Steady" => '>' (linear),
//     "Smooth" => '*' (quadratic). It glides this field FROM this segment's
//     value TO the next same-channel segment's value, over the whole segment
//     (window 'whole') or the first N ms (window {first_ms}). Compiled with the
//     animate-on-start convention (the prefix lives on the START boundary row;
//     see config_parser.c:1995 + lane_serialize.js).
//   - Pulse (mod) = the `^ ~ / \ _` periodic modulation ("wobble between
//     [start] and [end] every [n]s"). Self-contained `PREFIXstart:end:period`.
//
// Both can be set at once: ramp shapes the field's OUTGOING transition (emitted
// on this row), while a mod oscillates the field IN PLACE. Because a single
// firmware token only carries one prefix, the compiler prefers the mod for the
// in-segment token and warns if a ramp is also present on the same field.
//
// This module is pure (no DOM beyond the optional sparkline path helper) and is
// shared by gen/views/wizard_compile.js and gen/views/wizard.js.

import { cell } from './model.js';

// ---- shape / wave <-> Cell interp mappings ---------------------------------
export const RAMP_SHAPE_TO_INTERP = { linear: 'lin', quadratic: 'quad' };
export const INTERP_TO_RAMP_SHAPE = { lin: 'linear', quad: 'quadratic' };

export const MOD_WAVE_TO_INTERP = {
    triangle: 'tri', sine: 'sine', sawup: 'sawup', sawdown: 'sawdn', square: 'sq',
};
export const INTERP_TO_MOD_WAVE = {
    tri: 'triangle', sine: 'sine', sawup: 'sawup', sawdn: 'sawdown', sq: 'square',
};

export const MOD_WAVE_GLYPH = {
    triangle: '^', sine: '~', sawup: '/', sawdown: '\\', square: '_',
};

// ---- constructors ----------------------------------------------------------

export function field(value, ramp, mod) {
    return {
        value: (value === undefined || value === null) ? 0 : value,
        ramp: ramp || null,
        mod: mod || null,
    };
}

// "Steady" (linear `>`) / "Smooth" (quadratic `*`) transition over the whole
// segment, or the first N ms (partial window).
export function ramp(shape, windowFirstMs) {
    return {
        shape: shape === 'quadratic' ? 'quadratic' : 'linear',
        window: (windowFirstMs === undefined || windowFirstMs === null)
            ? 'whole' : { first_ms: windowFirstMs },
    };
}

// A periodic pulse: oscillate between value (start) and end every period_ms.
export function mod(wave, end, periodMs) {
    return {
        wave: MOD_WAVE_TO_INTERP[wave] ? wave : 'sine',
        end: (end === undefined || end === null) ? 0 : end,
        period_ms: (periodMs === undefined || periodMs === null) ? 1000 : periodMs,
    };
}

// ---- predicates ------------------------------------------------------------
export function fieldHasRamp(f) { return !!(f && f.ramp); }
export function fieldHasMod(f) { return !!(f && f.mod); }
export function partialWindowMs(f) {
    if (!f || !f.ramp || f.ramp.window === 'whole') return null;
    const w = f.ramp.window;
    return (w && Number.isFinite(w.first_ms)) ? w.first_ms : null;
}

// ---- Field <-> Cell conversions --------------------------------------------

// The in-place step cell (no prefix) — the plain steady value.
export function fieldStepCell(f) { return cell(f ? f.value : 0); }

// The in-place modulation cell (Phase C).
export function fieldModCell(f) {
    if (!f || !f.mod) return fieldStepCell(f);
    return cell(f.value, MOD_WAVE_TO_INTERP[f.mod.wave] || 'sine', f.mod.end, f.mod.period_ms);
}

// The outgoing-ramp cell (Phase B) — value stays the start, prefix is the ramp.
export function fieldRampCell(f) {
    if (!f || !f.ramp) return fieldStepCell(f);
    return cell(f.value, RAMP_SHAPE_TO_INTERP[f.ramp.shape] || 'lin');
}

// Rebuild a Field from a parsed Cell (used by structural import). A periodic
// interp becomes a mod; a lin/quad interp becomes a whole-window ramp.
export function cellToField(c) {
    if (!c) return field(0);
    if (c.interp === 'lin') return field(c.value, ramp('linear'));
    if (c.interp === 'quad') return field(c.value, ramp('quadratic'));
    const wave = INTERP_TO_MOD_WAVE[c.interp];
    if (wave) return field(c.value, null, mod(wave, c.modEnd, c.modPeriodMs));
    return field(c.value);
}

// ---- sparkline (UI helper, pure string maths) ------------------------------
// Returns an SVG path `d` string sketching the field's value over a unit box of
// `w` x `h`, given a target (the next segment's value) for ramps. Used by the
// Wizard's live transition/pulse previews. `lo`/`hi` bound the value axis.
export function sparklinePath(f, target, w, h, lo, hi) {
    w = w || 80; h = h || 24;
    const span = (hi - lo) || 1;
    const Y = (v) => h - ((Math.max(lo, Math.min(hi, v)) - lo) / span) * h;
    const N = 32;
    const pts = [];
    if (f && f.mod) {
        const a = f.value, b = f.mod.end;
        for (let i = 0; i <= N; i++) {
            const ph = i / N;
            let s;
            switch (f.mod.wave) {
                case 'sine':   s = 0.5 - 0.5 * Math.cos(2 * Math.PI * ph); break;
                case 'sawup':  s = ph; break;
                case 'sawdown': s = 1 - ph; break;
                case 'square': s = ph < 0.5 ? 0 : 1; break;
                default:       s = ph < 0.5 ? ph * 2 : 2 - ph * 2; break; // triangle
            }
            pts.push([i / N * w, Y(a + (b - a) * s)]);
        }
    } else if (f && f.ramp && target !== null && target !== undefined) {
        const a = f.value, b = target;
        for (let i = 0; i <= N; i++) {
            const t = i / N;
            const tt = f.ramp.shape === 'quadratic'
                ? (t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t)) : t;
            pts.push([i / N * w, Y(a + (b - a) * tt)]);
        }
    } else {
        pts.push([0, Y(f ? f.value : 0)]);
        pts.push([w, Y(f ? f.value : 0)]);
    }
    return 'M' + pts.map(p => p[0].toFixed(1) + ',' + p[1].toFixed(1)).join(' L');
}
