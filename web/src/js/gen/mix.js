// Pure stereo mixing helpers for the bounce.
//
// WHAT USED TO BE HERE. This file was `synth.js` and carried a second
// implementation of the device's audio engine, written for the offline bounce.
// That implementation has moved to `sim/audio.js`, which the Simulate view and
// the bounce now share, because the two copies had drifted apart in fifteen
// documented ways — three of which changed what you heard:
//
//   * it divided the mix by the number of ACTIVE channels, where the device
//     divides by a fixed 1/16 (audio_generator.c:875), leaving a two-channel
//     bounce ~18 dB hot and a one-channel bounce ~24 dB hot;
//   * it implemented only the legacy `1 + 0.1*sin()` tremolo, so an `env 0`
//     isochronic gate rendered as a 10 % wobble instead of a full-depth pulse;
//   * it omitted the 0.5 scale the device applies to noise waves, so every
//     noise bed sat 6 dB above the tones beside it.
//
// What remains here is the part that was never synthesis: allocating buffers
// and mixing already-rendered audio (a decoded BG clip, a TTS phrase) into
// them. None of it knows anything about the timeline.
//
// Pure: no DOM, no Web Audio. Unit-tested in web/test/mix.test.js.

import { SAMPLE_RATE } from './model.js';

// Allocate zeroed stereo buffers for a given duration.
export function zeroBuffers(totalMs, sr = SAMPLE_RATE) {
    const N = Math.max(1, Math.ceil((totalMs / 1000) * sr));
    return { left: new Float32Array(N), right: new Float32Array(N), sampleRate: sr };
}

// Add src (stereo Float32 channels) into dst at `offsetSamples`, scaled by gain.
// Overruns past dst are truncated. `loopToFill` (BG use) repeats src to cover
// [offsetSamples, dst.length). No clamping here — call clampBuffers() at the end.
export function mixInto(dstL, dstR, srcL, srcR, offsetSamples, gain = 1, loopToFill = false) {
    const N = dstL.length;
    const S = srcL.length;
    if (S === 0) return;
    if (loopToFill) {
        for (let i = Math.max(0, offsetSamples); i < N; i++) {
            const s = (i - offsetSamples) % S;
            dstL[i] += srcL[s] * gain;
            dstR[i] += (srcR ? srcR[s] : srcL[s]) * gain;
        }
    } else {
        for (let s = 0; s < S; s++) {
            const i = offsetSamples + s;
            if (i < 0) continue;
            if (i >= N) break;
            dstL[i] += srcL[s] * gain;
            dstR[i] += (srcR ? srcR[s] : srcL[s]) * gain;
        }
    }
}

// Hard-clamp stereo buffers to [-1,1] in place — the last stage before int16,
// mirroring what audio_test.c does after the BG and speech post-mix stages.
export function clampBuffers(left, right) {
    for (let i = 0; i < left.length; i++) {
        const l = left[i], r = right[i];
        left[i] = l > 1 ? 1 : (l < -1 ? -1 : l);
        right[i] = r > 1 ? 1 : (r < -1 ? -1 : r);
    }
}
