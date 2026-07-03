// Shared v2 pulse-field tri-state text <-> value helpers.
//
// Every structured editor (table / lane / wizard) surfaces the pulse fields
// (LED env/phase/attack/jitter, audio duty/env/phase/attack/jitter) with the
// SAME positional-token convention the .ledc format uses:
//
//   ''  (blank)  -> undefined  = omit  (field not emitted; engine default)
//   '-'          -> null       = leave unchanged (device skip-semantics)
//   value        -> set
//
// Keeping the parse/format here (pure, no DOM) means one tested implementation
// backs all three editors and the behaviour can't drift between them.

import { cell } from './model.js';

function norm(s) { return String(s == null ? '' : s).trim(); }

// Compound-cell fields (phase, attack, audio duty). A numeric edit produces a
// step cell — ramps on pulse fields are a deferred device feature (author via Text).
export function parsePulseCell(s) {
    s = norm(s);
    if (s === '') return undefined;
    if (s === '-') return null;
    const n = parseFloat(s);
    return cell(Number.isFinite(n) ? n : 0);
}
export function formatPulseCell(v) {
    if (v === undefined) return '';
    if (v === null) return '-';
    return String(v.value);
}

// env (enum int). Range-checking is left to the model/parser; here we only map
// the tri-state. Caller passes the raw select value.
export function parsePulseEnv(s) {
    s = norm(s);
    if (s === '') return undefined;
    if (s === '-') return null;
    const n = parseInt(s, 10);
    return Number.isFinite(n) ? n : undefined;
}
export function formatPulseEnv(v) {
    if (v === undefined) return '';
    if (v === null) return '-';
    return String(v);
}

// jitter: "amp" or "amp:period_ms" -> { amp, period }. period omitted -> undefined
// (serialize.js emits just the amp), which round-trips identically.
export function parsePulseJitter(s) {
    s = norm(s);
    if (s === '') return undefined;
    if (s === '-') return null;
    const parts = s.split(':');
    const amp = parseFloat(parts[0]) || 0;
    const period = parts.length > 1 ? (parseFloat(parts[1]) || 0) : undefined;
    return { amp, period };
}
export function formatPulseJitter(v) {
    if (v === undefined) return '';
    if (v === null) return '-';
    return (v.period === undefined || v.period === null) ? String(v.amp) : (v.amp + ':' + v.period);
}
