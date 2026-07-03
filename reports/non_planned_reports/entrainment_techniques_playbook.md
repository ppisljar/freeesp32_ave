# Entrainment Techniques Playbook — things to *learn from and build*

**Date:** 2026-07-02
**Purpose:** A learning-oriented, implementation-mapped digest of audio, visual, and
session-design techniques mined from patents (as public technical disclosures) and
research literature — to improve this open-source ESP32 AVE engine. Synthesized from
3 independent research passes. Each item: what it is → the math/parameters → a source →
the concrete `audio_generator.c` / `led_matrix_example.c` / `led_strip.*` / `config_parser.c`
/ `.ledc` change it implies. Not clinical advice; evidence quality flagged where relevant.

---

## 0. What all three passes agreed on (do these first — they're cheap and cross-cutting)

These surfaced independently from the audio, visual, AND protocol research — strongest signal:

1. **Mandatory fade-in/out envelopes (≥5 s, raised-cosine).** Anti-startle + clean phase
   at onset/offset. You already have the 5 ms de-click ramp (audio) and `>`/`*` brightness
   ramps (LED) — codify a *minimum* session onset/offset ramp (10 s for light) and enforce it.
2. **Slope-limited frequency ramps.** The frequency-following response can only track a
   slow glide: keep **~0.25–2 Hz/min**, warn above ~3 Hz/min. Your linear/quadratic sweeps
   already do this; add authoring guidance + a warn.
3. **Anti-habituation dither.** A dead-steady beat habituates in minutes. Add optional
   **±0.1–0.3 Hz** slow dither (non-commensurate LFO periods, e.g. 37 s / 53 s) — largely
   free via the existing `mods[]`/LFO machinery.
4. **Photosensitivity safety interlock.** 15–25 Hz bright flicker is the seizure-risk peak;
   saturated-red flicker is special-risk. Cap brightness/depth in that band, throttle red,
   ship contraindication text. Cheap, protects everything.
5. **Sync budget is milliseconds, not microseconds.** Cross-modal audio↔light simultaneity
   tolerance is ~20–40 ms; keeping sync <10 ms is imperceptible. The device's µs capability
   is margin — **spend engineering effort on envelopes/safety/waveforms, not tighter sync.**
   (Caveat: *intra-visual* antiphase for invisible flicker DOES need fine edge timing — see V-E1.)

---

## A. AUDIO techniques (map to `main/audio_generator.c`)

Engine facts that shape these: `apply_modulation()` is `sample *= 1 + mod_depth*sin(mod_phase)`
with `mod_depth` a **real per-channel field** (the "0.1" is only the parser default); binaural
routing bypasses pan when `freq_r>0 && != freq`; per-param LFOs already exist via
`audio_generator_set_mod(ch, AUDIO_PARAM_*, wave, start, end, period_ms)`; quadratic sweep
easing exists.

| # | Technique | Core math / params | Source | Code change |
|---|---|---|---|---|
| A1 | **Monaural beats** (flagship; works on speakers, ~50 dB modulation vs binaural ~3 dB) | Sum two carriers: `s=a·cos(2πf₁t)+a·cos(2πf₂t)`, envelope beats at Δf. Carrier 200–400 Hz, Δf=target. Halve to keep headroom. | [Exp Brain Res 10.1007/s00221-021-06155-z](https://link.springer.com/article/10.1007/s00221-021-06155-z) · [PMC8448709](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC8448709/) | Add `beat_mode` (BINAURAL/MONAURAL) to `audio_gen_params_t`; at binaural branch, MONAURAL → `mono=0.5*(sample_l+sample_r); L=R=mono`. Already have both detuned carriers. ~0 CPU. |
| A2 | **Trapezoid isochronic envelope** (attack steepness + off-time drive entrainment; hold/decay don't) | Duty-cycled raised-cosine gate: attack τ_a 2–5 ms, on-hold, decay 2–5 ms, off. Duty 0.25–0.50 (longer off = stronger). Depth 0.7–1.0 iso / 0.1–0.3 disguised. | [bioRxiv 10.1101/541359](https://www.biorxiv.org/content/10.1101/541359.full.pdf) | Add `iso_env`(SINE/TRAPEZOID)+`duty`+`attack_ms`; modify `apply_modulation()` to evaluate a trapezoid on `ph = mod_phase/2³²`. ~6–8 cy/sample. Keep sine default. Also upgrades AM-noise (A6). |
| A3 | **Anti-acclimatization beat progression** (NuCalm's whole premise) | Dither Δf ±0.3–1 Hz, non-commensurate LFO periods (37/53 s); or step carrier ±10–20 Hz every 3–5 min; ≤1 Hz/min ramps. | NuCalm [US 11,090,459](https://patents.justia.com/patent/11090459) | Binaural: add slow LFO on `freq_diff` (beat-offset param). Isochronic: works TODAY via `set_mod(ch,AUDIO_PARAM_MOD_FREQ,...)`. Expose as "anti-adaptation" toggle. |
| A4 | **Carrier-frequency selection** | Beat perception peaks ~**400 Hz** carrier (Δf≤35 Hz); **340 Hz** better for 40 Hz gamma. Per-band: δ~200, θ~250, α~370, β~420, γ~340–500 Hz. | [Oster curve](https://www.binauralbeatsmeditation.com/oster-curve/) · [Sci Rep 10.1038/s41598-025-88517-z](https://www.nature.com/articles/s41598-025-88517-z) | No engine change — authoring presets + optional "band+beat → fill freq/freq_r" UI helper. |
| A5 | **Harmonic / octave stacking** (richer, reinforces FFR; 20 Hz subharmonic boosts 40 Hz) | N channels same Δf on octave carriers (100/106, 200/206, 400/406 @ Δf6), upper octaves −3…−6 dB. | [PMC9998542](https://pmc.ncbi.nlm.nih.gov/articles/PMC9998542/) | Pure `.ledc` authoring (16 ch + `1/N` auto-scale exist). Optional "harmonic stack" editor macro. ~1.5% CPU/3 ch. |
| A6 | **AM-noise carrier** (broadband = wide cochlear drive, low fatigue; great for sleep) | Gate pink/brown noise at target: 0.8–1.0 Hz δ (sleep), 10 Hz α, 40 Hz γ; deep depth. | [NSS 10.2147/NSS.S243204](https://www.tandfonline.com/doi/full/10.2147/NSS.S243204) | Works TODAY: pink-noise channel + `mod_frequency=0.8, mod_depth=0.9`. Crisper with A2 trapezoid. Preset work. |
| A7 | **Timbre-embedded (shallow) modulation** (Brain.fm — "music not buzz") | Shallow depth 0.1–0.3 AM on a rich bed (harmonic stack / noise + detuned saws) at beat rate. | [US 7,674,224](https://patents.google.com/patent/US7674224B2/en) | Build a rich bed from primitives (A5) + global shallow AM. Full version needs decoded-audio source (future). |
| A8 | **Rotating pan / inter-aural phase drift** (immersion, novelty = anti-habituation) | Pan LFO sine −1↔+1, period 8–30 s; or slow phase offset on one ear over ~10 s. Carriers 200–500 Hz. | [PLOS ONE 10.1371/journal.pone.0286023](https://journals.plos.org/plosone/article/file?id=10.1371%2Fjournal.pone.0286023&type=printable) | Pan LFO works TODAY (`set_mod(ch,AUDIO_PARAM_PAN,sine,-1,1,15000)`). Phase drift = few lines near phase-update. |
| A9 | **FFR induction ramp / sub-bass tactile** | Start beat at ~10–12 Hz, glide to target ≤1 Hz/min, quadratic ease. 30–60 Hz monaural low channel → bone-conduction transducer. | (see §C) | Apply existing sweep to `frequency_r` (beat), not a jump. Tactile = one low monaural channel. |

---

## B. VISUAL techniques (map to `main/led_matrix_example.c` flicker engine + `led_strip.*` + `.ledc`)

Engine facts: flicker carrier is **hard square only**; one **gptimer ISR at 1 kHz tick** (→ 1 ms
edge quantization = **8% phase error at 40 Hz**); per-channel `cycle_start_time_us` phase anchor;
direct backend = 8× LEDC PWM (brightness only, ~25 kHz carrier), neopixel/dotstar carry RGB.

**Two low-level enablers unlock most of the list — do them first:**
- **V-E1 — raise flicker tick to ~10 kHz** (`s_ensure_timer_and_task`). 100 µs tick → 0.4% phase
  error at 40 Hz. Needed for clean antiphase (B1) and phase gradients (B6). ISR is cheap (~1000 cy).
- **V-E2 — add per-channel `phase_offset_deg`/`_us`** to `led_flicker_state_t`, applied to the
  initial `cycle_start_time_us`. The one field enabling split-field, antiphase, and traveling waves.

| # | Technique | Core method / params | Source | Code change |
|---|---|---|---|---|
| B1 | **Invisible / flicker-free spectral flicker (FLAGSHIP)** — two complementary-color banks 180° antiphase; mean luminance ~flat (no visible flicker, comfortable) but retina still gets the oscillation | 40 Hz, 50% duty, ~180° antiphase, residual luminance swing ~5%. RGB repro: **amber↔blue** or **red↔cyan** antiphase, brightness-matched so sum = steady white. | [Optoceutics US 11,433,253](https://patents.google.com/patent/US11433253B2/en) · [JAD 10.3233/JAD-220081] · [Aleddra US 12,507,332](https://patents.google.com/patent/US12507332B2/en) | Needs V-E1+V-E2 + RGB backend. Two channels via `channel_map`, phase 0°/180°, complementary RGB. Add a first-class **`pair`/antiphase** `.ledc` construct + per-entry `phase` token. Degrades to luminance-only on direct PWM. |
| B2 | **Sine / gamma-corrected flicker waveform** (square injects harmonics *into the brain* — 10 Hz→30 Hz; sine = clean, far less fatigue; every therapeutic AVS uses sine ≤10 Hz) | Sine THD ≤0.1%; gamma-correct in L\* (~γ2.2) → ~2.5× more energy on fundamental. 50% duty max fundamental; high duty+shallow depth = comfortable. 40 Hz fine as square. | [Teng 2011 10.1155/2011/364385](https://onlinelibrary.wiley.com/doi/10.1155/2011/364385) · [Han 2022 JoV](https://jov.arvojournals.org/article.aspx?articleid=2784440) | Add `carrier_waveform`(SQUARE/SINE/TRIANGLE) to `led_flicker_state_t` + `.ledc` token; ISR computes intra-cycle brightness from a **gamma-corrected sine LUT** (IRAM `uint8_t[256]`). Reuse existing Q16 phase math. |
| B3 | **Photosensitive-epilepsy interlock (MUST-HAVE)** | Worst **15–25 Hz** (peak ~16); safe non-red ≤3 flash/s; hazardous if Δ≥20 cd/m²; **saturated red (R/(R+G+B)≥0.8) → ≤3 Hz** regardless of luminance. | [Fisher/Harding Epilepsia 2005](https://doi.org/10.1111/j.1528-1167.2005.31305.x) · [ITU-R BT.1702-3] | Validation pass in `config_parser.c` + live clamp in flicker engine; `safety_mode` NVS (default ON). Cap bright/depth in 15–25 Hz; desaturate/throttle red>3 Hz. Web UI one-time epilepsy opt-in. |
| B4 | **40 Hz gamma preset (GENUS)** | 40 Hz square, **50% duty (12.5 ms on/off)**, white ~4000 K, ~3000 lux. Age-tune: young 34–38 Hz, 70+ 32–34 Hz. Amyloid claim contested; entrainment solid. | [Iaccarino 2016 Nature](https://doi.org/10.1038/nature20587) · [Singer 2018 Nat Protoc](https://doi.org/10.1038/s41596-018-0021-x) | `sessions/library/40hz_gamma.ledc`. Best delivered via B1 pair for comfort. No code change. |
| B5 | **Independent split-field / per-eye** (Siever) — different freq per eye/hemifield; chiasm crossover → per-hemisphere entrainment; rapid L/R alt = 2× beat | Lucid 16 Hz\|6 Hz; PMS 25\|10; ADD 21–26\|12–16; depression β-left/α-right. | [US 5,709,645](https://patents.google.com/patent/US5709645) | TODAY for different-freq: map eye pixels to channels via `channel_map`, two `.ledc` lines diff `freq`+`mask`. Phase-locked (same-freq offset) needs V-E2. |
| B6 | **Phase gradients / traveling waves** (phi motion, rotation, closed-eye mandalas) | `φ_k=360°·k/N` across N channels (8→45° steps); sign = direction; ISI ~60 ms; form constants strongest ~10 Hz. | [Ganzflicker PMC10825158] · [Bressloff/Cowan PMID 11316482] | Needs V-E1+V-E2. Add `phase` token and/or a `WAVE freq N dir` directive auto-assigning φ_k across masked channels. |
| B7 | **Ganzfeld: red bias, diffusion, ramps** | Eyelid passes ~14.5% @700 nm, ≤3% ≤580 nm → **bias amber/red ≥620 nm** for eyes-closed. Hold field >7 min (15–25 ideal). Ease-in up to ~30 s. | [Eyelid transmission PMC4790124] · [Ganzfeld PMC10825158] | Authoring + defaults: `>`/`*` brightness ramps already do envelopes; default amber/red for closed-eye presets; enforce ≥10 s onset ramp in B3 pass. Diffuser = hardware note. |
| B8 | **Color / wavelength SSVEP** | At matched luminance, **spectral extremes drive far stronger SSVEP**: amber/red 8.06 dB, blue/red 6.82, green/lime 2.85. Any pair w/ red or blue ≫ without. | [Sci Rep 10.1038/s41598-024-52679-z](https://doi.org/10.1038/s41598-024-52679-z) | Pure RGB authoring. Feeds B1 (amber↔blue = strongest SSVEP *and* complementary). Ship color presets. |
| B9 | **AudioStrobe/SpectraStrobe emit-or-decode; PWM carrier hygiene** | AudioStrobe 19.2 kHz inaudible carrier AM→brightness; SpectraStrobe 18.2/18.7/19.2/19.7 kHz (ref/R/G/B). Keep PWM(kHz)≫envelope(≥1kHz)≫entrainment. WS2812 ~400 Hz PWM strobe-breaks 8–30 Hz → prefer LEDC/APA102. | [tamaslab.com](https://tamaslab.com/the-theory-of-audiostrobe/) · [SpectraStrobe](https://github.com/jonaustin/SpectraStrobe) · IEEE 1789 | Since you generate audio+LED from one timeline, could *emit* these carriers to drive 3rd-party glasses. Niche differentiator. Prefer direct-LEDC/APA102 for clean flicker carriers. |

---

## C. SESSION PROTOCOLS, SYNC & SAFETY (authoring + engine defaults)

### Bands → uses → target freqs (evidence: gamma strongest; band→state mostly practitioner convention)
| Band | Hz | State | Target | Evidence |
|---|---|---|---|---|
| Delta | 0.5–4 | deep sleep | 1–3 (2) | low–mod |
| Theta | 4–8 | meditation, hypnagogia, lucidity | 4–7 (6) | low–mod |
| Alpha | 8–13 | relaxed wakefulness ("the bridge") | **10** | **most reliably entrainable** |
| Beta/SMR | 13–30 (SMR 12–15) | focus, calm-focus | 14, 15–18 | low–mod (SMR neurofeedback) |
| Gamma | 30–45+ | cognition, therapeutic 40 Hz | **40** | highest (GENUS RCTs) |

### Session arc (4-phase) & endings
Induction (start ~10–14 Hz where the brain already is, hold 1–3 min) → **Deepen** (glide
~0.5 Hz/min; Siever: even 0.3 Hz can flip effect) → **Hold** (dwell 5–20 min) → **Return**.
**Sleep ending = fade to dark at low freq, NO wake-up.** **Focus ending = ramp up to 12–15 Hz +
brighten** (avoid grogginess). Dual-freq L/R offset 0.3–0.5 Hz deepens the state.
Patent basis: [Rubins US 5,306,228](https://patents.google.com/patent/US5306228A/en).

### Audio-visual synchronization
Combined AV is **superadditive** (stronger steady-state than either alone; [Spectris PMC12788959](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC12788959/)).
No proven optimal phase offset → make it a per-session field, **default 0° (in-phase)**. Steady-state
stabilizes ~hundreds of ms after onset, so single-cycle jitter is fine — **real budget ≈ ms.**
Stopping/fading flicker at a *trough* (not a bright peak) disengages cleaner ([PMC7161378](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC7161378/)).

### GENUS 40 Hz template (highest-evidence; transcribe faithfully)
40 Hz light+sound **combined**; light ~390–400 lux square; **sound = 1 ms pulse / 25 ms (4% duty,
NOT binaural)**, 78 dB(healthy)/68(patient); 1 h/day × 3 mo. ([PLOS One](https://journals.plos.org/plosone/article?id=10.1371%2Fjournal.pone.0278412) · [PMC9714926](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC9714926/)).
→ set the gamma audio channel `mod` duty ≈4%; offer a dim comfort variant.

### Breath layer (coherence)
**0.1 Hz** LFO (≈6 breaths/min; 4 s in / 6 s out for calm) on *brightness/volume* envelope,
independent of entrainment freq. Individual resonance 0.075–0.117 Hz. Keep it fixed per segment.
([Front. Neurosci. 10.3389/fnins.2020.570400](https://www.frontiersin.org/journals/neuroscience/articles/10.3389/fnins.2020.570400/full)) → dedicated "breath LFO" mod type on brightness/vol.

### Safety (bake into defaults)
Mandatory fade-in/out (≥5 s audio, ≥10 s light); brightness clamp 13–26 Hz; no saturated-red
strobe; global intensity ceiling; ship contraindication text (epilepsy/photosensitivity, pregnancy,
pacemaker caution, don't-drive). Low-intensity first-run trial.
([Fisher Epilepsia 2022](https://www.epilepsy.com/sites/default/files/2022-10/Epilepsia_2022_fisher_visually_sensitive_seizures.pdf))

### Ready-to-transcribe starter arcs (→ `sessions/library/*.ledc`)
- **A. Sleep Descent (25 min, dim warm):** α10 (fade-in, hold 2 min) → ramp 10→2 Hz (~0.44 Hz/min, 18 min) → hold ~2 Hz, brightness→0, audio→silence. No wake-up.
- **B. Theta Dive (20 min):** α10 → ramp 10→6 (0.5 Hz/min) → hold 6 Hz 6 min (L6.0/R6.3) → ramp 6→10 → end alert.
- **C. Alpha+Breath (15 min):** hold α10 with **0.1 Hz breath LFO** (4 s up/6 s down) on brightness → ramp 10→12, brighten, end.
- **D. Focus/SMR (20 min, daytime):** SMR 14 Hz, **brightness ≤25% (photosensitivity), entrainment mainly in AUDIO**; optional 40 Hz audio-only burst mid-session; end full brightness.
- **E. Gamma-40 GENUS (60 min):** 40 Hz light (ramp to ~400 lux) + 40 Hz click (1 ms/25 ms, 4% duty), in-phase, steady 60 min, fade out. Offer dim comfort variant.

---

## Unified build order (impact × low effort first)

1. **Cross-cutting defaults** (§0): mandatory fade envelopes, ramp-slope warn, `safety_mode` clamp. Small, protects everything.
2. **V-E1 (10 kHz flicker tick) + V-E2 (per-channel phase field)** — unlocks B1/B5/B6.
3. **A1 Monaural mode** + **A2 trapezoid isochronic envelope + depth UI** — biggest audio capability jump; A2 also unlocks AM-noise (A6).
4. **B2 sine/gamma-corrected flicker** + **B3 safety interlock** — biggest comfort/safety-per-effort on the visual side.
5. **B1 invisible spectral flicker** (amber↔blue antiphase pair) — the flagship visual feature.
6. **A3 anti-acclimatization dither** (partly free) + **A8 rotating pan** (free).
7. **Authoring/preset library**: carrier/harmonic/noise presets (A4–A6), 40 Hz GENUS (B4), split-field (B5), color presets (B8), breath LFO, and the 5 starter arcs (§C).
8. **Polish/differentiation**: B6 traveling waves, B7 Ganzfeld templates, A7 timbre-embed, B9 AudioStrobe emit.

Files: `main/audio_generator.c/.h` (A1–A3, A8), `main/led_matrix_example.c` (V-E1/E2, B1/B2/B6),
`main/led_strip.h` (flicker-state fields), `main/config_parser.c/.h` (new tokens + B3 validation),
`main/settings.c` (`safety_mode`), `sessions/library/*.ledc` (presets), web editor (macros + epilepsy opt-in).

## Evidence caveats
Band→state mappings and ramp/dwell numbers are largely **practitioner/device convention**, not
settled neuroscience; the 40 Hz **amyloid/therapeutic** claim is contested (the *entrainment* itself
is solid); hemisphere-specific therapeutic claims are vendor-level; commercial fade times and full
production spectra (Optoceutics) are proprietary — numbers above are the patents'/literature's public
figures. Keep the project's framing experiential, not medical.
