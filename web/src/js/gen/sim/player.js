// Simulated-session playback.
//
// Owns the AudioContext and nothing else: no DOM, no device. The view drives
// it and reads `positionMs` from it.
//
// WHY A SCHEDULED BUFFER QUEUE RATHER THAN AN AudioWorklet
//
// A worklet is the textbook answer for streaming synthesis, and it is the wrong
// one here. A worklet needs its own script URL, and this project ships ONE
// esbuild bundle into a SPIFFS partition served by a microcontroller — there is
// no second entry point, no CDN, and `addModule` cannot be handed an inline
// string without a Blob URL, which is exactly the kind of moving part that
// breaks differently on the device, on the docs site, and under file://.
//
// Rendering half a second of two channels costs well under a millisecond
// (sim/audio.js is a tight per-sample loop over a handful of channels), so the
// main thread can stay several blocks ahead without ever being the bottleneck.
// Each block becomes an AudioBufferSourceNode scheduled back-to-back on the
// context clock, which is sample-accurate: the gaps the naive version of this
// has are a product of scheduling against wall time, not against ctx.currentTime.
//
// THE CLOCK IS THE AUDIO CLOCK. The LED view and the spectrum both read
// positionMs from here, so light and sound cannot drift apart on screen even if
// rAF stutters — which matters, because drift between them is the one thing
// this whole project exists to avoid.

import { SAMPLE_RATE } from '../model.js';

// Half a second per block: long enough that a GC pause or a slow frame cannot
// starve the queue, short enough that Stop and Seek feel immediate (the queue
// holds at most BLOCK_MS * QUEUE_BLOCKS of committed audio).
export const BLOCK_MS = 500;
export const QUEUE_BLOCKS = 3;

// How far ahead of ctx.currentTime the first block is scheduled. A context that
// has just resumed reports a currentTime that is already slightly stale, and
// scheduling into the past silently plays the block immediately, which sounds
// like a click.
const START_LEAD_S = 0.08;

/*
 * createPlayer()
 *
 *   load(makeRenderer, durationMs)  makeRenderer() must return a FRESH renderer
 *                                   (createRenderer from sim/audio.js). Called
 *                                   again on every seek, so it must be cheap.
 *   play() / pause() / stop()
 *   seek(ms)
 *   setGain(linear)   monitor trim — NOT part of the simulated signal path
 *   positionMs()      authoritative playhead, from the audio clock
 *   isPlaying()
 *   dispose()
 *
 * `onEnded` fires once when the queue runs past durationMs.
 */
export function createPlayer({ onEnded } = {}) {
    let ctx = null;
    let gainNode = null;
    let renderer = null;
    let makeRenderer = null;
    let durationMs = 0;
    let gain = 1;

    // Playback bookkeeping. `anchorCtxTime` is the context time at which
    // `anchorSample` was due to sound, so the playhead is a pure function of
    // the audio clock.
    let playing = false;
    let nextSample = 0;          // next sample index to render
    let nextCtxTime = 0;         // ctx time the next block is scheduled at
    let anchorSample = 0;
    let anchorCtxTime = 0;
    let pausedSample = 0;
    let pumpTimer = null;
    let live = [];               // scheduled sources, so stop() can kill them
    let endedFired = false;

    const blockFrames = Math.round((BLOCK_MS / 1000) * SAMPLE_RATE);
    let scratchL = new Float32Array(blockFrames);
    let scratchR = new Float32Array(blockFrames);

    function ensureCtx() {
        if (ctx) return ctx;
        const AC = (typeof window !== 'undefined') &&
                   (window.AudioContext || window.webkitAudioContext);
        if (!AC) throw new Error('Web Audio is not available in this browser.');
        // Ask for the device's rate so no resampling sits between the simulated
        // samples and the output. A browser is free to refuse, which is why
        // sampleRate below reports what we actually got.
        ctx = new AC({ sampleRate: SAMPLE_RATE });
        gainNode = ctx.createGain();
        gainNode.gain.value = gain;
        gainNode.connect(ctx.destination);
        return ctx;
    }

    function totalFrames() {
        return Math.max(1, Math.ceil((durationMs / 1000) * SAMPLE_RATE));
    }

    function killLive() {
        for (const s of live) {
            try { s.onended = null; s.stop(); } catch (e) { /* already finished */ }
        }
        live = [];
    }

    // Render and schedule blocks until the queue is QUEUE_BLOCKS deep.
    function pump() {
        if (!playing || !ctx || !renderer) return;
        const end = totalFrames();
        const horizon = ctx.currentTime + (QUEUE_BLOCKS * BLOCK_MS) / 1000;

        while (nextCtxTime < horizon) {
            if (nextSample >= end) {
                if (!endedFired) {
                    endedFired = true;
                    // Let the already-queued audio finish before reporting.
                    const delay = Math.max(0, (nextCtxTime - ctx.currentTime) * 1000);
                    setTimeout(() => { if (endedFired && onEnded) onEnded(); }, delay);
                }
                return;
            }
            const n = Math.min(blockFrames, end - nextSample);
            renderer.renderBlock(scratchL, scratchR, nextSample, n);

            const buf = ctx.createBuffer(2, n, ctx.sampleRate);
            buf.copyToChannel(scratchL.subarray(0, n), 0);
            buf.copyToChannel(scratchR.subarray(0, n), 1);

            const src = ctx.createBufferSource();
            src.buffer = buf;
            src.connect(gainNode);
            src.start(nextCtxTime);
            live.push(src);
            src.onended = () => {
                const i = live.indexOf(src);
                if (i >= 0) live.splice(i, 1);
            };

            nextSample += n;
            nextCtxTime += n / ctx.sampleRate;
        }
    }

    function startPump() {
        stopPump();
        // A quarter of a block: four chances to top the queue up before it
        // could possibly run dry. setInterval is fine here — unlike the device
        // PCM push, nothing breaks if a backgrounded tab throttles us; the
        // audio simply stops and the playhead stops with it, because the
        // playhead IS the audio clock.
        pumpTimer = setInterval(pump, BLOCK_MS / 4);
    }
    function stopPump() {
        if (pumpTimer !== null) { clearInterval(pumpTimer), pumpTimer = null; }
    }

    function armFrom(sample) {
        killLive();
        endedFired = false;
        renderer = makeRenderer ? makeRenderer() : null;
        if (renderer) renderer.seek(sample);
        nextSample = sample;
        nextCtxTime = ctx.currentTime + START_LEAD_S;
        anchorSample = sample;
        anchorCtxTime = nextCtxTime;
    }

    return {
        load(mk, ms) {
            const wasPlaying = playing;
            this.stop();
            makeRenderer = mk;
            durationMs = Math.max(0, ms || 0);
            pausedSample = 0;
            if (wasPlaying) this.play();
        },

        // Reload the renderer (the doc changed) while keeping the playhead.
        reload(mk, ms) {
            makeRenderer = mk;
            durationMs = Math.max(0, ms || 0);
            const at = Math.min(this.positionSample(), totalFrames());
            if (playing) {
                armFrom(at);
                pump();
            } else {
                pausedSample = at;
            }
        },

        /*
         * Create and resume the AudioContext, SYNCHRONOUSLY.
         *
         * Must be called from inside the click handler before anything is
         * awaited. A browser only honours resume() while the user activation is
         * still live, and the Play button has to await the photosensitivity
         * acknowledgement first — by the time that settles the activation can be
         * gone, leaving a context stuck in "suspended" whose currentTime never
         * advances. The symptom is the worst kind: no error, no sound, and a
         * playhead frozen at 0:00 because the playhead IS the audio clock.
         */
        prime() {
            ensureCtx();
            if (ctx.state === 'suspended') { try { ctx.resume(); } catch (e) { /* ignore */ } }
            return ctx.state;
        },

        play() {
            if (playing) return Promise.resolve();
            ensureCtx();
            playing = true;
            const resumed = (ctx.state === 'suspended') ? ctx.resume() : Promise.resolve();
            return resumed.then(() => {
                if (!playing) return;                      // paused during the await
                armFrom(Math.min(pausedSample, totalFrames()));
                pump();
                startPump();
            });
        },

        pause() {
            if (!playing) return;
            pausedSample = this.positionSample();
            playing = false;
            stopPump();
            killLive();
        },

        stop() {
            playing = false;
            stopPump();
            killLive();
            pausedSample = 0;
            endedFired = false;
        },

        seek(ms) {
            const s = Math.max(0, Math.min(totalFrames(),
                Math.round((ms / 1000) * SAMPLE_RATE)));
            if (playing) {
                armFrom(s);
                pump();
            } else {
                pausedSample = s;
            }
        },

        positionSample() {
            if (!playing || !ctx) return pausedSample;
            const elapsed = (ctx.currentTime - anchorCtxTime) * ctx.sampleRate;
            // Before the lead-in has elapsed, `elapsed` is negative and the
            // playhead should sit still at the anchor rather than run backwards.
            const s = anchorSample + Math.max(0, elapsed);
            return Math.min(s, totalFrames());
        },

        positionMs() { return (this.positionSample() / SAMPLE_RATE) * 1000; },
        durationMs() { return durationMs; },
        isPlaying() { return playing; },

        setGain(g) {
            gain = Math.max(0, g);
            if (gainNode && ctx) {
                // A short ramp rather than a step: a jumped gain clicks.
                gainNode.gain.cancelScheduledValues(ctx.currentTime);
                gainNode.gain.setTargetAtTime(gain, ctx.currentTime, 0.015);
            }
        },
        gain() { return gain; },

        // The rate the browser actually gave us. If it is not 44100, every
        // simulated frequency is still correct (the renderer is told the rate),
        // but the device parity claim weakens, so the UI says so.
        sampleRate() { return ctx ? ctx.sampleRate : SAMPLE_RATE; },
        contextState() { return ctx ? ctx.state : 'none'; },

        dispose() {
            this.stop();
            if (ctx) { try { ctx.close(); } catch (e) { /* ignore */ } }
            ctx = null; gainNode = null; renderer = null;
        },
    };
}
