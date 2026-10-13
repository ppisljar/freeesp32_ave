// Spectrum analysis — FFT, windowing, and the two reductions the UI needs.
//
// Written rather than borrowed: the device has no internet and the browser
// reaching it often has none either, so web/src carries no external libraries
// at all (web/README.md, and the style.css header makes the same point).
//
// The analyser deliberately runs on the SIMULATED signal rather than on a Web
// Audio AnalyserNode, for two reasons: it works while the output is muted, and
// it works while scrubbing with no audio playing at all. An AnalyserNode could
// only ever show what the speakers are doing.
//
// Pure: no DOM, no Web Audio. Unit-tested in web/test/sim_spectrum.test.js.

/*
 * A session's interesting content is almost entirely below 1 kHz (carriers of
 * 100-500 Hz, beats of 1-40 Hz), so the default FFT is long: 8192 points at
 * 44.1 kHz is a 5.4 Hz bin over 186 ms of signal, which is long enough to see
 * a 6 Hz gate's sidebands while still feeling live.
 *
 * WHAT IT CANNOT DO, deliberately: resolve a binaural pair. A 200/210 Hz pair
 * is 1.9 bins apart here and a Hann window's main lobe is 4 bins wide, so the
 * two carriers merge into one peak near their mean. Separating them needs
 * DETAIL_FFT_SIZE, whose 0.74 s window is too long to drive a 60 fps view.
 *
 * So the UI must not read carrier frequencies off the spectrum. The exact
 * numbers are already known from the timeline — createRenderer().channelStates()
 * returns freq, freqR and the beat directly — and peak-picking an FFT to
 * recover values the file states outright would be both slower and wrong.
 */
export const DEFAULT_FFT_SIZE = 8192;
export const DETAIL_FFT_SIZE = 32768;

// ---- Window ----------------------------------------------------------------

// Periodic Hann. Periodic (N, not N-1, in the denominator) because this is used
// for spectral analysis of a continuous signal rather than for filter design.
export function hannWindow(n) {
    const w = new Float32Array(n);
    for (let i = 0; i < n; i++) w[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / n);
    return w;
}

// Coherent gain of a window — the factor a pure tone's bin magnitude is scaled
// by, so it can be divided back out and a full-scale sine reads 0 dBFS.
export function windowGain(w) {
    let s = 0;
    for (let i = 0; i < w.length; i++) s += w[i];
    return s / w.length;
}

// ---- FFT -------------------------------------------------------------------

/*
 * In-place iterative radix-2 Cooley-Tukey FFT.
 *
 * re/im are Float32Array(n), n a power of two. Twiddles are precomputed per
 * size and cached, because the transport calls this every animation frame and
 * recomputing 8192 sin/cos pairs at 60 Hz is real work for no reason.
 */
const twiddleCache = new Map();

function twiddles(n) {
    let t = twiddleCache.get(n);
    if (t) return t;
    const cos = new Float32Array(n / 2);
    const sin = new Float32Array(n / 2);
    for (let i = 0; i < n / 2; i++) {
        cos[i] = Math.cos((-2 * Math.PI * i) / n);
        sin[i] = Math.sin((-2 * Math.PI * i) / n);
    }
    t = { cos, sin };
    twiddleCache.set(n, t);
    return t;
}

export function fft(re, im) {
    const n = re.length;
    if (n <= 1) return;
    if ((n & (n - 1)) !== 0) throw new Error('fft: size must be a power of two, got ' + n);

    // Bit-reversal permutation.
    for (let i = 1, j = 0; i < n; i++) {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            let t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    const { cos, sin } = twiddles(n);
    for (let len = 2; len <= n; len <<= 1) {
        const half = len >> 1;
        const step = n / len;
        for (let i = 0; i < n; i += len) {
            for (let k = 0; k < half; k++) {
                const ti = k * step;
                const wr = cos[ti], wi = sin[ti];
                const a = i + k, b = a + half;
                const xr = re[b] * wr - im[b] * wi;
                const xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

// ---- Analyser --------------------------------------------------------------

/*
 * A reusable analyser. Allocating 8192-point scratch arrays every frame is the
 * kind of thing that shows up as GC sawtooth in a 60 fps canvas loop, so the
 * buffers are owned here and reused.
 *
 *   size      FFT length (power of two)
 *   sampleRate
 *
 * analyse(samples, offset) fills and returns `mag` — magnitude in dBFS for bins
 * 0..size/2, where a full-scale sine at a bin centre reads ~0 dB.
 */
export function createAnalyser(size = DEFAULT_FFT_SIZE, sampleRate = 44100) {
    const re = new Float32Array(size);
    const im = new Float32Array(size);
    const win = hannWindow(size);
    const gain = windowGain(win);
    const bins = size / 2;
    const mag = new Float32Array(bins);
    // Floor rather than -Infinity: a log scale needs a bottom to draw against,
    // and -140 dB is already far below anything audible.
    const FLOOR_DB = -140;

    function analyse(samples, offset = 0) {
        const n = Math.min(size, Math.max(0, samples.length - offset));
        for (let i = 0; i < n; i++) re[i] = samples[offset + i] * win[i];
        for (let i = n; i < size; i++) re[i] = 0;
        im.fill(0);
        fft(re, im);
        // Single-sided: double every bin except DC and Nyquist, then divide out
        // the window's coherent gain and the transform length.
        const norm = 1 / (size * gain);
        for (let k = 0; k < bins; k++) {
            const a = re[k] * norm, b = im[k] * norm;
            let m = Math.sqrt(a * a + b * b);
            if (k > 0) m *= 2;
            mag[k] = m > 0 ? Math.max(FLOOR_DB, 20 * Math.log10(m)) : FLOOR_DB;
        }
        return mag;
    }

    return {
        analyse,
        mag,
        size,
        bins,
        sampleRate,
        binHz: sampleRate / size,
        floorDb: FLOOR_DB,
    };
}

// Centre frequency of a bin.
export function binToHz(k, size, sampleRate) { return (k * sampleRate) / size; }

// Nearest bin to a frequency.
export function hzToBin(hz, size, sampleRate) { return Math.round((hz * size) / sampleRate); }

/*
 * Reduce a linear magnitude array to log-spaced display columns.
 *
 * A linear frequency axis is the wrong axis for this material: half the pixels
 * would be spent on 11-22 kHz, where a session has nothing, and the 100-400 Hz
 * region that carries every carrier would be squeezed into a few columns. Each
 * output column takes the MAXIMUM of the bins falling inside it rather than the
 * mean, so a narrow carrier stays a visible spike instead of being averaged
 * into the noise floor around it.
 */
export function logBands(mag, size, sampleRate, cols, fMin = 20, fMax = 20000) {
    const out = new Float32Array(cols);
    const logMin = Math.log2(fMin);
    const logMax = Math.log2(Math.min(fMax, sampleRate / 2));
    const span = logMax - logMin;
    for (let c = 0; c < cols; c++) {
        const f0 = Math.pow(2, logMin + (span * c) / cols);
        const f1 = Math.pow(2, logMin + (span * (c + 1)) / cols);
        let k0 = Math.floor((f0 * size) / sampleRate);
        let k1 = Math.ceil((f1 * size) / sampleRate);
        if (k1 <= k0) k1 = k0 + 1;
        if (k0 < 0) k0 = 0;
        if (k1 > mag.length) k1 = mag.length;
        let peak = -Infinity;
        for (let k = k0; k < k1; k++) if (mag[k] > peak) peak = mag[k];
        out[c] = peak === -Infinity ? -140 : peak;
    }
    return out;
}

/*
 * Peak picking — the carriers actually present, with parabolic interpolation so
 * a 207.5 Hz carrier reads as 207.5 and not as the nearest 5.4 Hz bin.
 *
 * Returns [{ hz, db }] sorted loudest first, at most `limit` entries, ignoring
 * anything below `minDb` and anything that is not a local maximum.
 */
export function findPeaks(mag, size, sampleRate, { limit = 8, minDb = -90, minHz = 10 } = {}) {
    const peaks = [];
    const kMin = Math.max(1, Math.floor((minHz * size) / sampleRate));
    for (let k = kMin; k < mag.length - 1; k++) {
        if (mag[k] <= minDb) continue;
        if (mag[k] <= mag[k - 1] || mag[k] < mag[k + 1]) continue;
        // Parabolic vertex through the three dB values around the peak.
        const a = mag[k - 1], b = mag[k], c = mag[k + 1];
        const denom = a - 2 * b + c;
        const delta = denom !== 0 ? (0.5 * (a - c)) / denom : 0;
        peaks.push({
            hz: ((k + delta) * sampleRate) / size,
            db: b - 0.25 * (a - c) * delta,
        });
    }
    peaks.sort((x, y) => y.db - x.db);
    return peaks.slice(0, limit);
}

/*
 * The ENVELOPE spectrum — where the entrainment rate actually lives.
 *
 * A 10 Hz binaural beat built from 200 Hz and 210 Hz has NO 10 Hz component in
 * the ordinary spectrum. It has two carriers, and the beat only exists as the
 * amplitude envelope of their sum. The same is true of a monaural pair, and an
 * isochronic gate puts its rate in the envelope by construction. So the panel
 * that answers "what rate is this session actually driving?" cannot be a
 * carrier FFT at all — it has to be an FFT of the envelope.
 *
 * Method: rectify the mono sum, then box-average `decim` samples per output
 * point. The box average is both the decimation and the anti-alias filter; its
 * first null sits at sampleRate/decim (689 Hz at the default 64), which is far
 * above the 0.5-60 Hz band this is for. Then transform at the decimated rate.
 *
 * At decim 64 and size 1024 that is 0.67 Hz bins over 0-344 Hz, from 1.49 s of
 * signal — enough to separate a 6 Hz theta gate from a 10 Hz alpha one, and to
 * show a 40 Hz gamma rate with room to spare.
 *
 * THE RIPPLE, and why the display band stops at 60 Hz. Rectifying a sine
 * produces harmonics at 2f, 4f, ... and the box average does not fully kill
 * them, so what survives aliases down into the decimated band. Measured on a
 * steady unmodulated carrier: a residual around -13 to -25 dB, always landing
 * ABOVE 60 Hz (88.8 Hz for a 300 Hz carrier, 275.2 Hz for 207 Hz, 191.1 Hz for
 * 440 Hz), while inside 0.5-60 Hz the same tone sits at -79 to -87 dB — silent.
 *
 * So ENV_DISPLAY_MAX_HZ is not a cosmetic choice. Everything an entrainment
 * session drives is below it, and everything the method's own rectification
 * artifact produces is above it.
 *
 * Returns { mag, binHz, size, rate } where `mag` is dB relative to a
 * full-depth modulation.
 */
export const ENV_DISPLAY_MIN_HZ = 0.5;
export const ENV_DISPLAY_MAX_HZ = 60;
export const ENV_DECIM = 64;
export const ENV_FFT_SIZE = 1024;

export function createEnvelopeAnalyser(size = ENV_FFT_SIZE, sampleRate = 44100,
                                       decim = ENV_DECIM) {
    const rate = sampleRate / decim;
    const an = createAnalyser(size, rate);
    const env = new Float32Array(size);

    /*
     * `mono` must hold size*decim samples (1.49 s at the defaults). Anything
     * shorter is zero-padded, which widens the peak but does not move it.
     */
    function analyse(mono) {
        const need = size * decim;
        const have = Math.min(mono.length, need);
        let dc = 0;
        for (let i = 0; i < size; i++) {
            let acc = 0;
            const base = i * decim;
            let n = 0;
            for (let k = 0; k < decim; k++) {
                const j = base + k;
                if (j >= have) break;
                const v = mono[j];
                acc += v < 0 ? -v : v;          // rectify
                n++;
            }
            const v = n ? acc / n : 0;
            env[i] = v;
            dc += v;
        }
        // Remove the mean: the envelope of any audible signal is mostly DC, and
        // leaving it in puts a peak at 0 Hz tall enough to flatten everything
        // the panel exists to show.
        dc /= size;
        for (let i = 0; i < size; i++) env[i] -= dc;
        return an.analyse(env, 0);
    }

    return { analyse, size, rate, binHz: an.binHz, floorDb: an.floorDb, mag: an.mag };
}

/*
 * Build a whole-session spectrogram as a column-major dB grid.
 *
 * `render(startSample, count, outL, outR)` is called to fill scratch buffers,
 * so this works off the streaming renderer without materialising the session.
 * Returns { cols, rows, data: Float32Array(cols*rows), sampleRate, size,
 * msPerCol }, where row 0 is the lowest display frequency.
 *
 * Columns are mono (L+R)/2: a spectrogram of a binaural session wants to show
 * both carriers at once, and two side-by-side single-channel plots would make
 * the beat harder to see rather than easier.
 */
export function buildSpectrogram(render, totalSamples, opts = {}) {
    const size = opts.size || 4096;
    const sampleRate = opts.sampleRate || 44100;
    const cols = Math.max(1, opts.cols || 600);
    const rows = Math.max(1, opts.rows || 240);
    const fMin = opts.fMin || 20;
    const fMax = opts.fMax || 2000;

    const an = createAnalyser(size, sampleRate);
    const mono = new Float32Array(size);
    const bl = new Float32Array(size);
    const br = new Float32Array(size);
    const data = new Float32Array(cols * rows);
    const hop = Math.max(1, Math.floor(totalSamples / cols));

    for (let c = 0; c < cols; c++) {
        const start = Math.min(Math.max(0, totalSamples - size), c * hop);
        render(start, size, bl, br);
        for (let i = 0; i < size; i++) mono[i] = (bl[i] + br[i]) * 0.5;
        const mag = an.analyse(mono, 0);
        const band = logBands(mag, size, sampleRate, rows, fMin, fMax);
        for (let r = 0; r < rows; r++) data[c * rows + r] = band[r];
    }

    return {
        cols, rows, data, sampleRate, size, fMin, fMax,
        msPerCol: (hop / sampleRate) * 1000,
        floorDb: an.floorDb,
    };
}
