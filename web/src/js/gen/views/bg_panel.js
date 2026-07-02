// Background-audio panel (bg_browser_push_plan.md, Phase 3 + 3.5).
//
// A section under the Generator page that lets the user:
//   - GENERATE ambient BG (white/pink/brown noise, drone) in the browser,
//   - LOAD any browser-decodable audio file from disk,
//   - store clips in the IndexedDB library (bgstore) and DOWNLOAD them,
//   - SELECT a clip as the session's BG (writes a `BG push://<name>` row so the
//     .ledc round-trips) and PUSH it to the device (/api/bg-stream),
//   - BOUNCE the whole session's entrainment audio to one WAV (opt-in button),
//     then download / save-to-library / push it.
//
// Everything the device receives is conformed to 44100/16/stereo WAV by
// bgaudio.js, so the firmware reuses its existing WAV path unchanged.

import { showMessage } from '../../util.js';
import { bg, bgRow } from '../model.js';
import {
    generateNoise, generateDrone, decodeFile, conform,
    audioBufferToWav16, wavBlob,
} from '../bgaudio.js';
import * as store from '../bgstore.js';
import { pushBg, stopBg } from '../transport.js';
import { bounceSessionToWav } from '../synth.js';
import { encodeWav16 } from '../bgaudio.js';

export function initBgPanel(ctx) {
    const root = document.getElementById('genBgPanel');
    if (!root) return { refresh() {} };

    // Client-driven loop state: when looping, re-push the same clip after each
    // POST resolves. `token` guards against a stale loop continuing after Stop
    // or after a different clip starts.
    let loop = { on: false, name: null, token: 0 };

    root.innerHTML = `
      <h2>Background audio</h2>
      <div class="bg-help">Generate or load ambient audio in your browser and stream it to the device
        (works even in SoftAP mode — no internet needed). Or bounce the whole session to a single WAV.</div>

      <div class="bg-controls">
        <div class="bg-group">
          <strong>Generate</strong>
          <select id="bgGenKind">
            <option value="white">White noise</option>
            <option value="pink">Pink noise</option>
            <option value="brown">Brown noise</option>
            <option value="drone">Drone</option>
          </select>
          <label>dur <input id="bgGenDur" type="number" value="60" min="1" max="1800" style="width:56px"> s</label>
          <label>gain <input id="bgGenGain" type="number" value="50" min="0" max="100" style="width:52px"> %</label>
          <input id="bgGenName" type="text" placeholder="clip name" style="width:120px">
          <button id="bgGenBtn">Generate → library</button>
        </div>

        <div class="bg-group">
          <strong>Load file</strong>
          <input id="bgFileInput" type="file" accept="audio/*" style="display:none">
          <button id="bgFileBtn">Choose file…</button>
        </div>

        <div class="bg-group">
          <strong>Playback</strong>
          <label>pan <input id="bgPan" type="number" value="0" min="-100" max="100" style="width:56px"></label>
          <label>vol <input id="bgLoud" type="number" value="50" min="0" max="100" style="width:52px"></label>
          <label><input id="bgLoop" type="checkbox"> loop</label>
          <button id="bgStopBtn" style="background:#dc3545;color:#fff">■ Stop BG</button>
        </div>

        <div class="bg-group">
          <strong>Whole session</strong>
          <button id="bgBounceBtn" title="Render the entire session's audio to one WAV (opt-in)">Bounce session → WAV</button>
          <span class="bg-help">Renders binaural/iso/sweeps/noise for the whole timeline.</span>
        </div>
      </div>

      <div class="bg-library">
        <strong>Library</strong>
        <div id="bgList" class="bg-list"></div>
      </div>
    `;

    const $ = (id) => root.querySelector('#' + id);

    // ---- Library rendering ------------------------------------------------
    async function renderList() {
        const listEl = $('bgList');
        listEl.textContent = 'Loading…';
        let items = [];
        try { items = await store.list(); }
        catch (e) { listEl.textContent = 'Library unavailable: ' + e.message; return; }
        if (!items.length) { listEl.textContent = 'No clips yet — generate, load, or bounce one.'; return; }
        listEl.innerHTML = '';
        for (const m of items) {
            const row = document.createElement('div');
            row.className = 'bg-item';
            const secs = Math.round((m.durationMs || 0) / 1000);
            const kb = Math.round((m.bytes || 0) / 1024);
            const meta = document.createElement('span');
            meta.className = 'bg-item-meta';
            meta.textContent = `${m.name}  ·  ${m.sourceKind}  ·  ${secs}s  ·  ${kb} KB`;
            row.appendChild(meta);
            const btns = document.createElement('span');
            btns.className = 'bg-item-btns';
            mkBtn(btns, 'Set as BG', () => setAsBg(m.name));
            mkBtn(btns, '▶ Push', () => playClip(m.name));
            mkBtn(btns, 'Download', () => downloadClip(m.name));
            mkBtn(btns, 'Delete', () => delClip(m.name));
            row.appendChild(btns);
            listEl.appendChild(row);
        }
    }
    function mkBtn(parent, label, fn) {
        const b = document.createElement('button');
        b.type = 'button'; b.className = 'bg-item-btn'; b.textContent = label;
        b.addEventListener('click', fn);
        parent.appendChild(b);
    }

    // ---- Save a rendered AudioBuffer / WAV to the library ------------------
    async function saveClip(name, wavU8, durationMs, sourceKind) {
        if (!name || !name.trim()) { showMessage('Give the clip a name', 'error'); return false; }
        try {
            await store.put({ name: name.trim(), wavBlob: wavBlob(wavU8), durationMs, sourceKind });
            await renderList();
            showMessage('Saved "' + name.trim() + '" to library', 'success');
            return true;
        } catch (e) { showMessage('Save failed: ' + e.message, 'error'); return false; }
    }

    // ---- Generate ---------------------------------------------------------
    async function onGenerate() {
        const kind = $('bgGenKind').value;
        const durationMs = Math.max(1, parseFloat($('bgGenDur').value) || 60) * 1000;
        const gain = Math.max(0, Math.min(100, parseFloat($('bgGenGain').value) || 50)) / 100;
        let name = $('bgGenName').value.trim() || (kind + '-' + Math.round(durationMs / 1000) + 's');
        showMessage('Generating ' + kind + '…', 'info');
        try {
            const audioBuf = (kind === 'drone')
                ? await generateDrone({ durationMs, gain })
                : await generateNoise(kind, { durationMs, gain, stereo: true });
            const wav = audioBufferToWav16(audioBuf);
            await saveClip(name, wav, durationMs, kind === 'drone' ? 'drone' : 'noise');
        } catch (e) { showMessage('Generate failed: ' + e.message, 'error'); }
    }

    // ---- Load file --------------------------------------------------------
    async function onFile(ev) {
        const file = ev.target.files && ev.target.files[0];
        if (!file) return;
        showMessage('Decoding ' + file.name + '…', 'info');
        try {
            const audioBuf = await decodeFile(file);
            const wav = audioBufferToWav16(audioBuf);
            const name = file.name.replace(/\.[^.]+$/, '');
            await saveClip(name, wav, Math.round(audioBuf.duration * 1000), 'file');
        } catch (e) { showMessage('Decode failed: ' + e.message, 'error'); }
        ev.target.value = '';
    }

    // ---- Select as the session BG (writes push://<name> row) --------------
    function setAsBg(name) {
        const pan = clampNum($('bgPan').value, -100, 100, 0);
        const loud = clampNum($('bgLoud').value, 0, 100, 50);
        const url = 'push://' + name;
        const d = ctx.getDoc();
        const desc = bg(url, pan, loud);
        // Update an existing bg row if present, else append one.
        let found = false;
        for (const r of d.rows) if (r.kind === 'bg') { r.bg = desc; found = true; }
        if (!found) d.rows.push(bgRow(desc));
        d.bg = desc;
        ctx.setDoc(d);
        showMessage('Session BG set to "' + name + '" (push://). Push it or Play the session.', 'success');
    }

    // ---- Push a clip to the device (with optional loop) -------------------
    async function playClip(name) {
        const rec = await store.get(name);
        if (!rec) { showMessage('Clip not found: ' + name, 'error'); return; }
        const pan = clampNum($('bgPan').value, -100, 100, 0);
        const loud = clampNum($('bgLoud').value, 0, 100, 50);
        loop.on = $('bgLoop').checked;
        loop.name = name;
        const myToken = ++loop.token;
        showMessage('Pushing "' + name + '" to device…', 'info');
        const once = () => pushBg(rec.wavBlob, { pan, loudness: loud })
            .then(() => {
                if (loop.on && loop.token === myToken) return once();  // re-POST for loop
                if (!loop.on) showMessage('BG "' + name + '" finished', 'info');
            })
            .catch(err => { if (loop.token === myToken) showMessage('BG push error: ' + err, 'error'); });
        once();
    }

    async function downloadClip(name) {
        const rec = await store.get(name);
        if (!rec) { showMessage('Clip not found: ' + name, 'error'); return; }
        triggerDownload(rec.wavBlob, name + '.wav');
    }

    async function delClip(name) {
        if (!confirm('Delete clip "' + name + '"?')) return;
        try { await store.remove(name); await renderList(); showMessage('Deleted "' + name + '"', 'info'); }
        catch (e) { showMessage('Delete failed: ' + e.message, 'error'); }
    }

    function onStopBg() {
        loop.on = false; loop.token++;   // cancel any loop continuation
        stopBg().then(() => showMessage('BG stopped', 'info'))
                .catch(err => showMessage('Stop error: ' + err, 'error'));
    }

    // ---- Bounce whole session to a single WAV (opt-in) --------------------
    async function onBounce() {
        const d = ctx.getDoc();
        const hasAudio = (d.rows || []).some(r => r.kind === 'audio');
        if (!hasAudio) { showMessage('No audio channels in the session to bounce', 'error'); return; }
        showMessage('Bouncing session… (this runs entirely in the browser)', 'info');
        // Yield once so the status paints before the synchronous render.
        await new Promise(r => setTimeout(r, 20));
        let result;
        try {
            result = bounceSessionToWav(d, { tailMs: 2000 }, encodeWav16);
        } catch (e) { showMessage('Bounce failed: ' + e.message, 'error'); return; }
        const name = 'session-' + Math.round(result.durationMs / 1000) + 's';
        const blob = wavBlob(result.wav);
        // Offer all three outputs.
        await store.put({ name, wavBlob: blob, durationMs: result.durationMs, sourceKind: 'bounce' })
            .then(() => renderList()).catch(() => {});
        triggerDownload(blob, name + '.wav');
        const pan = clampNum($('bgPan').value, -100, 100, 0);
        const loud = clampNum($('bgLoud').value, 0, 100, 50);
        loop.on = false; loop.token++;
        pushBg(blob, { pan, loudness: loud })
            .then(() => showMessage('Bounced ' + Math.round(result.durationMs / 1000) + 's → saved, downloaded, pushed', 'success'))
            .catch(err => showMessage('Bounced + saved + downloaded (push failed: ' + err + ')', 'info'));
    }

    // ---- helpers ----------------------------------------------------------
    function clampNum(v, lo, hi, dflt) {
        const n = parseFloat(v);
        if (!Number.isFinite(n)) return dflt;
        return Math.max(lo, Math.min(hi, n));
    }
    function triggerDownload(blob, filename) {
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
    }

    // ---- wiring -----------------------------------------------------------
    $('bgGenBtn').addEventListener('click', onGenerate);
    $('bgFileBtn').addEventListener('click', () => $('bgFileInput').click());
    $('bgFileInput').addEventListener('change', onFile);
    $('bgStopBtn').addEventListener('click', onStopBg);
    $('bgBounceBtn').addEventListener('click', onBounce);

    // ---- Push the session's push:// BG (called from Play) -----------------
    // The device treats a `BG push://<name>` line as a no-op pull, so after the
    // timeline starts the browser must stream the actual clip here. Returns a
    // promise (fire-and-forget from the caller) or null if there's no push BG.
    function pushForDoc(d) {
        const desc = d && d.bg;
        if (!desc || !desc.url || desc.url.indexOf('push://') !== 0) return null;
        const name = desc.url.slice('push://'.length);
        return store.get(name).then(rec => {
            if (!rec) {
                showMessage('Session BG "' + name + '" is not in this browser’s library — generate/load it, then Set as BG', 'error');
                return;
            }
            loop.on = false; loop.token++;   // a session push plays once
            return pushBg(rec.wavBlob, { pan: desc.pan, loudness: desc.loudness })
                .catch(err => showMessage('Session BG push error: ' + err, 'error'));
        });
    }

    renderList();
    return {
        refresh() { /* library is independent of the doc model */ },
        pushForDoc,
    };
}
