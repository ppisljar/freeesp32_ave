// Browser BG audio engine (bg_browser_push_plan.md, Phase 2).
//
// Two halves:
//   - PURE helpers (encodeWav16 / interleave / floatToInt16) — no Web Audio, no
//     DOM, unit-tested in Node. These turn Float32 PCM into the device's
//     canonical WAV contract (44100 Hz, 16-bit, stereo, little-endian).
//   - Web Audio helpers (conform / generateNoise / generateDrone / decodeFile)
//     — need AudioContext/OfflineAudioContext, so they only run in a browser.
//
// The device (main/wav_parser.c) accepts ONLY 16-bit / 44100 Hz / stereo PCM
// WAV, and bg_convert_to_stereo_float divides samples by 32768. So encodeWav16
// mirrors that: clamp to [-1,1], scale by 32768, round to int16.

export const DEVICE_SAMPLE_RATE = 44100;
export const DEVICE_CHANNELS = 2;

// ---- PURE: float PCM -> canonical 16-bit WAV -------------------------------

// Clamp+scale one float sample to int16. Matches the device's /32768 decode.
export function floatToInt16(x) {
    if (x > 1) x = 1; else if (x < -1) x = -1;
    // Round toward nearest; clamp to the signed 16-bit range.
    let v = Math.round(x * 32768);
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    return v;
}

// Interleave N planar Float32 channels into one Int16Array [L0,R0,L1,R1,...].
// `channels` is an array of Float32Array, all the same length.
export function interleaveInt16(channels) {
    const numCh = channels.length;
    const frames = channels[0] ? channels[0].length : 0;
    const out = new Int16Array(frames * numCh);
    for (let i = 0; i < frames; i++) {
        for (let c = 0; c < numCh; c++) {
            out[i * numCh + c] = floatToInt16(channels[c][i]);
        }
    }
    return out;
}

// Encode planar Float32 channels to a canonical PCM WAV. Returns a Uint8Array.
// `channels`: array of Float32Array (1 = mono, 2 = stereo, ...). For the device
// pass exactly 2 channels at 44100 Hz (use conform() first).
export function encodeWav16(channels, sampleRate = DEVICE_SAMPLE_RATE) {
    const numCh = channels.length;
    const frames = channels[0] ? channels[0].length : 0;
    const pcm = interleaveInt16(channels);
    const dataBytes = pcm.length * 2;
    const buf = new ArrayBuffer(44 + dataBytes);
    const dv = new DataView(buf);

    const writeStr = (off, s) => { for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i)); };
    const byteRate = sampleRate * numCh * 2;
    const blockAlign = numCh * 2;

    writeStr(0, 'RIFF');
    dv.setUint32(4, 36 + dataBytes, true);   // ChunkSize
    writeStr(8, 'WAVE');
    writeStr(12, 'fmt ');
    dv.setUint32(16, 16, true);              // Subchunk1Size (PCM)
    dv.setUint16(20, 1, true);               // AudioFormat = PCM
    dv.setUint16(22, numCh, true);
    dv.setUint32(24, sampleRate, true);
    dv.setUint32(28, byteRate, true);
    dv.setUint16(32, blockAlign, true);
    dv.setUint16(34, 16, true);              // BitsPerSample
    writeStr(36, 'data');
    dv.setUint32(40, dataBytes, true);       // Subchunk2Size

    // PCM payload (little-endian int16).
    let off = 44;
    for (let i = 0; i < pcm.length; i++, off += 2) dv.setInt16(off, pcm[i], true);
    return new Uint8Array(buf);
}

// Wrap an AudioBuffer's channels into a canonical WAV Uint8Array. Downmixes /
// upmixes are NOT done here — pass a 2-channel buffer (see conform()).
export function audioBufferToWav16(audioBuffer) {
    const numCh = audioBuffer.numberOfChannels;
    const channels = [];
    for (let c = 0; c < numCh; c++) channels.push(audioBuffer.getChannelData(c));
    return encodeWav16(channels, Math.round(audioBuffer.sampleRate));
}

// Convenience: WAV bytes -> Blob for download / fetch body.
export function wavBlob(u8) { return new Blob([u8], { type: 'audio/wav' }); }

// ---- Web Audio helpers (browser only) --------------------------------------

// Lazily created shared AudioContext for decodeFile (OfflineAudioContext is
// created per render for generators/conform so sample rate is exact).
let _ctx = null;
function ctx() {
    if (!_ctx) _ctx = new (window.AudioContext || window.webkitAudioContext)();
    return _ctx;
}

// Resample+remix any AudioBuffer to exactly DEVICE_SAMPLE_RATE / stereo.
// Returns a Promise<AudioBuffer>. Mono is upmixed to stereo by the graph.
export async function conform(audioBuffer) {
    const frames = Math.max(1, Math.ceil(audioBuffer.duration * DEVICE_SAMPLE_RATE));
    const off = new OfflineAudioContext(DEVICE_CHANNELS, frames, DEVICE_SAMPLE_RATE);
    const src = off.createBufferSource();
    src.buffer = audioBuffer;
    // The default connect() upmixes mono→stereo / downmixes to the 2-channel
    // destination, so no explicit merger is needed.
    src.connect(off.destination);
    src.start(0);
    return off.startRendering();
}

// Decode a File/Blob/ArrayBuffer of any browser-supported format (mp3/wav/ogg/
// m4a) to an AudioBuffer, then conform to the device contract.
export async function decodeFile(fileOrArrayBuffer) {
    let ab;
    if (fileOrArrayBuffer instanceof ArrayBuffer) ab = fileOrArrayBuffer;
    else ab = await fileOrArrayBuffer.arrayBuffer();
    // decodeAudioData wants its own copy (it detaches the buffer).
    const decoded = await ctx().decodeAudioData(ab.slice(0));
    return conform(decoded);
}

// ---- Generators (offline-rendered AudioBuffers) ----------------------------

// White/pink/brown noise. Returns Promise<AudioBuffer> at device rate/stereo.
//   kind: 'white' | 'pink' | 'brown'
//   opts: { durationMs, gain (0..1), stereo (bool, independent L/R) }
export async function generateNoise(kind, opts = {}) {
    const durationMs = opts.durationMs || 60000;
    const gain = (opts.gain === undefined) ? 0.5 : opts.gain;
    const frames = Math.max(1, Math.ceil((durationMs / 1000) * DEVICE_SAMPLE_RATE));
    const off = new OfflineAudioContext(DEVICE_CHANNELS, frames, DEVICE_SAMPLE_RATE);
    const buf = off.createBuffer(DEVICE_CHANNELS, frames, DEVICE_SAMPLE_RATE);

    for (let c = 0; c < DEVICE_CHANNELS; c++) {
        const data = buf.getChannelData(c);
        fillNoise(kind, data, gain);
        if (!opts.stereo && c === 1) { data.set(buf.getChannelData(0)); }
    }
    const src = off.createBufferSource();
    src.buffer = buf;
    src.connect(off.destination);
    src.start(0);
    return off.startRendering();
}

// Fill a Float32Array with the requested noise colour, scaled by gain.
export function fillNoise(kind, data, gain = 0.5) {
    const n = data.length;
    if (kind === 'pink') {
        // Paul Kellet's economical pink filter.
        let b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (let i = 0; i < n; i++) {
            const w = Math.random() * 2 - 1;
            b0 = 0.99886 * b0 + w * 0.0555179;
            b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;
            b5 = -0.7616 * b5 - w * 0.0168980;
            const pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11;
            b6 = w * 0.115926;
            data[i] = pink * gain;
        }
    } else if (kind === 'brown') {
        let last = 0;
        for (let i = 0; i < n; i++) {
            const w = Math.random() * 2 - 1;
            last = (last + 0.02 * w) / 1.02;
            data[i] = last * 3.5 * gain;   // ~normalise brown to comparable level
        }
    } else { // white
        for (let i = 0; i < n; i++) data[i] = (Math.random() * 2 - 1) * gain;
    }
}

// A simple ambient drone: N detuned oscillators + slow gain LFO for movement.
//   opts: { durationMs, baseHz, voices, detuneCents, waveform, gain, lfoHz }
export async function generateDrone(opts = {}) {
    const durationMs = opts.durationMs || 60000;
    const baseHz = opts.baseHz || 110;
    const voices = Math.max(1, opts.voices || 3);
    const detune = (opts.detuneCents === undefined) ? 8 : opts.detuneCents;
    const waveform = opts.waveform || 'sine';
    const gain = (opts.gain === undefined) ? 0.4 : opts.gain;
    const lfoHz = (opts.lfoHz === undefined) ? 0.08 : opts.lfoHz;
    const dur = durationMs / 1000;
    const frames = Math.max(1, Math.ceil(dur * DEVICE_SAMPLE_RATE));
    const off = new OfflineAudioContext(DEVICE_CHANNELS, frames, DEVICE_SAMPLE_RATE);

    const master = off.createGain();
    master.gain.value = gain / voices;
    master.connect(off.destination);

    // Slow amplitude LFO for gentle movement.
    const lfo = off.createOscillator();
    lfo.frequency.value = lfoHz;
    const lfoGain = off.createGain();
    lfoGain.gain.value = 0.25;
    lfo.connect(lfoGain).connect(master.gain);
    lfo.start(0);

    for (let v = 0; v < voices; v++) {
        const osc = off.createOscillator();
        osc.type = waveform;
        osc.frequency.value = baseHz;
        osc.detune.value = (v - (voices - 1) / 2) * detune;
        // Spread voices across the stereo field via a panner.
        const pan = off.createStereoPanner ? off.createStereoPanner() : null;
        if (pan) {
            pan.pan.value = voices > 1 ? (v / (voices - 1)) * 1.4 - 0.7 : 0;
            osc.connect(pan).connect(master);
        } else {
            osc.connect(master);
        }
        osc.start(0);
        osc.stop(dur);
    }
    return off.startRendering();
}
