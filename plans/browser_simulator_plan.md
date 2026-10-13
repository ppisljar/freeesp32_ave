# Browser session simulator + standalone generator

**Status: DONE** — built, 368 web tests green, verified in Chrome on both hosts.

Give the Generator a **Simulate** view that plays a session in the browser with
no ESP32 attached, and publish the whole Generator to the project site so people
can write and hear a session before they own hardware.

One codebase, two hosts:

| Host | Entry | Output | Format |
|---|---|---|---|
| Device UI | `web/src/js/main.js` | `web/data/*.gz` → SPIFFS | ESM, gzipped |
| Project site | `web/src/js/main-standalone.js` | `docs/generator/` | IIFE, plain |

---

## 1. What was built

### The simulation core — `web/src/js/gen/sim/`

Pure modules: no DOM, no network, no Web Audio. All unit-tested in Node.

| Module | What it does |
|---|---|
| `resolve.js` | `.ledc` doc → per-channel resolved entry lists. Owns the `-` carry, LED mask expansion, and the ramp-target rules. |
| `audio.js` | Streaming synth ported from `main/audio_generator.c`. Renders a block at a time with seek, so a 30-minute session costs no memory. |
| `led.js` | Flicker engine ported from `main/led_matrix_example.c`, including the integer pixel chain and the physical 74-pixel NeoHEX geometry. |
| `spectrum.js` | Radix-2 FFT, Hann window, log-band reduction, peak picking, whole-session spectrogram. Written rather than imported — the project ships no external libraries. |
| `lint.js` | Findings that need a *resolved* timeline, which `gen/validate.js` cannot reach. |
| `player.js` | The AudioContext transport. The playhead IS the audio clock, so light and sound cannot drift apart on screen. |

### The view — `web/src/js/gen/views/sim.js`

Four panels: **Light** (the real zone geometry plus a 2 ms-resolution scope
trace per channel), **Spectrum** (live FFT with the exact carriers marked from
the timeline), **Timeline at the playhead** (every channel's resolved state),
and **Whole session** (an on-demand spectrogram).

### Standalone build — `web/build.mjs`

A second target emitting `docs/generator/`, with `STANDALONE` folded in at build
time so the bundle contains no device code paths at all.

---

## 2. Decisions worth remembering

**A scheduled `AudioBufferSourceNode` queue, not an AudioWorklet.** A worklet
needs its own script URL; this project ships one esbuild bundle into a SPIFFS
partition served by a microcontroller, with no second entry point and no CDN.
Rendering half a second of audio measures **44× realtime in the browser**, so
the main thread stays several blocks ahead without ever being the bottleneck.
Blocks are scheduled against `ctx.currentTime`, which is sample-accurate.

**IIFE for the docs bundle, not ESM.** `docs/README.md` keeps the property that
every page opens correctly from a local checkout with the network unplugged, and
`<script type="module">` is blocked under `file://` by the opaque origin. For the
same reason the example sessions are **bundled as text** rather than fetched.

**The generator's markup is lifted out of `src/index.html`** between
`GEN-PAGE:START` / `GEN-PAGE:END` markers rather than copied, so there is one
copy of it in the repo. The build throws if the markers go missing.

**The two stylesheets are reconciled automatically.** The docs page loads both;
they collide on exactly two bare-element selectors (`body`, `h1`) — resolved by
load order, site second — and on four custom properties, which `build.mjs` lifts
out of the app's compiled `:root` and re-declares scoped to the app container.
No colour literal is hand-written anywhere, and a retheme cannot drift.

**No WASM.** The firmware's engine is not a portable DSP core: it is `IRAM_ATTR`
code inside a gptimer ISR, with spinlocks and a deliberate 32-bit-only
arithmetic discipline that exists because 64-bit libgcc helpers live in flash.
More decisively, the device's output is **not a pure function of the session** —
the LED ISR tick depends on session history, carrier start phase depends on
arming lateness, and jitter phase is keyed to the wall clock — so a WASM build
would not be bit-identical either, it would only look authoritative. The chosen
answer is a JS port whose every known divergence is documented and pinned by a
test. See `plans/ave_studio_adaptations_plan.md` §"Rejected".

---

## 3. Fidelity: what is reproduced, and what is not

### Reproduced exactly

- **The fixed 1/16 mix divisor** (`audio_generator.c:875`), never `1/n_active`.
  Pinned against three measured device levels: a centre-panned sine at vol 100
  peaks at int16 **1448** (−27.1 dBFS), a binaural channel at −24.1 dBFS.
- **The five isochronic envelopes** and the env-dependent `depth` defaults
  (no columns → 10 %, env 0–3 → 100 %, env 4 → 10 %), `duty 0` as a no-op while
  `attack 0` is written, and `duty` being silently ignored by env 1 and env 2.
- **Gate phase re-anchoring** to each entry's own timestamp, which a free-running
  accumulator misses by a measured ~48 ms.
- **The integer pixel chain**, including the double truncation on smooth carriers
  and the raised-sine LUT's deliberate clamp to 65535 rather than 65536.
- **No gamma anywhere** — which is the point, not an omission: it is what lets a
  180° antiphase pair of sine carriers sum to flat luminance. There is a test
  asserting exactly that.
- **`phase` as a LEAD**, pinned against the measured photodiode table at 10 Hz
  (0° → 0.00 ms, 90° → 74.76, 180° → 49.93, 270° → 24.69).
- **`freq 0` as a DC channel** that ignores duty, env and phase but still runs
  brightness and colour LFOs — 19 shipped sessions render black if this is wrong.
- **Out-of-range LED rows dropped, not clamped**, leaving the zone on its
  previous state.

### Deliberately idealised, and said out loud in the UI

- **Flicker edge quantisation.** The device's ISR tick is
  `clamp(floor(freq) × 250, 1000, 10000)` Hz and only ever rises within a
  session, so its real resolution depends on history. The simulator computes
  continuous time and reports the tick rate in the Light caption instead.
- **The strip refresh ceiling.** One 74-pixel WS2812 frame takes 2.41 ms, so a
  40 Hz smooth carrier gets ~10 distinct levels per cycle on hardware where the
  simulator draws the ideal curve. `lint.js` raises `STRIP_COARSE` for this.
- **Absolute carrier phase**, which is not reproducible even between two runs on
  the device (arming lateness is 0–1024 frames).
- **The +46.439 ms A/V anchor.** The device offsets the LED cycle origin by
  `AUDIO_DMA_PIPELINE_LAG_US` so light lines up with DAC emission; here both
  come from one clock, so they align by construction.
- **Jitter LFO phase**, which the device keys to the absolute wall clock.

---

## 4. Divergences fixed in passing

The port surfaced fifteen documented differences between `gen/synth.js` and the
firmware. The simulator closes all of them. Three change what you hear, and
**`gen/synth.js` itself was deliberately left alone** so the shipped bounce path
did not change behaviour under this work — see the adaptations plan.

Fixed in shared code (and therefore live for every view):

- `parse.js` / `model.js` gained `freqRPresent`, because an absent `freq_r` token
  and an explicit `0` both stored `0` but mean opposite things: absent keeps the
  live right carrier, `0` makes the channel mono. Without it, a row that moves
  `freq` and omits `freqR` silently killed a binaural beat.
- `resolve.js` uses **channel 0** as the default for a bare `A` line, matching
  `config_parser.c:1816`. `gen/synth.js` used 1, so a file mixing bare lines with
  explicit `ch 0` lines grouped differently in the browser than on the device.
- `tts.js` no longer injects the Puter CDN script at module load in the
  standalone build — a public page has no business contacting a third party on
  load, and nothing there can reach speech anyway.

---

## 5. Performance notes

Three fixes took the view from unusable to 120 fps with all panels live:

1. **Hoist constant fields out of the per-sample loop.** The render loop now
   walks one entry-run at a time; a plain step value is evaluated once per run
   rather than 44 100 times a second. 44× realtime in the browser.
2. **Stop reallocating canvas backing stores every frame.** Writing
   `canvas.width` reallocates and clears; at DPR 2 these canvases are over a
   megapixel each, and there are four of them.
3. **Cache the scope traces on a fixed 2 ms grid.** A 1.5 s window is 750 points
   per zone; anchoring them to a global grid instead of the playhead means a
   frame that advanced 16 ms computes 8 new points, not 2 250.

Measured per frame with all five panels live: ~0.4 ms, plus an 8192-point FFT
(~2.9 ms) at a third of the frame rate.

---

## 6. Cost

| | Before | After |
|---|---|---|
| SPIFFS (gzipped) | 82.8 KB | 109.0 KB |
| Headroom (4 MB board, 512 KB partition) | 429 KB | 403 KB |
| Web tests | 269 | 368 |

---

## 7. Verified

- `npm test` — 368 passing, 0 failing.
- `npm run build` — both targets clean.
- Chrome, device UI at `#generator` → Simulate: audio plays, the clock advances
  in exact real time off the audio clock, LED zones animate, scope traces show
  the 10 Hz square and the 180°-offset raised sine, spectrum and inspector live.
- Chrome, `docs/generator/index.html`: loads with **zero external requests**,
  lands on Simulate with an example loaded, plays, and shows no device controls.

**Not verified on hardware** — nothing in this work touches the firmware.
