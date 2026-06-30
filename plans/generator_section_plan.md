# Generator Section — Built-in Visual `.led` Timeline Editor (Plan & Contract)

> **STATUS (2026-06-30): PLANNED — not started.** Pure web/UI feature under
> `freeesp32_ave/web/src`. **No firmware / C changes are required** (see the
> "Firmware impact" line in each phase; the conclusion is *zero* C edits).
> Background analysis lives in `reports/generator_analysis/` (read
> `000_SYNTHESIS.md` first). Authoritative grammar is `main/config_parser.c` /
> `main/config_parser.h` — treat the C parser as canonical at all times.

## Goal

Add a new **"Generator"** tab to the device web UI that lets users build `.led`
timeline files **visually** instead of typing raw commands. It supersedes the
old external `freeesp32_ave_generator` server app (and its lossy, buggy
exporter). The editor exposes **four view layers of increasing abstraction over
one shared model**:

```
raw TEXT  →  TABLE (grid)  →  DAW LANE view  →  WIZARD (segments)
\___________________ all edit the SAME shared model ___________________/
```

The automation-curve concept (per-field ramp/LFO regions) is realized **inside
the DAW lane view**, not as a 5th layer.

### Non-negotiable design decisions (from the user — the plan reflects these exactly)

1. **One shared in-memory core model = the flat `.led` entry list.** A single
   `parse` / `serialize` pair gives **lossless round-trip**; `.led` text is the
   source of truth; comments/blank lines are preserved where feasible. Every
   view is a *projection* of this one model.
2. **Four view layers** (TEXT → TABLE → LANE → WIZARD), all editing the shared
   model and staying in sync. Automation curves fold into the LANE view as
   per-field ramp/LFO regions.
3. **No "Phase 0".** Do **not** touch the old `freeesp32_ave_generator` app.
4. The new editor **must avoid the old exporter's correctness bugs** (Part 2 of
   `reports/generator_analysis` — enumerated in the "Bug-avoidance contract"
   below).
5. The 3 previously-"unrepresentable" features are **authoring conveniences**
   that expand into ordinary multi-channel timeline entries — implemented as
   macros, **no firmware changes**.
6. **Build constraints:** vanilla JS, no framework; bundle via esbuild
   (`web/build.mjs`); build with `freeesp32_ave/build_web.sh`; assets gzipped;
   fast iteration via `flash_web.sh`. Integrate as a nav tab via `nav.js`;
   reuse `configstore.js` for save/load and the existing `/api/*` endpoints.

The active project dir is `freeesp32_ave` (web build: `cd web && npm run build`;
firmware not rebuilt by this feature). Subagents must NOT flash.

---

## Firmware grammar — the contract every view serializes to

(Verified against `config_parser.c`/`.h`. `NUM_AUDIO_CHANNELS = 16`,
`NUM_LED_CHANNELS = 8`, `CONFIG_PARSER_MAX_ENTRIES = 100`, same-timestamp
`MAX_BATCH_SIZE = 50`.)

**LED line** — 5-field legacy *or* 8-field canonical (9-field RGBW **rejected**):
```
time freq duty bright mask                 # 5-field legacy (import-only; default RGB=255,255,255)
time freq duty bright R G B mask           # 8-field canonical (always EMIT this form)
```
- `time` uint32 ms (no prefix). `freq` Hz, `duty` 0..100, `bright` 0..100, each
  interpolatable. `R G B` 0..255 each, **independently** interpolatable.
- `mask` uint8 **bitmask** bits 0..7 = LED channels 1..8; **`mask==0` rejected**.

**Audio line** — `A` then 5–8 tokens:
```
A time freq pan volume mod [channel] [freq_r] [wave_type]
```
- `time` ms. `freq` Hz, `pan` −100..+100, `volume` 0..100, `mod` Hz — all
  interpolatable. `channel` int 1..16 (token 6). `freq_r` Hz (token 7, no
  prefix; `0` = mono). `wave_type` int 0..6 (token 8, no prefix): 0 sine,
  1 square, 2 triangle, 3 sawtooth, 4 white, 5 pink, 6 brown.

**BG line** (session-level, last-wins): `BG <url> <pan> <loudness>` — url scheme
`http(s)://` or `sdcard://`; pan −100..100; loudness 0..100.

**Per-field prefixes** (`parse_value_with_interpolation` + `parse_mod_extras`):
- Ramp (one-shot, target = next same-channel/field entry's value over the time
  gap; prefix lives on the *start* entry): `>` linear, `*` quadratic.
- Modulation (self-contained `PREFIXstart:end:period_ms`; runs until preempted):
  `^` triangle, `~` sine, `/` saw-up, `\` saw-down, `_` square. Defaults when
  `:end`/`:period` omitted: `end=start`, `period=1000`.
- No prefix = immediate step.
- `#` to end-of-line = comment (whole-line or inline trailing).

## Bug-avoidance contract (the serializer MUST satisfy all of these)

| # | Old exporter bug | Required behavior |
|---|---|---|
| 1 | LED channel written as a shared 1-based running counter | Emit a real OR'd **8-bit `channel_mask`** from explicit region/channel selection; never a counter. |
| 2 | Pan exported `×127` | Emit pan **×1 in −100..+100**. |
| 3 | `wave_type` (8th field) dropped → all sine | Emit `wave_type` when non-sine (emit a `0` placeholder in the 7th `freq_r` slot to hold position). |
| 4 | `freq_r` (7th field) dropped | Emit `freq_r` for single-channel binaural. |
| 5 | RGB dropped, lossy 5-field line | Always emit **8-field** LED line with `R G B`. |
| 6 | Noise emitted as freq=0 sine → silence | Noise = audio layer with `wave_type` 4/5/6, no carrier-freq dependency: `A t 0 0 vol 0 ch 0 4`. |
| 7 | Only `>` linear ever emitted | Full prefix support: `> *` ramps and `^ ~ / \ _` mods on every field. |
| 8/10 | Inheritance unresolved; channel binding unstable | Compiler/serializer carries prior value forward explicitly; **stable session-wide channel allocation** (Wizard/Lane). |
| 9 | No "off" line when a layer is removed | Emit terminal-boundary rows that zero/hold dropped fields. |
| 11 | Visual capped at 4 channels | Address all **8** LED channels (region chips + "Ch 5–8"/"All"=0xFF). |

---

## Shared module layout (all new files under `web/src/js/gen/`)

```
web/src/js/gen/
  model.js        # Row/Cell type constructors, helpers, NUM_* constants, wave/interp tables
  parse.js        # text -> { doc, diagnostics[] }   (mirrors config_parser.c byte-for-byte)
  serialize.js    # doc  -> text                      (8-field LED, minimal audio tokens, prefixes)
  validate.js     # semantic checks mirroring the C clamps (mask!=0, ranges, ramp-has-target...)
  transport.js    # play / stop / patch / state helpers
  macros.js       # convenience macros: binaural, delayed-tone, harmonics, fades
  generator.js    # the Generator-tab controller: shared-model bus, view switching, save/load wiring
  views/
    text.js       # Phase 2
    table.js      # Phase 3 (+ mobile card fallback)
    lane.js       # Phase 4 (canvas lanes + ramp/LFO regions)
    wizard.js     # Phase 5 (segment forms + Field/Transition/Pulse)
```

### Reuse vs new work (important — the existing browser parser is *lossy*)

There are **two** parsers in the system. The **device parser** (`main/config_parser.c`)
is the source of truth and already supports the **entire** grammar — **no firmware
or API change is needed**. The **browser parser** (`config.js`) is a client-only
helper built for the session report; it is a one-way *lossy read*, not an editor model.

What `config.js` provides and how the Generator reuses it:

| Existing piece (`config.js`) | Reuse in Generator |
|---|---|
| `playConfig`/`stopConfig` POST logic | **~as-is** → `gen/transport.js` |
| `bindFileInput`, `loadExample` (file/example load) | **~as-is** |
| interp **evaluators** `lerp`,`quad`,`interpField`,`audioStateAtTime`,`ledStateAtTime` | **as-is** → shared `gen/interp.js`, used for live preview/playhead |
| `parseValueInterp`,`parseConfigStructured` (structured read) | **skeleton only** — extended into the lossless parser |

The **genuinely new** Phase 1 work is therefore: a **lossless round-trip model**
+ a **serializer** (neither exists today), plus extending the parser to cover what
the browser one omits: the 5 modulation prefixes (`^ ~ / \ _` with
`start:end:period_ms`), audio `freq_r` (token 7) and `wave_type` (token 8),
channel-less audio lines, `BG` lines, and comment/blank-line/order preservation.
The lossy `parseConfigStructured` is kept usable for the report path; the new
lossless parser is the editing source of truth. Shared evaluators are factored
into `gen/interp.js` and re-imported by `config.js` so the report and Generator
agree on interpolation math.

### View-coexistence decision — **Option A (chosen)**

The existing **Home/Config tab raw editor (`exampleConfig` textarea + report
flow) stays independent and unchanged.** The Generator tab gets its **own**
model-synced Text view (Phase 2). This avoids any risk to the working
report/playback path (which reads `exampleConfig` directly). Two text editors
co-exist for now; consolidating Home into the Generator's Text view is a possible
later cleanup, explicitly deferred.

### The shared core model (precise JS structures — Phase 1 deliverable)

```js
// One compound "value + interpolation" cell. Used for every interpolatable field.
Cell = {
  value:       Number,                  // step value, ramp start, or modulation start
  interp:      'none'|'lin'|'quad'|'tri'|'sine'|'sawup'|'sawdn'|'sq',
  modEnd:      Number|null,             // periodic mods only (tri/sine/sawup/sawdn/sq)
  modPeriodMs: Number|null,             // periodic mods only
}

// A document is an ORDERED list of rows (preserves source order for round-trip)
// plus the single session-level BG descriptor.
LedDoc = { rows: Row[], bg: Bg|null }

// Row variants (discriminated by .kind). leading-blank/comment lines are their
// own rows so file order + comments survive a round-trip.
Row =
  | { kind:'blank' }
  | { kind:'comment', text:String }                     // a whole-line "# ..."
  | { kind:'raw',     text:String, error:String }       // unparsable line, held verbatim + flagged
  | { kind:'led',  time:Number, freq:Cell, duty:Cell, bright:Cell,
      r:Cell, g:Cell, b:Cell, mask:Number,              // mask 1..255
      legacy5:Boolean,                                  // true only if imported as 5-field & unedited
      inlineComment:String }
  | { kind:'audio', time:Number, freq:Cell, pan:Cell, vol:Cell, mod:Cell,
      channel:Number|null,                              // 1..16; null => omit token (defaults 0)
      freqR:Number,                                     // 0 => mono / omit token
      waveType:Number|null,                             // 0..6; null => omit token (sine)
      inlineComment:String }

Bg = { url:String, pan:Number, loudness:Number }        // pan -100..100, loudness 0..100
```

**Serialization rules** (`serialize.js`):
- LED rows always emit **8-field** (bug #5). A row keeps `legacy5:true` (and emits
  5-field) only if imported legacy *and* never edited — first edit clears it.
- Audio rows emit the **minimal trailing tokens**: drop `channel` if null, `freq_r`
  if 0 (unless a later token forces the slot — then emit `0`), `wave_type` if null.
  Bug #3: when `wave_type` is set, emit `0` in the `freq_r` slot if `freqR==0`.
- A `Cell` renders `value` with its prefix: `none`→bare, `lin`→`>v`, `quad`→`*v`,
  periodic→`<glyph>start:end:period_ms` (always emit explicit end+period — bug #7).
- Comments re-attached: whole-line comment rows verbatim; `inlineComment` appended.

**Round-trip invariants (unit-tested):** `parse(serialize(doc)) ≡ doc` and
`serialize(parse(text)) ≡ text` modulo opt-in whitespace normalization.

The LANE and WIZARD views need richer *intent* (lane names, segment names,
partial-ramp windows, true-stereo vs `freq_r` choice). That intent is stored as
**additive metadata comment lines** inside the same `.ledc` file
(`# @ave-lane v1 {json}` / `# @ave-wizard v1 {json}`), parsed as `comment` rows
the firmware ignores. **No separate sidecar file** — `configstore.js`'s
`NAME_RE` (`/^[A-Za-z0-9._-]+\.ledc$/`) requires the name to *end* in `.ledc`,
so a `name.ledc.json` sidecar would be rejected; the metadata-comment approach
keeps one file that is 100% valid `.led` and passes the name rule. (See
Open-items if a true sidecar is ever wanted.)

---

## Phase 1 — Shared core (model + parser/serializer + tab scaffold + transport)

**Goal:** a tested, lossless `.led` engine, a "Generator" nav tab shell, and
save/load + transport wiring. No editing UI yet beyond a read-only serialized
preview, but everything downstream builds on this.

**Firmware impact: none.**

### Steps
1. `gen/model.js`: define `Cell`, `Row`, `LedDoc`, `Bg` constructors + helpers
   (`emptyDoc()`, `cell(value)`, `isModInterp()`), and constants
   (`NUM_AUDIO_CHANNELS=16`, `NUM_LED_CHANNELS=8`, `MAX_ENTRIES=100`,
   `WAVE_TYPES`, `INTERP_GLYPHS`, the prefix↔interp table).
2. `gen/parse.js`: port + extend `config.js`'s parser to a faithful mirror of
   `parse_line`/`parse_led_line`/`parse_audio_line`/`parse_bg_line`/
   `parse_value_with_interpolation`/`parse_mod_extras`. Must additionally handle
   (the bits `config.js` lacks today): the 5 modulation prefixes with
   `start:end:period`, audio tokens 7 (`freq_r`) & 8 (`wave_type`), per-channel
   RGB interp, `mask==0` rejection, channel/wave range checks. Returns
   `{ doc, diagnostics:[{line,severity,msg}] }`; bad lines become `raw` rows.
3. `gen/serialize.js`: `serialize(doc)` per the rules above (8-field LED, minimal
   audio tokens, explicit mod end/period, comment re-attach). Satisfies the
   Bug-avoidance contract by construction.
4. `gen/validate.js`: semantic validator mirroring C clamps — `mask!=0`, audio
   `channel 1..16`, LED mask bits 1..8, `freq ≤ SAMPLE_RATE/2`, RGB clamp warn,
   modulation requires end+period, `>`/`*` ramp **warns** if no following
   same-channel/field entry to target, `MAX_ENTRIES`/batch-size warnings.
5. `gen/transport.js`: `playDoc(doc)` (`serialize` → `POST /api/play-config`
   text/plain), `stop()` (`POST /api/stop`), `patchLine(line)`
   (`POST /api/patch-config`), `getState()` (`GET /api/state`), `getCaps()`
   (read channel caps from `/api/state` if present, else fall back to the
   `NUM_*` constants — see Open-items).
6. Nav scaffold: add `'generator'` to `TABS` in `nav.js`; add a
   `<button class="tab" data-tab="generator">Generator</button>` and a
   `<div class="page" id="page-generator">` shell in `index.html` (view-switch
   sub-tabs Text/Table/Lanes/Wizard + a transport bar: Play/Stop + "⚡ Apply
   live" + a row-count meter + a read-only serialized preview pane).
7. `gen/generator.js`: the tab controller. Owns the single `doc`, an event bus
   (`onModelChanged`) the views subscribe to, view switching, and wires
   `configstore.js` Load/Save/Save-As + the transport bar. Import + init from
   `main.js` (`bind(...)`, `generatorInit()`).
8. Save/Load: reuse `configstore.js` as-is (`refreshConfigList`, `loadSelected`,
   `saveCurrent`, `saveAsDialog`) — Load parses the loaded text into `doc`; Save
   serializes `doc`. (The Generator gets its own load/save buttons bound to the
   same store functions; **Option A**: the Home/Config tab textarea + report flow
   stays independent and unchanged.)
9. `npm run build` clean.

### Files
- New: `gen/model.js`, `gen/parse.js`, `gen/serialize.js`, `gen/validate.js`,
  `gen/transport.js`, `gen/generator.js` (+ `gen/interp.js` for the shared lerp/quad).
- Modify: `web/src/index.html` (nav button + page shell), `web/src/js/nav.js`
  (add tab), `web/src/js/main.js` (import+init), `web/src/js/config.js`
  (re-import migrated parser fns), `web/src/css/style.css` (tab/preview styling).

### Acceptance criteria
- `parse`→`serialize` round-trips `config_parser_get_example()` (served at
  `/api/example`) byte-for-byte (modulo whitespace normalization).
- A corpus of saved `.ledc` files round-trips losslessly; comments/blank lines
  and the `BG` line survive.
- Load → Play sends serialized text that the device parses without warnings for
  a known-good config; Stop works; Apply-live posts a single patch line.
- Build clean; gz assets regenerate.

### Coverage note
Engine-level: can represent and round-trip **every** `.led` construct (all
LED/audio/BG fields, all 7 prefixes, comments). No authoring UI yet — coverage
is exercised via the read-only preview + transport.

---

## Phase 2 — Text view (raw `.led` editor synced to the model)

**Goal:** a first-class raw-text editor kept in sync with the shared model — the
always-available escape hatch. Largely the existing Home textarea behavior,
adapted to drive `doc`.

**Firmware impact: none.**

### Steps
1. `gen/views/text.js`: a `<textarea>` bound to `doc`. On `onModelChanged`
   (from another view), write `serialize(doc)` into the textarea **only when it
   is not focused** (no caret fight).
2. On textarea `input`, **debounce ~250 ms**, then `parse(text)`:
   - parse clean → replace `doc`, emit `onModelChanged`, badge `✓ synced`.
   - parse error on line N → **keep last-good `doc`**, badge `⚠ N error(s)`,
     render a gutter marker + tooltip (the exact diagnostic) on offending lines;
     hold those as `raw` rows so the rest still round-trips.
3. A "Problems" strip beneath the editor summarizing `validate(doc)` +
   parse diagnostics, click-to-jump to the line.
4. `npm run build` clean.

### Files
- New: `gen/views/text.js`. Modify: `gen/generator.js` (register view),
  `index.html` (textarea + gutter container in the generator page), `style.css`.

### Acceptance criteria
- Typing valid `.led` updates the model; switching to the preview shows the same
  text. An invalid line shows a precise error and does not corrupt the model.
- Round-trip badge reflects `✓ synced` vs `~ formatted` correctly.

### Coverage note
**100%** — anything expressible in `.led` can be authored here (escape hatch).

---

## Phase 3 — Table view (spreadsheet grid + compound cell + mobile cards)

**Goal:** a typed spreadsheet grid over `doc.rows`, with the compound
"value+interp" cell, bidirectional sync with model/text, and a mobile
card-per-row fallback.

**Firmware impact: none.**

### Steps
1. `gen/views/table.js`: render a single time-sorted grid of rows. A leading
   **Type** dropdown {LED, A, BG, comment} per row; a filter chip set to
   show/hide LED-only / Audio-only rows. Columns (typed widgets + validation):

   | Col | Applies | Widget | Validation |
   |---|---|---|---|
   | Time | LED/A | numeric + `mm:ss.mmm` helper | uint32 ≥0; dup-time allowed (batch) |
   | Freq | LED/A | compound cell | LED ≥0; A ≤ SR/2 |
   | Duty | LED | compound cell | 0..100 |
   | Bright | LED | compound cell | 0..100 |
   | Color (R/G/B) | LED | swatch → picker, each R/G/B a compound cell | 0..255, clamp warn |
   | Channels | LED | 8 chips (1..8) → mask; presets "legacy 4"(0x0F)/"all"(0xFF) | mask≠0 |
   | Pan | A | compound cell | −100..100 |
   | Vol | A | compound cell | 0..100 |
   | Mod | A | compound cell | ≥0 |
   | FreqR | A | numeric (no interp) | 0 or ≤ SR/2 |
   | Wave | A | dropdown sine..brown | 0..6 |
   | Channel | A | dropdown 1..16 | required when mixing channels |
   | URL/Pan/Loud | BG | text + numerics | scheme + ranges |

2. **Compound cell** (`gen/views/cell.js`, shared with Lane/Wizard): inline
   glyph render (`→` lin, `x²` quad, wave icon for mod, plain for step, `■→■`
   for color ramp) + a popover/inspector editing the full `Cell` struct (radio:
   Step / `>` / `*` / `^` / `~` / `/` / `\` / `_`; `end` + `period` fields appear
   only for periodic mods; ramp shows the resolved target "→ value at t=next").
3. Bidirectional sync: grid edit mutates the relevant `Row` field → emit
   `onModelChanged` (text + preview reflect it). Incoming `onModelChanged`
   re-renders the grid (preserving selection/scroll).
4. Row actions (⋮): duplicate, insert-after (copies time), delete, move-up/down.
   "+ Add row" (LED / A / BG).
5. **Mobile card fallback** (`@media (max-width:640px)`): replace the grid with
   stacked cards (`Type · Channel · Time` header; primary fields visible,
   `more ▾` expands freq_r/wave/mod); interp popovers + color picker become
   full-screen sheets; `+ Add entry` at the bottom.
6. `npm run build` clean.

### Files
- New: `gen/views/table.js`, `gen/views/cell.js`. Modify: `gen/generator.js`,
  `index.html`, `style.css`.

### Acceptance criteria
- Editing any cell updates text/preview; editing text updates the grid.
- The compound cell can produce every prefix incl. `^ ~ / \ _` with
  `start:end:period`; channel chips emit a correct OR'd mask (bug #1); color
  emits 8-field RGB (bug #5); pan stays ±100 (bug #2); wave/freq_r emit correctly
  (bugs #3/#4); noise selectable via Wave=white/pink/brown (bug #6).
- Mobile cards render and edit below 640 px.

### Coverage note
All **per-row** constructs (every field, all prefixes, mask, RGB, BG). Does not
add cross-row authoring conveniences (those are Lane/Wizard).

---

## Phase 4 — DAW lane view (canvas lanes + ramp/LFO regions)

**Goal:** a zoomable, touch-first canvas with one lane per used channel, each
lane stacking per-field automation sub-lanes; the automation-curve ideas fold in
here as per-field ramp/LFO **regions**. Serializes back to the flat model via the
implicit-breakpoint rule, with lossless intent in a `# @ave-lane` metadata
comment.

**Firmware impact: none.**

### Steps
1. `gen/views/lane.js`: a **lane projection** built from `doc` — group audio rows
   by `channel`, LED rows by `mask` → one lane each. Lane = stack of automation
   sub-lanes:
   - Audio: Carrier(freq) / Pan / Volume / Iso(mod); side-channels Binaural
     (`freq_r`, stepped ghost curve) + Wave markers (`wave_type`).
   - LED: Flicker(freq) / Duty / Brightness / Color (expandable to R/G/B).
   Each sub-lane is a sparse list of keyframes; each keyframe owns the **outgoing
   segment shape** (`step`/`lin`/`quad`/periodic) — isomorphic to the per-field
   prefix model. (Reconcile design-1's `{curve, mod}` with design-3's unified
   `shape` + `lfoEnd`/`lfoPeriodMs` — use the unified `shape` form.)
2. **Canvas + pointer gestures** (mouse/touch/pen, 44px hit targets,
   requestAnimationFrame-batched, canvas not DOM nodes):
   tap-empty=add keyframe; tap=select+inspector; drag=move (t snaps to grid, v
   to nice values); long-press=context menu (delete/dup/set-curve/convert-to-mod);
   segment-midpoint handle cycles step→`>`→`*`; pinch=zoom time (10 ms…whole
   session); two-finger drag=pan; ruler drag=scrub playhead (from `/api/state`
   `position_ms`). Flat undo/redo command stack (Ctrl/Cmd-Z) is mandatory.
3. **Ramp/LFO regions** (folded automation curves): selecting a periodic shape
   turns the segment into an oscillation band drawn between a **start rail**
   (keyframe value) and an **end-handle** rail (`lfoEnd`), with a draggable
   **period handle** (one wavelength on the time axis; tiled waveshape; a
   "≈ N cycles" readout); numeric `end`/`period_ms` mirror in the inspector.
   Bands run until the next same-field keyframe.
4. **Serializer (the critical correctness rule):** per channel, take the union
   of all sub-lane keyframe times; emit one row per time restating every field.
   - **Implicit breakpoint / resample at the union grid:** a `>`/`*` ramp crossed
     by a foreign-field keyframe at interior time `t1` emits the ramping field's
     *interpolated* value at `t1` carrying the **same prefix**, so
     `t0→t1→t2` reproduces the ramp (piecewise-linear = linear). Quadratic
     crossed by a foreign keyframe → quad-then-quad approximation, exact intent
     preserved in `# @ave-lane` metadata, UI shows an "≈" badge.
   - **LFO interior conflict:** re-emitting a field mid-LFO restarts its phase.
     v1 rule: the editor **forbids** placing a breakpoint on field Y at an
     interior time while field X carries an LFO on the same channel; it warns and
     offers to snap Y's points to band boundaries. (Documented limitation.)
   - One lane = one single-bit channel by default; multi-bit masks are an
     explicit advanced grouping (firmware per-bit lookup caveat).
5. **Round-trip:** on save, write a `# @ave-lane v1 {json}` comment capturing
   lane names / collapsed state / exact quad spans / split-color choices. On
   open, presence of that comment → exact restore; absence → reconstruct lanes
   from raw rows (group by channel/mask; coalesce consecutive same-curve segments
   whose interior values lie on the interpolation line, undoing implicit
   breakpoints; detect mod glyphs; `freq_r`→binaural; `wave_type`→markers).
6. Respect `MAX_ENTRIES=100` / batch 50 — warn before exceeding.
7. `npm run build` clean.

### Files
- New: `gen/views/lane.js`, `gen/views/lane_canvas.js` (render+gesture),
  `gen/views/lane_serialize.js` (union-grid + implicit-breakpoint), `gen/undo.js`.
  Modify: `gen/generator.js`, `index.html`, `style.css`.

### Acceptance criteria
- Authoring a linear ramp across a foreign keyframe round-trips to a piecewise
  ramp the device plays identically (verified by the JS interpreter; spot-checked
  on device by the orchestrator).
- LFO regions emit correct `<glyph>start:end:period_ms`; importing them rebuilds
  the band.
- `# @ave-lane` metadata gives byte-perfect reopen; a hand-written `.led` (no
  metadata) imports into sane lanes.
- Coalescing reverses implicit breakpoints on reopen (no row explosion).

### Coverage note
All single-field and cross-field automation incl. ramps and all 5 periodic mods
per field; binaural via `freq_r`; per-channel wave markers; multi-bit LED masks
(advanced). The interior-LFO-conflict case is intentionally constrained.

---

## Phase 5 — Wizard view (segment forms + Field/Transition/Pulse + macros)

**Goal:** the lowest-floor, mobile-first guided authoring path. Users build a
session as a vertical stack of **segments**, each containing **layers**; a
**segment compiler** flattens to the shared model. Convenience macros
(delayed tones, harmonics, fades, binaural) live here and in `gen/macros.js`.

**Firmware impact: none.**

### Steps
1. `gen/views/wizard.js`: the wizard projection — a `session` of contiguous,
   non-overlapping segments (the user never types a timestamp; absolute time =
   running sum of durations):
   ```js
   session = { name, version:1, bg:null|Bg, segments:[Segment] }
   Segment = { id, name, duration_ms, layers:[Layer] }
   AudioLayer = { kind:'tone'|'binaural'|'noise', channel, channelR,
                  wave_type, fields:{ freq, freqR, beat, pan, volume, mod } }
   LightLayer = { kind:'light', channelMask, fields:{ freq, duty, bright, r, g, b } }
   ```
2. **The reusable `Field` abstraction** (`gen/field.js`, shared concept):
   ```js
   Field = {
     value: Number,
     ramp:  null | { shape:'linear'|'quadratic', window:'whole'|{first_ms} },  // Transition
     mod:   null | { wave:'triangle'|'sine'|'sawup'|'sawdown'|'square', end, period_ms }, // Pulse
   }
   ```
   - **Transition** = the `>`/`*` ramp ("Ease in from previous / Jump instantly";
     Smooth=`*` / Steady=`>`; over "whole segment" or "first N s"; live sparkline).
   - **Pulse** = the `^ ~ / \ _` modulation ("Wobble between [start] and [end]
     every [n] s"; shape icons; animated sparkline; "+ add another wobble").
   - Each field independently owns ramp+mod → per-field automation supported
     natively. Collapsed chip shows `↗` (ramp) / `∿` (mod) badges.
3. **Progressive-disclosure UI** (mobile-first, 360–430 px; pure vertical scroll;
   layer editor = bottom sheet): Tier 0 chip summary → Tier 1 preset+base
   sliders → Tier 2 ▸Transition / ▸Add a pulse → Tier 3 ▸Advanced (pan, waveform,
   explicit carrier-R, manual channel pin, split RGB, channel-mask multiselect).
   Desktop ≥1024 px: left mini-map (navigation only) + right segment list +
   read-only `.led` preview.
4. **Segment compiler** (`gen/views/wizard_compile.js`) → flat `doc`:
   - **Channel allocation** (session-wide, stable): assign a fixed channel per
     layer across all segments (binaural = one `freq_r` channel by default, or
     two panned channels in true-stereo; tone/noise = one each), respecting
     16 audio / 8 LED caps (bug #8/#10). LED regions → mask bits (Inner-L 0x01,
     Outer-L 0x02, Outer-R 0x04, Inner-R 0x08, "Ch 5–8", "All"=0xFF; bug #11).
   - **Phase A flatten:** `start[0]=0`, `start[i]=start[i-1]+duration[i-1]`; each
     segment emits its layers' steady values at its own start; a **terminal
     boundary** zeroes/fades dropped fields (bug #9).
   - **Phase B ramps:** put the `>`/`*` prefix on the *previous* boundary's row
     (start value) so the engine ramps to the next row's value; partial windows
     (`first_ms`) insert an extra anchor row.
   - **Phase C mods:** emit `PREFIXstart:end:period_ms` in place; re-state the
     field at the next boundary so the mod ends cleanly (unless "keep pulsing").
   - Units: pan ×1, volume ×1, RGB 0..255, period s→ms; always 8-field LED.
   - Respect `MAX_ENTRIES=100` (live "X / 100 rows used" meter; coalesce
     unchanged-field rows).
5. **Macro library** (`gen/macros.js`, also usable from Table/Lane):
   - **Binaural:** one layer → one `freq_r` channel (default) or two hard-panned
     channels; "beat" slider drives R = base + beat.
   - **Stereo delayed tone:** two channels, same carrier, one pan −100 / one
     pan +100, the second entry's `time_ms` offset later by `delayTime` ms.
   - **Harmonic layering:** one channel per harmonic (octave ×2, fifth ×1.5,
     …), each an ordinary `A` line scaled by `harmonicVolume`, capped at 16 ch.
   - **Per-tone fade in/out:** volume-field `>` (or `*`) ramps — fade-in from 0
     to target over `fadeDuration`; fade-out `>0` into the terminal boundary.
   - **Presets:** brainwave (Delta 2 / Theta 6 / Alpha 10 / Beta 18 / Gamma 40),
     color ("calm blue"…), noise (rain≈pink, surf≈brown), segment + session
     templates.
6. **Round-trip:** save embeds `# @ave-wizard v1 {json}` (the `session` model) in
   the `.ledc`. Open prefers that comment for lossless reopen (segment names,
   presets, partial-ramp windows). No matching comment → best-effort structural
   import: parse with `gen/parse.js`, infer segments from timestamp clusters
   (each full restatement = a boundary), ramps→Field.ramp, mods→Field.mod,
   binaural detected via two channels sharing a carrier or one with `freq_r`;
   flag "Imported — names/structure are guesses". (Lossy on intent, never on
   playback.)
7. **Live tuning bridge:** a slider can drive `POST /api/patch-config` (one-line
   patch) for instant feedback before committing.
8. `npm run build` clean.

### Files
- New: `gen/views/wizard.js`, `gen/views/wizard_compile.js`, `gen/field.js`,
  `gen/macros.js`. Modify: `gen/generator.js`, `index.html`, `style.css`.

### Acceptance criteria
- A 3-segment session (e.g. Alpha glide 12→8 Hz, deep hold, wake-up ramp)
  compiles to a valid `.led` the device plays with correct ramps and stable
  channels; reopening from the embedded comment restores segment names/intent.
- Each macro expands to correct multi-channel/multi-entry rows (verified via the
  serialized preview).
- All four bug-avoidance properties hold in compiler output (mask, pan, wave,
  RGB, freq_r, terminal off-rows).

### Coverage note
The dominant therapeutic use case (sequenced phases, binaural, fades, presets)
end-to-end, plus the 3 convenience macros. Power-user single-field automation
beyond segment granularity routes to Lane/Table.

---

## Shared coverage table (every `.led` construct → which view handles it)

| `.led` construct | Text | Table | Lane | Wizard |
|---|:--:|:--:|:--:|:--:|
| LED 8-field (freq/duty/bright/RGB/mask) | ✅ | ✅ | ✅ | ✅ (region chips) |
| LED 5-field legacy (import) | ✅ | ✅ (auto→8) | ✅ | ✅ |
| `channel_mask` (OR'd, 8 ch) | ✅ | ✅ chips | ✅ lane header | ✅ regions |
| Audio freq/pan/vol/mod | ✅ | ✅ | ✅ | ✅ |
| Audio `channel` 1..16 | ✅ | ✅ | ✅ | ✅ (allocator) |
| `freq_r` (binaural) | ✅ | ✅ | ✅ ghost curve | ✅ beat slider |
| `wave_type` 0..6 (incl. noise 4/5/6) | ✅ | ✅ | ✅ markers | ✅ preset |
| `>` linear / `*` quadratic ramp | ✅ | ✅ cell | ✅ region | ✅ Transition |
| `^ ~ / \ _` periodic mods `start:end:period` | ✅ | ✅ cell | ✅ LFO band | ✅ Pulse |
| Per-field independent interp | ✅ | ✅ | ✅ | ✅ |
| `BG` line | ✅ | ✅ row | ✅ (`bg`) | ✅ |
| Comments / blank lines | ✅ verbatim | ✅ rows | ✅ (metadata) | ✅ (metadata) |
| Convenience macros (delay/harmonics/fades) | manual | via macro lib | via macro lib | ✅ native |
| Cross-field implicit breakpoints | manual | manual | ✅ auto | ✅ auto |

Escape hatch: any construct not comfortable in a higher view is always editable
in **Text**, which is 100% complete.

## Mobile strategy

- **Single breakpoint** `@media (max-width:640px)` across all views.
- **Wizard is the mobile-default** (pure vertical scroll, bottom-sheet layer
  editor, big targets, no pinch/drag-precision) — the lowest floor.
- **Table → card-per-row** below 640 px (`Type · Channel · Time` header, `more ▾`
  disclosure, full-screen interp/color sheets).
- **Lane** is touch-built but desktop-leaning on phones: show one focused lane in
  detail + a thin all-lanes minimap, vertical flick between lanes, pinch-zoom on
  time, bottom-sheet inspector, nudge buttons `◀10 ◀1 1▶ 10▶` for exact values.
- **Text** is a plain full-width textarea — always usable on mobile as the
  fallback for any field a view doesn't surface.
- All interactions pointer-event based (no hover affordances); 44×44 px minimum
  hit targets.

## Verification / open-items (confirm against firmware before/while building)

1. **Pan scaling — ×127 vs ±100.** Canon is **±100** (`parse_bg_line` clamps
   ±100, `config_parser.c:1402-1406`; audio apply does `e->pan/100.0f`,
   `:815`/`:843`). Emit ±100; do **not** copy the old ×127.
2. **Channel field semantics.** LED channel field is an **8-bit `channel_mask`**
   (`parse_led_line`, mask==0 rejected `:1269`). The old shared `channelCounter`
   is wrong — never emit a counter. Audio uses a separate `channel` token (1..16);
   confirm the device's internal 0- vs 1-based mapping (file token is the literal
   channel; `apply_patch_audio_entry` indexes `e->channel` directly and rejects
   `>= NUM_AUDIO_CHANNELS`, and `find_next_audio_for_bit` uses `1u<<channel`).
   Round-trip preserves the literal token regardless; confirm the live-state map
   (`/api/state.audio` is keyed 1-based, `.led` 0-based) when wiring playhead.
3. **Patch-path `mod_depth=0`.** `apply_patch_audio_entry` starts a fresh channel
   with `audio_gen_params_t p={0}` → `mod_depth=0` (`:812-819`) → isochronic via
   the `mod` field is inaudible over `/api/patch-config`; the timeline path uses a
   hardcoded shallow `mod_depth≈0.1`. Don't promise true gated isochronic from
   the `mod` field; set UI expectations. Confirm the update-existing-channel
   branch behavior.
4. **`led_strip.h:104` stale "0..3" channel-map comment** vs `NUM_LED_CHANNELS=8`
   (`led_strip.h:32`, masks to 255). Confirm addressable backends really drive 8
   logical channels before exposing all 8 chips; DIRECT backend is brightness-only
   (ignores R/G/B) — auto-hide color controls when caps say no color.
5. **`/api/state` shape / caps.** Confirm whether `/api/state` returns channel
   caps (`caps.num_audio_ch`/`num_led_ch`/`led_color`) and `position_ms`/
   `running`; if not, fall back to the `NUM_*` constants and poll for playhead.
6. **`.led` vs `.ledc` extension.** `configstore`'s `NAME_RE` requires a name
   ending in `.ledc`; the upload accepts `.led/.ledc/.txt`. Keep saving `.ledc`;
   embed higher-view intent as `#` metadata comments (not a `.ledc.json` sidecar,
   which `NAME_RE` would reject). If a true sidecar is ever wanted, that needs a
   one-line web-only `NAME_RE` relaxation — no firmware change.
7. **No `/api/validate`.** `config_parser_validate_syntax` is not exported — all
   validation is client-side (`gen/validate.js`); the only server round-trip is
   `play-config`.

## Testing strategy

- **Round-trip property tests** (`web/test/`, plain Node, run in `npm test`): for
  a corpus of `.led` samples (the firmware example string `/api/example`, the
  in-tree `ledc/*.ledc`, plus hand-written edge cases) assert
  `parse(serialize(parse(text))) ≡ parse(text)` and field-level equality.
- **Bug-regression tests:** assert serializer output for crafted models — mask is
  a real bitmask (not a counter), pan ±100, 8-field RGB present, `wave_type` +
  `freq_r` emitted with the `0` placeholder rule, noise as `wave 4/5/6`, all 7
  prefixes emitted with explicit mod end/period.
- **Parser-mirror drift guard:** a dev script diffs `gen/parse.js` behavior
  against the C grammar's example; **treat `config_parser.c` as canonical** — if
  the firmware grammar changes, this test fails first.
- **Implicit-breakpoint tests (Phase 4):** ramp-across-foreign-keyframe serializes
  to a piecewise chain that the JS interpreter evaluates identically to the
  intended curve; import coalesces it back without row explosion.
- **Compiler tests (Phase 5):** segment sessions compile to expected flat rows
  (stable channels, correct ramp wiring, terminal off-rows); macros expand to the
  documented multi-entry forms.
- **Device spot-checks (orchestrator only):** after each shippable phase, the
  orchestrator may `flash_web.sh` and `POST /api/play-config` a generated file to
  confirm on-hardware behavior. Subagents never flash.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| JS parser drifts from `config_parser.c` | Mirror it byte-for-byte; drift-guard test against the example; C is canonical. |
| Ramp sweep direction ambiguity (prefix-on-this vs example comments) | Follow the runtime convention in `log_entry_summary`/`execute_timeline_entry_ctx` (prefix on start entry, target = next); spot-check on device; round-trip is unaffected. |
| Interior LFO + foreign keyframe restarts phase | v1 forbids it with a guided fix (snap to band boundaries); documented limitation. |
| `MAX_ENTRIES=100` overflow from segment×layer×anchor explosion | Live row-count meter; coalesce unchanged fields; warn before exceeding. |
| Canvas perf with dense keyframes on phones | Canvas (not DOM), rAF-batched, one focused lane + minimap on mobile. |
| Lossy import of hand-written files into Lane/Wizard | Never lossy on playback; flag "structure is a guess"; offer trust-file vs trust-editor on metadata drift. |

## Out of scope

- Any change to `freeesp32_ave_generator` (the old external app) — superseded.
- Firmware/C changes (none required; the 3 conveniences are macros).
- New device endpoints — reuse `play-config`/`patch-config`/`stop`/`state`/
  `configs`/`example`/`mod`/`appconfig`.
- True isochronic gating beyond the device's `mod_depth` behavior (flagged, not
  promised).
- A real `.ledc.json` sidecar (blocked by `NAME_RE`; using metadata comments
  instead).

---

## Todo list

- [ ] **Phase 1 — Shared core** (model, parser, serializer, validate, transport,
      tab scaffold, save/load) — round-trip green on example + corpus.
  - [ ] `gen/model.js` types + constants
  - [ ] `gen/parse.js` faithful mirror (+ mods, freq_r, wave_type, RGB interp)
  - [ ] `gen/serialize.js` (8-field LED, minimal audio tokens, explicit mod extras)
  - [ ] `gen/validate.js` semantic checks
  - [ ] `gen/transport.js` play/stop/patch/state
  - [ ] nav tab + page shell + `gen/generator.js` controller + configstore wiring
  - [ ] migrate `config.js` parser fns; build clean
- [ ] **Phase 2 — Text view** synced to model + diagnostics gutter
- [ ] **Phase 3 — Table view** grid + compound cell + mobile cards
- [ ] **Phase 4 — DAW lane view** canvas + ramp/LFO regions + implicit-breakpoint
      serializer + `# @ave-lane` round-trip + undo/redo
- [ ] **Phase 5 — Wizard view** segment forms + Field/Transition/Pulse + segment
      compiler + macro library + `# @ave-wizard` round-trip
- [ ] Round-trip + bug-regression + compiler test suites
- [ ] Orchestrator device spot-checks per phase (no subagent flashing)
