// FFT and spectrum reduction. The calibration assertion is the one that
// matters: a full-scale sine at a bin centre must read 0 dBFS, because the
// spectrum view's dB readings are compared against the device's measured
// levels (a centre-panned tone at vol 100 sits at -27.1 dBFS).

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    fft, hannWindow, windowGain, createAnalyser, logBands, findPeaks,
    binToHz, hzToBin, buildSpectrogram, DEFAULT_FFT_SIZE,
} from '../src/js/gen/sim/spectrum.js';

const SR = 44100;

function sine(n, hz, sr = SR, amp = 1) {
    const a = new Float32Array(n);
    for (let i = 0; i < n; i++) a[i] = amp * Math.sin((2 * Math.PI * hz * i) / sr);
    return a;
}

// ---- FFT -------------------------------------------------------------------

test('fft: a DC signal puts all its energy in bin 0', () => {
    const n = 16;
    const re = new Float32Array(n).fill(1);
    const im = new Float32Array(n);
    fft(re, im);
    assert.ok(Math.abs(re[0] - n) < 1e-4);
    for (let k = 1; k < n; k++) {
        assert.ok(Math.abs(re[k]) < 1e-4 && Math.abs(im[k]) < 1e-4, 'bin ' + k);
    }
});

test('fft: a sine at an exact bin lands entirely in that bin', () => {
    const n = 64, k0 = 5;
    const re = new Float32Array(n);
    const im = new Float32Array(n);
    for (let i = 0; i < n; i++) re[i] = Math.sin((2 * Math.PI * k0 * i) / n);
    fft(re, im);
    const mag = (k) => Math.sqrt(re[k] * re[k] + im[k] * im[k]);
    assert.ok(Math.abs(mag(k0) - n / 2) < 1e-3, 'got ' + mag(k0));
    for (let k = 1; k < n / 2; k++) {
        if (k === k0) continue;
        assert.ok(mag(k) < 1e-3, 'leakage into bin ' + k + ': ' + mag(k));
    }
});

test('fft: rejects a non-power-of-two size rather than returning nonsense', () => {
    assert.throws(() => fft(new Float32Array(12), new Float32Array(12)),
                  /power of two/);
});

test('fft: Parseval — total energy is preserved', () => {
    const n = 128;
    const re = new Float32Array(n);
    const im = new Float32Array(n);
    for (let i = 0; i < n; i++) re[i] = Math.sin(i * 0.3) + 0.4 * Math.cos(i * 1.1);
    let timeEnergy = 0;
    for (let i = 0; i < n; i++) timeEnergy += re[i] * re[i];
    fft(re, im);
    let freqEnergy = 0;
    for (let k = 0; k < n; k++) freqEnergy += re[k] * re[k] + im[k] * im[k];
    assert.ok(Math.abs(freqEnergy / n - timeEnergy) / timeEnergy < 1e-5);
});

// ---- Window ----------------------------------------------------------------

test('hannWindow: periodic, zero at the start, peaking in the middle', () => {
    const w = hannWindow(8);
    assert.equal(w[0], 0);
    // Float32Array storage, so the tolerance is single-precision, not double.
    assert.ok(Math.abs(w[4] - 1) < 1e-6);
    assert.ok(Math.abs(w[1] - w[7]) < 1e-6);
    // A periodic Hann's coherent gain is exactly 0.5.
    assert.ok(Math.abs(windowGain(w) - 0.5) < 1e-6);
});

// ---- Analyser calibration --------------------------------------------------

test('a full-scale sine at a bin centre reads 0 dBFS', () => {
    const size = 4096;
    const an = createAnalyser(size, SR);
    const hz = binToHz(100, size, SR);              // exactly on a bin
    const mag = an.analyse(sine(size, hz), 0);
    assert.ok(Math.abs(mag[100]) < 0.1, 'expected ~0 dB, got ' + mag[100]);
});

test('half amplitude reads -6 dBFS', () => {
    const size = 4096;
    const an = createAnalyser(size, SR);
    const hz = binToHz(100, size, SR);
    const mag = an.analyse(sine(size, hz, SR, 0.5), 0);
    assert.ok(Math.abs(mag[100] - (-6.02)) < 0.1, 'expected -6 dB, got ' + mag[100]);
});

function twoTones(size, f1, f2) {
    const mix = new Float32Array(size);
    for (let i = 0; i < size; i++) {
        mix[i] = 0.5 * Math.sin((2 * Math.PI * f1 * i) / SR)
               + 0.5 * Math.sin((2 * Math.PI * f2 * i) / SR);
    }
    return mix;
}

test('the default FFT does NOT resolve a 10 Hz binaural pair — it reads as one peak', () => {
    // This is a real limit, documented rather than papered over. At 8192 points
    // the bin is 5.38 Hz, so a 200/210 Hz pair is 1.9 bins apart while a Hann
    // window's main lobe is 4 bins wide: the two carriers merge into a single
    // peak near their mean. The exact carriers come from the timeline inspector
    // (createRenderer().channelStates(), which knows freq and freqR), never
    // from peak-picking the spectrum.
    const size = DEFAULT_FFT_SIZE;
    const an = createAnalyser(size, SR);
    const mag = an.analyse(twoTones(size, 200, 210), 0);
    const peaks = findPeaks(mag, size, SR, { limit: 4, minDb: -40 });
    assert.equal(peaks.length, 1, 'expected the pair to merge, got ' + peaks.length);
    assert.ok(Math.abs(peaks[0].hz - 205) < 3,
        'the merged peak should sit near the mean, got ' + peaks[0].hz);
});

test('a 32768-point FFT does resolve the same pair', () => {
    // 1.35 Hz bins put the carriers 7.4 bins apart, comfortably outside the
    // main lobe — at the cost of a 0.74 s window, which is why this is the
    // "detail" setting and not the default.
    const size = 32768;
    const an = createAnalyser(size, SR);
    const mag = an.analyse(twoTones(size, 200, 210), 0);
    const peaks = findPeaks(mag, size, SR, { limit: 4, minDb: -40 });
    assert.ok(peaks.length >= 2, 'expected two carriers, got ' + peaks.length);
    const hz = peaks.slice(0, 2).map(p => p.hz).sort((x, y) => x - y);
    assert.ok(Math.abs(hz[0] - 200) < 1.5, 'low carrier ' + hz[0]);
    assert.ok(Math.abs(hz[1] - 210) < 1.5, 'high carrier ' + hz[1]);
});

test('findPeaks interpolates between bins instead of snapping to one', () => {
    const size = 4096;
    const an = createAnalyser(size, SR);
    // Deliberately BETWEEN bins: bin spacing here is 10.766 Hz.
    const mag = an.analyse(sine(size, 207.5), 0);
    const peaks = findPeaks(mag, size, SR, { limit: 1, minDb: -40 });
    assert.equal(peaks.length, 1);
    assert.ok(Math.abs(peaks[0].hz - 207.5) < 2,
        'parabolic interpolation should recover 207.5, got ' + peaks[0].hz);
});

test('the analyser zero-pads a short input rather than reading past it', () => {
    const an = createAnalyser(1024, SR);
    const mag = an.analyse(new Float32Array(100), 0);
    assert.equal(mag.length, 512);
    for (let k = 0; k < mag.length; k++) assert.equal(mag[k], an.floorDb);
});

// ---- Reductions ------------------------------------------------------------

test('logBands keeps a narrow carrier as a spike by taking the max, not the mean', () => {
    const size = 4096;
    const an = createAnalyser(size, SR);
    const mag = an.analyse(sine(size, binToHz(100, size, SR)), 0);
    const cols = 64;
    const band = logBands(mag, size, SR, cols, 20, 20000);
    assert.equal(band.length, cols);
    let best = -Infinity, bestCol = -1;
    for (let c = 0; c < cols; c++) if (band[c] > best) { best = band[c]; bestCol = c; }
    // A mean would have buried a single 0 dB bin among hundreds of -140 dB ones.
    assert.ok(best > -1, 'the carrier should survive the reduction at ~0 dB, got ' + best);
    // And it should land in the column covering its own frequency.
    const hz = binToHz(100, size, SR);
    const logMin = Math.log2(20), logMax = Math.log2(20000);
    const expectedCol = Math.floor(((Math.log2(hz) - logMin) / (logMax - logMin)) * cols);
    assert.ok(Math.abs(bestCol - expectedCol) <= 1,
        'peak in column ' + bestCol + ', expected ~' + expectedCol);
});

test('hzToBin and binToHz round-trip', () => {
    const size = 8192;
    for (const hz of [10, 200, 440, 1000]) {
        const k = hzToBin(hz, size, SR);
        assert.ok(Math.abs(binToHz(k, size, SR) - hz) < SR / size);
    }
});

test('buildSpectrogram fills a column-major grid from a streaming render', () => {
    const totalSamples = SR * 2;
    // A render callback that produces a steady 300 Hz tone.
    const render = (start, count, outL, outR) => {
        for (let i = 0; i < count; i++) {
            const v = Math.sin((2 * Math.PI * 300 * (start + i)) / SR);
            outL[i] = v; outR[i] = v;
        }
    };
    const sg = buildSpectrogram(render, totalSamples,
        { size: 1024, sampleRate: SR, cols: 20, rows: 32, fMin: 50, fMax: 2000 });
    assert.equal(sg.cols, 20);
    assert.equal(sg.rows, 32);
    assert.equal(sg.data.length, 20 * 32);
    assert.ok(sg.msPerCol > 0);
    // Every column should show the same loudest row, since the tone is steady.
    const loudestRow = (c) => {
        let best = -Infinity, r0 = -1;
        for (let r = 0; r < sg.rows; r++) {
            const v = sg.data[c * sg.rows + r];
            if (v > best) { best = v; r0 = r; }
        }
        return r0;
    };
    const r0 = loudestRow(0);
    for (let c = 1; c < sg.cols; c++) {
        assert.equal(loudestRow(c), r0, 'column ' + c + ' drifted');
    }
});
