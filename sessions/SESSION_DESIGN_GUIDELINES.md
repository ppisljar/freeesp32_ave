# Session Design Guidelines

> The normative rulebook for authoring entrainment sessions in our `.ledc` format.
> Distilled from the **Entrainment Techniques Playbook**
> (`reports/non_planned_reports/entrainment_techniques_playbook.md`, 3-pass patent +
> literature research) and the earlier session research (`reports/session_research/`,
> start at `000_SYNTHESIS.md`). Every `.ledc` in `sessions/library/` MUST follow these
> rules and pass `sessions/validate_session.mjs` (0 errors).
>
> **Framing is experiential, not medical.** Band→state mappings and ramp/dwell numbers
> are largely practitioner/device convention, not settled neuroscience; the 40 Hz
> amyloid claim is contested (the *entrainment* itself is solid). Say so in session
> headers. Confidence tags: **[E]** evidence, **[P]** product/practitioner practice,
> **[W]** weak/marketing.

---

## 1. The 4-phase arc

Every session is timestamped segments joined by `>` (linear) / `*` (quadratic) ramps.
Four phases:

1. **Onset / Induction** — start where the brain already is (~10–14 Hz alpha), hold
   1–3 min. This is where light and volume **fade IN** (audio ≥30 s, light ≥10 s).
2. **Deepen** — glide from onset toward the target band, slope-limited (§3).
3. **Hold** — sit at the target band; the therapeutic core. **Dwell ≥5–6 min**
   (10–20 min for solid theta/delta). **[E/P]**
4. **Return** — ramp back toward alpha/SMR before "eyes open."

**Goal-specific endings** (do NOT use a generic return for these):
- **Sleep / lucid** — skip Return. End in a **Soft Off**: brightness AND volume fade
  to zero *simultaneously* at the low target rate. **No wake-up.** **[P]**
  (See `01_sleep_onset`, `30_splitfield_lucid`.)
- **Focus / energize** — end **bright and alert**: ramp *up* toward 12–15 Hz and
  *brighten* the light; never fade to dark (avoids grogginess). **[P]**
  (See `05_focus_smr` — ends at 40 % brightness, 12 Hz.)
- **Meditation / relaxation** — Return to alpha, then a short calm close.

## 2. Goal → band → parameters

Bands are convention; **gamma 40 Hz is the highest-evidence target**. **[E]**

| Band | Hz | State | Typical target | Ending | Conf. |
|------|-----|-------|----------------|--------|-------|
| Delta | 0.5–4 | deep sleep | 1–3 (glide down to it) | Soft Off, dark, warm amber | [P]+δ [E] |
| Theta | 4–8 | meditation, hypnagogia, lucidity | 6 (7.83 optional) | Return to alpha | θ [E], 7.83 [W] |
| Alpha | 8–13 | relaxed wakefulness ("the bridge") | **10** | most reliably entrainable **[E]** | [E] |
| SMR / low-beta | 12–16 | calm focus | 14 (audio-carried) | bright/alert | [E/P] |
| Beta | 16–25 | energize | 18–20 | bright, cool white | [P] |
| Gamma | 30–45 | cognition, 40 Hz protocol | **40 fixed** | fade out | **[E] best** |

## 3. Ramp-slope guidance

The frequency-following response can only track a **slow glide**. **[E]**
- **Sleep descents:** ~0.1–0.5 Hz/min (`01_sleep_onset` glides 10→2 Hz at ~0.44 Hz/min).
- **Meditation/relaxation:** ~0.5 Hz/min (`04_meditation_theta` glides 10→6 at 0.5 Hz/min).
- **Energize / wake:** up to ~2 Hz/min.
- **Hard ceiling:** keep **≤2 Hz/min**; the validator (and reviewers) **WARN above
  ~3 Hz/min**. Even 0.3 Hz can flip the effect (Siever). **[P]**

**Ramping a binaural beat:** `freq_R` is **NOT interpolatable**. Hold `freq_R` FIXED
and ramp the carrier `freq` so beat = `freq_R − freq` slides smoothly (see §7). For an
**isochronic** beat slide, ramp the audio `mod` field directly — that one IS interpolatable.

## 4. Stimulation: drivers & carriers

- **Light flicker is the strongest driver; isochronic > binaural** for waking states.
  Light pulse and audio pulse run **1:1** at the same Hz. **[E]**
- **Entrain with brightness/white; use RGB for mood, circadian, and SSVEP colour drive.**
- **Combined audio+visual is superadditive** — stronger steady-state than either alone. **[E]**

### Carrier-frequency table (audio) — `BRAINWAVE_PRESETS` in `web/src/js/gen/macros.js`
Beat perception peaks near a **~400 Hz** carrier (Δf ≤ ~35 Hz); ~340 Hz is best for the
40 Hz gamma beat. **[E]** Keep carrier ≤ ~400 Hz, beat ≤ ~35 Hz.

| Band | Carrier | Beat | Preset key |
|------|---------|------|------------|
| Delta | 200 Hz | 2 | `delta2` |
| Theta | 250 Hz | 6 | `theta6` |
| Alpha | 370 Hz | 10 | `alpha10` |
| Beta | 420 Hz | 18 | `beta18` |
| Gamma | 340 Hz | 40 | `gamma40` |

### Monaural vs binaural
- **Binaural** (`freq_R`): two detuned carriers, one per ear; beat forms *in the brain*
  (~3 dB effective modulation). Needs stereo separation / headphones. Reserve for
  gentle/low/sleep content. Carriers <1000 Hz, L/R offset <35 Hz. `binaural()` macro.
- **Monaural** (`monaural()` macro): two centre-panned channels (`pan=0`) at `base` and
  `base+beat`. The beat forms **acoustically in the air** (~50 dB modulation), so it
  **works on a single speaker or one ear** — the stronger, more robust driver. Prefer
  monaural for speaker playback and waking states.
- **Isochronic** (audio `mod` at the beat Hz): the hardest driver for waking states.
- **Harmonic stack** (`harmonicStack`/`harmonicCarriers` macros): N octave carriers
  sharing one Δf (100/106, 200/206, 400/406 @ Δf6), upper octaves −4.5 dB/oct and 1/N
  auto-scaled so the sum stays inside headroom. Richer bed, reinforces the FFR; optional
  20 Hz subharmonic layer boosts 40 Hz gamma. **[E]**

### Flicker waveform / envelope (LED + audio pulse — v2 `env` field)
- `env=0` **square** — strongest drive, hardest edges (LED legacy default).
- `env=1` **sine** gate — gentlest, far less fatigue; every therapeutic AVS uses sine
  ≤10 Hz. Used by `15_ganzflicker_imagery`. **[E]**
- `env=2` **triangle** gate.
- `env=3` **trapezoid** gate — crisp isochronic bursts; uses `duty` + `attack` (raised
  edge, click-safe ≥2 ms). Used by `29_noise_sleep_pulsed` (40 % duty, 50 ms attack).
- `env=4` **sine tremolo** (audio only, bipolar — audio legacy default).
- **Duty:** 50 % default; 25–40 % for gamma; **4 %** for the GENUS click (`07_genus_40hz`).

### Colour / SSVEP strength — `COLOR_PRESETS`
At matched luminance, **spectral extremes drive far stronger SSVEP**. Prefer amber/red
or blue for *drive*; green is ambience only. **[E]**

| Colour | R,G,B | SSVEP (dB) |
|--------|-------|-----------|
| Warm amber | 255,160,32 | 8.06 |
| Deep red | 255,24,16 | 8.06 |
| Calm blue | 32,96,255 | 6.82 |
| Cool cyan | 32,220,255 | 6.82 |
| Soft green | 32,220,96 | 2.85 (weak) |

**Eyes-closed** sessions must bias **amber/red ≥620 nm**: the eyelid passes ~14.5 % at
700 nm but ≤3 % below 580 nm, so only warm light reaches the closed-eye retina
(`31_ganzfeld_amber`). **[E]**

**Invisible / flicker-free spectral flicker** (`FLICKER_PAIRS`): two complementary-colour
banks run 180° antiphase so mean luminance stays flat (no visible flicker) while the
retina still gets the oscillation. `amberBlue` is both the strongest SSVEP pair *and*
luminance-complementary; `redCyan` is the runner-up. Requires per-channel `phase` and an
RGB backend (`27_genus_40hz_dim` — needs firmware, see §8).

## 5. The breath layer (coherence, 0.1 Hz)

Independent of the entrainment frequency, a **0.1 Hz** LFO (≈6 breaths/min) on
*brightness* and/or *volume* paces coherence breathing. Author it as a periodic mod:
`~15:35:10000` (sine, swings 15↔35 % every 10 000 ms). Macros: `breathMod` /
`breathPeriodMs(bpm)`. Individual resonance is 0.075–0.117 Hz; keep it fixed per segment.
`26_alpha_breath` holds 10 Hz alpha with a 0.1 Hz brightness breath. **[P]**

A true asymmetric 4 s-in / 6 s-out pace needs a new firmware LFO shape (§8) — ship the
symmetric sine now.

## 6. Audio-visual synchronization — "ms budget, µs is margin"

Cross-modal audio↔light simultaneity tolerance is ~20–40 ms; keeping sync <10 ms is
imperceptible. The steady-state stabilises hundreds of ms after onset, so single-cycle
jitter is fine. **The real budget is milliseconds** — the device's µs timing is *margin*.
Spend authoring effort on envelopes, safety, and waveform choice, not on chasing tighter
sync. Default AV phase offset is **0° (in-phase)**. Fading flicker out at a **trough**
(not a bright peak) disengages more cleanly. **[E]**
(Exception: intra-visual *antiphase* for invisible flicker needs fine edge timing — that
lives in the firmware engine, not the timeline.)

## 7. `.ledc` field mapping & format gotchas

| Concept | `.ledc` |
|---|---|
| Audio carrier | audio `freq` (L pitch) |
| Binaural beat | `freq_R` = carrier + beat (FIXED; ramp the carrier to slide the beat) |
| Monaural beat | two `pan=0` channels at `freq` and `freq+beat` |
| Isochronic pulse | audio `mod` at the beat Hz |
| Freq/beat slide | `>` / `*` on `freq` / `pan` / `vol` / `mod` (**NOT** `freq_R`) |
| Pulse envelope | `env` (0 sq / 1 sine / 2 tri / 3 trapezoid / 4 tremolo) + `duty` + `attack` |
| Pulse phase | `phase` deg (180 on one bank of a colour pair → antiphase invisible flicker) |
| Anti-habituation | `jitter <amp>[:period_ms]` — slow wander of the pulse rate |
| Noise | `waveType` 4 white / 5 pink / 6 brown |
| Background bed | `BG <url> <pan> <loudness>` |
| Speech cue | `S <time> <voice> <vol> "text"` (**browser-only**, stripped before device) |
| Light flicker | LED `freq` (= audio beat, 1:1) |
| Depth / master | LED `bright` (0 = dark for sleep, high = energize) |
| Mood colour | LED `R G B` (+ ramp for transitions) |
| Breathing | `bright`/`vol` sine LFO ~10 s |
| Skip / hold field | `-` sentinel — keep the channel's current value for that field |

**Gotchas (verified against `main/config_parser.c` / `web/src/js/gen/parse.js`):**
- **`freq_R` is NOT interpolatable** — a `>`/`*` prefix on it is ignored. Hold it fixed,
  ramp the carrier.
- **Ramp = animate-on-start:** the `>`/`*` prefix lives on the *earlier* entry; its value
  is the **start**, the target is the next same-channel entry's value, swept over the gap.
  A ramp on the last entry never fires.
- **LED `freq 0` = steady-on (no flicker)** — the Focus/Energize "steady dim light" trick
  relies on this (`05_focus_smr`).
- `env` and `waveType` are **discrete** (set per entry, not ramped).
- New pulse fields default to legacy behaviour (LED `env=0`, audio `env=4`, `phase=0`,
  `attack=3`, `jitter=off`); absent trailing fields round-trip byte-identically.

## 8. Firmware cross-deps (status as of 2026-07-03)

The v2 engine work (`entrainment_firmware_plan.md`) is **build-clean and DONE**, pending
the user's one-time hardware flash. Treat these as **available after the pending flash**:

- **Sine / trapezoid flicker carriers** (LED `env` 1/3) — invisible-flicker + gentle gates.
- **Per-channel flicker `phase`** — cool/warm antiphase "invisible" flicker (`27_genus_40hz_dim`).
- **Trapezoid isochronic `env` + `attack`** — crisp AM-noise bursts (`29_noise_sleep_pulsed`).
- **GENUS audio `duty`** — the 4 % click (`07_genus_40hz`, `27_genus_40hz_dim`).
- **Beat `jitter`** — anti-habituation wander.
- **Full per-channel `.ledc` v2 wiring** with the `-` skip sentinel.

Sessions that use these fields (`27`, `29`, and the `env`/`duty` columns in `07`, `15`)
render fully **only after that flash**; before it they degrade gracefully (a v2 field on a
tolerant older parser falls back to legacy behaviour — square gate, no phase, sine AM).

**Still genuinely future firmware work — do NOT author as if present:**
- **(a) Photosensitivity clamp `safety_mode`** was **cut/skipped**. There is NO device-side
  brightness/depth clamp in the 15–25 Hz band. The epilepsy interlock is **browser-only**
  (`web/src/js/safety.js`, one-time opt-in). Author safety into the timeline by hand (§9).
- **(b) Asymmetric breath LFO shape** (e.g. 4 s in / 6 s out) needs a new LFO shape —
  ship symmetric sine (§5) for now.
- **(c) Ramps / modulation ON the v2 pulse fields** — `env`, `phase`, and `attack` are
  **step-only on-device** for now (set per entry, not swept). Don't put `>`/`*`/`~` on them.
- **Fixed ~0.1 AM modulation depth:** the audio AM/isochronic depth is hard-fixed at ~0.1,
  so "deep" isochronic and AM-noise bursts are **gentle by design** until a firmware depth
  control lands. `07`, `28`, `29` note this in their headers.

## 9. SAFETY — mandatory, non-negotiable

Because the device-side clamp was cut (§8a), **safety is entirely the author's
responsibility in the timeline** plus the browser opt-in. Every session must:

- **AVOID bright full-field flicker 15–25 Hz** (peak 16–20 Hz; ~96 % of photosensitive
  reactions). For SMR/beta/Focus & Energize, **let AUDIO carry the beat** and keep light
  **steady (`freq 0`), <12 Hz, or dim** (`05_focus_smr` holds LED at 0 Hz the whole run). **[E]**
- **CAUTION 3–60 Hz:** cap brightness, **ramp light in/out ≥10 s**, never stop abruptly. **[E]**
- **SAFER zones:** <3 Hz and >65 Hz. **GENUS 40 Hz is above the danger band** → allowed,
  but still ramp in/out and cap brightness (`07` caps 45 %). **[E]**
- **No saturated-red strobe in 3–60 Hz**; no red↔blue alternation at flicker rates. Warm
  amber for sleep is fine (sleep flicker is <4 Hz); a *steady* red field is fine
  (`31_ganzfeld_amber`) — the rule concerns *flickering* red. **[E]**
- **Brightness floor** (avoid full black↔white); first-use intensity low; audio ≤~65 dB. **[P]**
- **Contraindications** (put in every session header + surfaced by the browser opt-in):
  photosensitive epilepsy / seizure / photic-migraine history; caution if pregnant, or
  wearing a pacemaker/implant; **never while driving or operating machinery**; stop if
  dizzy or unwell.

**Split-field caveat:** `30_splitfield_lucid` intentionally drives one eye at 16 Hz (in
the danger band) — kept **dim (≤18 %) and single-eye** (never full-field bright). The
validator **WARNs on those two rows by design**; document the intent in the header.

## 10. Authoring checklist (every session must pass)
- [ ] Follows the 4-phase arc, with the correct **goal-specific ending** (§1).
- [ ] Hold dwell ≥5–6 min; ramp rates within §3 (≤2 Hz/min; validator warns >3).
- [ ] Volume fades in ≥30 s; sleep/lucid fade out to 0 simultaneously with light.
- [ ] Light flicker 1:1 with audio beat **except** where §9 forces audio-only/steady.
- [ ] No bright 15–25 Hz visual flicker; light ramps in/out ≥10 s; brightness capped.
- [ ] Binaural carriers <1000 Hz, offset <35 Hz; monaural for speaker/waking content.
- [ ] Eyes-closed → amber/red bias; RGB otherwise mood/SSVEP only.
- [ ] v2 pulse fields (`env`/`phase`/`attack`) are **step values**, not ramped (§8c).
- [ ] Header comment block: name, goal, duration, bands, EXPERIMENTAL note where relevant,
      and the full **safety / contraindication** block.
- [ ] Passes `node validate_session.mjs library/<file>.ledc` — **0 errors** (warnings only
      where intentional and documented, e.g. `30`).

## 11. Worked examples from the library

**`01_sleep_onset` (Sleep Descent, ~25 min):** alpha entry at 10 Hz (binaural carrier
240 / `freq_R` 250 → 10 Hz beat) with a 10 s brightness ramp-in and a breathing LFO, a
continuous 10→2 Hz glide at ~0.44 Hz/min (carrier ramps 240→248, `freq_R` held), warm
amber→deep red, then a simultaneous **Soft Off** of light + audio at ~2 Hz. No wake-up —
the textbook sleep arc + ending.

**`05_focus_smr` (Focus/SMR, ~30 min):** the 14 Hz SMR beat is delivered **100 % by audio
isochronic `mod`**; the LED `freq` is held at **0 Hz (steady cool-white)** the entire
session — 14 Hz sits at the danger-band edge, so a steady lamp cannot photically drive.
Brightness capped ≤25 %, and the session **ends bright (40 %) and alert** at 12 Hz — the
focus-specific ending. This is the canonical example of §9's "audio carries the beat" rule.

## 12. Out of scope
Live audio→colour FFT, EEG closed-loop, true random anti-habituation. Approximate the last
with `jitter` and slow periodic LFOs on the pulse rate.
