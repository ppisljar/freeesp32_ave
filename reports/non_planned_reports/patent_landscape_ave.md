# AVE / Mind-Machine Patent & Company Landscape

**Date:** 2026-07-02
**Purpose:** Prior-art / freedom-to-operate (FTO) *awareness* for this open-source
ESP32 audio-visual entrainment project. Synthesized from 3 independent web-research
passes; every patent number was confirmed on a Google Patents (or equivalent) page.

> ⚠️ **Not legal advice.** Web research only, US-focused (check EP/Espacenet
> separately for other markets). Legal status and expiry are *estimates* (≈20 y
> from earliest filing; pre-1995-06-08 US patents = greater of 17 y from grant /
> 20 y from filing) and must be confirmed in USPTO Patent Center / Espacenet /
> CIPO before any commercial reliance. A formal FTO opinion from patent counsel
> is the right next step if this is ever commercialized.

---

## TL;DR

- **The entire core of this project is public domain.** Binaural beats, isochronic/
  AM tones, photic (LED) flicker at EEG frequencies, split-field stimulation,
  audio-visual synchronization, **and timeline session-sequencing with frequency
  ramps** are all covered only by **expired** patents (filed 1970–1997).
- **Most consumer brands hold no blocking patents** — MindPlace, Photosonix,
  Pandora Star, roXiva, BrainTap, NeuroVIZR, Ajna Light rely on trade secrets /
  branding. Lumenate has only a 2025 UK application with no claims yet.
- **Live risk is narrow and feature-specific.** The one genuinely broad active
  patent is **MIT US 10,682,490** (35–45 Hz gamma). Everything else active is a
  specific *feature* you can simply not implement.

---

## The single most important patent: MIT US 10,682,490 B2 (GENUS)

- **Assignee:** MIT (inventors Tsai, Boyden, Martorell). Priority Nov 2015 →
  **active to ~2036.** Licensed to **Cognito Therapeutics.**
- **Independent claim 1** (paraphrased): *administering a non-invasive stimulus of
  ~35–45 Hz to induce synchronized gamma oscillations in a brain region.*
  Dependent claims add light (9–12), sound (13–14), haptic (15), ~40 Hz (cl. 2).
- **Why it matters:** a device that ships **light and/or sound in the 35–45 Hz
  band** can read on this claim. **Relabeling as "meditation/relaxation" does NOT
  avoid it** — the claim requires neither a disease nor a therapeutic purpose.
  (Some sibling MIT/Cognito claims, e.g. US 10,159,816 / US 10,265,497 /
  US 10,960,225 / US 10,293,177, *are* disease-limited to Alzheimer's/dementia.)
- **Design-around:** don't offer any 35–45 Hz **preset** out of the box (at most a
  user-entered custom frequency), and never use Alzheimer's/dementia/medical
  language in code, docs, README, or marketing.
  https://patents.google.com/patent/US10682490B2/en

---

## Active patents to be aware of (design-around candidates)

| Patent | Assignee | ~Expiry | Scope (what it actually claims) | Relevance |
|---|---|---|---|---|
| **US 10,682,490 B2** | MIT / Cognito | 2036 | ANY non-invasive 35–45 Hz stimulus inducing gamma (light/sound/haptic) | 🔴 Broad — avoid the 35–45 Hz band as a default |
| US 10,960,225 B2 | MIT | 2038 | Chronic ~30–50 Hz **visual** gamma to treat dementia | 🟠 Disease-limited |
| US 10,293,177 B2 | Cognito | 2037 | **Auditory** gamma for Alzheimer's | 🟠 Disease-limited |
| US 10,279,192 B2 | Cognito | 2037 | Eyeglasses + fovea-directed light + ambient photodiode feedback | 🟡 Specific hardware — don't copy |
| US 11,433,253 B2 | Optoceutics/DTU | 2038 | Two complementary-spectrum sources ~180° out of phase ("invisible flicker") 20–50 Hz | 🟡 Don't copy invisible-flicker; plain strobe is free |
| US 12,507,332 B2 / US 12,563,650 B2 | Aleddra | 2039 / 2044 | Superimposed two-source invisible 20–65 Hz flicker | 🟡 Same — invisible flicker only |
| **US 11,536,965 B2** | Mind Alive (Siever) | 2041 | **Randomized / jittered** multi-channel light+sound in glasses | 🟠 Don't add frequency randomization in an eyeset |
| **US 11,322,042 B2** | Mind Alive (Siever) | 2040 | AVE synced to **breathing cues + HRV** monitoring | 🟠 Only if you add breath-pacing/HRV |
| **US 9,731,092 B2** (EP2437839B1) | Lucia (Winkler & Proeckl) | ~2029–31 (EP largely lapsed) | **Constant light + superimposed flicker + auto frequency-ramp** to target | 🟠 Needs the whole combination |
| US 7,674,224 B2 | Brain.fm (ex-Transparent Corp) | 2028 | Embedding entrainment modulation into a **pre-existing music track** / disguised in instrument timbres | 🟡 Only if you filter/remix source music |
| US 10,039,471 B2 | Hardt | 2035 | **Closed-loop EEG** isochronic biofeedback | 🟡 Only if you drive tones from live EEG |
| US 11,400,252 B2 | Sana Health | ~2039 | Visual and/or auditory sensory stimulus for **pain** (FDA De Novo cleared Jan 2026) | 🟡 Pain-management framing |
| US 12,083,286 B2 | Neltner (Lumenate-style) | 2041 | **Strobing** color LED panel 10–60 Hz filling ≥5% FOV | 🟡 Static uniform field designs around it |
| US 8,579,795 B2 | Sensora (Martel) | ~2030 | LFO-modulated light intensity/color patterns | 🟡 Specific patterns |
| US 11,090,459 B2 | NuCalm/Solace | active | **Non-linear** binaural progressions (anti-acclimatization) | 🟡 Audio-only, specific |
| US 12,248,289 B2 | Endel | 2041 | Sensor-adaptive **generative** music | 🟡 Generative audio only |

Non-AV / different paradigm (low relevance): Apollo Neuro US 10,974,020 (haptic),
Hapbee/EMulate (EM), Muse/InteraXon (EEG neurofeedback), Flow (tDCS), Vielight
(40 Hz-pulsed NIR photobiomodulation).

---

## Expired / abandoned — FREE prior art (the core concept is unpatentable)

| Patent | What it established | Inventor | Status |
|---|---|---|---|
| US 3,884,218 | Sleep induction via AM/EEG-modulated audio | Monroe | EXPIRED (1992) |
| **US 5,213,562** | Binaural-beat consciousness induction (Hemi-Sync) | Monroe / Interstate | EXPIRED (2010) |
| US 5,356,368 | Multi-EEG binaural beat induction | Monroe / Interstate | EXPIRED (2011) |
| **US 5,036,858** / RE36,348 | Sweep light+sound toward target EEG freq; canonical closed-loop | Carter & Russell / Neurotrain | EXPIRED (2010) |
| US 4,191,175 | Pulsed noise-like audio entrainment | Nagle | EXPIRED (1998) |
| US 4,315,502 | Pulsing-light mask + audio, independent phase/freq | Gorges | EXPIRED (1999) |
| US 5,070,399 | Colored-light intensity modulation 2–20 Hz | Martel | EXPIRED (2010) |
| **US 5,709,645** | Independent left/right visual-field photic stimulator (flicker glasses) | Siever / Comptronic (DAVID) | EXPIRED (2016) |
| US 5,954,629 | EEG-sensing light stimulation | Pioneer | EXPIRED (2017) |
| **US 5,306,228** | Synchronized LED+audio with **frequencies that step/ramp during a session** | Rubins | EXPIRED (2012) |
| US 4,883,067 | Real-time EEG→music feedback loop | Neurosonics | EXPIRED (2007) |
| US 4,777,937 | Continuous uniform Ganzfeld light + pink noise mask | Tranquil Times | EXPIRED (~2005) |
| US 5,896,457 | Audio-reactive LED flicker via music zero-crossings (<50 Hz) | Tyrrel / Virtual Imagination | LAPSED (2007) |

**US 5,306,228 (Rubins, expired 2012)** is the key one for this project: it is the
clearest foundational patent for **timeline sequencing of synchronized AV
frequencies with ramps** — i.e. exactly what the `.led` timeline does — and it is
public domain.

---

## Company landscape (beyond the six incumbents)

| Company / Product | Technique | Patents? |
|---|---|---|
| **BrainTap** (light+sound headset) | Photic + binaural + iso (AudioStrobe®) | **None** — closest competitor, favorable FTO signal |
| **Cognito Therapeutics** (MIT spin-out) | 40 Hz AV gamma | **Yes** — licenses MIT GENUS; large portfolio |
| **Optoceutics** (EVY LIGHT) | 40 Hz "invisible spectral flicker" | Yes (US 11,433,253) |
| **Aleddra** | Superimposed invisible 20–65 Hz flicker | Yes (US 12,507,332 / US 12,563,650) |
| **Sana Health** | Bilateral alternating light/sound (pain) | Yes (US 11,400,252) — first FDA-cleared AV stimulator |
| **Sensora / Sensortech** (Anadi Martel) | Photic light modulation + audio | Yes (US 8,579,795) |
| **NeuroVIZR** | AV flicker + pulsed sound, incl. 40 Hz | **None found** — cloneable |
| **Ajna Light** | Stroboscopic forehead lamp + binaural | **None** — cloneable |
| **NuCalm** (Solace) | Binaural "non-linear oscillation" | Yes (US 11,090,459, US 9,079,030) |
| **Endel** | Generative adaptive audio | Yes (US 12,248,289) |
| **NeoRhythm/OmniPEMF, Sensate, Apollo, Hapbee, Muse, Flow** | PEMF / vibro / haptic / EM / EEG / tDCS | Various — non-AV, low relevance |

**Incumbents (your README list):** Mind Alive holds the only classic-maker active
patents (randomization; breathing/HRV — above). MindPlace, Photosonix, Pandora
Star, roXiva hold **no** relevant patents. Lucia holds the flicker+ramp family
above. Lumenate has one 2025 UK application (GB2504099) with no claims yet.

**Naming corrections found:** "Lightform Ltd" is NOT tied to Pandora Star (it's an
unrelated AR startup); Photosonix on Google Patents = an unrelated ultrasound firm;
"Sens960/SensoryX" is a mislabel of Sensora; AudioStrobe® is a licensed Tamas Lab
(Germany) standard, not owned by BrainTap/MindPlace/Photosonix; Monroe's binaural
patent is US 5,213,562 (NOT US 4,141,344, which is an unrelated recording patent).

---

## Action checklist for this project

1. **Keep everything strictly non-medical.** No Alzheimer's/dementia/pain/
   "treatment"/"therapy" claims anywhere in code, comments, README, UI, or
   marketing. This is the cheapest, highest-value risk reducer — it steps around
   every disease-limited MIT/Cognito/Sana claim and induced-infringement theory.
2. **Don't ship a 35–45 Hz preset by default** (MIT US 10,682,490). If gamma is
   wanted, expose it only as a user-entered custom frequency, or omit the band.
   The project's current presets should be audited for any 40 Hz default.
3. **Don't implement AudioStrobe encoding/decoding** by that name/format — licensed
   (Tamas Lab), not public.
4. **Avoid these specific active features** unless reviewed: frequency
   randomization/jitter in an eyeset (Mind Alive US 11,536,965); breathing-cue/HRV
   sync (Mind Alive US 11,322,042); constant-light + auto-ramping-flicker
   *combination* (Lucia US 9,731,092); two-source "invisible flicker" (Optoceutics/
   Aleddra); disguising modulation inside music timbres (Brain.fm US 7,674,224);
   EEG-closed-loop tone driving (Hardt US 10,039,471).
5. **Safe to keep as-is:** binaural + isochronic tone generation, on/off LED flicker
   at chosen frequencies, split-field / per-channel LED control, audio-reactive
   VU/spectrum LEDs, and the `.led`/`.ledc` timeline with linear/quadratic frequency
   ramps — all anticipated by expired patents (esp. Rubins US 5,306,228).

*Sources: all patents linked inline above via patents.google.com. Compiled from
three independent research passes; confirm status with counsel before commercial use.*
