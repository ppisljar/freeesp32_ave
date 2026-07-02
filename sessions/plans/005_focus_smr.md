# Session Plan 005 — Focus / Flow (SMR)

> Authoring plan. Transcribe mechanically to `sessions/library/005_focus_smr.ledc`.
> Rulebook: `sessions/SESSION_DESIGN_GUIDELINES.md`. Evidence: `reports/session_research/`.

## 1. Header
- **Name:** Focus / Flow (SMR)
- **Goal / persona:** Knowledge worker or student who needs sustained, calm-alert
  concentration ("flow") — e.g. a 30-minute deep-work block. Not sleepy, not wired.
- **Total duration:** 30:00 (mm:ss)
- **Target band:** SMR / low-beta, **hold 14 Hz** (tunable 14–16 Hz).
- **Rationale:** 12–15 Hz sensorimotor rhythm = calm focus, reduced motor
  restlessness; classic neurofeedback target for attention (synthesis cheat-sheet
  "Focus / study 12–16, hold ~14" **[E/P]**; `scientific_research_protocols.md`
  §SMR/Beta — passive-entrainment evidence is moderate, deliver via **audio** because
  the band overlaps the seizure-provocative flicker zone). This is the **sustained**
  focus preset (single held target, **not** a cyclic/oscillating session).

## 2. Safety classification (§4 — mandatory)
Applicable §4 rules and exactly how each is satisfied:

- **"AVOID bright full-field flicker 15–25 Hz; for SMR/Focus let AUDIO carry the beat
  and keep light steady/<12 Hz/dim."** → 14 Hz sits right at the lower edge of the
  15–25 Hz peak-risk band. **The 14 Hz beat is carried 100 % by AUDIO** (isochronic
  `modulation` on ch1). **The LED never flickers at the beat rate** — LED `frequency`
  is held at **0 Hz (steady illumination)** the entire session. A steady (non-pulsed)
  lamp cannot photically drive or trigger seizures regardless of brightness.
- **"CAUTION 3–60 Hz: ramp light in/out ≥10 s, never stop abruptly."** → Even though
  the light is steady, its brightness is ramped **up over 15 s** at the start (0 → 25 %)
  and is **never cut abruptly**; at session end it simply stays on (alert end).
- **Brightness floor / first-use ≤50 %.** → Brightness capped at 40 % (dim, cool).
- **No saturated-red strobe / no red↔blue alternation.** → N/A (no flicker); colour is
  steady cool white.
- **Audio ≤ ~65 dB.** → Volume capped at 60/100; user sets absolute SPL.
- **PSE contraindication** is enforced by the firmware UI acknowledgement; no visual
  flicker is present here so risk is minimal.

## 3. Channel allocation
**Audio (16 ch available):**
- **ch1 — isochronic driver.** Carrier **300 Hz** sine (`wave_type 0`), `pan 0`
  (centre). The beat is delivered as audio `modulation` = beat Hz. This is the only
  entraining stimulus.
- **ch2 — pink-noise focus bed (optional but recommended).** `wave_type 5` (pink),
  `pan 0`, steady (`modulation 0`), low volume (~20). Masks ambient distraction; not
  an entrainer.

**LED (8 ch):**
- **mask 15** (all four legacy zones r1–r4 = full visible frame; use `255` if all 8
  channels are pixel-mapped). **Steady** cool white, **freq 0 / duty 100**, brightness
  dim→moderate. No flicker.

**Pan layout:** everything centred (`pan 0`). Focus benefits from a stable, non-moving
image; no auto-pan.

## 4. Segment table (absolute times)
LED row is the steady cool-white lamp (one mask-15 channel). Audio rows are ch1 (driver)
and ch2 (bed). Cool white RGB = **(180, 200, 255)**.

| t (mm:ss) | phase | audio carrier Hz | beat (modulation) Hz | wave | pan | vol | LED freq Hz | duty % | bright % | RGB | ramp / LFO notes |
|-----------|-------|------------------|----------------------|------|-----|-----|-------------|--------|----------|-----|------------------|
| 00:00 | induction start | 300 (ch1) | 10 | sine | 0 | **0 → (>)** | 0 (steady) | 100 | **0 → (>)** | 180,200,255 | vol fade-in begins; brightness ramp-in begins |
| 00:15 | induction | 300 | 10 | sine | 0 | — | 0 | 100 | **25** | 180,200,255 | brightness reaches 25 (≥10 s ramp-in done) |
| 00:45 | induction | 300 | 10 | sine | 0 | **55** | 0 | 100 | 25 | 180,200,255 | vol fade-in complete (≈45 s) |
| 05:00 | deepening | 300 | **10 → (>) → 14** | sine | 0 | 55 | 0 | 100 | **25 → (>)** | 180,200,255 | beat ramp 10→14 over 5 min (~0.8 Hz/min); brightness 25→40 |
| 10:00 | **hold start** | 300 | **14** (sustained) | sine | 0 | 60 | 0 | 100 | 40 | 180,200,255 | hold 14 Hz, steady dim light — therapeutic core |
| 27:00 | exit start | 300 | **14 → (>) → 12** | sine | 0 | 60 | 0 | 100 | 40 | 180,200,255 | beat eases 14→12 (toward eyes-open alertness) |
| 30:00 | **end (ALERT)** | 300 | **12** | sine | 0 | **50** | 0 | 100 | **35** | 180,200,255 | ends ON — NO fade to 0 (focus end is alert) |

Pink bed (ch2), parallel: 00:00 vol `>0`; 00:45 vol 20 (steady pink); held 20 to 27:00;
30:00 vol 18 (still on at alert end). No modulation, pan 0 throughout.

- **Hold dwell:** 14 Hz from 10:00–27:00 = **17 min** (≫ §3 minimum 5–6 min). ✔
- **Ramp rate:** 10→14 Hz over 5 min ≈ 0.8 Hz/min (gentle; well within limits). ✔
- **Sustained, not cyclic:** single held target; **no LFO** on the beat. (An optional
  ±0.5 Hz anti-habituation sine LFO on `modulation`, period ≥120 s, is available per §3
  but is intentionally **omitted** to honour the "sustained, not cyclic" requirement.)

## 5. Envelope notes
- **Volume fade-in:** ch1 0 → 55 over ~45 s (`>` on volume at t=0, target at 00:45);
  ch2 mirrors. ✔ (§3 30–60 s).
- **End = ALERT (no fade to 0).** Both audio channels and the lamp remain ON at session
  end; beat parked at 12 Hz (SMR/alpha border) so the user surfaces alert, not drowsy.
- **No Soft Off** (this is not a sleep/lucid session).

## 6. Illustrative `.ledc` excerpt (first ~5 real lines)
Ramp convention (verified in `config_parser.c` ~L1995): the `>` prefix lives on the
**earlier** entry and its value is the **start**; the **target** is the **next**
same-channel entry's value, swept over the gap.

```
# 005 Focus / Flow (SMR) — 14 Hz held, AUDIO carries beat, light steady dim cool-white
# LED: time freq duty bright R G B mask   |   Audio: A time freq pan vol mod ch [freq_r] [wave]
0      0 100 >0  180 200 255 15      # lamp: STEADY (freq 0), brightness fade-in from 0
15000  0 100 25  180 200 255 15      # brightness reaches 25% (15 s ramp-in, ≥10 s rule)
A 0     300 0 >0  10 1               # ch1 isochronic: 300 Hz carrier, vol fade-in, beat 10 Hz
A 45000 300 0 55  10 1               # ch1 vol settled at 55, still 10 Hz (alpha induction)
A 0     220 0 >0  0  2 0 5           # ch2 pink-noise bed (wave 5), steady, vol fade-in
A 45000 220 0 20  0  2 0 5           # ch2 bed at vol 20
A 300000 300 0 55 >10 1              # t=5:00 beat ramp START 10 Hz ...
A 600000 300 0 60 14  1              # t=10:00 ... TARGET 14 Hz (hold begins)
```

## 7. Open questions / choices
- **Hold frequency:** 14 Hz chosen (conservative, matches cheat-sheet "hold ~14").
  Could raise to 15–16 Hz for a more "beta-alert" feel — still audio-only, still safe.
- **Carrier:** 300 Hz sine per research SMR carrier. A 250–420 Hz tone or a pink-noise
  carrier modulated at the beat are alternatives; sine is the cleanest/least fatiguing.
- **LED freq 0 = steady-on:** assumes the engine treats `frequency 0` as continuous
  illumination (no flicker). **Verify on device**; if 0 is interpreted as "off", use a
  sub-1 Hz value isn't acceptable — instead drive a constant via duty 100 with the
  lowest non-flickering path the firmware supports.
- **Pink bed:** optional; drop ch2 if the user prefers silence under the tone, or swap
  for a `BG <url> 0 25` ambient bed.
```
