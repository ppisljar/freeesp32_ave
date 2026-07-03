// ===========================================================
// Live Control panel — direct-control sliders for every audio
// and LED channel. Sends single-line patches to /api/patch-config
// and polls /api/state once a second to keep sliders in sync.
// ===========================================================
import { showMessage } from './util.js';

const CTRL_ANIMATE_MS = 1000;   // patch animation duration
const CTRL_THROTTLE_MS = 250;   // min gap between patches per control
const CTRL_POLL_MS = 1000;      // /api/state polling interval
const CTRL_LOCK_AFTER_MS = 1000; // after last input, how long to keep poll-paused
let ctrlState = null;           // most recent /api/state response
let ctrlPrevTlRunning = false;  // prior timeline.running, for edge detection
const ctrlInteracting = new Set(); // keys currently being dragged → poll-paused
const ctrlLastPatchAt = new Map(); // control key → last-send timestamp
const ctrlPending = new Map();  // control key → pending value (throttle queue)

// ---- Channel locking (browser-side only) ---------------------------
// ctrlLocks[domain][idx] = leaderIdx means channel idx FOLLOWS leaderIdx.
//   LED locks are MIRROR locks: follower copies the leader's values.
//   Audio locks are RELATIVE locks: follower freq tracks the leader's
//   freq changes, preserving the offset captured at lock time (pan/vol
//   /mod stay independent). Chains resolve to a single root.
let ctrlLocks = { led: {}, aud: {} };
const ctrlAudOffset = {};        // audio follower idx → freq offset (Hz)
const CTRL_STORE_KEY = 'esp32_ctrl_locks_v1';
function ctrlSaveLocks() { try { localStorage.setItem(CTRL_STORE_KEY, JSON.stringify(ctrlLocks)); } catch (e) {} }
function ctrlLoadLocks() { try { const s = localStorage.getItem(CTRL_STORE_KEY); if (s) { const o = JSON.parse(s); if (o && o.led && o.aud) ctrlLocks = o; } } catch (e) {} }
// Walk the lock chain to the ultimate root (an unlocked channel).
function ctrlRoot(domain, idx) { const L = ctrlLocks[domain]; let cur = idx, g = 0; while (L[cur] != null && g++ < 64) cur = L[cur]; return cur; }
// All channels (direct + transitive) whose root is rootIdx, excluding it.
function ctrlFollowers(domain, rootIdx) { const L = ctrlLocks[domain]; const res = []; for (const k in L) { const i = parseInt(k); if (L[i] == null) continue; if (i !== rootIdx && ctrlRoot(domain, i) === rootIdx) res.push(i); } return res; }
// Would locking idx→target create a loop? (target already follows idx?)
function ctrlWouldCycle(domain, idx, target) { if (idx === target) return true; let cur = target, g = 0; while (cur != null && g++ < 64) { if (cur === idx) return true; cur = ctrlLocks[domain][cur]; } return false; }
// LED mirror mask: leader bit OR'd with every follower bit.
function ctrlLedMask(idx) { let m = 1 << idx; ctrlFollowers('led', idx).forEach(f => m |= (1 << f)); return m; }
// Current device freq for audio slider idx (state is 1-indexed, +1 offset).
function ctrlAudFreq(i) { return (ctrlState && ctrlState.audio && ctrlState.audio[i + 1]) ? ctrlState.audio[i + 1].freq : 0; }
// Resolve a follower's target freq given the root's new freq, summing the
// captured offsets along the chain from root down to idx.
function ctrlAudResolvedFreq(idx, rootNewFreq, rootIdx) { if (idx === rootIdx) return rootNewFreq; const leader = ctrlLocks.aud[idx]; if (leader == null) return rootNewFreq; return ctrlAudResolvedFreq(leader, rootNewFreq, rootIdx) + (ctrlAudOffset[idx] || 0); }
// Build the lock dropdown for one channel (every other channel + none).
function ctrlPopulateLockSelect(domain, idx, count) {
    const prefix = (domain === 'aud') ? 'aud-' : 'led-';
    const letter = (domain === 'aud') ? 'A' : 'L';
    const sel = document.getElementById(prefix + idx + '-lock');
    if (!sel) return;
    let html = '<option value="">—</option>';
    for (let j = 0; j < count; j++) { if (j === idx) continue; html += '<option value="' + j + '">' + letter + (j + 1) + '</option>'; }
    sel.innerHTML = html;
    const cur = ctrlLocks[domain][idx];
    sel.value = (cur == null) ? '' : String(cur);
}
// Enable/disable a channel's controls to reflect its lock state. Disables both
// the slider and its number input ("-v") for each affected field.
function ctrlApplyLockUI(domain, idx) {
    const locked = ctrlLocks[domain][idx] != null;
    const dis = (id) => { const e = document.getElementById(id); if (e) e.disabled = locked; };
    if (domain === 'aud') {
        dis('aud-' + idx + '-freq'); dis('aud-' + idx + '-freq-v');
        const row = document.getElementById('ctrl-aud-' + idx); if (row) row.classList.toggle('locked', locked);
    } else {
        ['freq', 'duty', 'bright'].forEach(fld => { dis('led-' + idx + '-' + fld); dis('led-' + idx + '-' + fld + '-v'); });
        const c = document.getElementById('led-' + idx + '-col'); if (c) c.disabled = locked;
        const row = document.getElementById('ctrl-led-' + idx); if (row) row.classList.toggle('locked', locked);
    }
}
// Handle a lock-dropdown change: validate, store, capture offset, sync.
function ctrlOnLockChange(domain, idx, sel) {
    const val = sel.value;
    const target = (val === '') ? null : parseInt(val);
    const L = (domain === 'aud') ? 'A' : 'L';
    if (target != null && ctrlWouldCycle(domain, idx, target)) {
        showMessage('Cannot lock ' + L + (idx + 1) + ' to ' + L + (target + 1) + ': that would create a loop.', 'error');
        sel.value = (ctrlLocks[domain][idx] == null) ? '' : String(ctrlLocks[domain][idx]);
        return;
    }
    if (target == null) { delete ctrlLocks[domain][idx]; delete ctrlAudOffset[idx]; }
    else {
        ctrlLocks[domain][idx] = target;
        if (domain === 'aud') { ctrlAudOffset[idx] = ctrlAudFreq(idx) - ctrlAudFreq(target); }
        else if (ctrlState && ctrlState.led && ctrlState.led[target]) {
            // LED mirror: snap the follower to the leader's values now.
            const ld = ctrlState.led[target];
            ctrlPatch(CTRL_ANIMATE_MS + ' ' + ld.freq.toFixed(2) + ' ' + Math.round(ld.duty) + ' ' + Math.round(ld.bright) + ' ' + Math.round(ld.r) + ' ' + Math.round(ld.g) + ' ' + Math.round(ld.b) + ' ' + (1 << idx));
        }
    }
    ctrlSaveLocks();
    ctrlApplyLockUI(domain, idx);
}

function ctrlPatch(line) {
    return fetch('/api/patch-config', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: line,
    }).catch(err => console.warn('patch failed:', err));
}

// ---- Per-field modulation (the ∿ icon) -----------------------------------
// Last-applied modulation settings per field, keyed 'aud-<idx>-<field>' /
// 'led-<idx>-<field>', so reopening the popup pre-fills what you last set.
const ctrlModState = {};
const MOD_WAVES = [
    ['none', 'None (off)'], ['triangle', 'Triangle'], ['sine', 'Sine'],
    ['sawup', 'Saw up'], ['sawdown', 'Saw down'], ['square', 'Square'],
];

// Carrier waveforms for a TONE channel (noise 4/5/6 live on ch 9, handled
// separately). Value = the integer sent in the 8th audio field / applied by
// audio_generator_set_wave_type_locked. 7 = EEG-contour carrier.
const CARRIER_WAVES = [
    [0, 'Sine'], [1, 'Square'], [2, 'Triangle'], [3, 'Saw'], [7, 'EEG'],
];
// Isochronic/pulse envelope shapes. Audio has an extra 4=Tremolo (legacy bipolar
// sine); LED envelopes are 0..3 only.
const ISO_ENVS_AUDIO = [[0, 'Square'], [1, 'Sine'], [2, 'Triangle'], [3, 'Trapezoid'], [4, 'Tremolo']];
const ISO_ENVS_LED   = [[0, 'Square'], [1, 'Sine'], [2, 'Triangle'], [3, 'Trapezoid']];
// Last-applied pulse settings per channel key, so reopening the popup pre-fills.
const ctrlPulseState = {};

// Current device value of a field, used as a sensible default for "from".
function ctrlFieldValue(domain, idx, field) {
    if (domain === 'aud') {
        const a = ctrlState && ctrlState.audio && ctrlState.audio[idx + 1];
        return a ? (a[field] || 0) : 0;
    }
    const l = ctrlState && ctrlState.led && ctrlState.led[idx];
    return l ? (l[field] || 0) : 0;
}

function ctrlSendMod(body) {
    return fetch('/api/mod', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    }).then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); })
      .catch(err => showMessage('Modulation failed: ' + err, 'error'));
}

// Popup to set/change/clear the modulation on one field.
function ctrlOpenModPopup(domain, idx, field) {
    const key = domain + '-' + idx + '-' + field;
    const label = (domain === 'aud' ? 'A' + (idx + 1) : 'L' + (idx + 1)) + ' ' + field;
    const cur = ctrlFieldValue(domain, idx, field);
    const st = ctrlModState[key] || { wave: 'sine', from: cur, to: cur, period: 1000 };

    const back = document.createElement('div');
    back.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.4);display:flex;align-items:center;justify-content:center;z-index:1000;';
    const box = document.createElement('div');
    box.style.cssText = 'background:#fff;padding:20px;border-radius:8px;min-width:280px;box-shadow:0 4px 20px rgba(0,0,0,0.3);font-size:14px;';
    box.innerHTML =
        '<h3 style="margin:0 0 12px 0;">Modulation — ' + label + '</h3>'
      + '<div style="margin-bottom:8px;"><label>Type<br><select id="modWave" style="width:100%;padding:6px;">'
      + MOD_WAVES.map(w => '<option value="' + w[0] + '"' + (w[0] === st.wave ? ' selected' : '') + '>' + w[1] + '</option>').join('')
      + '</select></label></div>'
      + '<div style="display:flex;gap:8px;margin-bottom:8px;">'
      + '<label style="flex:1;">From<br><input id="modFrom" type="number" step="any" value="' + st.from + '" style="width:100%;padding:6px;"></label>'
      + '<label style="flex:1;">To<br><input id="modTo" type="number" step="any" value="' + st.to + '" style="width:100%;padding:6px;"></label>'
      + '</div>'
      + '<div style="margin-bottom:14px;"><label>Period (ms)<br><input id="modPeriod" type="number" min="1" step="1" value="' + st.period + '" style="width:100%;padding:6px;"></label></div>'
      + '<div style="text-align:right;"><button id="modCancel">Cancel</button> <button id="modApply" style="background:#28a745;">Apply</button></div>';
    back.appendChild(box);
    document.body.appendChild(back);
    const close = () => document.body.removeChild(back);
    back.addEventListener('click', (e) => { if (e.target === back) close(); });
    box.querySelector('#modCancel').addEventListener('click', close);
    box.querySelector('#modApply').addEventListener('click', () => {
        const wave = box.querySelector('#modWave').value;
        const from = parseFloat(box.querySelector('#modFrom').value) || 0;
        const to = parseFloat(box.querySelector('#modTo').value) || 0;
        const period = parseInt(box.querySelector('#modPeriod').value, 10) || 1000;
        ctrlModState[key] = { wave, from, to, period };
        const ch = (domain === 'aud') ? (idx + 1) : idx;   // audio: generator ch = idx+1
        ctrlSendMod({ domain: (domain === 'aud') ? 'audio' : 'led', ch, field, wave, from, to, period_ms: period })
            .then(() => showMessage(wave === 'none'
                ? 'Modulation cleared on ' + label
                : 'Modulation set on ' + label + ' (' + wave + ')', 'success'));
        close();
    });
}

// ---- Per-channel pulse / isochronic shaping (the ⚙ icon) -------------------
// Audio: env/duty/attack/depth (→ /api/iso-env?ch=), phase (→ /api/audio-phase),
//        beat jitter (→ /api/beat-jitter, DEVICE-WIDE).
// LED:   carrier (→ /api/flicker-carrier?mask=), attack (→ /api/flicker-attack),
//        phase (→ /api/flicker-phase), jitter (→ /api/flicker-jitter?mask=).
function ctrlGet(url) {
    return fetch(url).then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); })
        .catch(err => showMessage('Pulse update failed: ' + err, 'error'));
}
function ctrlOpenPulsePopup(domain, idx) {
    const isAud = (domain === 'aud');
    const key = domain + '-' + idx + '-pulse';
    const label = (isAud ? 'A' : 'L') + (idx + 1) + ' pulse';
    const st = ctrlPulseState[key] || (isAud
        ? { env: 4, duty: 50, attack: 5, depth: 0, phase: 0, jamp: 0, jper: 45000 }
        : { env: 0, attack: 0, phase: 0, jamp: 0, jper: 45000 });
    const envList = isAud ? ISO_ENVS_AUDIO : ISO_ENVS_LED;
    const envLabel = isAud ? 'Envelope' : 'Carrier';

    const num = (id, lbl, val, min) =>
        '<label style="flex:1;">' + lbl + '<br><input id="' + id + '" type="number" step="any"'
        + (min != null ? ' min="' + min + '"' : '') + ' value="' + val + '" style="width:100%;padding:6px;"></label>';

    const back = document.createElement('div');
    back.style.cssText = 'position:fixed;inset:0;background:rgba(0,0,0,0.4);display:flex;align-items:center;justify-content:center;z-index:1000;';
    const box = document.createElement('div');
    box.style.cssText = 'background:#fff;padding:20px;border-radius:8px;min-width:300px;box-shadow:0 4px 20px rgba(0,0,0,0.3);font-size:14px;';
    box.innerHTML =
        '<h3 style="margin:0 0 12px 0;">Pulse — ' + label + '</h3>'
      + '<div style="margin-bottom:8px;"><label>' + envLabel + '<br><select id="pEnv" style="width:100%;padding:6px;">'
      + envList.map(e => '<option value="' + e[0] + '"' + (e[0] === st.env ? ' selected' : '') + '>' + e[1] + '</option>').join('')
      + '</select></label></div>'
      + '<div style="display:flex;gap:8px;margin-bottom:8px;">'
      + (isAud ? num('pDuty', 'Duty %', st.duty, 0) : '')
      + num('pAttack', 'Attack ms', st.attack, 0)
      + (isAud ? num('pDepth', 'Depth %', st.depth, 0) : '')
      + num('pPhase', 'Phase °', st.phase, 0)
      + '</div>'
      + '<div style="display:flex;gap:8px;margin-bottom:14px;">'
      + num('pJamp', 'Jitter Hz', st.jamp, 0)
      + num('pJper', 'Jitter period ms', st.jper, 1)
      + '</div>'
      + (isAud ? '<div style="font-size:12px;color:#888;margin-bottom:10px;">Beat jitter is device-wide (affects all audio channels).</div>' : '')
      + '<div style="text-align:right;"><button id="pCancel">Cancel</button> <button id="pApply" style="background:#28a745;">Apply</button></div>';
    back.appendChild(box);
    document.body.appendChild(back);
    const close = () => document.body.removeChild(back);
    back.addEventListener('click', (e) => { if (e.target === back) close(); });
    box.querySelector('#pCancel').addEventListener('click', close);
    box.querySelector('#pApply').addEventListener('click', () => {
        const g = (id) => { const el = box.querySelector('#' + id); return el ? parseFloat(el.value) : 0; };
        const env = parseInt(box.querySelector('#pEnv').value, 10) || 0;
        const attack = g('pAttack'), phase = g('pPhase'), jamp = g('pJamp'), jper = g('pJper') || 45000;
        if (isAud) {
            const duty = g('pDuty'), depth = g('pDepth');
            ctrlPulseState[key] = { env, duty, attack, depth, phase, jamp, jper };
            const ch = idx + 1;   // audio channel index matches state.audio[] / .ledc channel
            ctrlGet('/api/iso-env?ch=' + ch + '&env=' + env + '&duty=' + duty + '&attack=' + attack + '&depth=' + depth);
            ctrlGet('/api/audio-phase?ch=' + ch + '&deg=' + Math.round(phase));
            ctrlGet('/api/beat-jitter?amp=' + jamp + '&period=' + jper);
        } else {
            ctrlPulseState[key] = { env, attack, phase, jamp, jper };
            const mask = ctrlLedMask(idx);
            ctrlGet('/api/flicker-carrier?mask=' + mask + '&wave=' + env);
            ctrlGet('/api/flicker-attack?mask=' + mask + '&ms=' + Math.round(attack));
            ctrlGet('/api/flicker-phase?mask=' + mask + '&deg=' + Math.round(phase));
            ctrlGet('/api/flicker-jitter?mask=' + mask + '&amp=' + jamp + '&period=' + jper);
        }
        showMessage('Pulse applied on ' + (isAud ? 'A' : 'L') + (idx + 1), 'success');
        close();
    });
}

// Build a one-line audio patch. Reads CURRENT values for non-changed
// fields from ctrlState so the patch's animation sweeps only the
// changed field (other fields' current=target → no-op animation).
//
// chIdx is 0..7 (UI slider index). The .ledc convention is 1-indexed
// for audio channels (channel 0 is unused by convention), so slider
// A1 == .ledc channel 1 == internal audio_channels[1] == state.audio[1].
// We therefore read state at index chIdx+1 and send channel chIdx+1.
function ctrlBuildAudioPatch(chIdx, field, value) {
    if (!ctrlState || !ctrlState.audio) return null;
    const ch = ctrlState.audio[chIdx + 1];
    if (!ch) return null;
    const f = (field === 'freq') ? value : ch.freq;
    const p = (field === 'pan')  ? value : ch.pan;
    const v = (field === 'vol')  ? value : ch.vol;
    const m = (field === 'mod')  ? value : ch.mod;
    // freq_r (binaural right-ear Hz) + wave are carried on EVERY patch so a plain
    // slider nudge preserves them (both apply idempotently on the device).
    const fr = (field === 'freqr') ? value : (ch.freq_r || 0);
    const wv = (field === 'wave')  ? value : (ch.wave   || 0);
    // A time freq pan vol mod channel freq_r wave
    return 'A ' + CTRL_ANIMATE_MS + ' ' + f.toFixed(3) + ' ' + p.toFixed(1)
         + ' ' + v.toFixed(1) + ' ' + m.toFixed(2) + ' ' + (chIdx + 1)
         + ' ' + (+fr).toFixed(3) + ' ' + (wv | 0);
}

// Build a one-line LED patch. Same current-value-preservation logic.
function ctrlBuildLedPatch(chIdx, field, value) {
    if (!ctrlState || !ctrlState.led) return null;
    const ch = ctrlState.led[chIdx];
    const freq   = (field === 'freq')   ? value : ch.freq;
    const duty   = (field === 'duty')   ? value : ch.duty;
    const bright = (field === 'bright') ? value : ch.bright;
    const r = (field === 'r') ? value : ch.r;
    const g = (field === 'g') ? value : ch.g;
    const b = (field === 'b') ? value : ch.b;
    // time freq duty bright R G B mask
    return CTRL_ANIMATE_MS + ' ' + freq.toFixed(2) + ' ' + Math.round(duty)
         + ' ' + Math.round(bright) + ' ' + Math.round(r) + ' ' + Math.round(g)
         + ' ' + Math.round(b) + ' ' + ctrlLedMask(chIdx);
}

// Throttle: per-key, fire at most one POST per CTRL_THROTTLE_MS.
// Queued value supersedes any earlier pending one (last-wins).
function ctrlSendThrottled(key, lineBuilder) {
    const now = Date.now();
    const lastSent = ctrlLastPatchAt.get(key) || 0;
    const gap = now - lastSent;
    if (gap >= CTRL_THROTTLE_MS) {
        const line = lineBuilder();
        if (line) {
            ctrlPatch(line);
            ctrlLastPatchAt.set(key, now);
        }
        ctrlPending.delete(key);
    } else {
        // Schedule the latest value to fire when the throttle window opens.
        ctrlPending.set(key, lineBuilder);
        if (!ctrlPending.has(key + ':timer')) {
            ctrlPending.set(key + ':timer', true);
            setTimeout(() => {
                ctrlPending.delete(key + ':timer');
                const pending = ctrlPending.get(key);
                if (!pending) return;
                const line = pending();
                if (line) {
                    ctrlPatch(line);
                    ctrlLastPatchAt.set(key, Date.now());
                }
                ctrlPending.delete(key);
            }, CTRL_THROTTLE_MS - gap);
        }
    }
}

// Build one row of audio-channel controls.
function ctrlRenderAudioRow(idx) {
    const div = document.createElement('div');
    div.className = 'ctrl-row';
    div.id = 'ctrl-aud-' + idx;
    div.innerHTML =
        '<div class="label"><span class="active-dot" id="aud-' + idx + '-dot"></span> A' + (idx+1)
      +   ' <span class="mod-ico" id="aud-' + idx + '-pulse" title="Pulse / isochronic shaping — click to edit">⚙</span></div>'
      + '<div class="field"><label>freq</label><input type="range" min="0" max="20000" step="0.1" id="aud-' + idx + '-freq"><input type="number" class="val numv" min="0" max="20000" step="any" id="aud-' + idx + '-freq-v"><span class="mod-ico" id="aud-' + idx + '-freq-m" title="Modulation — click to edit">∿</span></div>'
      + '<div class="field"><label>R-freq</label><input type="number" class="val numv" min="0" max="20000" step="any" id="aud-' + idx + '-freqr-v" title="Right-ear carrier Hz for binaural (0 = mono)"></div>'
      + '<div class="field"><label>pan</label><input type="range" min="-100" max="100" step="1" id="aud-' + idx + '-pan"><input type="number" class="val numv" min="-100" max="100" step="any" id="aud-' + idx + '-pan-v"><span class="mod-ico" id="aud-' + idx + '-pan-m" title="Modulation — click to edit">∿</span></div>'
      + '<div class="field"><label>vol</label><input type="range" min="0" max="100" step="1" id="aud-' + idx + '-vol"><input type="number" class="val numv" min="0" max="100" step="any" id="aud-' + idx + '-vol-v"><span class="mod-ico" id="aud-' + idx + '-vol-m" title="Modulation — click to edit">∿</span></div>'
      + '<div class="field"><label>mod-f</label><input type="range" min="0" max="40" step="0.01" id="aud-' + idx + '-mod"><input type="number" class="val numv" min="0" max="40" step="any" id="aud-' + idx + '-mod-v"><span class="mod-ico" id="aud-' + idx + '-mod-m" title="Modulation — click to edit">∿</span></div>'
      + '<div class="field"><label>wave</label><select class="lockSel" id="aud-' + idx + '-wave" title="Carrier waveform">'
      +   CARRIER_WAVES.map(w => '<option value="' + w[0] + '">' + w[1] + '</option>').join('') + '</select></div>'
      + '<div class="field"><label>lock</label><select class="lockSel" id="aud-' + idx + '-lock" title="Follow another channel\'s frequency changes"></select></div>';
    return div;
}

// Build one row of LED-channel controls. RGB inputs are hidden if
// caps.led_color is false (direct-GPIO backend, no per-pixel colour).
function ctrlRenderLedRow(idx, hasColor) {
    const div = document.createElement('div');
    div.className = 'ctrl-row';
    div.id = 'ctrl-led-' + idx;
    let html =
        '<div class="label"><span class="active-dot" id="led-' + idx + '-dot"></span> L' + (idx+1)
      +   ' <span class="mod-ico" id="led-' + idx + '-pulse" title="Pulse / flicker shaping — click to edit">⚙</span></div>'
      + '<div class="field"><label>freq</label><input type="range" min="0" max="30" step="0.1" id="led-' + idx + '-freq"><input type="number" class="val numv" min="0" max="30" step="any" id="led-' + idx + '-freq-v"><span class="mod-ico" id="led-' + idx + '-freq-m" title="Modulation — click to edit">∿</span></div>'
      + '<div class="field"><label>duty</label><input type="range" min="0" max="100" step="1" id="led-' + idx + '-duty"><input type="number" class="val numv" min="0" max="100" step="any" id="led-' + idx + '-duty-v"><span class="mod-ico" id="led-' + idx + '-duty-m" title="Modulation — click to edit">∿</span></div>'
      + '<div class="field"><label>bright</label><input type="range" min="0" max="100" step="1" id="led-' + idx + '-bright"><input type="number" class="val numv" min="0" max="100" step="any" id="led-' + idx + '-bright-v"><span class="mod-ico" id="led-' + idx + '-bright-m" title="Modulation — click to edit">∿</span></div>';
    if (hasColor) {
        html += '<div class="field"><label>colour</label><input type="color" id="led-' + idx + '-col"></div>';
    }
    html += '<div class="field"><label>lock</label><select class="lockSel" id="led-' + idx + '-lock" title="Mirror another channel\'s values"></select></div>';
    div.innerHTML = html;
    return div;
}

// Wire one slider — pause polling while user is interacting, send
// throttled patch on input, clear interacting flag after lock window.
function ctrlBindSlider(el, key, onChange) {
    const pauseFor = () => {
        ctrlInteracting.add(key);
        clearTimeout(el._ctrlReleaseTimer);
        el._ctrlReleaseTimer = setTimeout(() => ctrlInteracting.delete(key), CTRL_LOCK_AFTER_MS);
    };
    el.addEventListener('mousedown', pauseFor);
    el.addEventListener('touchstart', pauseFor, { passive: true });
    el.addEventListener('input', (ev) => {
        pauseFor();
        onChange(parseFloat(ev.target.value));
    });
    el.addEventListener('change', (ev) => {
        onChange(parseFloat(ev.target.value));
    });
}

function ctrlBindColor(el, key, onChange) {
    const pauseFor = () => {
        ctrlInteracting.add(key);
        clearTimeout(el._ctrlReleaseTimer);
        el._ctrlReleaseTimer = setTimeout(() => ctrlInteracting.delete(key), CTRL_LOCK_AFTER_MS);
    };
    el.addEventListener('mousedown', pauseFor);
    el.addEventListener('input', () => { pauseFor(); onChange(el.value); });
    el.addEventListener('change', () => onChange(el.value));
}

// Wire an editable number input (precise typing, e.g. 50.243). Shares the same
// poll-pause key as its slider so /api/state doesn't overwrite the field while
// typing; mirrors the typed value onto the slider (which clamps to its range).
function ctrlBindNumber(numEl, sliderEl, key, onChange) {
    if (!numEl) return;
    const pauseFor = () => {
        ctrlInteracting.add(key);
        clearTimeout(numEl._ctrlReleaseTimer);
        numEl._ctrlReleaseTimer = setTimeout(() => ctrlInteracting.delete(key), CTRL_LOCK_AFTER_MS);
    };
    numEl.addEventListener('input', () => {
        const v = parseFloat(numEl.value);
        if (isNaN(v)) return;
        pauseFor();
        if (sliderEl) sliderEl.value = v;
        onChange(v);
    });
    numEl.addEventListener('change', () => {
        const v = parseFloat(numEl.value);
        if (!isNaN(v)) onChange(v);
    });
}

// First-paint: fetch capabilities, render channel rows, attach handlers.
export async function ctrlInit() {
    let st;
    try { st = await fetch('/api/state').then(r => r.json()); }
    catch (e) { document.getElementById('ctrl-status').textContent = '(state unavailable)'; return; }
    ctrlState = st;
    ctrlLoadLocks();
    const caps = st.caps || { led_color: false, num_led_ch: 8, num_audio_ch: 16 };
    const audN = Math.min(8, caps.num_audio_ch);

    const audDiv = document.getElementById('ctrl-audio');
    audDiv.innerHTML = '<h3>Audio Channels</h3>';
    // First 8 channels are user-controllable tone channels. Channel 9
    // is the noise channel (rendered separately below). Channels 10-16
    // are reserved/unused and not exposed in the UI.
    for (let i = 0; i < audN; i++) {
        const row = ctrlRenderAudioRow(i);
        audDiv.appendChild(row);
        const idx = i;
        // Freq change: patch this channel, then push the relative target
        // to every follower locked (directly or transitively) to it. Bound to
        // BOTH the slider (coarse) and the number input (precise typing).
        const freqKey = 'aud-' + idx + '-freq';
        const onFreq = (v) => {
            ctrlSendThrottled(freqKey, () => ctrlBuildAudioPatch(idx, 'freq', v));
            ctrlFollowers('aud', idx).forEach(f => {
                const tf = ctrlAudResolvedFreq(f, v, idx);
                ctrlSendThrottled('aud-' + f + '-freq', () => ctrlBuildAudioPatch(f, 'freq', tf));
            });
        };
        ctrlBindSlider(document.getElementById(freqKey), freqKey, onFreq);
        ctrlBindNumber(document.getElementById(freqKey + '-v'), document.getElementById(freqKey), freqKey, onFreq);
        // pan / vol / mod: bind both the slider and its number input.
        const bindAud = (field) => {
            const key = 'aud-' + idx + '-' + field;
            const on = (v) => ctrlSendThrottled(key, () => ctrlBuildAudioPatch(idx, field, v));
            ctrlBindSlider(document.getElementById(key), key, on);
            ctrlBindNumber(document.getElementById(key + '-v'), document.getElementById(key), key, on);
        };
        bindAud('pan'); bindAud('vol'); bindAud('mod');
        // Modulation icon per field → opens the mod popup.
        ['freq', 'pan', 'vol', 'mod'].forEach(field => {
            const ico = document.getElementById('aud-' + idx + '-' + field + '-m');
            if (ico) ico.addEventListener('click', () => ctrlOpenModPopup('aud', idx, field));
        });
        // Carrier waveform dropdown → patch the 8th (wave) field.
        const waveSel = document.getElementById('aud-' + idx + '-wave');
        if (waveSel) waveSel.addEventListener('change', (e) =>
            ctrlPatch(ctrlBuildAudioPatch(idx, 'wave', parseInt(e.target.value, 10) || 0)));
        // Binaural right-ear frequency (number only; applied instantly on the device).
        const frKey = 'aud-' + idx + '-freqr';
        ctrlBindNumber(document.getElementById(frKey + '-v'), null, frKey,
            (v) => ctrlSendThrottled(frKey, () => ctrlBuildAudioPatch(idx, 'freqr', v)));
        // Pulse ⚙ → per-channel isochronic/pulse popup.
        const audPulse = document.getElementById('aud-' + idx + '-pulse');
        if (audPulse) audPulse.addEventListener('click', () => ctrlOpenPulsePopup('aud', idx));
        ctrlPopulateLockSelect('aud', idx, audN);
        if (ctrlLocks.aud[idx] != null) ctrlAudOffset[idx] = ctrlAudFreq(idx) - ctrlAudFreq(ctrlLocks.aud[idx]);
        document.getElementById('aud-' + idx + '-lock').addEventListener('change', (e) => ctrlOnLockChange('aud', idx, e.target));
        ctrlApplyLockUI('aud', idx);
    }

    const noiseDiv = document.getElementById('ctrl-noise');
    noiseDiv.innerHTML = '<h3>Noise (Audio Ch 9)</h3>'
      + '<div class="ctrl-row">'
      +   '<div class="label"><span class="active-dot" id="noise-dot"></span> N</div>'
      +   '<div class="field"><label>type</label><select id="noise-type">'
      +     '<option value="0">Off</option><option value="4">White</option>'
      +     '<option value="5">Pink</option><option value="6">Brown</option></select></div>'
      +   '<div class="field"><label>vol</label><input type="range" min="0" max="100" step="1" id="noise-vol"><input type="number" class="val numv" min="0" max="100" step="any" id="noise-vol-v"></div>'
      + '</div>';
    // Noise type change: send a patch on ch9 with wave_type set
    document.getElementById('noise-type').addEventListener('change', (e) => {
        const wt = parseInt(e.target.value);
        const vol = parseFloat(document.getElementById('noise-vol').value || '0');
        // wave_type lives at position [freq_r] [wave_type] in the audio line.
        // Use freq_r=0 placeholder. If wt=0 (Off), set volume to 0.
        const v = (wt === 0) ? 0 : vol;
        const line = 'A ' + CTRL_ANIMATE_MS + ' 0 0 ' + v.toFixed(1) + ' 0 9 0 ' + wt;
        ctrlPatch(line);
    });
    const onNoiseVol = (v) => {
        const wt = parseInt(document.getElementById('noise-type').value || '0');
        if (wt === 0) return;  // type Off — vol has no effect
        const line = 'A ' + CTRL_ANIMATE_MS + ' 0 0 ' + v.toFixed(1) + ' 0 9 0 ' + wt;
        ctrlSendThrottled('noise-vol', () => line);
    };
    ctrlBindSlider(document.getElementById('noise-vol'), 'noise-vol', onNoiseVol);
    ctrlBindNumber(document.getElementById('noise-vol-v'), document.getElementById('noise-vol'), 'noise-vol', onNoiseVol);

    const ledDiv = document.getElementById('ctrl-led');
    ledDiv.innerHTML = '<h3>LED Channels' + (caps.led_color ? '' : ' (no colour — direct GPIO backend)') + '</h3>';
    for (let i = 0; i < caps.num_led_ch; i++) {
        const row = ctrlRenderLedRow(i, caps.led_color);
        ledDiv.appendChild(row);
        const idx = i;
        // freq / duty / bright: bind both the slider and its number input.
        const bindLed = (field) => {
            const key = 'led-' + idx + '-' + field;
            const on = (v) => ctrlSendThrottled(key, () => ctrlBuildLedPatch(idx, field, v));
            ctrlBindSlider(document.getElementById(key), key, on);
            ctrlBindNumber(document.getElementById(key + '-v'), document.getElementById(key), key, on);
        };
        bindLed('freq'); bindLed('duty'); bindLed('bright');
        // Modulation icon per field → opens the mod popup.
        ['freq', 'duty', 'bright'].forEach(field => {
            const ico = document.getElementById('led-' + idx + '-' + field + '-m');
            if (ico) ico.addEventListener('click', () => ctrlOpenModPopup('led', idx, field));
        });
        // Pulse ⚙ → per-channel flicker/pulse popup.
        const ledPulse = document.getElementById('led-' + idx + '-pulse');
        if (ledPulse) ledPulse.addEventListener('click', () => ctrlOpenPulsePopup('led', idx));
        if (caps.led_color) {
            ctrlBindColor(document.getElementById('led-' + idx + '-col'), 'led-' + idx + '-col', (hex) => {
                const r = parseInt(hex.substr(1, 2), 16);
                const g = parseInt(hex.substr(3, 2), 16);
                const b = parseInt(hex.substr(5, 2), 16);
                // Send R, G, B as one patch (all three change together).
                const ch = ctrlState && ctrlState.led ? ctrlState.led[idx] : { freq: 0, duty: 50, bright: 100 };
                const line = CTRL_ANIMATE_MS + ' ' + ch.freq.toFixed(2) + ' ' + Math.round(ch.duty) + ' ' + Math.round(ch.bright) + ' ' + r + ' ' + g + ' ' + b + ' ' + ctrlLedMask(idx);
                ctrlPatch(line);
            });
        }
        ctrlPopulateLockSelect('led', idx, caps.num_led_ch);
        document.getElementById('led-' + idx + '-lock').addEventListener('change', (e) => ctrlOnLockChange('led', idx, e.target));
        ctrlApplyLockUI('led', idx);
    }

    // Now that DOM is built, run the first poll to populate slider positions.
    ctrlPoll();
    setInterval(ctrlPoll, CTRL_POLL_MS);
}

function ctrlSetSliderIfNotInteracting(id, key, value, fmt) {
    if (ctrlInteracting.has(key)) return;
    const el = document.getElementById(id);
    if (!el) return;
    el.value = value;
    const v = document.getElementById(id + '-v');
    if (v) {
        const txt = fmt ? fmt(value) : value;
        // The "-v" element may be a read-only <span> or an editable <input>.
        if (v.tagName === 'INPUT') v.value = txt; else v.textContent = txt;
    }
}

// Poll /api/state and update every slider not currently being dragged.
async function ctrlPoll() {
    let st;
    try { st = await fetch('/api/state').then(r => r.json()); }
    catch (e) { return; }
    ctrlState = st;
    const stat = document.getElementById('ctrl-status');
    const tlRunning = !!(st.timeline && st.timeline.running);
    if (tlRunning) {
        stat.textContent = 'timeline @ ' + Math.round(st.timeline.position_ms / 1000) + 's';
    } else {
        stat.textContent = 'idle';
    }
    // Fire 'sessionended' on the running→stopped edge so config.js can fetch the
    // report at the moment the session actually ends (natural end or early STOP)
    // instead of guessing from the parsed duration.
    if (ctrlPrevTlRunning && !tlRunning) {
        window.dispatchEvent(new CustomEvent('sessionended',
            { detail: { position_ms: (st.timeline && st.timeline.position_ms) || 0 } }));
    }
    ctrlPrevTlRunning = tlRunning;
    // Audio sliders A1..A8 correspond to state.audio[1..8] — the .ledc
    // convention is 1-indexed and channel 0 is unused, so we offset by
    // +1 when reading state. Slider DOM IDs stay 0..7 internally; only
    // the state lookup is offset.
    for (let i = 0; i < 8 && (i + 1) < (st.audio || []).length; i++) {
        const a = st.audio[i + 1];
        ctrlSetSliderIfNotInteracting('aud-' + i + '-freq', 'aud-' + i + '-freq', a.freq,    v => v.toFixed(3));
        ctrlSetSliderIfNotInteracting('aud-' + i + '-pan',  'aud-' + i + '-pan',  a.pan,     v => Math.round(v));
        ctrlSetSliderIfNotInteracting('aud-' + i + '-vol',  'aud-' + i + '-vol',  a.vol,     v => Math.round(v));
        ctrlSetSliderIfNotInteracting('aud-' + i + '-mod',  'aud-' + i + '-mod',  a.mod,     v => v.toFixed(2));
        const dot = document.getElementById('aud-' + i + '-dot');
        if (dot) dot.classList.toggle('on', !!a.active);
        const row = document.getElementById('ctrl-aud-' + i);
        if (row) row.classList.toggle('inactive', !a.active);
        // Light the ∿ icon when that field is being modulated.
        ['freq', 'pan', 'vol', 'mod'].forEach(f => {
            const ico = document.getElementById('aud-' + i + '-' + f + '-m');
            if (ico) ico.classList.toggle('active', !!(a.modf && a.modf[f]));
        });
        // Reflect binaural R-freq + carrier wave (skip while the user is editing).
        if (!ctrlInteracting.has('aud-' + i + '-freqr')) {
            const frEl = document.getElementById('aud-' + i + '-freqr-v');
            if (frEl && document.activeElement !== frEl) frEl.value = (a.freq_r || 0).toFixed(3);
        }
        const wSel = document.getElementById('aud-' + i + '-wave');
        if (wSel && document.activeElement !== wSel) wSel.value = String(a.wave == null ? 0 : a.wave);
    }
    // Noise channel — .ledc channel 9 → state.audio[9].
    if (st.audio && st.audio.length > 9) {
        const n = st.audio[9];
        const dot = document.getElementById('noise-dot');
        if (dot) dot.classList.toggle('on', !!n.active && n.vol > 0);
        if (!ctrlInteracting.has('noise-vol')) {
            const vEl = document.getElementById('noise-vol');
            if (vEl) { vEl.value = n.vol; const vv = document.getElementById('noise-vol-v'); if (vv) vv.value = Math.round(n.vol); }
        }
        const tEl = document.getElementById('noise-type');
        if (tEl && !ctrlInteracting.has('noise-type')) {
            // Map current wave back to dropdown — only update if active
            tEl.value = (n.active && n.vol > 0) ? String(n.wave) : '0';
        }
    }
    for (let i = 0; i < (st.led || []).length; i++) {
        const l = st.led[i];
        ctrlSetSliderIfNotInteracting('led-' + i + '-freq',   'led-' + i + '-freq',   l.freq,   v => v.toFixed(2));
        ctrlSetSliderIfNotInteracting('led-' + i + '-duty',   'led-' + i + '-duty',   l.duty,   v => Math.round(v));
        ctrlSetSliderIfNotInteracting('led-' + i + '-bright', 'led-' + i + '-bright', l.bright, v => Math.round(v));
        const cEl = document.getElementById('led-' + i + '-col');
        if (cEl && !ctrlInteracting.has('led-' + i + '-col')) {
            const hex = '#' + l.r.toString(16).padStart(2,'0') + l.g.toString(16).padStart(2,'0') + l.b.toString(16).padStart(2,'0');
            cEl.value = hex;
        }
        const dot = document.getElementById('led-' + i + '-dot');
        if (dot) dot.classList.toggle('on', !!l.active);
        const row = document.getElementById('ctrl-led-' + i);
        if (row) row.classList.toggle('inactive', !l.active);
        // Light the ∿ icon when that field is being modulated.
        ['freq', 'duty', 'bright'].forEach(f => {
            const ico = document.getElementById('led-' + i + '-' + f + '-m');
            if (ico) ico.classList.toggle('active', !!(l.mod && l.mod[f]));
        });
    }
}
