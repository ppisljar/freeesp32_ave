# Session Design Guidelines (Lessons Learned)

> The normative rulebook for authoring entrainment sessions in our `.ledc` format.
> Distilled from the 5-agent field research — evidence + citations live in
> `reports/session_research/` (start at `000_SYNTHESIS.md`). Every `.ledc` session
> in `sessions/library/` MUST follow these rules. Confidence tags: **[E]** evidence,
> **[P]** product practice, **[W]** weak/marketing.

---

## 1. The session arc (every session is segments + ramps)
Four phases, expressed as timestamped `.ledc` entries with `>` (linear) / `*` (quad) ramps:

1. **Induction** — start near alpha (~10 Hz), 5–10 min. Eases the user in; also where volume fades IN.
2. **Deepening** — ramp from induction toward the target band.
3. **Hold** — sit at the target band; the therapeutic core. **Dwell ≥5–6 min** (10–30 min for solid theta/delta). **[E/P]**
4. **Exit** — ramp back to alpha/SMR before "eyes open."

**Sleep & lucid sessions skip the Exit** and end in a **Soft Off**: light brightness and audio volume fade to zero *simultaneously*. **[P]**

## 2. Goal → band → parameters (consensus across products, presets, science)

| Goal | Band | Target Hz | Total | Light | Conf. |
|------|------|-----------|-------|-------|------|
| Sleep onset | delta | 10→8→5→2→1 | 40–60 min | fade to OFF; warm amber | [P]+delta [E] |
| Power nap | delta→wake | ↓~3.4 then ↑~12 | 20–25 min | dim → rise on wake | [P] |
| Deep relaxation | beta→theta | 14→10→6→(10) | ~30 min | dim, cool→warm | [P] |
| Meditation | theta | 6–7 (7.83 opt) | 30–45 min | low, blue/violet | theta [E], 7.83 [W] |
| Focus | SMR/low-beta | 12–16 (hold 14) | 25–50 min | **steady/dim** | [E/P] |
| Energize | beta | →18–20 | 15–20 min | bright, cool white | [P] |
| Cognition | gamma | **40 fixed** | 60 min | **40 Hz, 50% duty** | **[E] best** |
| Calm/anxiety | alpha | ~10 | 20–30 min | dim, blue | binaural g≈0.45 [E] |

## 3. Stimulation rules
- **Light flicker is the strongest driver; isochronic > binaural** for driving. Light pulse and audio pulse run **1:1** at the same Hz. **[E]**
- **Entrain with WHITE / brightness; use RGB only for mood/circadian.** Color does not entrain. **[E/P]**
- **Flicker waveform:** square = strongest, sine = gentlest. **Duty 50% default; 25–40% for gamma.** **[E]**
- **Binaural (`freq_R`):** carrier **<1000 Hz**, L/R offset **<35 Hz**; reserve for gentle/low/sleep content. Carriers: theta~250, alpha~370, beta~420 Hz; sleep 250 Hz / 0.25–3 Hz. No noise masking needed. **[E]**
- **Isochronic:** sharp pulse via audio `modulation` at the beat Hz (the harder driver for waking states).
- **Ramp rates:** ~0.1 Hz/min for sleep descents; up to ~5 Hz/min only for energize/wake. **[P]**
- **Envelopes:** volume **fade-in 30–60 s always**; fade-out to zero for bedtime/lucid. **[P]**
- **Texture:** breathing = `brightness` LFO (sine/triangle, ~10 s period). Anti-habituation = a small, slow periodic LFO on the beat (approximates RAVE ±1 Hz; we can't do true random). **[P]**
- **Bed:** optional `BG` drone / nature / pink–brown noise under the tones.

## 4. SAFETY — mandatory, non-negotiable
- **AVOID bright full-field flicker 15–25 Hz** (peak 16–20 Hz; 96% of photosensitive reactions). For SMR/beta/Focus & Energize, **let AUDIO carry the beat** and keep light **steady, <12 Hz, or dim**. **[E]**
- **CAUTION 3–60 Hz:** brightness caps, **ramp light in/out ≥10 s**, never stop abruptly. **[E/P]**
- **SAFER zones:** <3 Hz and >65 Hz. Decorative ≤3 flashes/s. **[E]**
- **No saturated-red strobe in 3–60 Hz**; no red↔blue alternation at flicker rates. (Warm amber for sleep is fine — sleep flicker is <4 Hz.) **[E]**
- **Brightness floor** (avoid full black↔white); first-use intensity ≤50%; audio ≤~65 dB. **[P]**
- **GENUS 40 Hz is above the danger band** → allowed; still ramp in/out and cap brightness.
- **Contraindication:** photosensitive epilepsy / seizure / photic-migraine history. (Also enforced by the Firmware UI brightness cap + acknowledgement.)

## 5. `.ledc` field mapping (concept → format)

| Concept | `.ledc` |
|---|---|
| Carrier | audio `frequency` |
| Binaural beat | `freq_R` = frequency + beat (fixed beat: set `freq_R` directly) |
| Isochronic pulse | audio `modulation` at beat Hz |
| Freq slide | `>` / `*` on `frequency` / `pan` / `volume` / `modulation` (NOT `freq_R`) |
| Auto-pan / bilateral | `pan` LFO (slow triangle/sine) |
| Noise | `wave_type` 4=white / 5=pink / 6=brown |
| Background bed | `BG <url> <pan> <loudness>` |
| Light flicker | LED `frequency` (= audio beat, 1:1) |
| Flicker shape/duty | square + `duty` (50%, or 25–40% gamma) |
| Depth / master | LED `brightness` (off=sleep, high=energize) |
| Mood color | LED `RGB` (+ ramp for transitions) |
| Breathing | `brightness` sine/triangle LFO ~10 s |
| Multi-band layering | separate audio + LED channels (16 + 8) |

## 6. Authoring checklist (every session must pass)
- [ ] Follows the 4-phase arc (or descent+soft-off for sleep/lucid).
- [ ] Hold dwell ≥5–6 min; ramp rates within §3.
- [ ] Volume fades in 30–60 s; sleep/lucid fade out to 0.
- [ ] Light flicker 1:1 with audio beat **except** where §4 forces audio-only.
- [ ] No bright 15–25 Hz visual flicker; light ramps in/out ≥10 s; brightness capped.
- [ ] Binaural carriers <1000 Hz, offset <35 Hz.
- [ ] White/brightness drives entrainment; RGB only for mood.
- [ ] Parses clean against `config_parser.c` (validate via the generator's `validate.js` / round-trip).
- [ ] Header comment block: name, goal, duration, bands, safety note.

## 5a. Format constraints (verified against `config_parser.c` — easy to get wrong)
- **`freq_R` is NOT interpolatable.** The audio entry has interp flags only for
  `frequency`, `pan`, `volume`, `modulation` — there is no `freq_R_interp`. A `>`/`*`
  prefix on the 7th token is ignored (or parses as 0). **To slide a binaural beat,
  hold `freq_R` FIXED and ramp the carrier `frequency`** so beat = `freq_R − frequency`
  changes smoothly. (For an isochronic beat slide, ramp `modulation` directly — that
  one IS interpolatable.)
- **Ramp convention = animate-on-start:** the `>`/`*` prefix lives on the *earlier*
  entry; its value is the **start**; the **target** is the next same-channel entry's
  value, swept over the gap. (The built-in example's last "fade to 0" line has no
  next entry, so its sweep never fires — stale comment.)
- **LED `frequency 0` = steady-on (no flicker), assumed but VERIFY on device.** The
  "steady dim light" safety technique (Focus/Energize) relies on this. If `freq 0`
  turns the LED *off* instead, use a sub-3 Hz flicker or brightness-only steady light.
- Wave types: 0 sine, 1 square, 2 triangle, 3 saw, 4 white, 5 pink, 6 brown.

## 7. Out of scope (can't express in a static timeline)
Live audio→color FFT, EEG closed-loop, true random anti-habituation. Approximate the last with slow periodic LFOs.
