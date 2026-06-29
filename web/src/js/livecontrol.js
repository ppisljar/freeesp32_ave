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
// Enable/disable a channel's controls to reflect its lock state.
function ctrlApplyLockUI(domain, idx) {
    const locked = ctrlLocks[domain][idx] != null;
    if (domain === 'aud') {
        const f = document.getElementById('aud-' + idx + '-freq'); if (f) f.disabled = locked;
        const row = document.getElementById('ctrl-aud-' + idx); if (row) row.classList.toggle('locked', locked);
    } else {
        ['freq', 'duty', 'bright'].forEach(fld => { const e = document.getElementById('led-' + idx + '-' + fld); if (e) e.disabled = locked; });
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
    // A time freq pan vol mod channel
    return 'A ' + CTRL_ANIMATE_MS + ' ' + f.toFixed(2) + ' ' + p.toFixed(1)
         + ' ' + v.toFixed(1) + ' ' + m.toFixed(2) + ' ' + (chIdx + 1);
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
        '<div class="label"><span class="active-dot" id="aud-' + idx + '-dot"></span> A' + (idx+1) + '</div>'
      + '<div class="field"><label>freq</label><input type="range" min="0" max="2000" step="0.1" id="aud-' + idx + '-freq"><span class="val" id="aud-' + idx + '-freq-v"></span><span class="mod-badge" id="aud-' + idx + '-freq-m">mod</span></div>'
      + '<div class="field"><label>pan</label><input type="range" min="-100" max="100" step="1" id="aud-' + idx + '-pan"><span class="val" id="aud-' + idx + '-pan-v"></span><span class="mod-badge" id="aud-' + idx + '-pan-m">mod</span></div>'
      + '<div class="field"><label>vol</label><input type="range" min="0" max="100" step="1" id="aud-' + idx + '-vol"><span class="val" id="aud-' + idx + '-vol-v"></span><span class="mod-badge" id="aud-' + idx + '-vol-m">mod</span></div>'
      + '<div class="field"><label>mod-f</label><input type="range" min="0" max="40" step="0.01" id="aud-' + idx + '-mod"><span class="val" id="aud-' + idx + '-mod-v"></span><span class="mod-badge" id="aud-' + idx + '-mod-m">mod</span></div>'
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
        '<div class="label"><span class="active-dot" id="led-' + idx + '-dot"></span> L' + (idx+1) + '</div>'
      + '<div class="field"><label>freq</label><input type="range" min="0" max="30" step="0.1" id="led-' + idx + '-freq"><span class="val" id="led-' + idx + '-freq-v"></span></div>'
      + '<div class="field"><label>duty</label><input type="range" min="0" max="100" step="1" id="led-' + idx + '-duty"><span class="val" id="led-' + idx + '-duty-v"></span></div>'
      + '<div class="field"><label>bright</label><input type="range" min="0" max="100" step="1" id="led-' + idx + '-bright"><span class="val" id="led-' + idx + '-bright-v"></span></div>';
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
        // to every follower locked (directly or transitively) to it.
        ctrlBindSlider(document.getElementById('aud-' + idx + '-freq'), 'aud-' + idx + '-freq', (v) => {
            ctrlSendThrottled('aud-' + idx + '-freq', () => ctrlBuildAudioPatch(idx, 'freq', v));
            ctrlFollowers('aud', idx).forEach(f => {
                const tf = ctrlAudResolvedFreq(f, v, idx);
                ctrlSendThrottled('aud-' + f + '-freq', () => ctrlBuildAudioPatch(f, 'freq', tf));
            });
        });
        ctrlBindSlider(document.getElementById('aud-' + idx + '-pan'),  'aud-' + idx + '-pan',  (v) => ctrlSendThrottled('aud-' + idx + '-pan',  () => ctrlBuildAudioPatch(idx, 'pan',  v)));
        ctrlBindSlider(document.getElementById('aud-' + idx + '-vol'),  'aud-' + idx + '-vol',  (v) => ctrlSendThrottled('aud-' + idx + '-vol',  () => ctrlBuildAudioPatch(idx, 'vol',  v)));
        ctrlBindSlider(document.getElementById('aud-' + idx + '-mod'),  'aud-' + idx + '-mod',  (v) => ctrlSendThrottled('aud-' + idx + '-mod',  () => ctrlBuildAudioPatch(idx, 'mod',  v)));
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
      +   '<div class="field"><label>vol</label><input type="range" min="0" max="100" step="1" id="noise-vol"><span class="val" id="noise-vol-v"></span></div>'
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
    ctrlBindSlider(document.getElementById('noise-vol'), 'noise-vol', (v) => {
        const wt = parseInt(document.getElementById('noise-type').value || '0');
        if (wt === 0) return;  // type Off — slider has no effect
        const line = 'A ' + CTRL_ANIMATE_MS + ' 0 0 ' + v.toFixed(1) + ' 0 9 0 ' + wt;
        ctrlSendThrottled('noise-vol', () => line);
    });

    const ledDiv = document.getElementById('ctrl-led');
    ledDiv.innerHTML = '<h3>LED Channels' + (caps.led_color ? '' : ' (no colour — direct GPIO backend)') + '</h3>';
    for (let i = 0; i < caps.num_led_ch; i++) {
        const row = ctrlRenderLedRow(i, caps.led_color);
        ledDiv.appendChild(row);
        const idx = i;
        ctrlBindSlider(document.getElementById('led-' + idx + '-freq'),   'led-' + idx + '-freq',   (v) => ctrlSendThrottled('led-' + idx + '-freq',   () => ctrlBuildLedPatch(idx, 'freq',   v)));
        ctrlBindSlider(document.getElementById('led-' + idx + '-duty'),   'led-' + idx + '-duty',   (v) => ctrlSendThrottled('led-' + idx + '-duty',   () => ctrlBuildLedPatch(idx, 'duty',   v)));
        ctrlBindSlider(document.getElementById('led-' + idx + '-bright'), 'led-' + idx + '-bright', (v) => ctrlSendThrottled('led-' + idx + '-bright', () => ctrlBuildLedPatch(idx, 'bright', v)));
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
    if (v) v.textContent = fmt ? fmt(value) : value;
}

// Poll /api/state and update every slider not currently being dragged.
async function ctrlPoll() {
    let st;
    try { st = await fetch('/api/state').then(r => r.json()); }
    catch (e) { return; }
    ctrlState = st;
    const stat = document.getElementById('ctrl-status');
    if (st.timeline && st.timeline.running) {
        stat.textContent = 'timeline @ ' + Math.round(st.timeline.position_ms / 1000) + 's';
    } else {
        stat.textContent = 'idle';
    }
    // Audio sliders A1..A8 correspond to state.audio[1..8] — the .ledc
    // convention is 1-indexed and channel 0 is unused, so we offset by
    // +1 when reading state. Slider DOM IDs stay 0..7 internally; only
    // the state lookup is offset.
    for (let i = 0; i < 8 && (i + 1) < (st.audio || []).length; i++) {
        const a = st.audio[i + 1];
        ctrlSetSliderIfNotInteracting('aud-' + i + '-freq', 'aud-' + i + '-freq', a.freq,    v => v.toFixed(1));
        ctrlSetSliderIfNotInteracting('aud-' + i + '-pan',  'aud-' + i + '-pan',  a.pan,     v => Math.round(v));
        ctrlSetSliderIfNotInteracting('aud-' + i + '-vol',  'aud-' + i + '-vol',  a.vol,     v => Math.round(v));
        ctrlSetSliderIfNotInteracting('aud-' + i + '-mod',  'aud-' + i + '-mod',  a.mod,     v => v.toFixed(2));
        const dot = document.getElementById('aud-' + i + '-dot');
        if (dot) dot.classList.toggle('on', !!a.active);
        const row = document.getElementById('ctrl-aud-' + i);
        if (row) row.classList.toggle('inactive', !a.active);
        ['freq','pan','vol','mod'].forEach(f => {
            const b = document.getElementById('aud-' + i + '-' + f + '-m');
            if (b) b.classList.toggle('active', !!(a.modf && a.modf[f]));
        });
    }
    // Noise channel — .ledc channel 9 → state.audio[9].
    if (st.audio && st.audio.length > 9) {
        const n = st.audio[9];
        const dot = document.getElementById('noise-dot');
        if (dot) dot.classList.toggle('on', !!n.active && n.vol > 0);
        if (!ctrlInteracting.has('noise-vol')) {
            const vEl = document.getElementById('noise-vol');
            if (vEl) { vEl.value = n.vol; const vv = document.getElementById('noise-vol-v'); if (vv) vv.textContent = Math.round(n.vol); }
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
    }
}
