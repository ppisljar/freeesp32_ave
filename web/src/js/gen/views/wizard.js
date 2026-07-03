// Wizard view (Phase 5) — the lowest-floor, mobile-first guided authoring path.
//
// A projection of the single shared `doc` (same ctx { getDoc, setDoc } contract
// as text/table/lane). While active, the `session` model is the working source
// of truth; every edit recompiles to the flat doc via wizard_compile.compileSession
// (which also embeds the `# @ave-wizard` metadata for lossless reopen). Incoming
// model changes from other views are re-imported via sessionFromDoc.
//
// UI is progressive disclosure (Tier 0 chip summary -> Tier 1 preset + base
// sliders -> Tier 2 Transition / Pulse -> Tier 3 Advanced). Desktop >=1024px
// gets a left mini-map; the read-only .led preview lives in the shared pane.

import { WAVE_TYPES } from '../model.js';
import { field, ramp, mod, fieldHasRamp, fieldHasMod } from '../field.js';
import { compileSession, sessionFromDoc, LED_REGION_MASKS } from './wizard_compile.js';
import { patchLine } from '../transport.js';
import {
    BRAINWAVE_PRESETS, COLOR_PRESETS, NOISE_PRESETS,
    harmonicCarriers, breathMod, rotatingPanMod,
} from '../macros.js';
import {
    parsePulseCell, formatPulseCell, parsePulseEnv, formatPulseEnv,
    parsePulseJitter, formatPulseJitter,
} from '../pulse.js';

let uid = 0;
function newId(p) { return (p || 'id') + (++uid) + '_' + Math.random().toString(36).slice(2, 6); }

function emptySession() {
    return { name: 'New session', version: 1, bg: null, segments: [] };
}

function newSegment() {
    return { id: newId('seg'), name: 'Segment', duration_ms: 60000, layers: [] };
}

function newLayer(kind) {
    if (kind === 'light') {
        return {
            id: newId('led'), kind: 'light', channelMask: LED_REGION_MASKS.all,
            fields: {
                freq: field(8), duty: field(50), bright: field(50),
                r: field(0), g: field(64), b: field(255),
            },
        };
    }
    if (kind === 'binaural') {
        return {
            id: newId('bin'), kind: 'binaural', stereo: false, channel: undefined, channelR: undefined,
            wave_type: null,
            fields: { freq: field(200), beat: field(10), pan: field(0), volume: field(50), mod: field(0) },
        };
    }
    if (kind === 'noise') {
        return {
            id: newId('noi'), kind: 'noise', channel: undefined, wave_type: NOISE_PRESETS.rain.wave,
            fields: { freq: field(0), pan: field(0), volume: field(40), mod: field(0) },
        };
    }
    // tone
    return {
        id: newId('ton'), kind: 'tone', channel: undefined, wave_type: null,
        fields: { freq: field(220), pan: field(0), volume: field(50), mod: field(0) },
    };
}

export function initWizardView(ctx) {
    const root = document.getElementById('genWizardView');
    if (!root) return { refresh() {}, show() {}, hide() {} };

    let session = emptySession();
    let applyingEdit = false;
    let visible = false;
    let importedFlag = false;

    root.innerHTML = '';

    const banner = document.createElement('div');
    banner.className = 'gen-wiz-banner';
    banner.style.display = 'none';
    root.appendChild(banner);

    const layoutEl = document.createElement('div');
    layoutEl.className = 'gen-wiz-layout';
    const minimap = document.createElement('div');
    minimap.className = 'gen-wiz-minimap';
    const main = document.createElement('div');
    main.className = 'gen-wiz-main';
    layoutEl.appendChild(minimap);
    layoutEl.appendChild(main);
    root.appendChild(layoutEl);

    const warnStrip = document.createElement('div');
    warnStrip.className = 'gen-wiz-warn';
    warnStrip.style.display = 'none';
    root.appendChild(warnStrip);

    // ---- model <-> doc ----------------------------------------------------
    function importFromDoc() {
        const res = sessionFromDoc(ctx.getDoc());
        session = res.session || emptySession();
        if (!session.segments) session.segments = [];
        importedFlag = !!res.imported;
    }

    function commit() {
        const { doc, warnings, rowCount } = compileSession(session, { withMeta: true });
        renderWarnings(warnings, rowCount);
        applyingEdit = true;
        try {
            const d = ctx.getDoc();
            d.rows = doc.rows;
            d.bg = doc.bg;
            ctx.setDoc(d);
        } finally { applyingEdit = false; }
    }

    function renderWarnings(warnings, rowCount) {
        warnStrip.innerHTML = '';
        const msgs = (warnings || []).slice();
        if (!msgs.length) { warnStrip.style.display = 'none'; return; }
        warnStrip.style.display = '';
        for (const m of msgs) {
            const d = document.createElement('div');
            d.className = 'gen-wiz-warn-item';
            d.textContent = '⚠ ' + m;
            warnStrip.appendChild(d);
        }
    }

    function changed() { commit(); renderMain(); renderMinimap(); }
    function structureChanged() { commit(); render(); }

    // ---- small builders ---------------------------------------------------
    function el(tag, cls, text) {
        const e = document.createElement(tag);
        if (cls) e.className = cls;
        if (text !== undefined) e.textContent = text;
        return e;
    }
    function button(label, cls, fn) {
        const b = el('button', cls, label);
        b.type = 'button';
        b.addEventListener('click', fn);
        return b;
    }

    // A labelled value slider+number bound to a Field's .value.
    function sliderRow(label, f, lo, hi, step, onLive) {
        const wrap = el('div', 'gen-wiz-field');
        wrap.appendChild(el('span', 'gen-wiz-flabel', label));
        const range = el('input');
        range.type = 'range'; range.min = lo; range.max = hi; range.step = step || 1;
        range.value = f.value;
        const numIn = el('input', 'gen-wiz-num');
        numIn.type = 'number'; numIn.min = lo; numIn.max = hi; numIn.step = step || 1;
        numIn.value = f.value;
        const sync = (v) => { f.value = v; range.value = v; numIn.value = v; commit(); };
        range.addEventListener('input', () => sync(parseFloat(range.value)));
        numIn.addEventListener('input', () => sync(parseFloat(numIn.value)));
        wrap.appendChild(range);
        wrap.appendChild(numIn);
        if (onLive) {
            wrap.appendChild(button('⚡', 'gen-wiz-live', () => onLive(f.value)));
        }
        // Tier 0 chip badges (ramp / mod present).
        const badge = el('span', 'gen-wiz-badge');
        if (fieldHasRamp(f)) badge.textContent += ' ↗';
        if (fieldHasMod(f)) badge.textContent += ' ∿';
        wrap.appendChild(badge);
        return wrap;
    }

    // Tier 2 — Transition (ramp) + Pulse (mod) editors for one Field.
    function dynamicsEditor(f) {
        const det = el('details', 'gen-wiz-dyn');
        det.appendChild(el('summary', null, '▸ Transition / Pulse'));

        // Transition.
        const tr = el('div', 'gen-wiz-subrow');
        tr.appendChild(el('span', 'gen-wiz-flabel', 'Transition'));
        const trSel = el('select');
        [['none', 'Off'], ['linear', 'Steady (>)'], ['quadratic', 'Smooth (*)']]
            .forEach(([v, t]) => { const o = el('option', null, t); o.value = v; trSel.appendChild(o); });
        trSel.value = f.ramp ? f.ramp.shape : 'none';
        const winIn = el('input', 'gen-wiz-num');
        winIn.type = 'number'; winIn.min = 0; winIn.placeholder = 'first s (blank=whole)';
        winIn.title = 'Ramp window: blank = whole segment, else first N seconds';
        if (f.ramp && f.ramp.window !== 'whole') winIn.value = (f.ramp.window.first_ms / 1000);
        const syncRamp = () => {
            if (trSel.value === 'none') { f.ramp = null; }
            else {
                const s = winIn.value === '' ? null : Math.round(parseFloat(winIn.value) * 1000);
                f.ramp = ramp(trSel.value, s);
            }
            commit();
        };
        trSel.addEventListener('change', syncRamp);
        winIn.addEventListener('input', syncRamp);
        tr.appendChild(trSel);
        tr.appendChild(winIn);
        det.appendChild(tr);

        // Pulse.
        const pu = el('div', 'gen-wiz-subrow');
        pu.appendChild(el('span', 'gen-wiz-flabel', 'Pulse'));
        const puSel = el('select');
        [['none', 'Off'], ['sine', '~ sine'], ['triangle', '^ tri'], ['sawup', '/ saw up'],
            ['sawdown', '\\ saw down'], ['square', '_ square']]
            .forEach(([v, t]) => { const o = el('option', null, t); o.value = v; puSel.appendChild(o); });
        puSel.value = f.mod ? f.mod.wave : 'none';
        const endIn = el('input', 'gen-wiz-num');
        endIn.type = 'number'; endIn.placeholder = 'to'; endIn.title = 'wobble to this value';
        if (f.mod) endIn.value = f.mod.end;
        const perIn = el('input', 'gen-wiz-num');
        perIn.type = 'number'; perIn.min = 0; perIn.placeholder = 'every s';
        perIn.title = 'period in seconds';
        if (f.mod) perIn.value = (f.mod.period_ms / 1000);
        const syncMod = () => {
            if (puSel.value === 'none') { f.mod = null; }
            else {
                const end = endIn.value === '' ? f.value : parseFloat(endIn.value);
                const per = perIn.value === '' ? 1000 : Math.round(parseFloat(perIn.value) * 1000);
                f.mod = mod(puSel.value, end, per);
            }
            commit();
        };
        puSel.addEventListener('change', syncMod);
        endIn.addEventListener('input', syncMod);
        perIn.addEventListener('input', syncMod);
        pu.appendChild(puSel);
        pu.appendChild(endIn);
        pu.appendChild(perIn);
        det.appendChild(pu);

        return det;
    }

    // ---- layer editor (bottom-sheet-style card) ---------------------------
    function layerEditor(seg, layer, idx) {
        const card = el('div', 'gen-wiz-layer');
        const head = el('div', 'gen-wiz-layer-head');
        const title = layerTitle(layer);
        head.appendChild(el('strong', null, title));
        const del = button('✕', 'gen-wiz-del', () => {
            seg.layers.splice(idx, 1); structureChanged();
        });
        head.appendChild(del);
        card.appendChild(head);

        if (layer.kind === 'light') {
            card.appendChild(regionChips(layer));
            card.appendChild(presetBar('Colour', COLOR_PRESETS, (p) => {
                layer.fields.r.value = p.r; layer.fields.g.value = p.g; layer.fields.b.value = p.b; changed();
            }));
            card.appendChild(sliderRow('Brightness', layer.fields.bright, 0, 100, 1));
            // One-click breath swell (A8): 0.1 Hz sine on brightness (6 breaths/min).
            const bl = el('div', 'gen-wiz-presets');
            bl.appendChild(el('span', 'gen-wiz-flabel', 'Breath'));
            bl.appendChild(button('🫁 Breath swell (0.1 Hz)', 'gen-wiz-preset', () => {
                layer.fields.bright.value = 50;            // swing low
                layer.fields.bright.mod = breathMod({});   // sine 50→100 over 10 s
                changed();
            }));
            card.appendChild(bl);
            const cf = dynamicsEditor(layer.fields.bright); card.appendChild(cf);
            card.appendChild(sliderRow('Flicker Hz', layer.fields.freq, 0, 60, 0.1));
            card.appendChild(advancedLight(layer));
            return card;
        }

        // audio kinds
        if (layer.kind === 'binaural') {
            card.appendChild(presetBar('Brainwave', BRAINWAVE_PRESETS, (p) => {
                layer.fields.beat.value = p.beat;
                if (p.carrier !== undefined) layer.fields.freq.value = p.carrier; // A4 research carrier
                changed();
            }));
            card.appendChild(sliderRow('Carrier Hz', layer.fields.freq, 20, 1000, 1,
                liveAudio(layer, 'freq')));
            card.appendChild(dynamicsEditor(layer.fields.freq));
            card.appendChild(sliderRow('Beat Hz', layer.fields.beat, 0.5, 40, 0.5));
            card.appendChild(sliderRow('Volume', layer.fields.volume, 0, 100, 1,
                liveAudio(layer, 'volume')));
            card.appendChild(dynamicsEditor(layer.fields.volume));
            card.appendChild(advancedAudio(layer, { stereo: true }));
            return card;
        }
        if (layer.kind === 'noise') {
            card.appendChild(presetBar('Noise', NOISE_PRESETS, (p) => {
                layer.wave_type = p.wave; changed();
            }));
            card.appendChild(sliderRow('Volume', layer.fields.volume, 0, 100, 1,
                liveAudio(layer, 'volume')));
            card.appendChild(dynamicsEditor(layer.fields.volume));
            card.appendChild(advancedAudio(layer, {}));
            return card;
        }
        // tone
        card.appendChild(sliderRow('Freq Hz', layer.fields.freq, 20, 2000, 1,
            liveAudio(layer, 'freq')));
        card.appendChild(dynamicsEditor(layer.fields.freq));
        card.appendChild(sliderRow('Volume', layer.fields.volume, 0, 100, 1,
            liveAudio(layer, 'volume')));
        card.appendChild(dynamicsEditor(layer.fields.volume));
        card.appendChild(advancedAudio(layer, {}));
        return card;
    }

    function liveAudio(layer, fname) {
        // Push a one-line patch for instant feedback (best-effort).
        return () => {
            const f = layer.fields;
            const ch = Number.isFinite(layer.channel) ? layer.channel : 1;
            const line = 'A 0 ' + (f.freq ? f.freq.value : 0) + ' ' + (f.pan ? f.pan.value : 0) +
                ' ' + (f.volume ? f.volume.value : 0) + ' ' + (f.mod ? f.mod.value : 0) + ' ' + ch;
            patchLine(line).catch(() => {});
        };
    }

    // v2 pulse-shape controls for a layer (env/phase/attack/jitter, audio duty).
    // Layer-level: applies to every row the layer expands to. Stored in layer.pulse
    // (created lazily; removed when emptied). Blank = off, - = leave unchanged.
    function pulseRows(layer, isAudio) {
        const frag = document.createDocumentFragment();
        const read = () => layer.pulse || {};
        function apply(f, val) {
            if (!layer.pulse) layer.pulse = {};
            if (val === undefined) delete layer.pulse[f]; else layer.pulse[f] = val;
            if (!Object.keys(layer.pulse).length) delete layer.pulse;
            changed();
        }
        frag.appendChild(el('div', 'gen-wiz-flabel', 'Pulse shape  (blank = off · - = leave unchanged)'));

        const erow = el('div', 'gen-wiz-subrow');
        erow.appendChild(el('span', 'gen-wiz-flabel', 'Env'));
        const esel = el('select');
        const names = isAudio
            ? ['square', 'sine', 'triangle', 'trapezoid', 'tremolo']
            : ['square', 'sine', 'triangle', 'trapezoid'];
        const mk = (v, lbl) => { const o = el('option', null, lbl); o.value = v; esel.appendChild(o); };
        mk('', '(off)'); mk('-', '— leave');
        names.forEach((n, i) => mk(String(i), i + ' ' + n));
        esel.value = formatPulseEnv(read().env);
        esel.addEventListener('change', () => apply('env', parsePulseEnv(esel.value)));
        erow.appendChild(esel);
        frag.appendChild(erow);

        function textRow(label, f, fmt, parse) {
            const row = el('div', 'gen-wiz-subrow');
            row.appendChild(el('span', 'gen-wiz-flabel', label));
            const inp = el('input', 'gen-wiz-num'); inp.type = 'text'; inp.placeholder = 'off';
            inp.value = fmt(read()[f]);
            inp.addEventListener('change', () => apply(f, parse(inp.value)));
            row.appendChild(inp);
            frag.appendChild(row);
        }
        if (isAudio) textRow('Duty %', 'duty', formatPulseCell, parsePulseCell);
        textRow('Phase°', 'phase', formatPulseCell, parsePulseCell);
        textRow('Attack ms', 'attack', formatPulseCell, parsePulseCell);
        textRow('Jitter', 'jitter', formatPulseJitter, parsePulseJitter);
        return frag;
    }

    function advancedAudio(layer, opts) {
        const det = el('details', 'gen-wiz-adv');
        det.appendChild(el('summary', null, '▸ Advanced'));
        if (layer.fields.pan) det.appendChild(sliderRow('Pan', layer.fields.pan, -100, 100, 1));
        // Waveform
        const wrow = el('div', 'gen-wiz-subrow');
        wrow.appendChild(el('span', 'gen-wiz-flabel', 'Waveform'));
        const wsel = el('select');
        const noneOpt = el('option', null, 'default (sine)'); noneOpt.value = '';
        wsel.appendChild(noneOpt);
        WAVE_TYPES.forEach((w, i) => { const o = el('option', null, w); o.value = String(i); wsel.appendChild(o); });
        wsel.value = (layer.wave_type === null || layer.wave_type === undefined) ? '' : String(layer.wave_type);
        wsel.addEventListener('change', () => {
            layer.wave_type = wsel.value === '' ? null : parseInt(wsel.value, 10); changed();
        });
        wrow.appendChild(wsel);
        det.appendChild(wrow);
        // Stereo / monaural toggles (binaural)
        if (opts.stereo) {
            const srow = el('div', 'gen-wiz-subrow');
            const lab = el('label', null, ' True-stereo (two hard-panned channels)');
            const cb = el('input'); cb.type = 'checkbox'; cb.checked = !!layer.stereo;
            cb.addEventListener('change', () => { layer.stereo = cb.checked; changed(); });
            lab.prepend(cb);
            srow.appendChild(lab);
            det.appendChild(srow);

            // Monaural: two centre-panned channels — the beat forms in the air, so
            // it works on a single speaker / one ear (A4). Stereo wins if both set.
            const mrow = el('div', 'gen-wiz-subrow');
            const mlab = el('label', null, ' Monaural (centre-panned pair · works on speakers)');
            const mcb = el('input'); mcb.type = 'checkbox'; mcb.checked = !!layer.monaural;
            mcb.addEventListener('change', () => { layer.monaural = mcb.checked; changed(); });
            mlab.prepend(mcb);
            mrow.appendChild(mlab);
            det.appendChild(mrow);
        }
        // One-click LFO macros (A8): rotating pan + breath-paced volume swell.
        if (layer.fields.pan || layer.fields.volume) {
            const lfo = el('div', 'gen-wiz-subrow');
            lfo.appendChild(el('span', 'gen-wiz-flabel', 'LFO'));
            if (layer.fields.pan) {
                lfo.appendChild(button('↻ Rotate pan', 'gen-wiz-preset', () => {
                    layer.fields.pan.value = -100;              // swing start (left)
                    layer.fields.pan.mod = rotatingPanMod({});  // sine -100→100 over 15 s
                    changed();
                }));
            }
            if (layer.fields.volume) {
                lfo.appendChild(button('🫁 Breath vol', 'gen-wiz-preset', () => {
                    layer.fields.volume.value = 50;             // swing low
                    layer.fields.volume.mod = breathMod({});    // sine 50→100, 0.1 Hz
                    changed();
                }));
            }
            det.appendChild(lfo);
        }
        // Manual channel pin
        const crow = el('div', 'gen-wiz-subrow');
        crow.appendChild(el('span', 'gen-wiz-flabel', 'Pin channel'));
        const cin = el('input', 'gen-wiz-num'); cin.type = 'number'; cin.min = 1; cin.max = 16;
        cin.placeholder = 'auto';
        if (Number.isFinite(layer.channel)) cin.value = layer.channel;
        cin.addEventListener('input', () => {
            const v = parseInt(cin.value, 10);
            layer.channel = (Number.isFinite(v) && v >= 1 && v <= 16) ? v : undefined; changed();
        });
        crow.appendChild(cin);
        det.appendChild(crow);
        det.appendChild(pulseRows(layer, true));
        return det;
    }

    function advancedLight(layer) {
        const det = el('details', 'gen-wiz-adv');
        det.appendChild(el('summary', null, '▸ Advanced (split RGB)'));
        det.appendChild(sliderRow('R', layer.fields.r, 0, 255, 1));
        det.appendChild(dynamicsEditor(layer.fields.r));
        det.appendChild(sliderRow('G', layer.fields.g, 0, 255, 1));
        det.appendChild(dynamicsEditor(layer.fields.g));
        det.appendChild(sliderRow('B', layer.fields.b, 0, 255, 1));
        det.appendChild(dynamicsEditor(layer.fields.b));
        det.appendChild(sliderRow('Duty %', layer.fields.duty, 0, 100, 1));
        det.appendChild(pulseRows(layer, false));
        return det;
    }

    function regionChips(layer) {
        const wrap = el('div', 'gen-wiz-regions');
        const defs = [['Inner-L', 0x01], ['Outer-L', 0x02], ['Outer-R', 0x04], ['Inner-R', 0x08],
            ['Ch 5-8', 0xF0], ['All', 0xFF]];
        defs.forEach(([label, bits]) => {
            const chip = el('button', 'gen-wiz-chip', label);
            chip.type = 'button';
            const on = (bits === 0xFF) ? (layer.channelMask === 0xFF)
                : ((layer.channelMask & bits) === bits);
            if (on) chip.classList.add('on');
            chip.addEventListener('click', () => {
                if (bits === 0xFF) { layer.channelMask = 0xFF; }
                else if (bits === 0xF0) { layer.channelMask ^= 0xF0; }
                else { layer.channelMask ^= bits; }
                layer.channelMask &= 0xFF;
                if (!layer.channelMask) layer.channelMask = bits; // never 0 (firmware rejects)
                changed();
            });
            wrap.appendChild(chip);
        });
        return wrap;
    }

    function presetBar(label, presets, apply) {
        const wrap = el('div', 'gen-wiz-presets');
        wrap.appendChild(el('span', 'gen-wiz-flabel', label));
        for (const k in presets) {
            wrap.appendChild(button(presets[k].label, 'gen-wiz-preset', () => apply(presets[k])));
        }
        return wrap;
    }

    function layerTitle(layer) {
        if (layer.kind === 'light') return '💡 Light';
        if (layer.kind === 'binaural') return '🧠 Binaural';
        if (layer.kind === 'noise') return '🌊 Noise';
        return '🎵 Tone';
    }

    // ---- segment card -----------------------------------------------------
    function segmentCard(seg, si, start) {
        const card = el('div', 'gen-wiz-seg');
        const head = el('div', 'gen-wiz-seg-head');
        const nameIn = el('input', 'gen-wiz-segname');
        nameIn.value = seg.name || ('Segment ' + (si + 1));
        nameIn.addEventListener('input', () => { seg.name = nameIn.value; renderMinimap(); commit(); });
        head.appendChild(nameIn);

        const durWrap = el('span', 'gen-wiz-dur');
        durWrap.appendChild(el('span', null, 'for '));
        const durIn = el('input', 'gen-wiz-num');
        durIn.type = 'number'; durIn.min = 0; durIn.step = 1;
        durIn.value = (seg.duration_ms / 1000);
        durIn.addEventListener('input', () => {
            const v = parseFloat(durIn.value); seg.duration_ms = Math.max(0, (Number.isFinite(v) ? v : 0) * 1000);
            renderMinimap(); commit();
        });
        durWrap.appendChild(durIn);
        durWrap.appendChild(el('span', null, ' s'));
        head.appendChild(durWrap);

        head.appendChild(el('span', 'gen-wiz-segstart', '@ ' + (start / 1000).toFixed(1) + 's'));

        const acts = el('span', 'gen-wiz-seg-acts');
        acts.appendChild(button('⧉', 'gen-wiz-del', () => {
            const copy = JSON.parse(JSON.stringify(seg));
            copy.id = newId('seg');
            copy.layers.forEach(l => { l.id = newId('l'); });
            session.segments.splice(si + 1, 0, copy); structureChanged();
        }));
        acts.appendChild(button('✕', 'gen-wiz-del', () => {
            session.segments.splice(si, 1); structureChanged();
        }));
        head.appendChild(acts);
        card.appendChild(head);

        // Layers
        seg.layers.forEach((layer, idx) => card.appendChild(layerEditor(seg, layer, idx)));

        // Add-layer bar
        const addBar = el('div', 'gen-wiz-addlayer');
        ['tone', 'binaural', 'noise', 'light'].forEach(kind => {
            addBar.appendChild(button('+ ' + kind, 'gen-wiz-addbtn', () => {
                seg.layers.push(newLayer(kind)); structureChanged();
            }));
        });
        card.appendChild(addBar);
        return card;
    }

    // ---- render -----------------------------------------------------------
    function renderMinimap() {
        minimap.innerHTML = '';
        minimap.appendChild(el('div', 'gen-wiz-mini-title', 'Timeline'));
        let start = 0;
        session.segments.forEach((seg, i) => {
            const item = el('button', 'gen-wiz-mini-item',
                (seg.name || ('Seg ' + (i + 1))) + ' · ' + (seg.duration_ms / 1000) + 's');
            item.type = 'button';
            item.addEventListener('click', () => {
                const target = main.querySelectorAll('.gen-wiz-seg')[i];
                if (target) target.scrollIntoView({ behavior: 'smooth', block: 'start' });
            });
            minimap.appendChild(item);
            start += seg.duration_ms;
        });
        minimap.appendChild(el('div', 'gen-wiz-mini-total', 'Total ' + (start / 1000).toFixed(1) + 's'));
    }

    function renderMain() {
        main.innerHTML = '';

        // Session header.
        const hdr = el('div', 'gen-wiz-header');
        const nameIn = el('input', 'gen-wiz-sessname');
        nameIn.value = session.name || 'New session';
        nameIn.addEventListener('input', () => { session.name = nameIn.value; commit(); });
        hdr.appendChild(el('span', 'gen-wiz-flabel', 'Session'));
        hdr.appendChild(nameIn);
        main.appendChild(hdr);

        // Segments.
        let start = 0;
        session.segments.forEach((seg, si) => {
            main.appendChild(segmentCard(seg, si, start));
            start += seg.duration_ms;
        });

        // Add-segment.
        const addSeg = el('div', 'gen-wiz-addseg');
        addSeg.appendChild(button('+ Add segment', 'gen-wiz-addbtn', () => {
            session.segments.push(newSegment()); structureChanged();
        }));
        addSeg.appendChild(button('+ Binaural template', 'gen-wiz-addbtn', () => {
            const seg = newSegment();
            seg.name = 'Binaural';
            seg.layers.push(newLayer('binaural'));
            session.segments.push(seg); structureChanged();
        }));
        // Harmonic stack (A5): octave carriers sharing one beat Δf, 1/N-scaled so
        // the summed level stays in headroom. Each octave is a mono-binaural layer.
        addSeg.appendChild(button('+ Harmonic stack', 'gen-wiz-addbtn', () => {
            const seg = newSegment();
            seg.name = 'Harmonic stack';
            harmonicCarriers({ base: 100, beat: 6, count: 3 }).forEach((c) => {
                const l = newLayer('binaural');
                l.fields.freq.value = c.carrier;
                l.fields.beat.value = c.beat;
                l.fields.volume.value = c.volume;
                seg.layers.push(l);
            });
            session.segments.push(seg); structureChanged();
        }));
        main.appendChild(addSeg);
    }

    function render() {
        banner.style.display = importedFlag ? '' : 'none';
        if (importedFlag) banner.textContent =
            '⚠ Imported — segment names/structure are best-effort guesses. Editing recompiles cleanly.';
        renderMinimap();
        renderMain();
    }

    // ---- view contract ----------------------------------------------------
    function refresh() {
        if (applyingEdit) return;
        if (!visible) return;
        importFromDoc();
        render();
    }
    function show() {
        visible = true;
        root.style.display = '';
        importFromDoc();
        render();
    }
    function hide() {
        visible = false;
        root.style.display = 'none';
    }
    return { refresh, show, hide };
}
