// Home status strip — "what is this device doing right now".
//
// IT ADDS NO POLLING. livecontrol.js already fetches /api/state once a second
// and now broadcasts the result as a 'devicestate' event, so this module is a
// pure listener. A second poller here would spend one of the device's seven
// httpd sockets to learn something the page already knows — see the comment at
// the top of poll.js for what happens when those sockets run out.

const $ = (id) => document.getElementById(id);

function fmtClock(ms) {
    if (!isFinite(ms) || ms < 0) return '--:--';
    const t = Math.round(ms / 1000);
    const h = Math.floor(t / 3600);
    const m = Math.floor((t % 3600) / 60);
    const s = t % 60;
    const mm = String(m).padStart(h ? 2 : 1, '0');
    return (h ? h + ':' : '') + mm + ':' + String(s).padStart(2, '0');
}

// Largest timestamp any line in a .ledc carries — the session's length.
//
// Deliberately a loose scan rather than a call into the real parser: this value
// only ever draws a progress bar, so being wrong costs a wrong-width bar and
// nothing else. Lines it cannot read are skipped rather than guessed at.
export function sessionDurationMs(text) {
    let max = 0;
    for (const raw of String(text || '').split('\n')) {
        const line = raw.split('#')[0].trim();
        if (!line) continue;
        const tok = line.split(/\s+/);
        // "A <time> …" / "S <time> …" put the timestamp second; BG has none.
        const head = tok[0].toLowerCase();
        if (head === 'bg') continue;
        const t = (head === 'a' || head === 's') ? Number(tok[1]) : Number(tok[0]);
        if (Number.isFinite(t) && t > max) max = t;
    }
    return max;
}

// Cache the parse: the editor text only changes when a config is loaded or
// typed into, but a devicestate tick arrives every second.
let cachedText = null, cachedDur = 0;
function durationNow() {
    const ta = $('exampleConfig');
    const text = ta ? ta.value : '';
    if (text !== cachedText) { cachedText = text; cachedDur = sessionDurationMs(text); }
    return cachedDur;
}

function renderMeta() {
    const meta = $('homeSrcMeta');
    if (!meta) return;
    const d = durationNow();
    const lines = (cachedText || '').split('\n').filter(l => l.trim() && !l.trim().startsWith('#')).length;
    meta.textContent = d > 0 ? `${lines} entries · ${fmtClock(d)}` : (lines ? `${lines} entries` : '');
}

function render(st) {
    const box = $('homeNow');
    if (!box) return;

    const running = !!(st && st.timeline && st.timeline.running);
    const pos = (st && st.timeline && st.timeline.position_ms) || 0;
    const dur = durationNow();

    box.setAttribute('data-running', running ? 'true' : 'false');
    $('homeNowLabel').textContent = running ? 'Playing' : 'Idle';

    const file = ($('loadedFilename') && $('loadedFilename').textContent.trim()) || '';
    $('homeNowSub').textContent = running
        ? (file || 'Session in progress')
        : (file ? 'Loaded — ' + file : 'No session running');

    $('homeNowClock').textContent = running
        ? fmtClock(pos) + (dur > 0 ? ' / ' + fmtClock(dur) : '')
        : (dur > 0 ? fmtClock(dur) : '--:--');

    const bar = $('homeNowBar');
    if (bar) {
        const pct = (running && dur > 0) ? Math.max(0, Math.min(100, (pos / dur) * 100)) : 0;
        bar.style.width = pct + '%';
    }
    renderMeta();
}

export function homeInit() {
    window.addEventListener('devicestate', (e) => render(e.detail));
    const ta = $('exampleConfig');
    if (ta) ta.addEventListener('input', () => { render(null); });
    // Paint once before the first poll lands, so the strip is never blank.
    render(null);
}
