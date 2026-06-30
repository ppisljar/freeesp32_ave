// Shared interpolation evaluators.
//
// These mirror the device-side per-field interpolation so the session report
// (config.js) and the Generator preview/playhead resolve identical live values.
// They operate on the report's lightweight `{v, interp}` field shape produced
// by config.js's parseConfigStructured() — NOT on the gen model's Cell. The two
// agree numerically; this module is the single source of the math.
//
// Extracted verbatim from config.js (Option A keeps the report path behavior
// identical). config.js imports these instead of redefining them.

// Parse a numeric field that may carry a `>` (linear) or `*` (quadratic)
// ramp prefix. Modulation prefixes are not resolved here (the report path
// does not animate them); they fall through to interp:'none' on the value.
export function parseValueInterp(s) {
    let interp = 'none';
    if (s[0] === '>') { interp = 'linear'; s = s.slice(1); }
    else if (s[0] === '*') { interp = 'quadratic'; s = s.slice(1); }
    return { v: parseFloat(s), interp: interp };
}

export function lerp(a, b, t) { return a + (b - a) * t; }

export function quad(a, b, t) {
    const tt = t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
    return a + (b - a) * tt;
}

// Resolve one field's live value at time tMs given the active entry and the
// next entry on the same channel. The ramp prefix lives on the NEXT entry's
// field (report convention) and targets the next entry's value.
export function interpField(active, next, tMs, fname) {
    if (!active[fname]) return null;
    if (next && next[fname] && next[fname].interp !== 'none') {
        const win = next.time - active.time;
        if (win <= 0) return active[fname].v;
        const p = Math.max(0, Math.min(1, (tMs - active.time) / win));
        const fn = next[fname].interp === 'linear' ? lerp : quad;
        return fn(active[fname].v, next[fname].v, p);
    }
    return active[fname].v;
}

export function audioStateAtTime(audioEntries, tMs, channel) {
    const seq = audioEntries.filter(e => e.channel === channel)
                            .sort((a, b) => a.time - b.time);
    let active = null, next = null;
    for (let i = 0; i < seq.length; i++) {
        if (seq[i].time <= tMs) { active = seq[i]; next = seq[i + 1] || null; }
        else break;
    }
    if (!active) return null;
    return {
        freq: interpField(active, next, tMs, 'freq'),
        pan:  interpField(active, next, tMs, 'pan'),
        vol:  interpField(active, next, tMs, 'vol'),
        mod:  interpField(active, next, tMs, 'mod'),
    };
}

export function ledStateAtTime(ledEntries, tMs, ledCh) {
    const bit = 1 << ledCh;
    const seq = ledEntries.filter(e => e.mask & bit)
                          .sort((a, b) => a.time - b.time);
    let active = null, next = null;
    for (let i = 0; i < seq.length; i++) {
        if (seq[i].time <= tMs) { active = seq[i]; next = seq[i + 1] || null; }
        else break;
    }
    if (!active) return null;
    return {
        freq: interpField(active, next, tMs, 'freq'),
        duty: interpField(active, next, tMs, 'duty'),
        bri:  interpField(active, next, tMs, 'brightness'),
        r:    interpField(active, next, tMs, 'r'),
        g:    interpField(active, next, tMs, 'g'),
        b:    interpField(active, next, tMs, 'b'),
    };
}
