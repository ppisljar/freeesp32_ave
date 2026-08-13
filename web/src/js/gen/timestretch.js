// Pitch-preserving time-stretch (WSOLA) for speech AudioBuffers.
//
// Neither TTS engine gives us a reliable fractional speed knob (Google Translate
// TTS only has a binary "slow" toggle; Puter's txt2speech doesn't reliably pass
// SSML `<prosody rate>`), so we slow speech down here instead — engine-agnostic,
// applied to the decoded audio before it is cached/mixed.
//
// rate < 1 = slower (longer output); rate > 1 = faster. Pitch is preserved
// because frames are re-synthesised with overlap-add at the ORIGINAL sample rate
// (a naive resample would drop the pitch). Each incoming frame is realigned to
// the previous output tail by a short waveform-similarity search (the "WS" in
// WSOLA), which keeps voiced speech from going phasey.

// Frame/window in samples. ~46 ms at 44.1 kHz — long enough to hold a pitch
// period of a low voice, short enough to track speech transients.
const FRAME = 2048;
const SYN_HOP = FRAME >> 1;      // 50% overlap → constant-overlap-add with Hann
const SEARCH = 256;              // ±5.8 ms alignment search radius
const CORR_STRIDE = 4;           // subsample the correlation for speed

// Precompute one Hann window per FRAME size (cached across calls).
let _win = null;
function hann() {
    if (_win) return _win;
    const w = new Float32Array(FRAME);
    for (let i = 0; i < FRAME; i++) w[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (FRAME - 1));
    _win = w;
    return w;
}

// Time-stretch `buffer` by `rate` (0<rate; rate<1 slows it down). Returns a new
// AudioBuffer at the same sample rate, or the original buffer if rate≈1.
export function timeStretch(buffer, rate) {
    if (!buffer || !(rate > 0) || Math.abs(rate - 1) < 1e-3) return buffer;

    const sr = buffer.sampleRate;
    const numCh = buffer.numberOfChannels;
    const inLen = buffer.length;
    const anaHop = Math.max(1, Math.round(SYN_HOP * rate));
    const outLen = Math.floor(inLen / rate) + FRAME;
    const win = hann();

    const inCh = [];
    for (let c = 0; c < numCh; c++) inCh.push(buffer.getChannelData(c));
    const outCh = [];
    for (let c = 0; c < numCh; c++) outCh.push(new Float32Array(outLen));
    const norm = new Float32Array(outLen);   // running window-sum for normalization

    const ref0 = outCh[0];   // alignment search uses channel 0 (TTS is ~mono)
    const in0 = inCh[0];
    let outPos = 0;
    let anaPos = 0;

    while (anaPos + FRAME + SEARCH < inLen && outPos + FRAME < outLen) {
        // Find the offset in [-SEARCH, SEARCH] whose input frame best continues
        // the audio already written into the output overlap region.
        let bestDelta = 0, bestCorr = -Infinity;
        for (let d = -SEARCH; d <= SEARCH; d++) {
            const base = anaPos + d;
            if (base < 0 || base + SYN_HOP >= inLen) continue;
            let corr = 0;
            for (let i = 0; i < SYN_HOP; i += CORR_STRIDE) {
                corr += ref0[outPos + i] * in0[base + i];
            }
            if (corr > bestCorr) { bestCorr = corr; bestDelta = d; }
        }
        const src = anaPos + bestDelta;

        // Overlap-add the windowed frame into every channel at the same offset.
        for (let c = 0; c < numCh; c++) {
            const ic = inCh[c], oc = outCh[c];
            for (let i = 0; i < FRAME; i++) {
                const s = src + i;
                if (s >= 0 && s < inLen) oc[outPos + i] += ic[s] * win[i];
            }
        }
        for (let i = 0; i < FRAME; i++) norm[outPos + i] += win[i];

        outPos += SYN_HOP;
        anaPos += anaHop;
    }

    const total = Math.max(1, outPos + SYN_HOP);
    for (let c = 0; c < numCh; c++) {
        const oc = outCh[c];
        for (let i = 0; i < total; i++) if (norm[i] > 1e-6) oc[i] /= norm[i];
    }

    // Wrap the Float32 channels back into an AudioBuffer (OfflineAudioContext as
    // a buffer factory, matching tts.js/concatBuffers).
    const octx = new OfflineAudioContext(numCh, total, sr);
    const out = octx.createBuffer(numCh, total, sr);
    for (let c = 0; c < numCh; c++) out.getChannelData(c).set(outCh[c].subarray(0, total));
    return out;
}
