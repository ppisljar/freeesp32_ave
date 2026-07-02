# Session Plan 007 — 40 Hz GENUS (Cognition)

> Authoring plan. Transcribe mechanically to `sessions/library/007_genus_40hz.ledc`.
> Rulebook: `sessions/SESSION_DESIGN_GUIDELINES.md`. Evidence: `reports/session_research/`.

## 1. Header
- **Name:** 40 Hz GENUS (Cognition)
- **Goal / persona:** The flagship evidence-based protocol — daily cognitive /
  neuroprotective gamma stimulation (attention, memory, Alzheimer's-pathology research
  use). Persona: a user doing a daily 60-minute 40 Hz session, eyes open or relaxed.
- **Total duration:** 60:00 (mm:ss)
- **Target band:** gamma, **40 Hz fixed** for the whole session (no arc/descent — GENUS
  holds a single frequency).
- **Rationale / evidence:** MIT Tsai-Lab **GENUS** is the most rigorously parameterised
  BWE protocol (`scientific_research_protocols.md` §1, **[strong]**): **visual = white
  light flickering at 40 Hz, 50 % duty (12.5 ms on / 12.5 ms off)**; **auditory = train
  of 10 kHz tone pips, 1 ms long, repeated at 40 Hz (~4 % duty click train), ~60–65 dB**;
  **combined audio-visual at 40 Hz synchronously = best multimodal outcome**; **dose
  1 h/day**. Reduces amyloid/tau and improves cognition in models and early human trials;
  now in Phase III (Cognito Therapeutics). Combined light + sound at the same frequency
  is the best-validated multimodal approach in the field.

## 2. Safety classification (§4 — mandatory)
- **"GENUS 40 Hz is ABOVE the 15–25 Hz danger band → visual flicker is allowed."** →
  This is the §4 exception that legitimises full-field 40 Hz visual flicker. 40 Hz is
  above the peak photosensitive band (15–20 Hz, 96 % of reactions) and was tested safe
  in GENUS pilots (at controlled luminance, screened populations).
- **"Still ramp light in/out ≥10 s and cap brightness."** → LED brightness **ramps in
  0 → 45 % over 15 s** at start and **ramps out 45 → 0 % over 30 s** at end — never an
  abrupt start/stop. Brightness **capped at 45 %** (moderate, well under any strobe
  intensity). 50 % duty keeps a brightness floor (avoids full black↔white DC contrast).
- **No saturated-red strobe.** → Light is neutral **white (255,255,255)**, scaled by the
  45 % brightness cap.
- **Audio ≤ ~65 dB.** → Volume capped at 60/100; matches GENUS 60–65 dB.
- **PSE contraindication.** → Even though 40 Hz is above the peak band, the firmware UI
  photosensitivity acknowledgement still gates the session (do not assume *any* 40 Hz
  flicker is safe at arbitrary brightness — hence the hard brightness cap).

## 3. Channel allocation
**Audio (16 ch):**
- **ch1 — GENUS auditory (the 40 Hz driver).** Carrier **10000 Hz** sine
  (`wave_type 0`), `pan 0`, **`modulation 40`** (gates the tone at 40 Hz → the click-like
  pip train). This is the audio half of the synchronous 40 Hz AV stimulus.
- **(optional) ch2 — comfort bed.** Pink noise (`wave_type 5`), `pan 0`, **steady
  (modulation 0)**, low volume (~15) to soften the 10 kHz tone over a full hour. Not an
  entrainer; omit for strict GENUS fidelity.

**LED (8 ch):**
- **mask 15** (full visible frame; `255` if all 8 mapped). **40 Hz, duty 50, white
  (255,255,255)**, brightness ramped 0→45→0. This is the visual half — synchronous with
  ch1 (both 40 Hz, 1:1 per §3).

**Pan layout:** centred.

## 4. Segment table (absolute times)
Single sustained 40 Hz throughout; only the envelopes change. White RGB = (255,255,255).

| t (mm:ss) | phase | audio carrier Hz | beat (modulation) Hz | wave | pan | vol | LED freq Hz | duty % | bright % | RGB | ramp / LFO notes |
|-----------|-------|------------------|----------------------|------|-----|-----|-------------|--------|----------|-----|------------------|
| 00:00 | ramp-in | 10000 | **40** | sine | 0 | **0 → (>)** | **40** | **50** | **0 → (>)** | 255,255,255 | 40 Hz active immediately (entrainment is fast); audio vol fade-in + LED brightness ramp-in |
| 00:15 | ramp-in | 10000 | 40 | sine | 0 | — | 40 | 50 | **45** | 255,255,255 | brightness reaches cap 45 % (≥10 s ramp-in done) |
| 00:30 | **hold start** | 10000 | 40 | sine | 0 | **60** | 40 | 50 | 45 | 255,255,255 | audio vol fade-in complete (≈30 s); steady 40 Hz AV from here |
| 59:30 | ramp-out start | 10000 | 40 | sine | 0 | **60 → (>)** | 40 | 50 | **45 → (>)** | 255,255,255 | begin simultaneous fade-out: vol 60→0, brightness 45→0 |
| 60:00 | end (off) | 10000 | 40 | sine | 0 | **0** | 40 | 50 | **0** | 255,255,255 | light + audio at 0 (≥10 s/30 s gentle ramp-out, no abrupt stop) |

- **Hold dwell:** 40 Hz held 00:30–59:30 = **59 min** (GENUS holds 40 Hz the whole
  session — far exceeds the ≥5–6 min minimum). ✔
- **No frequency LFO / anti-habituation.** GENUS efficacy depends on a *fixed* 40 Hz;
  the beat is deliberately kept flat (do **not** add a beat LFO here).
- Light flicker (40 Hz) and audio modulation (40 Hz) are **1:1 synchronous** per §3. ✔

## 5. Envelope notes
- **Volume fade-in:** ch1 0 → 60 over ~30 s (§3 30–60 s). ✔
- **Ramp-in light:** 0 → 45 % over 15 s (≥10 s rule). ✔
- **Ramp-out:** at 59:30 both audio volume (60→0) and LED brightness (45→0) ramp down
  together over the final 30 s — gentle, never abrupt (§4 "never stop abruptly"). This
  is a clean session-end fade, not a sleep Soft Off (cognition session).

## 6. Illustrative `.ledc` excerpt (40 Hz LED flicker + 40 Hz audio modulation shown explicitly)
Ramp convention (verified in `config_parser.c` ~L1995): `>` prefix on the **earlier**
entry, value = **start**; **target** = next same-channel entry's value.

```
# 007 40 Hz GENUS (Cognition) — 40 Hz flicker 50% duty + 40 Hz audio modulation, 60 min hold
# LED: time freq duty bright R G B mask   |   Audio: A time freq pan vol mod ch [freq_r] [wave]
0      40 50 >0  255 255 255 15     # LED 40 Hz flicker, 50% duty, white, brightness ramp-in from 0
15000  40 50 45  255 255 255 15     # LED brightness reaches 45% cap (15 s ramp-in, ≥10 s rule)
A 0     10000 0 >0 40 1             # ch1 GENUS audio: 10 kHz carrier, vol fade-in, MODULATION 40 Hz
A 30000 10000 0 60 40 1            # ch1 vol settled 60; 40 Hz modulation held = synchronous with LED
A 0     440   0 >0 40 2 0 5        # (optional ch2 comfort bed: pink noise, steady-low; omit for strict GENUS)
3570000 40 50 >45 255 255 255 15   # t=59:30 LED brightness ramp-out START 45 ...
3600000 40 50 0  255 255 255 15    # t=60:00 ... -> 0 (light off); audio fades to 0 in parallel
```
(40 Hz LED flicker line = `... 40 50 ...`; 40 Hz audio line = `A ... 40 1`. Both present
and synchronous — the defining GENUS pairing.)

## 7. Open questions / choices
- **Carrier choice (biggest decision):** plan uses GENUS-faithful **10 kHz** tone. A
  gentler **~200–440 Hz tone** or **pink noise** modulated at 40 Hz is far less fatiguing
  over 60 min and may improve adherence, at some loss of literal protocol fidelity.
  Recommend offering both ("GENUS-strict 10 kHz" vs "comfort tone").
- **Pip duty fidelity:** real GENUS pips are 1 ms @ 40 Hz (~4 % duty); our audio
  `modulation 40` produces an amplitude-gated tone whose on/off shape (~50 %) is *not*
  the same as a 4 % click train. Entrainment should still occur (40 Hz amplitude
  envelope), but confirm the firmware's modulation gating depth/shape on device; if a
  sharper pip is wanted, that's a firmware feature request (no current field for audio
  duty).
- **Brightness cap:** 45 % chosen as a conservative moderate level; tune 40–55 % to
  taste within the cap, never to full intensity.
- **Comfort bed:** ch2 optional — include for hour-long comfort, drop for strict GENUS.
- **LED freq:** here `frequency 40` is a genuine flicker (not the steady-0 case of
  005/006); standard flicker path.
```
