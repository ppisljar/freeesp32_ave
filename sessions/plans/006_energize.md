# Session Plan 006 — Energize / Wake-Up

> Authoring plan. Transcribe mechanically to `sessions/library/006_energize.ledc`.
> Rulebook: `sessions/SESSION_DESIGN_GUIDELINES.md`. Evidence: `reports/session_research/`.

## 1. Header
- **Name:** Energize / Wake-Up
- **Goal / persona:** Morning wake-up or pre-task energiser — someone groggy who wants
  to feel alert and switched-on in ~18 minutes (the one preset that legitimately
  **up-ramps** arousal).
- **Total duration:** 18:00 (mm:ss)
- **Target band:** beta, **ramp 10 → 18–20 Hz** (hold 18–20).
- **Rationale:** Beta (15–20 Hz) = alertness / active concentration / arousal; ascending
  protocols are a documented effective class (synthesis cheat-sheet "Energize → 18–20,
  bright cool white" **[P]**; `scientific_research_protocols.md` §Beta + ascending-ramp
  protocols **[weak–moderate]**). Rising brightness + cool (circadian-alerting) white
  reinforce wakefulness.

## 2. Safety classification (§4 — mandatory)
- **"AVOID bright full-field flicker 15–25 Hz; let AUDIO carry the beat; visual stays
  <12 Hz or dim."** → The 10→20 Hz climb is carried **entirely by AUDIO** isochronic
  `modulation` on ch1. The LED is allowed a **gentle 10 Hz (alpha, <12 Hz) flicker only
  during the first 3 min** at **capped brightness (≤25 %)**; the instant the audio beat
  crosses ~12 Hz the **LED switches to STEADY (freq 0)** and thereafter only its
  *brightness* rises. **No LED flicker ever occurs in the 15–25 Hz danger band.** Per §3
  this intentional 1:1 decoupling is the sanctioned "except where §4 forces audio-only"
  case.
- **"CAUTION 3–60 Hz: ramp light in/out ≥10 s, never stop abruptly."** → Brightness
  ramps in 0→25 % over 12 s; the flicker→steady transition is a brightness ramp, not an
  abrupt cut; session ends with the lamp ON (alert).
- **Brightness cap / first-use ≤50 % during flicker.** → During the 10 Hz flicker phase
  brightness ≤25 %. The high-brightness phase (up to 80 %) is **steady (non-flickering)**
  light, which carries no seizure risk, so the >50 % brightness there is acceptable.
- **No saturated-red strobe.** → Colour is cool white only.
- **Audio ≤ ~65 dB.** → Volume capped at 65/100.

## 3. Channel allocation
**Audio (16 ch):**
- **ch1 — isochronic driver.** Carrier **420 Hz** sine (`wave_type 0`, research beta
  carrier), `pan 0`. Beat = audio `modulation`, ramped 10→20 Hz.
- **(optional) BG bed** — an upbeat music/nature loop via a `BG <url> 0 30` line, low
  loudness, for motivational lift. User-supplied URL; omit if none.

**LED (8 ch):**
- **mask 15** (full visible frame; `255` if all 8 mapped). Cool white
  **(200, 220, 255)**. Two regimes: (a) 10 Hz flicker, duty 40, dim during induction;
  (b) steady (freq 0, duty 100) and brightening through deepening + hold.

**Pan layout:** centred (`pan 0`).

## 4. Segment table (absolute times)
Cool white RGB = **(200, 220, 255)**. Audio = ch1 driver.

| t (mm:ss) | phase | audio carrier Hz | beat (modulation) Hz | wave | pan | vol | LED freq Hz | duty % | bright % | RGB | ramp / LFO notes |
|-----------|-------|------------------|----------------------|------|-----|-----|-------------|--------|----------|-----|------------------|
| 00:00 | induction start | 420 | 10 | sine | 0 | **0 → (>)** | **10** | 40 | **0 → (>)** | 200,220,255 | vol fade-in; LED 10 Hz flicker (alpha, <12 Hz, safe), brightness ramp-in |
| 00:12 | induction | 420 | 10 | sine | 0 | — | 10 | 40 | **25** | 200,220,255 | brightness reaches 25 (≥10 s ramp-in done) |
| 00:40 | induction | 420 | 10 | sine | 0 | **55** | 10 | 40 | 25 | 200,220,255 | vol fade-in complete |
| 03:00 | deepening | 420 | **10 → (>) → 18** | sine | 0 | 55 | **0 (steady)** | 100 | **25 → (>)** | 200,220,255 | **flicker OFF → steady**; beat climbs 10→18 (~1.1 Hz/min); brightness 25→70 |
| 10:00 | **hold start** | 420 | **18** | sine | 0 | 65 | 0 | 100 | **70 → (>)** | 200,220,255 | beta hold begins; bright steady cool white 70→80 |
| 14:00 | hold (lift) | 420 | **18 → (>) → 20** | sine | 0 | 65 | 0 | 100 | 80 | 200,220,255 | final push to 20 Hz; brightness peaks 80 |
| 16:00 | hold (peak) | 420 | **20** | sine | 0 | 65 | 0 | 100 | 80 | 200,220,255 | brief 20 Hz plateau |
| 16:30 | exit | 420 | **20 → (>) → 16** | sine | 0 | 65 | 0 | 100 | 80 | 200,220,255 | gentle ease 20→16 (alert, not jittery) |
| 18:00 | **end (ALERT)** | 420 | **16** | sine | 0 | **60** | 0 | 100 | 80 | 200,220,255 | ends ON, bright — NO fade to 0 |

- **Hold dwell:** beta band (18–20 Hz) occupied 10:00–16:30 ≈ **6.5 min** (≥ §3 min). ✔
- **Ramp rate:** 10→18 Hz over 7 min ≈ 1.1 Hz/min; ≤5 Hz/min energize allowance. ✔

## 5. Envelope notes
- **Volume fade-in:** ch1 0 → 55 over ~40 s. ✔
- **End = ALERT (no fade to 0).** Lamp stays bright, audio stays on; beat parked at
  16 Hz (low-beta) so the user finishes alert.
- **No Soft Off.**

## 6. Illustrative `.ledc` excerpt (first ~5 real lines)
Ramp convention (verified in `config_parser.c` ~L1995): `>` prefix on the **earlier**
entry, its value = **start**; **target** = the **next** same-channel entry's value.

```
# 006 Energize / Wake-Up — beta up-ramp; AUDIO carries 10→20 Hz; visual ≤10 Hz then STEADY
# LED: time freq duty bright R G B mask   |   Audio: A time freq pan vol mod ch [freq_r] [wave]
0      10 40 >0  200 220 255 15     # LED gentle 10 Hz flicker (alpha, <12 Hz), brightness fade-in
12000  10 40 25  200 220 255 15     # brightness 25% (12 s ramp-in)
180000 0 100 >25 200 220 255 15     # t=3:00 flicker OFF -> STEADY (freq 0), brightness ramp START
600000 0 100 70  200 220 255 15     # t=10:00 steady bright cool-white reaches 70%
A 0      420 0 >0 10 1              # ch1 isochronic: 420 Hz carrier, vol fade-in, beat 10 Hz
A 180000 420 0 55 >10 1            # t=3:00 beat ramp START 10 Hz ...
A 600000 420 0 65 18  1            # t=10:00 ... TARGET 18 Hz (beta hold, audio-only)
```

## 7. Open questions / choices
- **Peak beat:** 20 Hz chosen as the top; some users prefer capping at 18 Hz (less
  "buzzy"). Adjust the 14:00/16:00 rows.
- **Carrier:** 420 Hz sine per research beta carrier; 300–440 Hz all fine.
- **Visual policy:** plan keeps a 10 Hz flicker only in induction then goes steady. A
  fully flicker-free variant (LED steady from t=0, brightness-only energise) is even
  safer and is a valid alternative if the early flicker feels jarring at wake-up.
- **LED freq 0 = steady-on:** assumes engine treats `frequency 0` as continuous (no
  flicker). Verify on device (same caveat as 005).
- **BG bed:** optional; needs a user-provided URL. Recommended for the energiser feel.
```
