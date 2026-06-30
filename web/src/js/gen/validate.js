// Semantic validator — client-side mirror of the firmware's clamps + rejects
// (config_parser.c). There is no /api/validate endpoint; all validation is
// local. validate(doc) -> [{ row, severity, msg }] where `row` is the index
// into doc.rows ('error' = firmware would reject the line; 'warn' = firmware
// would clamp / the value is suspicious).

import {
    NUM_AUDIO_CHANNELS, NUM_LED_CHANNELS, MAX_ENTRIES, MAX_BATCH_SIZE,
    NYQUIST, WAVE_COUNT, isModInterp,
} from './model.js';

function isRamp(interp) { return interp === 'lin' || interp === 'quad'; }

// Does a periodic-mod cell carry a usable end + period?
function modComplete(c) {
    return c.modEnd !== null && c.modEnd !== undefined &&
           c.modPeriodMs !== null && c.modPeriodMs !== undefined && c.modPeriodMs > 0;
}

export function validate(doc) {
    const diags = [];
    if (!doc || !doc.rows) return diags;
    const rows = doc.rows;

    const add = (row, severity, msg) => diags.push({ row, severity, msg });

    // ---- Entry-count + same-timestamp batch checks --------------------------
    let entryCount = 0;
    const timeCounts = {}; // time_ms -> number of LED/audio rows
    for (const r of rows) {
        if (r.kind === 'led' || r.kind === 'audio') {
            entryCount++;
            timeCounts[r.time] = (timeCounts[r.time] || 0) + 1;
        }
    }
    if (entryCount > MAX_ENTRIES) {
        add(-1, 'warn', entryCount + ' timeline entries exceeds MAX_ENTRIES (' +
            MAX_ENTRIES + '); the firmware will drop the overflow.');
    }
    for (const t in timeCounts) {
        if (timeCounts[t] > MAX_BATCH_SIZE) {
            add(-1, 'warn', timeCounts[t] + ' entries share t=' + t +
                ' ms, exceeding the same-timestamp batch cap (' + MAX_BATCH_SIZE + ').');
        }
    }

    // Helper: find the next same-channel/field row that a ramp targets.
    function hasLedRampTarget(idx, mask, field) {
        for (let j = idx + 1; j < rows.length; j++) {
            const r = rows[j];
            if (r.kind === 'led' && (r.mask & mask) && r[field]) return true;
        }
        return false;
    }
    function hasAudioRampTarget(idx, channel, field) {
        for (let j = idx + 1; j < rows.length; j++) {
            const r = rows[j];
            if (r.kind === 'audio' && r.channel === channel && r[field]) return true;
        }
        return false;
    }

    const checkCell = (i, c, lo, hi, name, kind, mask, channel, field) => {
        if (!c) return;
        if (c.value < lo || c.value > hi) {
            add(i, 'warn', name + ' value ' + c.value + ' out of range [' + lo + '..' + hi + '] (will clamp).');
        }
        if (isModInterp(c.interp)) {
            if (!modComplete(c)) {
                add(i, 'warn', name + ' modulation is missing an end/period (defaults applied).');
            }
        } else if (isRamp(c.interp)) {
            const ok = (kind === 'led') ? hasLedRampTarget(i, mask, field)
                                        : hasAudioRampTarget(i, channel, field);
            if (!ok) {
                add(i, 'warn', name + ' has a ramp prefix but no later same-channel entry to ramp toward (holds at start).');
            }
        }
    };

    for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        if (r.kind === 'raw') {
            add(i, 'error', r.error || 'Unparsable line.');
            continue;
        }
        if (r.kind === 'led') {
            if (!r.mask || (r.mask & 0xFF) === 0) {
                add(i, 'error', 'LED channel_mask must be non-zero (firmware rejects mask==0).');
            }
            if (r.mask > 0xFF) {
                add(i, 'warn', 'LED channel_mask ' + r.mask + ' exceeds 8 bits; only bits 0..' +
                    (NUM_LED_CHANNELS - 1) + ' are used.');
            }
            checkCell(i, r.freq,   0, NYQUIST, 'LED freq',  'led', r.mask, null, 'freq');
            checkCell(i, r.duty,   0, 100,     'duty',      'led', r.mask, null, 'duty');
            checkCell(i, r.bright, 0, 100,     'brightness','led', r.mask, null, 'bright');
            checkCell(i, r.r,      0, 255,     'R',         'led', r.mask, null, 'r');
            checkCell(i, r.g,      0, 255,     'G',         'led', r.mask, null, 'g');
            checkCell(i, r.b,      0, 255,     'B',         'led', r.mask, null, 'b');
            continue;
        }
        if (r.kind === 'audio') {
            if (r.channel !== null && r.channel !== undefined) {
                if (r.channel < 1 || r.channel > NUM_AUDIO_CHANNELS) {
                    add(i, 'warn', 'audio channel ' + r.channel + ' outside 1..' +
                        NUM_AUDIO_CHANNELS + '.');
                }
            }
            if (r.waveType !== null && r.waveType !== undefined &&
                (r.waveType < 0 || r.waveType >= WAVE_COUNT)) {
                add(i, 'warn', 'wave_type ' + r.waveType + ' outside 0..' + (WAVE_COUNT - 1) + '.');
            }
            if (r.freqR && (r.freqR < 0 || r.freqR > NYQUIST)) {
                add(i, 'warn', 'freq_r ' + r.freqR + ' outside 0..' + NYQUIST + ' (treated as 0).');
            }
            checkCell(i, r.freq, 0, NYQUIST, 'audio freq', 'audio', null, r.channel, 'freq');
            checkCell(i, r.pan, -100, 100,   'pan',        'audio', null, r.channel, 'pan');
            checkCell(i, r.vol,    0, 100,   'volume',     'audio', null, r.channel, 'vol');
            checkCell(i, r.mod,    0, NYQUIST,'mod',       'audio', null, r.channel, 'mod');
            continue;
        }
        if (r.kind === 'bg') {
            const b = r.bg;
            if (b.pan < -100 || b.pan > 100) add(i, 'warn', 'BG pan ' + b.pan + ' outside -100..100 (will clamp).');
            if (b.loudness < 0 || b.loudness > 100) add(i, 'warn', 'BG loudness ' + b.loudness + ' outside 0..100 (will clamp).');
        }
    }

    return diags;
}
