// Browser-side session log.
//
// The device log (logcapture.js) tells you what the ESP32 did; this tells you
// what the BROWSER did during the same session — session compilation/bounce,
// HTTP calls to the device, and especially the BG WebSocket lifecycle and any
// errors. It's captured into a buffer while a session is active and stored with
// the report alongside the device log, so a session can be diagnosed end-to-end
// after the fact.
//
// Everything is also mirrored to the devtools console, so nothing is lost when
// devtools is open — the buffer just makes it survivable without devtools and
// linkable to a report.

let s_active = false;
let s_t0 = 0;
let s_lines = [];
const MAX_LINES = 8000;         // safety cap so a runaway loop can't eat memory

function pad2(n) { return String(n).padStart(2, '0'); }
function pad3(n) { return String(n).padStart(3, '0'); }

// "+12.34s 13:45:07.812" — session-relative (for correlating with the run) plus
// wall clock (for correlating with the device's own timestamps / other tools).
function stamp() {
    const now = Date.now();
    const rel = s_active ? '+' + ((now - s_t0) / 1000).toFixed(2) + 's' : '+—';
    const d = new Date(now);
    const wall = pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds()) + '.' + pad3(d.getMilliseconds());
    return rel.padStart(9) + ' ' + wall;
}

function toStr(a) {
    if (typeof a === 'string') return a;
    if (a instanceof Error) return a.name + ': ' + a.message;
    try { return JSON.stringify(a); } catch (e) { return String(a); }
}

// Core log call. `level` ∈ info|warn|error, `tag` groups by subsystem
// (e.g. 'ws', 'http', 'bounce', 'play'). Never throws — logging must not be able
// to break a session.
export function clog(level, tag, ...args) {
    try {
        const msg = args.map(toStr).join(' ');
        const c = (console[level] || console.log).bind(console);
        c('[' + tag + ']', ...args);
        if (!s_active) return;
        if (s_lines.length < MAX_LINES) {
            const lvl = level === 'info' ? '    ' : (level === 'warn' ? 'WARN' : 'ERR ');
            s_lines.push(stamp() + ' ' + lvl + ' ' + tag + ': ' + msg);
        } else if (s_lines.length === MAX_LINES) {
            s_lines.push('[client log truncated at ' + MAX_LINES + ' lines]');
        }
    } catch (e) { /* never let logging throw */ }
}

// Convenience wrappers.
export const clogI = (tag, ...a) => clog('info', tag, ...a);
export const clogW = (tag, ...a) => clog('warn', tag, ...a);
export const clogE = (tag, ...a) => clog('error', tag, ...a);

export function startClientLog() {
    s_active = true;
    s_t0 = Date.now();
    s_lines = [];
    clog('info', 'session', 'client log started');
}

export function stopClientLog() {
    if (s_active) clog('info', 'session', 'client log stopped');
    s_active = false;
}

export function isClientLogging() { return s_active; }
export function getClientLogText() { return s_lines.join('\n'); }

// Catch otherwise-invisible failures during a session (a throw in an event
// handler, a rejected promise nobody awaited — exactly the WS/stream errors the
// user wants recorded). Registered once at import.
if (typeof window !== 'undefined' && window.addEventListener) {
    window.addEventListener('error', (e) => {
        if (s_active) clog('error', 'window', (e && e.message) || 'error',
            e && e.filename ? '@ ' + e.filename + ':' + e.lineno : '');
    });
    window.addEventListener('unhandledrejection', (e) => {
        if (s_active) clog('error', 'promise', toStr(e && e.reason));
    });
}
