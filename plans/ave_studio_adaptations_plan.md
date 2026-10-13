# AVE Studio adaptations — follow-ups to the browser simulator

**Status: DONE** — everything adopted (A1-A7, B1, B2) is built, 421 web tests
green, verified in Chrome on the published page.

Source: `ave_studio_browser_simulator.zip` (an "AVE Studio" design proposal,
2026-10-13), read against the simulator as built. This plan records what is
worth taking from it, what is not, and why — so the rejected items stay
rejected for stated reasons rather than being re-proposed.

---

## Adopt — ALL DONE

Ordered by value per unit of work, which is also the order they were built.

### A1. Envelope / modulation spectrum beside the carrier spectrum
**DONE.** `sim/spectrum.js` `createEnvelopeAnalyser`, panel under the carrier
spectrum. Verified on `27_genus_40hz_dim`: the carrier spectrum shows no 40 Hz
line anywhere, and the envelope panel shows a single sharp peak at exactly
40 Hz. The display band stops at 60 Hz for a measured reason — rectification
leaves a residual that always lands ABOVE it (88.8 Hz for a 300 Hz carrier),
while inside the band a steady tone sits at −79 to −87 dB.


The proposal's sharpest technical point, and a real gap today.

A 10 Hz binaural beat built from 200 Hz and 210 Hz has **no 10 Hz spectral
peak** — the spectrum shows two carriers, and the thing the author actually
cares about is invisible. The entrainment rate lives in the *envelope* of the
summed signal.

Build: rectify the mono sum, decimate by 64 (box average, first null at 689 Hz,
far above anything that matters), and run a 1024-point FFT at the decimated
rate. That gives 0.67 Hz bins over 0–344 Hz for 1.5 s of signal, which resolves
a 10 Hz beat and a 40 Hz gamma gate comfortably. Display 0.5–60 Hz on a log
axis, labelled as the entrainment band.

One panel covers all three modalities: binaural (two carriers summing),
monaural (same, earlier in the chain) and isochronic (gate already in each
channel) all put the rate in the envelope.

Throttle to ~5 Hz — the window needs 65 536 samples, which is ~1.5 ms to render.

### A2. Audio waveforms on the scope's time axis
**DONE.** L and R min/max rows above the LED traces on one axis. The bucket
cache shifts on a fixed 2 ms grid like the LED traces do — keying it on the
window start instead re-rendered 66 150 samples every frame, which is more
audio than the renderer can produce in real time.


The scope draws LED levels at 2 ms resolution. Adding L and R as min/max
envelope rows on the *same* axis lets you see A/V alignment, which is this
project's whole thesis and currently the one thing the simulator cannot show.

Render the scope window once per spectrum tick (~20 Hz) and bucket min/max per
2 ms column; a per-frame re-render of 66 150 samples would be wasteful and is
not needed — the trace only moves 16 ms per frame.

### A3. Findings that link to the source line and seek the playhead
**DONE.** `views/text.js` gained `revealLine`, handed to the Simulate view
through its ctx rather than imported, so generator.js -> views/sim.js does not
become a cycle.


Half-built: `sim/lint.js` findings already carry `row` and `timeMs`, the panel
already shows "line N" and a seek button. What is missing is the other
direction — clicking a finding should also scroll the Text view to that line
and highlight it. Needs a small API on `views/text.js`.

### A4. Clipping detection on the summed signal
**DONE.** `renderBlock` records the PRE-clamp peak and clipped-sample count;
peak meter beside the monitor trim, red on clip.


The device hard-clamps at ±1 after the master gain. Only a simulator can see
when a session actually reaches that, and a clipped session sounds wrong on
hardware in a way nothing in the file reveals.

Build: have `renderBlock` record the pre-clamp peak and a clipped-sample count
per block, expose them on the renderer, and show a peak meter beside the monitor
trim with a clip indicator. Then lint the whole session offline (the renderer is
~44× realtime, so a 20-minute session scans in ~30 s) and report the worst
moments as findings with timestamps.

### A5. Phase wheel
**DONE.** One dial per flickering zone on the LED canvas. On
`27_genus_40hz_dim` the antiphase pair reads at a glance: L1/L3 at 180 degrees,
L2/L4 at 0.


`phase` is the subtlest column in the format — it is a *lead*, not a lag, and
across the whole session library it only ever takes 0 or 180. A small dial per
active zone showing the offset makes antiphase self-evident and would have made
the lead-vs-lag question obvious years earlier.

Cheap: one arc and one dot per zone on the existing LED canvas.

### A6. Exports
**DONE.** `sim/export.js`: WAV (with the device's TRUNCATING int16 conversion,
not the rounding one), plus CSV/JSON sampled at the strip frame period with a
provenance header that says the data is PREDICTED, not measured.


- **Stereo WAV** of the simulated session. Trivial: `renderSessionDevice` plus
  the existing `encodeWav16`. Must use the device's truncating int16 conversion
  (`toInt16Device`), not the rounding one in `bgaudio.js`.
- **CSV / JSON** of timestamped LED brightness and per-channel audio parameters.
  Matters for a project that already has a measurement rig and a marker bus:
  it makes the simulator's prediction directly comparable with a capture.
  Sample at the strip's real frame period (2.41 ms) rather than an arbitrary
  rate, so the rows mean something physical.

### A7. `M` marker rows in the web parser
**DONE.** parse/model/serialize plus `resolveMarkers`, which reproduces the
firmware's modality INFERENCE from the batch and the predicted-onset lag
(46 439 us audio, +led_count x 30/2 for light = 47 549 us at 74 px). Markers
now also count against the 100-entry budget, which they do on the device.


Not from the proposal — found while scouting. `main/config_parser.c` parses `M`
lines and `docs/sessions/markers.html` documents them, but `web/src/js/gen/parse.js`
does not: an `M` row becomes a `raw` row and shows as an error. Any session using
the marker bus currently looks broken in the editor.

Add parse + serialize + model support, then show markers on the simulator's
timeline with their three timestamps (scheduled / dispatched / predicted onset,
the last from the 46.439 ms DMA lag plus `pixel_index × 30 µs`).

---

## Consider later

### B1. Switch `gen/bounce.js` to the device-accurate renderer

**DONE.** `gen/synth.js` is now `gen/mix.js` and holds only the buffer/mixing
helpers; its duplicate engine is gone and the bounce renders with
`sim/audio.js`. Bounced WAVs are ~18-24 dB quieter, which is correct. The BG
clip's pan law is still equal-power where the device is linear (divergence D14)
— deliberately untouched, because the BG path is hardware-verified over the
WebSocket push and could not be re-verified here.

Original note:

`sim/audio.js` closes fifteen divergences that `gen/synth.js` still has, three of
which change what you hear — most importantly the mix divisor, which leaves the
bounce **~18 dB hot for two channels and ~24 dB for one**.

`gen/synth.js` was deliberately left untouched so this work could not regress a
shipped path. Switching `bounce.js` over is a one-line import change, but it
makes every bounced WAV much quieter (correctly so), which is a judgement call
about a user-visible artifact rather than a bug fix to slip in quietly.

If adopted: keep `renderSessionDevice` as the single implementation, delete the
duplicated synthesis from `gen/synth.js`, and decide separately whether the
*downloaded* WAV gets a documented makeup gain.

### B2. Display-curve option for the LED preview
**DONE.** `sim/led.js` carries the measured light/drive table from
docs/sessions/photic.html#bright and a "Measured light" toggle draws through
it. Applied in exactly one place — what is painted on the canvas — and a test
asserts the device pixel bytes are unchanged by it, since those are what the
inspector reports and the CSV export carries.

It also enabled a new lint, `DIM_NONLINEAR`, which is the better half of this
item: a brightness LFO living entirely below drive ~100 is distorted in SHAPE,
not merely shallow, and only a simulator can see that because it needs the
resolved colour and the resolved brightness extremes multiplied together. It
fires on the shipped `17_nondual_awareness` with exactly the drive range the
docs predict (16 to 44, 71 % of linear at the dim end against 90 % at the
bright end).


Measured on hardware, light output is not linear in drive below ~40 %: 0.135 at
drive 12 rising to 0.200 at 102+. Therapeutic sessions live in exactly that
region, so a linear preview shows a symmetric breath where the device compresses
the dim end.

If built, it must apply **only to the display mapping**, never to the computed
pixel bytes, and must be labelled. The pixel bytes are the device's and a test
should assert they are unaffected by whatever curve is chosen.

---

## Rejected, with reasons

### R1. WASM / a shared C core via Emscripten

The proposal's headline recommendation. Declined.

The firmware's engine is not a portable DSP core waiting to be extracted. It is
`IRAM_ATTR` functions running inside a gptimer ISR, with `portENTER_CRITICAL_ISR`
spinlocks and a deliberate **32-bit-only arithmetic discipline** that exists
because 64-bit libgcc helpers live in flash and would stall the ISR during
flash-cache windows. Lifting it out means rewriting it, at which point it is not
the same engine.

More decisively: **the device's output is not a pure function of the session.**
The LED ISR tick depends on the session's history (it only ever rises), carrier
start phase depends on how late the arming happened to be (0–1024 frames,
effectively random per run), and jitter phase is keyed to the absolute wall
clock. A WASM build would not be bit-identical either — it would merely *look*
authoritative while diverging where nobody checks.

The real risk was never "two implementations"; it was "two implementations with
no tests". Fifteen divergences are now closed and pinned against measured
hardware values. That cost a day instead of a refactor of the real-time core.

Emscripten would also break the docs site's open-from-`file://` property, the
same way ES modules do.

### R2. External libraries — CodeMirror 6, uPlot, an FFT library, Emscripten

The project has a written rule: one bundle, no external requests, works offline
from a local checkout, because the device has no internet and the browser
reaching it often has none either. The FFT is ~120 lines and the plots are
Canvas.

### R3. TypeScript

Deliberately vanilla JS, no framework, no build beyond esbuild.

### R4. A separate "AVE Studio" codebase

The proposal recommends building it apart from the embedded UI. The opposite was
chosen: one codebase, two hosts. The "don't put a heavy editor on the device"
concern does not apply — the whole UI is 109 KB gzipped against 403 KB of
remaining headroom on the tightest board.

### R5. 10-channel WAV (2 audio + 8 LED brightness)

LED brightness at 44.1 kHz is ~100× oversampled against a strip that cannot
exceed ~415 fps, audio editors mangle non-audio channels (the proposal concedes
this), and nothing consumes the format. A6's CSV covers the real need.

### R6. "Compact combined playback file for fast ESP32 playback"

A new *firmware* feature in a simulator costume: it would bypass the timeline
engine and create a second playback path on the device.

### R7. The visual session designer (proposal §9)

Already shipped. The **Lanes** view is a DAW timeline with draggable automation
points; the **Wizard** view is the beginner/preset mode. The proposal describes
building what exists.

### R8. Beginner / Advanced / Research mode split

The Text / Table / Lanes / Wizard subtabs already are that axis. A second,
orthogonal mode selector would make the UI harder to explain, not easier.

### R9. AudioWorklet

Needs its own script URL, which a single-bundle SPIFFS app does not have; the
proposal itself notes worklets need a dev server rather than `file://`. The
scheduled-buffer queue measures 44× realtime, so there is no performance case.

### R10. Checkpoint/replay for seeking

Treated as a blocker in the proposal. It mostly is not: the format is built on
absolute timestamps, so the gate phase is reconstructed analytically from the
entry time — which is exactly what the firmware does — and absolute carrier
phase is not reproducible on hardware anyway.

### R11. `AnalyserNode`

Cannot show anything while muted, paused or scrubbing. The FFT runs on the
simulated signal from a second renderer instead, which works in all three.

---

## Found while doing this, NOT fixed

**The Lanes and Wizard views drop every row kind they do not model.** Both
project the doc into their own structure and rebuild it, so committing an edit
in either view deletes `S` speech rows, `M` marker rows and any unparsable
`raw` row that was being preserved verbatim.

This is PRE-EXISTING, not a regression from the marker work — verified by
round-tripping an unknown `ZZ` line and an `S` line through
`lanesToDoc(docToLanes(doc))` before and after. Markers simply joined a club
that already had members.

It is the most valuable thing left: silent data loss in an editor, on a
documented shipped feature (narration), reachable by opening a session in
Lanes and moving one point. The Text, Table, Simulate and Community paths are
all safe — Table because it mutates the live doc in place and commits the same
object rather than rebuilding it.

Fix shape: have `docToLanes` stash the rows it cannot model (with their source
index) in the lane model's metadata, and have `lanesToDoc` splice them back at
their original positions. The same for the Wizard. Then a test per view that
round-trips a doc containing every row kind and asserts equality.
