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
import { resolveAudioChannels, resolveLedChannels, resolveMarkers, sessionEndMs }
    from '../sim/resolve.js';
import { createRenderer, sessionBeatJitter, MIX_GAIN } from '../sim/audio.js';
import {
    ledStateAt, neohexLayout, channelPixelCounts, DEFAULT_CHANNEL_MAP,
    LED_BRIGHTNESS_Q8_MAX, AUDIO_DMA_PIPELINE_LAG_US, markerPipelineLagUs,
    measuredLightPixel,
} from '../sim/led.js';
import {
    createAnalyser, createEnvelopeAnalyser, logBands, buildSpectrogram,
    ENV_FFT_SIZE, ENV_DECIM, ENV_DISPLAY_MIN_HZ, ENV_DISPLAY_MAX_HZ,
} from '../sim/spectrum.js';

/*
 * Window for the LIVE carrier spectrum.
 *
 * 4096 points is a 10.8 Hz bin over 93 ms — deliberately shorter than
 * spectrum.js's 8192 default. That default was chosen partly to show a slow
 * gate's sidebands, but the entrainment-band panel below now reads gate and
 * beat rates directly and far better, so the carrier view no longer has to.
 * Halving it halves both the render and the transform, which is the most
 * expensive thing in the frame.
 */
const LIVE_FFT_SIZE = 4096;
import { createPlayer } from '../sim/player.js';
import { exportWav, exportCsv, exportJson } from '../sim/export.js';
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
    let measuredLight = false;     // draw emitted light, not computed drive
    let reducedMotion = false;
    let analyser = null;
    let analysisRenderer = null;       // separate from playback, so analysing
                                       // never disturbs the audio's phase state
    let analysisL = null;
    let analysisR = null;
    let analysisMono = null;
    let inspectRenderer = null;
    let markers = [];
    let envAnalyser = null;
    let envMono = null;            // ENV_FFT_SIZE * ENV_DECIM samples
    let envL = null, envR = null;
    let scopeAudio = null;         // { t0, min:[], max:[] } per channel pair
    let scopeBufL = null, scopeBufR = null;
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
    // Peak meter: the PRE-clamp level, which is the only place a session that
    // overdrives the mix can be seen. After the clamp it is indistinguishable
    // from one that merely touches full scale — on the device as well as here.
    const peakWrap = el('span', 'sim-peakwrap');
    peakWrap.title = 'Peak level before the output clamp';
    const peakBar = el('span', 'sim-peakbar');
    const peakFill = el('i');
    peakBar.appendChild(peakFill);
    peakWrap.appendChild(el('span', 'sim-dim', 'Peak'));
    peakWrap.appendChild(peakBar);
    const peakLbl = el('span', 'sim-peaklbl', '−∞');
    peakWrap.appendChild(peakLbl);
    bar.appendChild(peakWrap);

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
    const lightLabel = el('label', 'sim-toggle');
    const lightBox = document.createElement('input');
    lightBox.type = 'checkbox';
    lightLabel.appendChild(lightBox);
    lightLabel.appendChild(el('span', null, 'Measured light'));
    lightLabel.title = 'Draw the light the LED actually emits rather than the '
        + 'drive the firmware computes. Display only — the pixel bytes in the '
        + 'inspector and the CSV export are unchanged.';
    lightHead.appendChild(lightLabel);
    lightBox.addEventListener('change', () => {
        measuredLight = lightBox.checked;
        renderLightMode();
        // Repaint now rather than waiting for the next frame: when the session
        // is paused there may not BE a next frame, and a toggle whose effect
        // only appears once you press Play reads as a broken toggle.
        if (visible) drawAll(player.positionMs());
    });
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
    specPanel.appendChild(el('div', 'sim-subhead', 'Entrainment band — the envelope'));
    const envCanvas = document.createElement('canvas');
    envCanvas.className = 'sim-canvas sim-env';
    specPanel.appendChild(envCanvas);
    specPanel.appendChild(el('div', 'sim-cap',
        'The spectrum above shows CARRIERS. A 10 Hz binaural beat made of 200 and '
      + '210 Hz has no 10 Hz line there at all — the beat exists only as the '
      + 'amplitude envelope of their sum. This panel transforms that envelope, so '
      + 'it reads the rate the session is actually driving, whether it is built '
      + 'from binaural carriers, a monaural pair or an isochronic gate.'));
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
    // Exports live beside the spectrogram, because both are "take the whole
    // session away and look at it" operations.
    function download(name, data, mime) {
        const blob = new Blob([data], { type: mime });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = name;
        document.body.appendChild(a); a.click();
        setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
    }
    function sessionName(ext) {
        return 'session-' + Math.round(durationMs / 1000) + 's.' + ext;
    }
    function guardedExport(label, fn) {
        if (!doc || !doc.rows.length) { showMessage('Nothing to export', 'error'); return; }
        showMessage('Rendering ' + label + '…', 'info');
        // Yield so the status paints before the synchronous render.
        setTimeout(() => {
            try { fn(); showMessage(label + ' downloaded', 'success'); }
            catch (err) { showMessage(label + ' failed: ' + (err.message || err), 'error'); }
        }, 0);
    }
    const btnWav = el('button', 'sim-sg-btn', '⬇ WAV');
    btnWav.title = 'The simulated audio, at device level (a single tone peaks near −27 dBFS)';
    btnWav.addEventListener('click', () => guardedExport('WAV', () => {
        const { bytes } = exportWav(doc);
        download(sessionName('wav'), bytes, 'audio/wav');
    }));
    const btnCsv = el('button', 'sim-sg-btn', '⬇ CSV');
    btnCsv.title = 'Per-channel audio and LED state sampled at the strip frame period';
    btnCsv.addEventListener('click', () => guardedExport('CSV', () => {
        download(sessionName('csv'), exportCsv(doc), 'text/csv');
    }));
    const btnJson = el('button', 'sim-sg-btn', '⬇ JSON');
    btnJson.addEventListener('click', () => guardedExport('JSON', () => {
        download(sessionName('json'), exportJson(doc), 'application/json');
    }));
    sgHead.appendChild(btnWav);
    sgHead.appendChild(btnCsv);
    sgHead.appendChild(btnJson);

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

        markers = resolveMarkers(doc, markerPipelineLagUs);

        if (!analyser || analyser.size !== LIVE_FFT_SIZE) {
            analyser = createAnalyser(LIVE_FFT_SIZE, SAMPLE_RATE);
            analysisL = new Float32Array(LIVE_FFT_SIZE);
            analysisR = new Float32Array(LIVE_FFT_SIZE);
            analysisMono = new Float32Array(LIVE_FFT_SIZE);
        }
        if (!envAnalyser) {
            envAnalyser = createEnvelopeAnalyser(ENV_FFT_SIZE, SAMPLE_RATE, ENV_DECIM);
            const n = ENV_FFT_SIZE * ENV_DECIM;
            envL = new Float32Array(n);
            envR = new Float32Array(n);
            envMono = new Float32Array(n);
        }
        if (!scopeBufL) {
            // One full scope window, which is also the worst case when the
            // playhead jumps and every bucket has to be refilled.
            const n = Math.ceil((SCOPE_MS / 1000) * SAMPLE_RATE) + SAMPLE_RATE / 100;
            scopeBufL = new Float32Array(n);
            scopeBufR = new Float32Array(n);
        }
        scopeAudio = null;

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
                // Clickable: a finding you cannot get to is only half a finding.
                const go = el('button', 'sim-finding-line', 'line ' + (f.row + 1));
                go.title = 'Open this line in the Text view';
                go.addEventListener('click', () => {
                    if (viewCtx.revealSourceLine) viewCtx.revealSourceLine(f.row);
                });
                line.appendChild(go);
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
        if (visible) drawAll(player.positionMs());
    });

    function renderLightMode() {
        const curve = measuredLight
            ? ' · measured light (non-linear below ~40 % drive)'
            : ' · linear drive, not measured light';
        if (reducedMotion) {
            lightMode.textContent = 'reduce-motion is on — showing steady '
                + 'cycle-average luminance' + curve;
            flickBox.disabled = true;
            return;
        }
        lightMode.textContent = (liveFlicker
            ? 'showing instantaneous level'
            : 'showing cycle-average luminance (steady on screen)') + curve;
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

    // Which zones get a phase wheel: the flickering ones (a DC zone has no
    // cycle for a phase to be an offset into).
    function rows2wheels(states) {
        const out = [];
        for (let bit = 0; bit < NUM_LED_CHANNELS; bit++) {
            const st = states[bit];
            if (!st || !st.active || st.dc) continue;
            out.push({
                n: bit + 1,
                phase: st.phase,
                colour: 'rgb(' + Math.max(70, st.r) + ',' + Math.max(70, st.g)
                      + ',' + Math.max(70, st.b) + ')',
            });
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
                // The one place the measured-light curve may be applied: what
                // is DRAWN. st.px stays the device's bytes everywhere else.
                const px = measuredLight ? measuredLightPixel(st.px) : st.px;
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

        /*
         * Phase wheels, one per flickering zone.
         *
         * `phase` is the subtlest column in the format: it is a LEAD, not a lag
         * (the firmware ADDS the offset to elapsed time), and across the whole
         * shipped session library it only ever takes 0 or 180. A dial makes an
         * antiphase pair self-evident, where two numbers in a table do not.
         */
        {
            const wheels = rows2wheels(states);
            const R = 13, gap = 42;
            let wx = w - 10 - wheels.length * gap + gap / 2;
            const wy = 26;
            c.font = '9px system-ui, sans-serif';
            c.textAlign = 'center';
            for (const wdef of wheels) {
                c.strokeStyle = PAL.stripEdge;
                c.lineWidth = 1;
                c.beginPath(); c.arc(wx, wy, R, 0, Math.PI * 2); c.stroke();
                // 12 o'clock is phase 0, clockwise, so 180 points straight down.
                const a = (wdef.phase - 90) * Math.PI / 180;
                c.strokeStyle = wdef.colour;
                c.lineWidth = 2;
                c.beginPath();
                c.moveTo(wx, wy);
                c.lineTo(wx + Math.cos(a) * (R - 2), wy + Math.sin(a) * (R - 2));
                c.stroke();
                c.fillStyle = PAL.dim;
                c.fillText(wdef.phase + '\u00B0', wx, wy + R + 10);
                c.fillText('L' + wdef.n, wx, wy - R - 3);
                wx += gap;
            }
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
                const p = (st && st.active)
                    ? (measuredLight ? measuredLightPixel(st.px) : st.px) : null;
                c.fillStyle = p ? 'rgb(' + p.r + ',' + p.g + ',' + p.b + ')'
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
     * The audio half of the scope: L and R as a min/max envelope on the SAME
     * time axis as the LED traces.
     *
     * That shared axis is the point. Whether light and sound line up is this
     * project's entire thesis, and until the two are drawn against one clock
     * you can only reason about it. Rendering the 1.5 s window is 66 150
     * samples, so it is recomputed at the spectrum's cadence rather than every
     * frame — the trace only moves 16 ms per frame anyway.
     */
    function audioScope(t0, steps) {
        if (!audioCh.size || !scopeBufL) return null;
        const startStep = Math.floor(t0 / SCOPE_STEP_MS);
        const perStep = Math.max(1, Math.round((SCOPE_STEP_MS / 1000) * SAMPLE_RATE));

        if (!scopeAudio || scopeAudio.steps !== steps) {
            scopeAudio = {
                steps,
                startStep: startStep - (steps + 1),     // force a full fill
                minL: new Float32Array(steps + 1), maxL: new Float32Array(steps + 1),
                minR: new Float32Array(steps + 1), maxR: new Float32Array(steps + 1),
            };
        }
        const a = scopeAudio;
        const shift = startStep - a.startStep;
        let from = 0;
        if (shift === 0) {
            return a;                                   // nothing new this frame
        } else if (shift > 0 && shift <= steps) {
            a.minL.copyWithin(0, shift); a.maxL.copyWithin(0, shift);
            a.minR.copyWithin(0, shift); a.maxR.copyWithin(0, shift);
            from = steps + 1 - shift;
        }

        // Render only the buckets that are actually new. Playing forward that is
        // about eight of them per frame, not all 750 — the difference between
        // ~700 samples of work per frame and 66 150, which is more audio than
        // the renderer can produce in real time.
        const nNew = steps + 1 - from;
        const need = nNew * perStep;
        if (need > 0) {
            const startSample = Math.max(0, Math.round(
                (((startStep + from) * SCOPE_STEP_MS) / 1000) * SAMPLE_RATE));
            const buf = Math.min(need, scopeBufL.length);
            analysisRenderer.seek(startSample);
            analysisRenderer.renderBlock(scopeBufL, scopeBufR, startSample, buf);
            for (let k = from; k <= steps; k++) {
                const o = (k - from) * perStep;
                let lo = 0, hi = 0, lo2 = 0, hi2 = 0;
                for (let i = 0; i < perStep; i++) {
                    const j = o + i;
                    if (j >= buf) break;
                    const l = scopeBufL[j], r = scopeBufR[j];
                    if (l < lo) lo = l; else if (l > hi) hi = l;
                    if (r < lo2) lo2 = r; else if (r > hi2) hi2 = r;
                }
                a.minL[k] = lo; a.maxL[k] = hi; a.minR[k] = lo2; a.maxR[k] = hi2;
            }
        }
        a.startStep = startStep;
        return a;
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
        const audioRows = audioCh.size ? 2 : 0;
        const H = Math.max(rowH, (rows.length + audioRows) * rowH) + 22;
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

        if (!rows.length && !audioRows) {
            c.fillStyle = PAL.dim;
            c.textAlign = 'center';
            c.fillText('nothing to show for this session', w / 2, h / 2);
            return;
        }

        const steps = Math.floor(SCOPE_MS / SCOPE_STEP_MS);
        const trace = scopeTraces(rows, t0, steps);

        // Audio first, so light sits under sound in reading order.
        if (audioRows) {
            const au = audioScope(t0, steps);
            ['L', 'R'].forEach((side, i) => {
                const y0 = i * rowH + 4;
                const y1 = y0 + rowH - 10;
                const mid = (y0 + y1) / 2;
                const halfH = (y1 - y0) / 2;
                c.strokeStyle = PAL.stripEdge;
                c.beginPath(); c.moveTo(left, mid); c.lineTo(w - 8, mid); c.stroke();
                c.fillStyle = PAL.gutterText;
                c.textAlign = 'left';
                c.fillText('audio ' + side, 8, y0 + 12);
                if (!au) return;
                const lo = i === 0 ? au.minL : au.minR;
                const hi = i === 0 ? au.maxL : au.maxR;
                // Full scale is +-1, but a device-level session peaks near 0.044
                // (the fixed 1/16 divisor), so draw against the window's own peak
                // or the trace would be an invisible line.
                let peak = 1e-4;
                for (let k = 0; k <= steps; k++) {
                    if (hi[k] > peak) peak = hi[k];
                    if (-lo[k] > peak) peak = -lo[k];
                }
                c.strokeStyle = PAL.step;
                c.lineWidth = 1;
                c.beginPath();
                for (let k = 0; k <= steps; k++) {
                    const x = xOf(t0 + k * SCOPE_STEP_MS);
                    c.moveTo(x, mid - (hi[k] / peak) * halfH);
                    c.lineTo(x, mid - (lo[k] / peak) * halfH);
                }
                c.stroke();
            });
        }

        rows.forEach((bit, i0) => {
            const i = i0 + audioRows;
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

        // Marker ticks. A marker is announced to an external recorder at its
        // scheduled time but reaches the senses at the PREDICTED onset, so both
        // are drawn: a solid tick at the schedule, a hollow one at the onset.
        for (const m of markers) {
            if (m.timeMs < t0 || m.timeMs > t0 + SCOPE_MS) continue;
            const x = xOf(m.timeMs);
            c.strokeStyle = PAL.wave;
            c.lineWidth = 1;
            c.beginPath(); c.moveTo(x, 0); c.lineTo(x, h - 20); c.stroke();
            c.fillStyle = PAL.wave;
            c.textAlign = 'left';
            c.fillText(m.label, x + 3, 10);
            if (m.lagUs > 0) {
                const xo = xOf(m.onsetMs);
                if (xo <= w - 8) {
                    c.setLineDash([2, 3]);
                    c.beginPath(); c.moveTo(xo, 0); c.lineTo(xo, h - 20); c.stroke();
                    c.setLineDash([]);
                }
            }
        }

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
        if (measuredLight) {
            parts.push('drawn as MEASURED LIGHT: the LED is only linear above '
                + 'about 40 % drive, so the dim end is compressed. The pixel '
                + 'bytes in the table below and in the CSV export are unchanged.');
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
        const start = Math.max(0, endSample - LIVE_FFT_SIZE);
        analysisRenderer.seek(start);
        analysisRenderer.renderBlock(analysisL, analysisR, start, LIVE_FFT_SIZE);
        for (let i = 0; i < LIVE_FFT_SIZE; i++) {
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
        const band = logBands(mag, LIVE_FFT_SIZE, SAMPLE_RATE, cols, F_MIN, F_MAX);
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
        specInfo.textContent = LIVE_FFT_SIZE + '-point FFT, ' + rate
            + ' Hz bins — amber lines are the carriers the timeline specifies';
    }

    /*
     * The entrainment-band panel: an FFT of the signal's ENVELOPE.
     *
     * Needs 1.49 s of audio (ENV_FFT_SIZE * ENV_DECIM samples), so it is both
     * the most expensive thing here and the slowest-moving — an entrainment
     * rate does not change in a tenth of a second. Drawn at a fifth of the
     * spectrum's rate.
     */
    function drawEnvelope(tMs) {
        const { ctx: c, w, h } = fitCanvas(envCanvas, 120);
        c.fillStyle = PAL.stripBg;
        c.fillRect(0, 0, w, h);
        c.font = '10px system-ui, sans-serif';

        if (!audioCh.size) {
            c.fillStyle = PAL.dim;
            c.textAlign = 'center';
            c.fillText('no audio rows in this session', w / 2, h / 2);
            return;
        }

        const need = ENV_FFT_SIZE * ENV_DECIM;
        const end = Math.round((tMs / 1000) * SAMPLE_RATE);
        const start = Math.max(0, end - need);
        analysisRenderer.seek(start);
        analysisRenderer.renderBlock(envL, envR, start, need);
        for (let i = 0; i < need; i++) envMono[i] = (envL[i] + envR[i]) * 0.5;
        const mag = envAnalyser.analyse(envMono);

        const left = 34, plotW = w - left - 6;
        const TOP = 0, BOT = -72;
        const yOf = (db) => h - 16 - Math.max(0, Math.min(1, (db - BOT) / (TOP - BOT))) * (h - 24);
        const logMin = Math.log2(ENV_DISPLAY_MIN_HZ);
        const logSpan = Math.log2(ENV_DISPLAY_MAX_HZ) - logMin;
        const xOfHz = (hz) => left + ((Math.log2(hz) - logMin) / logSpan) * plotW;

        c.strokeStyle = PAL.grid;
        c.fillStyle = PAL.dim;
        c.textAlign = 'left';
        for (let db = 0; db >= BOT; db -= 24) {
            const y = yOf(db);
            c.beginPath(); c.moveTo(left, y); c.lineTo(w - 4, y); c.stroke();
            c.fillText(db + ' dB', 2, y + 3);
        }

        // One bar per pixel column across the band.
        const cols = Math.max(24, Math.floor(plotW));
        const base = yOf(BOT);
        c.fillStyle = PAL.mod;
        for (let i = 0; i < cols; i++) {
            const hz0 = Math.pow(2, logMin + (logSpan * i) / cols);
            const hz1 = Math.pow(2, logMin + (logSpan * (i + 1)) / cols);
            let k0 = Math.max(1, Math.floor(hz0 / envAnalyser.binHz));
            let k1 = Math.max(k0 + 1, Math.ceil(hz1 / envAnalyser.binHz));
            if (k1 > mag.length) k1 = mag.length;
            let peak = -Infinity;
            for (let k = k0; k < k1; k++) if (mag[k] > peak) peak = mag[k];
            if (peak === -Infinity) continue;
            const y = yOf(peak);
            if (y < base) c.fillRect(left + i, y, 1, base - y);
        }

        // Band ticks, and the rates the TIMELINE says are being driven.
        c.fillStyle = PAL.dim;
        c.textAlign = 'center';
        for (const hz of [1, 2, 4, 8, 10, 20, 40, 60]) {
            if (hz < ENV_DISPLAY_MIN_HZ || hz > ENV_DISPLAY_MAX_HZ) continue;
            c.fillText(String(hz), xOfHz(hz), h - 4);
        }
        const states = inspectRenderer.channelStates(tMs);
        c.strokeStyle = PAL.quad;
        c.lineWidth = 1;
        const marked = new Set();
        for (const st of states) {
            if (!(st.vol > 0)) continue;
            for (const rate of [st.beatHz, st.mod > 0 ? st.mod : null]) {
                if (!rate || rate < ENV_DISPLAY_MIN_HZ || rate > ENV_DISPLAY_MAX_HZ) continue;
                const key = rate.toFixed(2);
                if (marked.has(key)) continue;
                marked.add(key);
                const x = xOfHz(rate);
                c.beginPath(); c.moveTo(x, yOf(TOP)); c.lineTo(x, base); c.stroke();
            }
        }
    }

    // Pre-clamp peak, read off the renderer the SPECTRUM drives (so it reflects
    // the window just analysed rather than whatever the player last queued).
    function drawPeak() {
        const st = analysisRenderer ? analysisRenderer.stats() : null;
        if (!st || !st.samples) return;
        const db = st.peak > 0 ? 20 * Math.log10(st.peak) : -Infinity;
        // -48 dB to +6 dB across the bar: a device-level session sits at -27.
        const frac = Math.max(0, Math.min(1, (db + 48) / 54));
        peakFill.style.width = (frac * 100).toFixed(1) + '%';
        peakWrap.classList.toggle('clip', st.clipped > 0);
        peakLbl.textContent = st.peak > 0 ? db.toFixed(1) + ' dB' : '−∞';
        if (st.clipped > 0) peakLbl.textContent += ' CLIP';
        analysisRenderer.resetStats();
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

        if (markers.length) {
            const t = el('table', 'sim-table');
            const thead = el('thead');
            const hr = el('tr');
            for (const col of ['Marker', 'Scheduled', 'Modality',
                               'Predicted onset', 'Keys']) {
                hr.appendChild(el('th', null, col));
            }
            thead.appendChild(hr);
            t.appendChild(thead);
            const tb = el('tbody');
            for (const m of markers) {
                const tr = el('tr');
                if (m.timeMs > tMs) tr.className = 'sim-silent';
                tr.appendChild(el('td', null, m.label));
                tr.appendChild(el('td', null, fmtClock(m.timeMs)));
                tr.appendChild(el('td', null,
                    m.modality + (m.inferred ? ' (inferred)' : '')));
                tr.appendChild(el('td', null, m.lagUs
                    ? '+' + (m.lagUs / 1000).toFixed(2) + ' ms'
                    : 'same instant'));
                tr.appendChild(el('td', null,
                    m.kv.map(p => p.k + '=' + p.v).join(' ') || '—'));
                tb.appendChild(tr);
            }
            t.appendChild(tb);
            inspBody.appendChild(el('div', 'sim-subhead', 'Stimulus markers'));
            inspBody.appendChild(t);
            inspBody.appendChild(el('div', 'sim-cap',
                'A marker carries three timestamps that must never be conflated: '
              + 'when it was SCHEDULED, when the device DISPATCHED it, and when '
              + 'the stimulus is PREDICTED to reach the senses. Only the first and '
              + 'third can be known without the hardware. The predicted onset is '
              + 'the scheduled time plus the pipeline lag for the modality — '
              + (AUDIO_DMA_PIPELINE_LAG_US / 1000).toFixed(3) + ' ms of DMA for '
              + 'audio, plus the strip write on top for light. Where no modality '
              + 'is stated the device infers it from what else shares the '
              + 'timestamp, and so does this table.'));
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
        drawEnvelope(tMs);
        drawPeak();
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
        if (frame % SPECTRUM_EVERY === 0) { drawSpectrum(tMs); drawPeak(); }
        // The envelope needs 1.49 s of audio and shows a quantity that cannot
        // change quickly, so it runs at a fifth of the spectrum's rate.
        // The envelope needs 1.49 s of audio and shows a quantity that cannot
        // change quickly, so it runs at a fifth of the spectrum's rate.
        if (frame % (SPECTRUM_EVERY * 5) === 0) drawEnvelope(tMs);
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
