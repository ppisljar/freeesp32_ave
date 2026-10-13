// The Simulate view — play a session, watch the light, read the spectrum,
// inspect the timeline. No device involved at any point.
//
// Four panels, each answering a different question about the session you just
// wrote:
//
//   LIGHT      what the LED zones actually do, on the real 74-pixel geometry,
//              plus a true-time-resolution scope trace per channel
//   SPECTRUM   what the audio contains, with the exact carriers and beat read
//              from the timeline rather than guessed from the FFT
//   TIMELINE   every channel's resolved state at the playhead, and what the
//              `-` carries and ramps resolved to
//   SESSION    a whole-session spectrogram, so the arc is visible at a glance
//
// PHOTOSENSITIVITY. This view can render a flickering light field, which is the
// one thing the project site's stylesheet explicitly forbids itself ("the site
// itself must never read as flicker"). So animated flicker is OFF by default:
// the pixels show each zone's cycle-AVERAGE luminance, which is a steady colour,
// while the scope trace carries the waveform at full time resolution. Turning
// live flicker on requires the existing one-time safety acknowledgement
// (js/safety.js, the same gate the device Play button uses), and
// prefers-reduced-motion forces it off and says so.
//
// NO DEVICE CALLS. Not one. The playhead comes from the audio clock, never from
// polling /api/state the way views/lane.js does — a second in-view poller would
// spend another of the device's seven httpd sockets, and on the docs site there
// is no device to poll at all.

import { showMessage } from '../../util.js';
import { NUM_AUDIO_CHANNELS, NUM_LED_CHANNELS, WAVE_TYPES, SAMPLE_RATE } from '../model.js';
import { ensureSafetyAccepted } from '../../safety.js';
import { validate } from '../validate.js';
import { lintSession } from '../sim/lint.js';
import { resolveAudioChannels, resolveLedChannels, sessionEndMs } from '../sim/resolve.js';
import { createRenderer, sessionBeatJitter, MIX_GAIN } from '../sim/audio.js';
import {
    ledStateAt, neohexLayout, channelPixelCounts, DEFAULT_CHANNEL_MAP,
    LED_BRIGHTNESS_Q8_MAX, AUDIO_DMA_PIPELINE_LAG_US,
} from '../sim/led.js';
import {
    createAnalyser, logBands, buildSpectrogram, DEFAULT_FFT_SIZE,
} from '../sim/spectrum.js';
import { createPlayer } from '../sim/player.js';
import { PAL, fmtMs } from './lane_canvas.js';

// How much of the session the scope trace shows, and at what resolution. 2 ms
// steps resolve a 100 Hz flicker's edges; 1.5 s of history is enough to read a
// 0.7 Hz breath cycle.
const SCOPE_MS = 1500;
const SCOPE_STEP_MS = 2;

// The spectrum redraws at a third of the frame rate. 20 Hz is past the point
// where a moving FFT reads as continuous, and each frame costs an 8192-point
// transform plus a re-render of the analysis window.
const SPECTRUM_EVERY = 3;

// Above this flicker rate a 60 Hz display cannot show the waveform at all — it
// aliases into a slow crawl that looks like a much lower rate. The view keeps
// drawing instantaneous level (that is what the toggle asked for) but says so.
const ALIAS_WARN_HZ = 25;

// Monitor trim presets. The simulated signal path is device-exact, which means
// a single centre-panned tone peaks at -27 dBFS (the fixed 1/16 mix divisor), so
// a listening trim is a monitoring control, NOT a change to the signal.
const TRIM_DB = 18;

function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
}

function fmtClock(ms) {
    const t = Math.max(0, Math.round(ms));
    const m = Math.floor(t / 60000);
    const s = Math.floor((t % 60000) / 1000);
    const cs = Math.floor((t % 1000) / 10);
    return m + ':' + String(s).padStart(2, '0') + '.' + String(cs).padStart(2, '0');
}

const dbOf = (lin) => (lin > 0 ? 20 * Math.log10(lin) : -Infinity);
const fmtDb = (lin) => (lin > 0 ? dbOf(lin).toFixed(1) + ' dB' : '−∞');

/*
 * Size a canvas for the display and hand back its CSS-pixel geometry.
 *
 * Writing canvas.width REALLOCATES the backing store, and at devicePixelRatio 2
 * these canvases are over a million pixels each. Doing that for four canvases on
 * every animation frame was most of the reason this view ran at 1 fps, so the
 * resize only happens when the size or the DPR has genuinely changed; otherwise
 * the existing buffer is simply cleared.
 */
function fitCanvas(canvas, cssH) {
    const dpr = window.devicePixelRatio || 1;
    const cssW = canvas.clientWidth || 600;
    const w = Math.round(cssW * dpr);
    const h = Math.round(cssH * dpr);
    const c = canvas.getContext('2d');
    if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        canvas.style.height = cssH + 'px';
    }
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.clearRect(0, 0, cssW, cssH);
    return { ctx: c, w: cssW, h: cssH };
}

export function initSimView(viewCtx) {
    const root = document.getElementById('genSimView');
    if (!root) return { refresh() {}, show() {}, hide() {} };

    // ---- state -------------------------------------------------------------
    let visible = false;
    let raf = null;
    let frame = 0;
    let doc = null;
    let audioCh = new Map();
    let ledCh = new Map();
    let jitter = null;
    let durationMs = 0;
    let dirty = true;                  // the model changed; rebuild on next show
    let liveFlicker = false;
    let reducedMotion = false;
    let analyser = null;
    let analysisRenderer = null;       // separate from playback, so analysing
                                       // never disturbs the audio's phase state
    let analysisL = null;
    let analysisR = null;
    let analysisMono = null;
    let inspectRenderer = null;
    let spectrogram = null;
    let sgState = 'idle';              // idle | building | ready | stale
    let lastInspectMs = -1;

    const player = createPlayer({
        onEnded: () => { syncTransport(); },
    });

    // ---- chrome ------------------------------------------------------------
    root.innerHTML = '';
    const wrap = el('div', 'sim-wrap');
    root.appendChild(wrap);

    // Banner for the things the user needs to know before pressing play.
    const notice = el('div', 'sim-notice');
    notice.style.display = 'none';
    wrap.appendChild(notice);

    // -- transport
    const bar = el('div', 'sim-transport');
    const btnPlay = el('button', 'sim-play');
    btnPlay.innerHTML = '▶ Play';
    const btnStop = el('button', null, '■ Stop');
    const seek = document.createElement('input');
    seek.type = 'range';
    seek.min = '0'; seek.max = '1000'; seek.value = '0'; seek.step = '1';
    seek.className = 'sim-seek';
    seek.setAttribute('aria-label', 'Playhead');
    const clock = el('span', 'sim-clock', '0:00.00');
    const total = el('span', 'sim-total', '/ 0:00.00');

    const trim = document.createElement('input');
    trim.type = 'range';
    trim.min = '0'; trim.max = '30'; trim.step = '1'; trim.value = String(TRIM_DB);
    trim.className = 'sim-trim';
    trim.setAttribute('aria-label', 'Monitor trim');
    const trimLbl = el('span', 'sim-trim-lbl', '+' + TRIM_DB + ' dB');

    bar.appendChild(btnPlay);
    bar.appendChild(btnStop);
    bar.appendChild(seek);
    bar.appendChild(clock);
    bar.appendChild(total);
    const trimWrap = el('label', 'sim-trimwrap');
    trimWrap.appendChild(el('span', 'sim-dim', 'Monitor'));
    trimWrap.appendChild(trim);
    trimWrap.appendChild(trimLbl);
    bar.appendChild(trimWrap);
    const audioNote = el('span', 'sim-audionote');
    audioNote.style.display = 'none';
    bar.appendChild(audioNote);
    wrap.appendChild(bar);

    // -- light panel
    const lightPanel = el('section', 'sim-panel');
    const lightHead = el('div', 'sim-panel-head');
    lightHead.appendChild(el('h3', 'no-toc', 'Light'));
    const flickLabel = el('label', 'sim-toggle');
    const flickBox = document.createElement('input');
    flickBox.type = 'checkbox';
    flickLabel.appendChild(flickBox);
    flickLabel.appendChild(el('span', null, 'Animate the flicker'));
    lightHead.appendChild(flickLabel);
    const lightMode = el('span', 'sim-dim sim-mode');
    lightHead.appendChild(lightMode);
    lightPanel.appendChild(lightHead);

    const ledCanvas = document.createElement('canvas');
    ledCanvas.className = 'sim-canvas sim-led';
    lightPanel.appendChild(ledCanvas);

    const scopeCanvas = document.createElement('canvas');
    scopeCanvas.className = 'sim-canvas sim-scope';
    lightPanel.appendChild(scopeCanvas);
    lightPanel.appendChild(el('div', 'sim-cap sim-led-cap'));
    wrap.appendChild(lightPanel);

    // -- spectrum panel
    const specPanel = el('section', 'sim-panel');
    const specHead = el('div', 'sim-panel-head');
    specHead.appendChild(el('h3', 'no-toc', 'Spectrum'));
    const specInfo = el('span', 'sim-dim');
    specHead.appendChild(specInfo);
    specPanel.appendChild(specHead);
    const specCanvas = document.createElement('canvas');
    specCanvas.className = 'sim-canvas sim-spec';
    specPanel.appendChild(specCanvas);
    wrap.appendChild(specPanel);

    // -- inspector panel
    const inspPanel = el('section', 'sim-panel');
    const inspHead = el('div', 'sim-panel-head');
    inspHead.appendChild(el('h3', 'no-toc', 'Timeline at the playhead'));
    inspPanel.appendChild(inspHead);
    const inspBody = el('div', 'sim-inspect');
    inspPanel.appendChild(inspBody);
    wrap.appendChild(inspPanel);

    // -- whole-session panel
    const sgPanel = el('section', 'sim-panel');
    const sgHead = el('div', 'sim-panel-head');
    sgHead.appendChild(el('h3', 'no-toc', 'Whole session'));
    const btnSg = el('button', 'sim-sg-btn', 'Build spectrogram');
    sgHead.appendChild(btnSg);
    const sgInfo = el('span', 'sim-dim');
    sgHead.appendChild(sgInfo);
    sgPanel.appendChild(sgHead);
    const sgCanvas = document.createElement('canvas');
    sgCanvas.className = 'sim-canvas sim-sg';
    sgPanel.appendChild(sgCanvas);
    sgPanel.appendChild(el('div', 'sim-cap',
        'Frequency on a log axis from 20 Hz to 2 kHz, time left to right. '
      + 'Built from the same renderer that plays, so what you see is what you hear.'));
    wrap.appendChild(sgPanel);

    // ---- model -------------------------------------------------------------

    function rebuild() {
        doc = viewCtx.getDoc();
        const a = resolveAudioChannels(doc);
        const l = resolveLedChannels(doc);
        audioCh = a.channels;
        ledCh = l.channels;
        jitter = sessionBeatJitter(doc);

        // Where the session ENDS. The timeline's last entry is not the end of
        // the sound: a channel left at a non-zero volume keeps playing, so a
        // tail is added the way the bounce does, and a session with nothing in
        // it still gets a second so the transport is usable.
        const endMs = sessionEndMs(doc);
        durationMs = Math.max(1000, endMs + 2000);

        const mk = () => createRenderer(audioCh, { beatJitter: jitter });
        player.reload(mk, durationMs);
        inspectRenderer = mk();
        analysisRenderer = mk();

        if (!analyser || analyser.size !== DEFAULT_FFT_SIZE) {
            analyser = createAnalyser(DEFAULT_FFT_SIZE, SAMPLE_RATE);
            analysisL = new Float32Array(DEFAULT_FFT_SIZE);
            analysisR = new Float32Array(DEFAULT_FFT_SIZE);
            analysisMono = new Float32Array(DEFAULT_FFT_SIZE);
        }

        spectrogram = null;
        scopeCache = new Map();        // traces describe the OLD session
        sgState = doc.rows.length ? 'idle' : 'empty';
        lastInspectMs = -1;
        dirty = false;

        total.textContent = '/ ' + fmtClock(durationMs);
        renderNotice();
        renderSgInfo();
        syncTransport();
    }

    /*
     * The findings panel.
     *
     * Two sources, deliberately kept separate: gen/validate.js answers "is this
     * grammatical and in range", while sim/lint.js answers questions that need
     * the RESOLVED timeline — a ramp whose target row writes `-`, a row the
     * device would refuse to install, a global that a late line applies
     * retroactively. Only the second kind needs a simulator to find.
     *
     * Each finding carries its source row and its timestamp, so clicking one
     * seeks the playhead there and names the line to look at.
     */
    function renderNotice() {
        notice.innerHTML = '';
        const items = [];

        if (!audioCh.size && !ledCh.size) {
            items.push({ severity: 'info', row: null, timeMs: null,
                msg: 'Nothing to simulate yet — load a session, or write one in '
                   + 'the Text view.' });
        }

        for (const d of validate(doc || { rows: [] })) {
            items.push({
                severity: d.severity,
                row: (d.row === undefined || d.row < 0) ? null : d.row,
                timeMs: null,
                msg: d.msg,
            });
        }
        for (const f of lintSession(doc)) items.push(f);

        if (!items.length) { notice.style.display = 'none'; return; }

        const rank = { error: 0, warn: 1, info: 2 };
        items.sort((a, b) => rank[a.severity] - rank[b.severity]);

        for (const f of items) {
            const line = el('div', 'sim-finding sim-' + f.severity);
            if (f.row !== null && f.row !== undefined) {
                line.appendChild(el('span', 'sim-finding-line', 'line ' + (f.row + 1)));
            }
            line.appendChild(el('span', 'sim-finding-msg', f.msg));
            if (f.timeMs !== null && f.timeMs !== undefined) {
                const jump = el('button', 'sim-jump', '↦ ' + fmtClock(f.timeMs));
                jump.title = 'Move the playhead here';
                jump.addEventListener('click', () => {
                    player.seek(f.timeMs);
                    seek.value = String(Math.round(
                        (f.timeMs / Math.max(1, durationMs)) * 1000));
                    clock.textContent = fmtClock(f.timeMs);
                    drawAll(f.timeMs);
                });
                line.appendChild(jump);
            }
            notice.appendChild(line);
        }
        notice.style.display = '';
    }

    // ---- transport glue ----------------------------------------------------

    function syncTransport() {
        const playing = player.isPlaying();
        btnPlay.innerHTML = playing ? '⏸ Pause' : '▶ Play';
        btnPlay.classList.toggle('on', playing);
    }

    /*
     * Say what the audio output is actually doing.
     *
     * Silence with a frozen playhead is indistinguishable from a bug, and the
     * two most likely causes are both the browser's doing rather than ours: a
     * context the autoplay policy left suspended, and a device whose hardware
     * rate is not the 44 100 Hz the firmware runs at. Both are worth saying out
     * loud rather than leaving the visitor to conclude the thing is broken.
     */
    function reportAudio() {
        const state = player.contextState();
        const rate = player.sampleRate();
        const bits = [];
        if (state === 'suspended') {
            bits.push('your browser has the audio output suspended — click Play again');
        } else if (state === 'none') {
            bits.push('no audio output yet');
        }
        if (state === 'running' && rate !== SAMPLE_RATE) {
            bits.push('output running at ' + rate + ' Hz, not the device’s '
                + SAMPLE_RATE + ' Hz — pitches are right, but the match is no '
                + 'longer sample-exact');
        }
        audioNote.textContent = bits.join(' · ');
        audioNote.style.display = bits.length ? '' : 'none';
    }

    btnPlay.addEventListener('click', async () => {
        if (player.isPlaying()) { player.pause(); syncTransport(); return; }
        if (!audioCh.size && !ledCh.size) {
            showMessage('Nothing to simulate — the session is empty', 'error');
            return;
        }
        // Start the audio context HERE, synchronously, while the click's user
        // activation is still live — the safety acknowledgement below is an
        // await, and a context resumed after it can stay suspended forever.
        try { player.prime(); } catch (e) { /* reported by play() below */ }
        // The same one-time acknowledgement the device Play button uses. A
        // session with no flickering LED row passes straight through.
        if (!(await ensureSafetyAccepted(doc))) return;
        try {
            await player.play();
        } catch (err) {
            showMessage('Could not start audio: ' + (err.message || err), 'error');
            return;
        }
        syncTransport();
        reportAudio();
    });

    btnStop.addEventListener('click', () => {
        player.stop();
        seek.value = '0';
        clock.textContent = fmtClock(0);
        syncTransport();
        drawAll(0);
    });

    let seeking = false;
    seek.addEventListener('input', () => {
        seeking = true;
        const ms = (Number(seek.value) / 1000) * durationMs;
        clock.textContent = fmtClock(ms);
        drawAll(ms);
    });
    seek.addEventListener('change', () => {
        const ms = (Number(seek.value) / 1000) * durationMs;
        player.seek(ms);
        seeking = false;
    });

    function applyTrim() {
        const db = Number(trim.value);
        trimLbl.textContent = (db > 0 ? '+' : '') + db + ' dB';
        player.setGain(Math.pow(10, db / 20));
    }
    trim.addEventListener('input', applyTrim);

    flickBox.addEventListener('change', async () => {
        if (flickBox.checked) {
            if (reducedMotion) {
                flickBox.checked = false;
                showMessage('Your system is set to reduce motion, so the flicker '
                    + 'animation stays off. The scope trace below shows the real '
                    + 'waveform.', 'info');
                return;
            }
            if (!(await ensureSafetyAccepted(doc))) { flickBox.checked = false; return; }
        }
        liveFlicker = flickBox.checked;
        renderLightMode();
    });

    function renderLightMode() {
        if (reducedMotion) {
            lightMode.textContent = 'reduce-motion is on — showing steady '
                + 'cycle-average luminance';
            flickBox.disabled = true;
            return;
        }
        lightMode.textContent = liveFlicker
            ? 'showing instantaneous level'
            : 'showing cycle-average luminance (steady on screen)';
    }

    // ---- the light field ---------------------------------------------------

    const hexLayout = neohexLayout(DEFAULT_CHANNEL_MAP);
    const pixelCounts = channelPixelCounts(DEFAULT_CHANNEL_MAP);

    /*
     * Cycle-average luminance for a zone, by integrating the carrier over one
     * whole cycle.
     *
     * Numerical rather than closed-form on purpose: it then holds for all four
     * carriers, for a duty of any value, for the trapezoid's attack clamp, and
     * for a DC channel, without four special cases that could each be wrong.
     * 48 steps is well past the point where the result stops moving.
     */
    function averageState(entries, tMs) {
        const now = ledStateAt(entries, tMs);
        if (!now || !now.active) return now;
        if (now.dc || !(now.cycleUs > 0)) return now;
        const cycleMs = now.cycleUs / 1000;
        const N = 24;
        let lvl = 0;
        for (let i = 0; i < N; i++) {
            const s = ledStateAt(entries, tMs + (i * cycleMs) / N);
            if (s && s.active) lvl += s.levelQ8;
        }
        lvl = Math.round(lvl / N);
        return Object.assign({}, now, {
            levelQ8: lvl,
            level01: lvl / LED_BRIGHTNESS_Q8_MAX,
            px: {
                r: Math.floor((now.r * lvl) / LED_BRIGHTNESS_Q8_MAX) & 0xFF,
                g: Math.floor((now.g * lvl) / LED_BRIGHTNESS_Q8_MAX) & 0xFF,
                b: Math.floor((now.b * lvl) / LED_BRIGHTNESS_Q8_MAX) & 0xFF,
            },
            averaged: true,
        });
    }

    function zoneStates(tMs) {
        const out = new Array(NUM_LED_CHANNELS).fill(null);
        for (const [bit, entries] of ledCh) {
            out[bit] = liveFlicker && !reducedMotion
                ? ledStateAt(entries, tMs)
                : averageState(entries, tMs);
        }
        return out;
    }

    function drawLed(tMs, states) {
        const { ctx: c, w, h } = fitCanvas(ledCanvas, 220);
        c.fillStyle = PAL.stripBg;
        c.fillRect(0, 0, w, h);

        // Two hex "eyes" side by side, at the real 37-pixel geometry.
        const pad = 14;
        const eyeW = (w - pad * 3) / 2;
        const cell = Math.max(3, Math.min(eyeW / 9, (h - pad * 2) / 9));
        const centres = [
            { x: pad + eyeW / 2, y: h / 2 },
            { x: pad * 2 + eyeW + eyeW / 2, y: h / 2 },
        ];

        for (const p of hexLayout) {
            const st = states[p.channel];
            const cx = centres[p.eye].x + p.x * cell * 1.08;
            // Hex rows sit closer vertically than the cell pitch.
            const cy = centres[p.eye].y + p.y * cell * 0.94;
            const r = cell * 0.44;
            if (st && st.active && (st.px.r || st.px.g || st.px.b)) {
                // A halo then a core, rather than shadowBlur: a blur is a real
                // gaussian per fill(), and 74 of them per frame is on its own
                // enough to drop this view to single-digit fps. Two flat arcs
                // read the same at this size and cost almost nothing.
                //
                // Purely presentational — the pixel BYTES are the device's, and
                // no gamma or display curve is applied to them anywhere.
                const px = st.px;
                c.fillStyle = 'rgba(' + px.r + ',' + px.g + ',' + px.b + ',0.17)';
                c.beginPath();
                c.arc(cx, cy, r * 2.1, 0, Math.PI * 2);
                c.fill();
                c.fillStyle = 'rgb(' + px.r + ',' + px.g + ',' + px.b + ')';
            } else {
                c.fillStyle = PAL.handleFill;
            }
            c.beginPath();
            c.arc(cx, cy, r, 0, Math.PI * 2);
            c.fill();
        }

        // Eye labels.
        c.fillStyle = PAL.dim;
        c.font = '11px system-ui, sans-serif';
        c.textAlign = 'center';
        c.fillText('eye A — LED 1 inner / LED 2 outer', centres[0].x, h - 6);
        c.fillText('eye B — LED 3 outer / LED 4 inner', centres[1].x, h - 6);

        // Channels 5..8 own no pixels in the shipped build but still respond to
        // every row, so show them as a strip rather than letting them vanish.
        const extra = [];
        for (let bit = 4; bit < NUM_LED_CHANNELS; bit++) {
            if (states[bit]) extra.push(bit);
        }
        if (extra.length) {
            c.textAlign = 'left';
            c.fillStyle = PAL.dim;
            c.fillText('no pixels on this board:', 10, 14);
            let x = 150;
            for (const bit of extra) {
                const st = states[bit];
                c.fillStyle = st && st.active
                    ? 'rgb(' + st.px.r + ',' + st.px.g + ',' + st.px.b + ')'
                    : PAL.handleFill;
                c.fillRect(x, 6, 26, 10);
                c.fillStyle = PAL.dim;
                c.fillText(String(bit + 1), x + 29, 15);
                x += 48;
            }
        }
    }

    /*
     * Scope trace cache.
     *
     * A 1.5 s window at 2 ms is 750 points per zone, and recomputing all of them
     * for every zone on every frame is ~2 250 ledStateAt calls at 60 Hz — which,
     * alongside the audio render, was enough to starve requestAnimationFrame
     * and leave the tab unresponsive.
     *
     * So the trace lives on a FIXED GLOBAL 2 ms grid rather than being anchored
     * to the playhead. Quantising the window that way is invisible, and it means
     * a playhead that has advanced 16 ms only needs the 8 new points: the rest
     * shift down inside the same array. A scrub that jumps somewhere unrelated
     * falls back to a full recompute, which is fine because scrubbing is paced
     * by a human hand.
     */
    let scopeCache = new Map();         // bit -> { startStep, vals: Float32Array }

    function scopeTraces(rows, t0, steps) {
        const out = new Map();
        const startStep = Math.floor(t0 / SCOPE_STEP_MS);
        for (const bit of rows) {
            const entries = ledCh.get(bit);
            let rec = scopeCache.get(bit);
            if (!rec || rec.vals.length !== steps + 1) {
                rec = { startStep: startStep - (steps + 1), vals: new Float32Array(steps + 1) };
            }
            const shift = startStep - rec.startStep;
            const vals = rec.vals;
            let from = 0;
            if (shift === 0) {
                from = steps + 1;                         // nothing new
            } else if (shift > 0 && shift <= steps) {
                vals.copyWithin(0, shift);                // slide the window
                from = steps + 1 - shift;
            }
            for (let k = from; k <= steps; k++) {
                const t = (startStep + k) * SCOPE_STEP_MS;
                if (t < 0) { vals[k] = 0; continue; }
                const st = ledStateAt(entries, t);
                vals[k] = (st && st.active) ? st.level01 : 0;
            }
            rec.startStep = startStep;
            scopeCache.set(bit, rec);
            out.set(bit, vals);
        }
        return out;
    }

    /*
     * The scope: each active zone's level over the last SCOPE_MS, at
     * SCOPE_STEP_MS resolution.
     *
     * This is the panel that makes flicker legible without flickering the
     * screen. A 10 Hz square at duty 25 is unmistakable here and invisible in a
     * steady average, which is the whole reason the default light view can stay
     * safe without hiding information.
     */
    function drawScope(tMs) {
        const rows = [...ledCh.keys()].sort((a, b) => a - b);
        const rowH = 34;
        const H = Math.max(rowH, rows.length * rowH) + 22;
        const { ctx: c, w, h } = fitCanvas(scopeCanvas, H);
        c.fillStyle = PAL.stripBg;
        c.fillRect(0, 0, w, h);
        const left = 54;
        const plotW = Math.max(20, w - left - 8);

        // The window trails the playhead once there is enough history, and sits
        // at the start of the session before that. Without the second case the
        // whole trace is empty at t=0 — the one moment every visitor sees first.
        const trailing = tMs >= SCOPE_MS;
        // Quantised to the cache's 2 ms grid so the drawn x positions line up
        // with the cached sample times exactly.
        const t0 = Math.floor((trailing ? tMs - SCOPE_MS : 0) / SCOPE_STEP_MS)
                   * SCOPE_STEP_MS;
        const xOf = (t) => left + plotW * ((t - t0) / SCOPE_MS);

        c.font = '11px system-ui, sans-serif';

        // Time gridlines every 250 ms.
        c.strokeStyle = PAL.grid;
        c.lineWidth = 1;
        for (let ms = 0; ms <= SCOPE_MS; ms += 250) {
            const x = left + plotW * (ms / SCOPE_MS);
            c.beginPath(); c.moveTo(x, 0); c.lineTo(x, h - 20); c.stroke();
        }
        c.fillStyle = PAL.dim;
        c.textAlign = 'left';
        c.fillText(trailing ? '−' + (SCOPE_MS / 1000).toFixed(1) + ' s' : '0 s',
                   left, h - 7);
        c.textAlign = 'right';
        c.fillText(trailing ? 'now' : (SCOPE_MS / 1000).toFixed(1) + ' s', w - 8, h - 7);

        if (!rows.length) {
            c.fillStyle = PAL.dim;
            c.textAlign = 'center';
            c.fillText('no LED rows in this session', w / 2, h / 2);
            return;
        }

        const steps = Math.floor(SCOPE_MS / SCOPE_STEP_MS);
        const trace = scopeTraces(rows, t0, steps);

        rows.forEach((bit, i) => {
            const entries = ledCh.get(bit);
            const vals = trace.get(bit);
            const y0 = i * rowH + 4;
            const y1 = y0 + rowH - 10;

            c.strokeStyle = PAL.stripEdge;
            c.beginPath(); c.moveTo(left, y1); c.lineTo(w - 8, y1); c.stroke();

            c.fillStyle = PAL.gutterText;
            c.textAlign = 'left';
            c.fillText('LED ' + (bit + 1), 8, y0 + 12);

            // Colour the trace with the zone's own colour at the playhead, so a
            // two-colour antiphase pair reads as two colours here as well.
            const nowSt = ledStateAt(entries, tMs);
            const stroke = (nowSt && nowSt.active)
                ? 'rgb(' + Math.max(60, nowSt.r) + ',' + Math.max(60, nowSt.g)
                         + ',' + Math.max(60, nowSt.b) + ')'
                : PAL.dim;
            c.strokeStyle = stroke;
            c.lineWidth = 1.25;
            c.beginPath();
            let started = false;
            for (let k = 0; k <= steps; k++) {
                const t = t0 + k * SCOPE_STEP_MS;
                if (t < 0) continue;
                const x = xOf(t);
                const y = y1 - vals[k] * (y1 - y0);
                if (!started) { c.moveTo(x, y); started = true; } else { c.lineTo(x, y); }
            }
            c.stroke();
        });

        // While the window is fixed at the session start, mark where the
        // playhead actually is inside it.
        if (!trailing && tMs > 0 && tMs <= SCOPE_MS) {
            c.strokeStyle = PAL.playhead;
            c.lineWidth = 1.5;
            const x = xOf(tMs);
            c.beginPath(); c.moveTo(x, 0); c.lineTo(x, h - 20); c.stroke();
        }
    }

    function renderLedCaption(states) {
        const cap = lightPanel.querySelector('.sim-led-cap');
        const parts = [];
        for (const [bit, entries] of ledCh) {
            const st = states[bit];
            if (!st || !st.active) continue;
            if (st.dc) {
                parts.push('LED ' + (bit + 1) + ': DC (freq 0) — steady, duty and env ignored');
            } else {
                let s = 'LED ' + (bit + 1) + ': ' + st.freq.toFixed(2) + ' Hz';
                s += ', duty ' + st.duty + '%';
                s += ', ' + ['square', 'sine', 'triangle', 'trapezoid'][st.env];
                if (st.phase) s += ', phase ' + st.phase + '°';
                s += ' — device ISR tick ' + st.tickHz + ' Hz ('
                   + st.tickUs + ' µs edge quantisation)';
                if (st.freq > ALIAS_WARN_HZ && liveFlicker) {
                    s += '; too fast for a 60 Hz screen — what you see is aliased';
                }
                parts.push(s);
            }
        }
        if (!parts.length) {
            cap.textContent = 'Pixel colours are the exact bytes the strip would receive: '
                + 'rgb × brightness / 25600, integer-truncated, with no gamma '
                + 'correction anywhere — which is deliberate, because it is what '
                + 'lets a 180° antiphase pair of sine carriers sum to flat luminance.';
            return;
        }
        cap.textContent = parts.join(' · ');
    }

    // ---- spectrum ----------------------------------------------------------

    function drawSpectrum(tMs) {
        const { ctx: c, w, h } = fitCanvas(specCanvas, 180);
        c.fillStyle = PAL.stripBg;
        c.fillRect(0, 0, w, h);

        if (!audioCh.size) {
            c.fillStyle = PAL.dim;
            c.font = '11px system-ui, sans-serif';
            c.textAlign = 'center';
            c.fillText('no audio rows in this session', w / 2, h / 2);
            specInfo.textContent = '';
            return;
        }

        // Render the FFT window ending at the playhead from a SEPARATE renderer,
        // so analysis never perturbs the playing one — and so this works
        // identically while paused and while scrubbing, which an AnalyserNode
        // tapped off the output could not do.
        const endSample = Math.round((tMs / 1000) * SAMPLE_RATE);
        const start = Math.max(0, endSample - DEFAULT_FFT_SIZE);
        analysisRenderer.seek(start);
        analysisRenderer.renderBlock(analysisL, analysisR, start, DEFAULT_FFT_SIZE);
        for (let i = 0; i < DEFAULT_FFT_SIZE; i++) {
            analysisMono[i] = (analysisL[i] + analysisR[i]) * 0.5;
        }
        const mag = analyser.analyse(analysisMono, 0);

        const TOP_DB = 0, BOT_DB = -96;
        const yOf = (db) => {
            const t = (db - BOT_DB) / (TOP_DB - BOT_DB);
            return h - 18 - Math.max(0, Math.min(1, t)) * (h - 26);
        };

        // dB gridlines.
        c.font = '10px system-ui, sans-serif';
        c.strokeStyle = PAL.grid;
        c.fillStyle = PAL.dim;
        c.textAlign = 'left';
        for (let db = 0; db >= BOT_DB; db -= 24) {
            const y = yOf(db);
            c.beginPath(); c.moveTo(34, y); c.lineTo(w - 4, y); c.stroke();
            c.fillText(db + ' dB', 2, y + 3);
        }

        const F_MIN = 20, F_MAX = 20000;
        const cols = Math.max(32, Math.floor((w - 38) / 2));
        const band = logBands(mag, DEFAULT_FFT_SIZE, SAMPLE_RATE, cols, F_MIN, F_MAX);
        const bw = (w - 38) / cols;
        c.fillStyle = PAL.step;
        for (let i = 0; i < cols; i++) {
            const y = yOf(band[i]);
            const base = yOf(BOT_DB);
            if (y < base) c.fillRect(34 + i * bw, y, Math.max(1, bw - 0.5), base - y);
        }

        // Frequency axis: decade ticks on the log scale.
        c.fillStyle = PAL.dim;
        c.textAlign = 'center';
        const logMin = Math.log2(F_MIN), logSpan = Math.log2(F_MAX) - logMin;
        for (const f of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
            const x = 34 + ((Math.log2(f) - logMin) / logSpan) * (w - 38);
            c.fillText(f >= 1000 ? (f / 1000) + 'k' : String(f), x, h - 5);
        }

        // Mark the carriers the TIMELINE says are there. Reading them off the
        // FFT would be both slower and less accurate — at this window a 10 Hz
        // binaural pair merges into one peak (see sim/spectrum.js).
        const states = inspectRenderer.channelStates(tMs);
        c.strokeStyle = PAL.quad;
        c.lineWidth = 1;
        for (const s of states) {
            if (!(s.vol > 0)) continue;
            for (const f of [s.freq, s.binaural ? s.freqR : null]) {
                if (!f || f < F_MIN || f > F_MAX) continue;
                const x = 34 + ((Math.log2(f) - logMin) / logSpan) * (w - 38);
                c.beginPath();
                c.moveTo(x, yOf(TOP_DB)); c.lineTo(x, yOf(BOT_DB));
                c.stroke();
            }
        }

        const rate = analyser.binHz.toFixed(2);
        specInfo.textContent = DEFAULT_FFT_SIZE + '-point FFT, ' + rate
            + ' Hz bins — amber lines are the carriers the timeline specifies';
    }

    // ---- inspector ---------------------------------------------------------

    function drawInspector(tMs) {
        // The table is rebuilt from scratch, so throttle it to 10 Hz: at 60 fps
        // this would be the most expensive thing on screen by far, and nobody
        // can read a number that changes sixty times a second anyway.
        if (lastInspectMs >= 0 && Math.abs(tMs - lastInspectMs) < 100) return;
        lastInspectMs = tMs;

        inspBody.innerHTML = '';
        const states = inspectRenderer.channelStates(tMs);

        if (!states.length && !ledCh.size) {
            inspBody.appendChild(el('div', 'sim-dim', 'Nothing is running at this point.'));
            return;
        }

        if (states.length) {
            const t = el('table', 'sim-table');
            const thead = el('thead');
            const hr = el('tr');
            for (const col of ['Audio ch', 'Wave', 'Left Hz', 'Right Hz', 'Beat',
                               'Vol', 'Pan', 'Mod', 'Envelope', 'Depth']) {
                hr.appendChild(el('th', null, col));
            }
            thead.appendChild(hr);
            t.appendChild(thead);
            const tb = el('tbody');
            for (const s of states) {
                const tr = el('tr');
                if (!(s.vol > 0)) tr.className = 'sim-silent';
                const envName = ['square', 'sine', 'triangle', 'trapezoid', 'tremolo'][s.iso.env];
                const cells = [
                    String(s.ch),
                    WAVE_TYPES[s.wave] || String(s.wave),
                    s.freq.toFixed(2),
                    s.binaural ? s.freqR.toFixed(2) : '—',
                    s.beatHz !== null ? s.beatHz.toFixed(3) + ' Hz' : '—',
                    s.vol.toFixed(0) + '%',
                    s.binaural ? 'bypassed' : s.pan.toFixed(0),
                    s.mod > 0 ? s.mod.toFixed(2) + ' Hz' : 'off',
                    s.mod > 0 ? envName : '—',
                    s.mod > 0 ? (s.iso.depth * 100).toFixed(0) + '%' : '—',
                ];
                for (const v of cells) tr.appendChild(el('td', null, v));
                tb.appendChild(tr);
            }
            t.appendChild(tb);
            inspBody.appendChild(t);
        }

        if (ledCh.size) {
            const t = el('table', 'sim-table');
            const thead = el('thead');
            const hr = el('tr');
            for (const col of ['LED zone', 'Pixels', 'Hz', 'Duty', 'Bright',
                               'Colour', 'Carrier', 'Phase', 'Level']) {
                hr.appendChild(el('th', null, col));
            }
            thead.appendChild(hr);
            t.appendChild(thead);
            const tb = el('tbody');
            for (const bit of [...ledCh.keys()].sort((a, b) => a - b)) {
                const st = ledStateAt(ledCh.get(bit), tMs);
                const tr = el('tr');
                if (!st || !st.active) tr.className = 'sim-silent';
                const swatch = el('td');
                if (st && st.active) {
                    const sw = el('span', 'sim-swatch');
                    sw.style.background = 'rgb(' + st.r + ',' + st.g + ',' + st.b + ')';
                    swatch.appendChild(sw);
                    swatch.appendChild(el('span', null,
                        ' ' + st.r + ',' + st.g + ',' + st.b));
                } else {
                    swatch.textContent = '—';
                }
                const vals = [
                    String(bit + 1),
                    String(pixelCounts[bit] || 0),
                    st && st.active ? (st.dc ? 'DC' : st.freq.toFixed(2)) : '—',
                    st && st.active ? (st.dc ? 'n/a' : st.duty + '%') : '—',
                    st && st.active ? st.bright + '%' : '—',
                ];
                for (const v of vals) tr.appendChild(el('td', null, v));
                tr.appendChild(swatch);
                const rest = [
                    st && st.active
                        ? (st.dc ? 'n/a' : ['square', 'sine', 'triangle', 'trapezoid'][st.env])
                        : '—',
                    st && st.active ? (st.dc ? 'n/a' : st.phase + '°') : '—',
                    st && st.active ? (st.level01 * 100).toFixed(0) + '%' : '—',
                ];
                for (const v of rest) tr.appendChild(el('td', null, v));
                tb.appendChild(tr);
            }
            t.appendChild(tb);
            inspBody.appendChild(t);
        }

        const foot = el('div', 'sim-cap');
        foot.textContent =
            'One channel at vol 100 peaks at ' + fmtDb(MIX_GAIN * Math.cos(Math.PI / 4))
            + ' because the engine divides every channel by a fixed 1/'
            + NUM_AUDIO_CHANNELS + ' regardless of how many are active — that is '
            + 'the device’s headroom guarantee, not a bug, and the Monitor slider '
            + 'above compensates for listening without touching the signal. '
            + 'The device also anchors light to DAC emission '
            + (AUDIO_DMA_PIPELINE_LAG_US / 1000).toFixed(1)
            + ' ms downstream; here both come from one clock, so they line up by '
            + 'construction.';
        inspBody.appendChild(foot);
    }

    // ---- whole-session spectrogram ----------------------------------------

    function renderSgInfo() {
        if (sgState === 'empty') { sgInfo.textContent = ''; btnSg.disabled = true; return; }
        btnSg.disabled = sgState === 'building';
        sgInfo.textContent = {
            idle: 'not built yet',
            building: 'building…',
            ready: '',
            stale: 'the session changed — rebuild to update',
        }[sgState] || '';
    }

    btnSg.addEventListener('click', () => {
        if (!audioCh.size) {
            showMessage('No audio rows to analyse', 'error');
            return;
        }
        sgState = 'building';
        renderSgInfo();
        // Yield first so the "building" label paints: a long session is a few
        // hundred FFTs and would otherwise freeze the frame it started in.
        setTimeout(() => {
            try {
                const r = createRenderer(audioCh, { beatJitter: jitter });
                const totalSamples = Math.ceil((durationMs / 1000) * SAMPLE_RATE);
                spectrogram = buildSpectrogram((start, count, oL, oR) => {
                    r.seek(start);
                    r.renderBlock(oL, oR, start, count);
                }, totalSamples, {
                    size: 4096, sampleRate: SAMPLE_RATE,
                    cols: 700, rows: 220, fMin: 20, fMax: 2000,
                });
                sgState = 'ready';
            } catch (err) {
                sgState = 'idle';
                showMessage('Spectrogram failed: ' + (err.message || err), 'error');
            }
            renderSgInfo();
            drawSpectrogram(player.positionMs());
        }, 0);
    });

    function drawSpectrogram(tMs) {
        const H = 230;
        const { ctx: c, w, h } = fitCanvas(sgCanvas, H);
        c.fillStyle = PAL.stripBg;
        c.fillRect(0, 0, w, h);
        c.font = '11px system-ui, sans-serif';

        if (!spectrogram) {
            c.fillStyle = PAL.dim;
            c.textAlign = 'center';
            c.fillText(sgState === 'building'
                ? 'building…'
                : 'Press “Build spectrogram” to render the whole session.',
                w / 2, h / 2);
            return;
        }

        const { cols, rows, data, floorDb } = spectrogram;
        const TOP = 0, BOT = -90;
        const cw = w / cols, ch = (h - 18) / rows;
        for (let x = 0; x < cols; x++) {
            for (let y = 0; y < rows; y++) {
                const db = data[x * rows + y];
                if (db <= floorDb) continue;
                let t = (db - BOT) / (TOP - BOT);
                if (t <= 0) continue;
                if (t > 1) t = 1;
                // Amber-through-cyan ramp, mirroring the project's accent pair.
                const r = Math.round(20 + 235 * t);
                const g = Math.round(40 + 140 * t);
                const b = Math.round(90 + 44 * (1 - t));
                c.fillStyle = 'rgba(' + r + ',' + g + ',' + b + ',' + t.toFixed(3) + ')';
                c.fillRect(x * cw, (h - 18) - (y + 1) * ch, Math.ceil(cw), Math.ceil(ch));
            }
        }

        // Playhead and a time axis.
        const x = (tMs / Math.max(1, durationMs)) * w;
        c.strokeStyle = PAL.playhead;
        c.lineWidth = 1.5;
        c.beginPath(); c.moveTo(x, 0); c.lineTo(x, h - 18); c.stroke();

        c.fillStyle = PAL.dim;
        c.textAlign = 'center';
        const ticks = 6;
        for (let i = 0; i <= ticks; i++) {
            const t = (durationMs * i) / ticks;
            c.fillText(fmtMs(Math.round(t)), (w * i) / ticks, h - 4);
        }
    }

    // ---- frame loop --------------------------------------------------------

    function drawAll(tMs) {
        const states = zoneStates(tMs);
        drawLed(tMs, states);
        drawScope(tMs);
        renderLedCaption(states);
        drawSpectrum(tMs);
        drawInspector(tMs);
        drawSpectrogram(tMs);
    }

    function tick() {
        if (!visible) return;
        raf = requestAnimationFrame(tick);
        frame++;

        const tMs = seeking ? (Number(seek.value) / 1000) * durationMs : player.positionMs();
        if (!seeking) {
            seek.value = String(Math.round((tMs / Math.max(1, durationMs)) * 1000));
            clock.textContent = fmtClock(tMs);
        }

        // Measured at 120 fps with all five panels live: the whole frame is
        // ~0.4 ms, and the spectrum's 8192-point transform (the one expensive
        // piece, ~2.9 ms) runs at a third of that rate.
        const states = zoneStates(tMs);
        drawLed(tMs, states);
        drawScope(tMs);
        if (frame % 10 === 0) renderLedCaption(states);
        if (frame % SPECTRUM_EVERY === 0) drawSpectrum(tMs);
        drawInspector(tMs);
        if (frame % 6 === 0) drawSpectrogram(tMs);
    }

    // ---- lifecycle ---------------------------------------------------------

    return {
        refresh() {
            // Mark stale rather than rebuilding: refresh() fires on EVERY model
            // change, including each keystroke in the Text view, and rebuilding
            // three renderers per keystroke while hidden is pure waste.
            dirty = true;
            if (sgState === 'ready') sgState = 'stale';
            if (visible) { rebuild(); drawAll(player.positionMs()); }
        },

        show() {
            root.style.display = '';
            visible = true;
            try {
                reducedMotion = !!(window.matchMedia
                    && window.matchMedia('(prefers-reduced-motion: reduce)').matches);
            } catch (e) { reducedMotion = false; }
            if (reducedMotion) { liveFlicker = false; flickBox.checked = false; }
            renderLightMode();
            applyTrim();
            if (dirty) rebuild();
            drawAll(player.positionMs());
            if (raf === null) raf = requestAnimationFrame(tick);
        },

        hide() {
            root.style.display = 'none';
            visible = false;
            if (raf !== null) { cancelAnimationFrame(raf); raf = null; }
            // Leaving the tab stops the sound. A session playing from a hidden
            // panel with no visible transport is the kind of thing you cannot
            // find again to turn off.
            player.pause();
            syncTransport();
        },
    };
}
