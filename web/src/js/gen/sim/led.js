// LED flicker engine, ported for simulation.
//
// Mirrors main/led_matrix_example.c's flicker ISR and main/led_strip.c's pixel
// compositing closely enough that the colours this produces are the bytes the
// device would put on the wire. The integer arithmetic is reproduced rather
// than floated, because several of the truncations are visible.
//
// WHAT IS FAITHFUL
//
//   - The pixel chain, including its DOUBLE integer truncation on the smooth
//     carriers: px = floor(rgb * floor(brightness_q8 * shape_q16 / 65536) / 25600).
//   - The raised-sine LUT, including its deliberate clamp to 65535 rather than
//     65536 (led_matrix_example.c:600 — 65536 does not fit a uint16_t, and the
//     first version that stored it wrapped to 0, dropping one flick per LFO
//     period at the brightest moment of the breath).
//   - NO GAMMA. There is none in the firmware and that is deliberate, not an
//     omission: the carrier shapes are luminance-linear specifically so a
//     180-degree antiphase pair of raised sines sums to a constant. Adding an
//     sRGB correction here to "make it look right" would stop antiphase pairs
//     cancelling and render the project's headline visual mode wrong.
//   - phase is a LEAD, not a lag (eff_elapsed += off, :1184). 90 degrees at
//     10 Hz measures 74.76 ms of apparent delay on hardware, not 25 ms.
//   - freq 0 is a first-class DC channel with its own branch (:896) that
//     `continue`s before the duty gate, the carrier switch AND the phase
//     offset — so duty, env and phase are all no-ops on a DC row, while
//     brightness and colour sweeps/mods still run.
//   - LED env is clamped to 0..3 with anything else folded to SQUARE
//     (:1746, config_parser.c:1667). The audio path folds out-of-range env to
//     4 instead, which is why the two engines must not share an env decoder.
//   - Out-of-range rows are DROPPED, not clamped (:1593, :2345): freq > 100,
//     duty > 100 or bright > 100 makes the install fail, and the channel keeps
//     rendering whatever it was already doing.
//
// WHAT IS IDEALISED (and why)
//
//   - TICK QUANTISATION. The device evaluates flicker in a gptimer ISR whose
//     rate is clamp(floor(freq_Hz) * 250, 1000, 10000) Hz and which only ever
//     RISES within a session, so the real edge resolution depends on the
//     session's history rather than on the row being rendered. This module
//     computes continuous time and exposes `tickHz`/`tickUs` for the row so the
//     UI can report the real quantisation instead of pretending it away. The
//     practical consequence is that the simulator looks smoother than the
//     hardware: at 40 Hz with env 1 the device resolves only ~10 levels per
//     cycle, and its strip cannot refresh faster than ~415 fps anyway
//     (2.41 ms for a 74-pixel WS2812 frame).
//   - THE +46.4 ms TRANSPORT ANCHOR. The device offsets every LED cycle origin
//     by AUDIO_DMA_PIPELINE_LAG_US = 46439 us so that light lines up with DAC
//     emission rather than with the parser. Since the simulator renders audio
//     and light from the same nominal clock, reproducing the offset on one side
//     only would introduce the very skew the constant exists to remove.
//   - JITTER PHASE. The device keys the jitter LFO to the absolute wall clock
//     (`now_us % period_us`), so its phase differs on every run and cannot be
//     reproduced. Here it is keyed to session time.
//   - MOD SHAPES are evaluated in floating point (in sim/resolve.js) rather
//     than through the firmware's Q16 split-multiply. The difference is below
//     one 8-bit colour step.
//
// Pure: no DOM, no timers. Unit-tested in web/test/sim_led.test.js.

import { MAX_LED_FLICKER_HZ } from '../model.js';
import { stateAtIndex, activeIndexAt, LED_CORE_FIELDS, LED_PULSE_FIELDS } from './resolve.js';

// led_strip.h:43 — brightness is Q8.8 percent, so 100% is 25600.
export const LED_BRIGHTNESS_Q8_MAX = 25600;

// led_matrix_example.c:191
export const LED_CARRIER_SQUARE = 0;
export const LED_CARRIER_SINE = 1;
export const LED_CARRIER_TRIANGLE = 2;
export const LED_CARRIER_TRAPEZOID = 3;

// led_matrix_example.c:208 — ISR tick sizing.
export const LED_FLICKER_TICK_MULT = 250;
export const LED_FLICKER_TICK_MIN = 1000;
export const LED_FLICKER_TICK_MAX = 10000;

// audio_config.h — (256 frames * 8 buffers) * 1e6 / 44100. Exposed so the UI
// can explain the device's A/V anchoring without this module applying it.
export const AUDIO_DMA_PIPELINE_LAG_US = 46439;

/*
 * How long one WS2812 strip frame takes to clock out — the real ceiling on how
 * often any pixel can change, and the reason a smooth carrier has far fewer
 * distinct levels per cycle on hardware than in this simulator.
 *
 * led_strip.c truncates its RMT ticks (NS_TO_RMT_TICKS divides by 100), so the
 * authored 350 ns T0H becomes 300 ns and every bit is 12 ticks = 1.2 µs — not
 * the nominal 1.25. 24 bits per pixel, plus a 280 µs reset, and
 * s_neopixel_refresh blocks on rmt_tx_wait_all_done before each frame, so
 * frames genuinely serialise at this rate.
 */
export const WS2812_BIT_US = 1.2;
export const WS2812_RESET_US = 280;
export function stripFrameUs(pixels = DEFAULT_CHANNEL_MAP.length) {
    return pixels * 24 * WS2812_BIT_US + WS2812_RESET_US;
}

/*
 * The raised-sine table, (1 - cos(2*pi*x))/2 in Q16, 257 entries so that
 * `index = phase_q16 >> 8` needs no clamp.
 *
 * Built here rather than transcribed, with the firmware's exact rounding and
 * its 65535 ceiling. Verified against the literal table in
 * led_matrix_example.c:586 at i = 0, 1, 2, 4, 64, 128, 192, 256.
 */
export const SINE_Q16 = (() => {
    const t = new Uint16Array(257);
    for (let i = 0; i <= 256; i++) {
        const v = Math.round(65536 * (1 - Math.cos(2 * Math.PI * i / 256)) / 2);
        t[i] = v > 65535 ? 65535 : v;
    }
    return t;
})();

/*
 * phase_q16 = elapsed * 65536 / cycle, computed the firmware's way.
 *
 * led_matrix_example.c:764 cannot afford a 64-bit multiply in the ISR, so it
 * shifts both operands down until the period fits in 16 bits and then does a
 * 32/32 divide. That costs a few low bits of phase, which this reproduces so
 * the two agree exactly rather than approximately.
 */
export function phaseQ16(elapsedUs, cycleUs) {
    if (cycleUs <= 0) return 0;
    let period = cycleUs >>> 0;
    let t = elapsedUs >>> 0;
    while (period > 0xFFFF) { period >>>= 1; t >>>= 1; }
    const p = Math.floor((t * 65536) / period);
    return p > 65536 ? 65536 : p;
}

/*
 * Per-tick carrier output level in Q8.8, 0..brightness_q8.
 * Port of led_carrier_level_iram (led_matrix_example.c:758).
 *
 * duty_q16 and attack_q16 are Q16 fractions OF THE WHOLE CYCLE, exactly as the
 * ISR's A3 cache computes them (:1222).
 */
export function carrierLevelQ8(wave, elapsedUs, cycleUs, brightnessQ8, dutyQ16, attackQ16) {
    if (cycleUs === 0) return brightnessQ8;
    const ph = phaseQ16(elapsedUs, cycleUs);
    let shape;
    if (wave === LED_CARRIER_TRIANGLE) {
        shape = ph < 32768 ? (ph << 1) : (131072 - (ph << 1));
    } else if (wave === LED_CARRIER_TRAPEZOID) {
        if (ph >= dutyQ16) {
            shape = 0;
        } else {
            let a = attackQ16;
            const half = dutyQ16 >> 1;
            if (a > half) a = half;                 // at most a triangle inside duty
            if (a === 0) shape = 65536;             // hard square within duty
            else if (ph < a) shape = Math.floor((ph * 65536) / a);
            else if (ph > dutyQ16 - a) shape = Math.floor(((dutyQ16 - ph) * 65536) / a);
            else shape = 65536;
            if (shape > 65536) shape = 65536;
        }
    } else {
        // SINE (1) and the default branch both read the true raised sine.
        shape = SINE_Q16[ph >> 8];
    }
    return Math.floor((brightnessQ8 * shape) / 65536);
}

// led_matrix_example.c:794 — slow bipolar triangle LFO returning a signed
// milliHz offset. Reproduced including the arithmetic-shift floor.
export function jitterMilliHz(nowUs, ampMilliHz, periodUs) {
    if (!ampMilliHz || !periodUs) return 0;
    let period = periodUs >>> 0;
    let t = (nowUs >>> 0) % periodUs;
    while (period > 0xFFFF) { period >>>= 1; t >>>= 1; }
    const ph = Math.floor((t * 65536) / period);
    let tri;
    if (ph < 16384) tri = ph << 2;                  //  0 -> +1
    else if (ph < 49152) tri = 131072 - (ph << 2);  // +1 -> -1
    else tri = (ph << 2) - 262144;                  // -1 ->  0
    return Math.floor((ampMilliHz * tri) / 65536);
}

// The ISR tick the device would be running for this rate, if the session
// started here. Informational: the real tick also never falls within a session,
// so a session that once ran fast keeps the finer tick. led_matrix_example.c:1489.
export function tickHzFor(freqHz) {
    const desired = Math.floor(freqHz) * LED_FLICKER_TICK_MULT;
    if (desired < LED_FLICKER_TICK_MIN) return LED_FLICKER_TICK_MIN;
    if (desired > LED_FLICKER_TICK_MAX) return LED_FLICKER_TICK_MAX;
    return desired;
}

/*
 * Would the device REFUSE to install this row?
 *
 * led_matrix_start_flicker_masked (:1593) and led_matrix_start_sweep_masked
 * (:2345, which checks BOTH endpoints of every ramp) return
 * ESP_ERR_INVALID_ARG rather than clamping, so the affected channels keep
 * rendering their previous state. Returns a reason string, or null if accepted.
 */
export function ledEntryRejection(entry) {
    const c = entry.cells;
    const lim = [
        ['freq', MAX_LED_FLICKER_HZ, 'flicker frequency'],
        ['duty', 100, 'duty'],
        ['bright', 100, 'brightness'],
    ];
    for (const [f, max, label] of lim) {
        const cl = c[f];
        if (!cl) continue;
        const ends = [cl.value];
        // A ramp is checked at both ends; an LFO's other extreme likewise rides
        // in modEnd and is installed as a mod rather than a sweep, but the
        // frequency setter sees it too.
        if (cl.modEnd !== null && cl.modEnd !== undefined) ends.push(cl.modEnd);
        const t = entry.t && entry.t[f];
        if (t && t.target !== null && t.target !== undefined &&
            (cl.interp === 'lin' || cl.interp === 'quad')) ends.push(t.target);
        for (const v of ends) {
            if (v > max) return label + ' ' + v + ' exceeds ' + max;
            if (f !== 'freq' && v < 0) return label + ' ' + v + ' is negative';
        }
    }
    return null;
}

// Index of the last entry at or before tMs that the device would have
// ACCEPTED. A rejected row leaves the channel on its previous state, so it is
// skipped at lookup rather than removed from the list — ramp targets still
// point at it, which is what the device's find_next_led_for_bit does.
function acceptedIndexAt(entries, tMs) {
    let i = activeIndexAt(entries, tMs);
    while (i >= 0) {
        if (entries[i]._rejection === undefined) {
            entries[i]._rejection = ledEntryRejection(entries[i]);
        }
        if (!entries[i]._rejection) return i;
        i--;
    }
    return -1;
}

const LED_FIELDS = LED_CORE_FIELDS.concat(LED_PULSE_FIELDS.filter(f => f !== 'jitter'));

/*
 * Full LED channel state at tMs.
 *
 * Returns null before the channel's first accepted entry (nothing is driving
 * it, which the device renders as black via the inactive-channel path).
 *
 *   { freq, duty, bright, r, g, b, env, phase, attack, jitter,
 *     dc, cycleUs, elapsedUs, effElapsedUs, levelQ8, level01,
 *     px: { r, g, b }, on, tickHz, tickUs, rejection, entryTime }
 *
 * `level01` is levelQ8 / 25600 — the fraction of full brightness, useful for a
 * scope trace. `px` is the 8-bit colour the strip would receive.
 */
export function ledStateAt(entries, tMs) {
    if (!entries || !entries.length) return null;
    const idx = acceptedIndexAt(entries, tMs);
    if (idx < 0) {
        // Before the first installable entry. Report why, if a row was dropped.
        const raw = activeIndexAt(entries, tMs);
        return raw >= 0
            ? { active: false, rejection: entries[raw]._rejection, px: { r: 0, g: 0, b: 0 },
                levelQ8: 0, level01: 0, on: false }
            : null;
    }

    const e = entries[idx];
    // Evaluate at the ACCEPTED index, not at activeIndexAt's — those differ
    // whenever the device dropped a row — but against the full list, so the
    // ramp annotations still point at the right targets.
    const st = stateAtIndex(entries, idx, tMs, LED_FIELDS);

    // The parser casts duty/bright to uint8_t and clamps RGB to 0..255.
    const duty = Math.max(0, Math.min(100, Math.trunc(st.duty)));
    const bright = Math.max(0, Math.min(100, Math.trunc(st.bright)));
    const r = Math.max(0, Math.min(255, Math.trunc(st.r)));
    const g = Math.max(0, Math.min(255, Math.trunc(st.g)));
    const b = Math.max(0, Math.min(255, Math.trunc(st.b)));
    // LED env folds anything outside 0..3 to SQUARE. The audio side does the
    // opposite — do not merge these.
    let env = Math.trunc(st.env);
    if (!(env >= 0 && env <= 3)) env = LED_CARRIER_SQUARE;
    const phase = ((Math.trunc(st.phase) % 360) + 360) % 360;
    const attackMs = Math.max(0, Math.trunc(st.attack));

    const brightnessQ8 = bright * 256;
    const dutyQ8 = duty * 256;

    const out = {
        active: true, entryTime: e.time, rejection: null,
        freq: st.freq, duty, bright, r, g, b, env, phase, attack: attackMs,
        jitter: e.jitterObj || null,
    };

    // ---- DC channel (freq 0) ------------------------------------------------
    // led_matrix_example.c:896. Steady brightness; duty, env and phase are all
    // ignored because the branch `continue`s before each of them.
    if (!(st.freq > 0)) {
        out.dc = true;
        out.cycleUs = 0;
        out.elapsedUs = 0;
        out.effElapsedUs = 0;
        out.levelQ8 = brightnessQ8;
        out.level01 = brightnessQ8 / LED_BRIGHTNESS_Q8_MAX;
        out.on = true;
        out.tickHz = LED_FLICKER_TICK_MIN;
        out.tickUs = 1000;
        out.px = compositePixel(r, g, b, brightnessQ8);
        return out;
    }

    out.dc = false;

    // ---- Flicker ------------------------------------------------------------
    let freqMilliHz = Math.trunc(st.freq * 1000);
    const jit = e.jitterObj;
    if (jit && jit.amp > 0) {
        // Jitter amp is clamped to [0, 5] Hz by the setter (:1759); its period
        // defaults to 45 s when only an amp was given (config_parser.c:1687).
        const ampMilliHz = Math.trunc(Math.max(0, Math.min(5, jit.amp)) * 1000);
        const periodUs = Math.trunc((jit.period === undefined || jit.period === null
            ? 45000 : jit.period) * 1000);
        let jf = freqMilliHz + jitterMilliHz(Math.trunc(tMs * 1000), ampMilliHz, periodUs);
        if (jf < 100) jf = 100;                       // hard clamp >= 0.1 Hz
        freqMilliHz = jf;
    }

    const cycleUs = Math.floor(1000000000 / freqMilliHz);
    out.cycleUs = cycleUs;
    out.tickHz = tickHzFor(freqMilliHz / 1000);
    out.tickUs = Math.floor(1000000 / out.tickHz);

    // Cycle origin is the entry's own time. The device steps its origin by one
    // period per cycle rather than re-anchoring to "now", which is equivalent
    // to this modulo while the rate holds — and the rate is re-read here every
    // evaluation anyway, which is the closest continuous analogue.
    const sinceEntryUs = Math.max(0, Math.trunc((tMs - e.time) * 1000));
    const elapsedUs = cycleUs > 0 ? (sinceEntryUs % cycleUs) : 0;
    out.elapsedUs = elapsedUs;

    // phase is a LEAD: it ADDS to elapsed time, so the edge arrives EARLIER.
    let eff = elapsedUs;
    if (phase !== 0) {
        eff += Math.floor((phase * cycleUs) / 360);
        if (eff >= cycleUs) eff -= cycleUs;
    }
    out.effElapsedUs = eff;

    if (env === LED_CARRIER_SQUARE) {
        // latched_on_time_us = cycle_us * duty_q8 / 25600 (:1054).
        const onTimeUs = Math.floor((cycleUs * dutyQ8) / LED_BRIGHTNESS_Q8_MAX);
        out.on = eff < onTimeUs;
        out.onTimeUs = onTimeUs;
        out.levelQ8 = out.on ? brightnessQ8 : 0;
    } else {
        const dutyQ16 = Math.floor((dutyQ8 * 65536) / LED_BRIGHTNESS_Q8_MAX);
        let attackUs = attackMs * 1000;
        if (attackUs > 60000) attackUs = 60000;       // ISR cap (:1224)
        const attackQ16 = cycleUs ? Math.floor((attackUs * 65536) / cycleUs) : 0;
        out.levelQ8 = carrierLevelQ8(env, eff, cycleUs, brightnessQ8, dutyQ16, attackQ16);
        out.on = out.levelQ8 > 0;
    }

    out.level01 = out.levelQ8 / LED_BRIGHTNESS_Q8_MAX;
    out.px = compositePixel(r, g, b, out.levelQ8);
    return out;
}

/*
 * The strip's brightness compositing (led_strip.c:458), truncating exactly as
 * the C does. Note this is the SECOND truncation on the smooth-carrier path —
 * levelQ8 was already floored out of brightness_q8 * shape_q16 / 65536 — and
 * doing the pair in floats gives a visibly different low-brightness ramp.
 */
export function compositePixel(r, g, b, brightnessQ8) {
    return {
        r: Math.floor((r * brightnessQ8) / LED_BRIGHTNESS_Q8_MAX) & 0xFF,
        g: Math.floor((g * brightnessQ8) / LED_BRIGHTNESS_Q8_MAX) & 0xFF,
        b: Math.floor((b * brightnessQ8) / LED_BRIGHTNESS_Q8_MAX) & 0xFF,
    };
}

// ---- Physical layout -------------------------------------------------------

/*
 * The shipped build's channel map (sdkconfig:618): 74 pixels, two chained
 * M5Stack NeoHEX boards of 37, one WS2812 data line.
 *
 * The pairing is MIRRORED on purpose, which is the single easiest thing to get
 * wrong when drawing it: bit 0 is eye A INNER, bit 1 eye A OUTER, bit 2 eye B
 * OUTER, bit 3 eye B INNER. So mask 9 (bits 0 and 3) is "both inner zones" and
 * mask 6 (bits 1 and 2) is "both outer rings". Assuming bit N is zone N in
 * reading order swaps the second eye's rings.
 *
 * Bits 4..7 exist in flicker_state[] and answer every API call, but own no
 * pixels in this build, so they are invisible on the hardware.
 */
export const DEFAULT_CHANNEL_MAP = (
    '1,1,1,1,1,0,0,0,1,1,0,0,0,0,1,1,0,0,0,0,0,1,1,0,0,0,0,1,1,0,0,0,1,1,1,1,1,' +
    '2,2,2,2,2,3,3,3,2,2,3,3,3,3,2,2,3,3,3,3,3,2,2,3,3,3,3,2,2,3,3,3,2,2,2,2,2'
).split(',').map(Number);

// Named masks worth offering in the UI, from docs/hardware/index.html:689.
export const ZONE_PRESETS = [
    { mask: 1, label: 'Eye A inner' },
    { mask: 2, label: 'Eye A outer' },
    { mask: 4, label: 'Eye B outer' },
    { mask: 8, label: 'Eye B inner' },
    { mask: 3, label: 'Eye A' },
    { mask: 12, label: 'Eye B' },
    { mask: 9, label: 'Both inner' },
    { mask: 6, label: 'Both outer' },
    { mask: 15, label: 'All four zones' },
];

/*
 * NeoHEX geometry, derived from the channel map's run lengths.
 *
 * Each board is 7 serpentine rows of widths 4,5,6,7,6,5,4 = 37. The run
 * lengths in the shipped map (5,3,2,4,2,5,2,4,2,3,5) match that layout
 * exactly, with the perimeter (18 px) on the outer channel and the interior
 * (19 px) on the inner one.
 *
 * Returns [{ index, channel, eye, col, row, x, y }] in hex-grid coordinates,
 * x/y normalised so a renderer can place them without knowing the topology.
 */
export function neohexLayout(channelMap = DEFAULT_CHANNEL_MAP) {
    const ROWS = [4, 5, 6, 7, 6, 5, 4];
    const out = [];
    const perBoard = ROWS.reduce((a, b) => a + b, 0);   // 37
    const boards = Math.ceil(channelMap.length / perBoard);
    for (let board = 0; board < boards; board++) {
        let i = 0;
        for (let row = 0; row < ROWS.length; row++) {
            const w = ROWS[row];
            for (let col = 0; col < w; col++) {
                const index = board * perBoard + i;
                if (index >= channelMap.length) break;
                // Serpentine: odd rows run right-to-left.
                const c = (row % 2 === 1) ? (w - 1 - col) : col;
                out.push({
                    index,
                    channel: channelMap[index],
                    eye: board,
                    row,
                    col: c,
                    // Centre each row, hex rows offset by half a cell vertically.
                    x: c - (w - 1) / 2,
                    y: row - (ROWS.length - 1) / 2,
                });
                i++;
            }
        }
    }
    return out;
}

// Pixel counts per channel, for the inspector.
export function channelPixelCounts(channelMap = DEFAULT_CHANNEL_MAP) {
    const counts = {};
    for (const ch of channelMap) counts[ch] = (counts[ch] || 0) + 1;
    return counts;
}
