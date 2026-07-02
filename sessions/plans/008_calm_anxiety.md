# Plan 008 — Calm / Anxiety

Status: **Planned (authoring spec).** Precise enough that transcription to
`sessions/library/08_calm_anxiety.ledc` is mechanical. No `.ledc` authored yet.

## Header
- **Name:** Calm / Anxiety
- **Goal / persona:** An anxious user reaching for a short, reliable "panic-button"
  preset. The shortest reliable-effect arc — alpha-emphasis, gentle, effects often felt
  within 10–15 min. Settle → long alpha hold → soft exit, staying calm-alert.
- **Total duration:** ~25 min (1,500,000 ms).
- **Target bands & rationale:**
  - **Alpha ~10 Hz** emphasis, brief settle from 12 Hz — `session_arc_design_by_usecase.md
    §8` (10 Hz alpha 10–15 min, escalate to theta only if still anxious; effects in
    10–15 min `[8]`); `000_SYNTHESIS.md §2` "Anxiety/calm alpha ~10".
  - Binaural for anxiety has the strongest single-purpose evidence in the set
    (**g ≈ 0.45** effect size) — `000_SYNTHESIS.md §2`. (Effect size, not a parameter.)
- **Driver:** **gentle binaural**. Per the brief: "**soft, no sharp flicker**."
  The LED therefore does **NOT** strobe — flicker frequency is held at **0 (steady dim
  blue)** with only a slow breathing brightness LFO. Entrainment is carried entirely by
  the binaural audio; the light is calming ambient, not a driver.
  **Beat-slide technique (guidelines §5a):** `freq_r` is NOT interpolatable, so the beat
  is slid by holding `freq_r` **fixed** and ramping the carrier `frequency`; beat =
  `freq_r − frequency` then changes smoothly. Here `freq_r` is fixed at **382 Hz** and the
  carrier ramps 370→372 Hz (an inaudible 2 Hz carrier shift).

## Safety classification (§4 rules applied)
- **Lowest-risk session in the family.** No visual flicker at all (LED freq = 0 steady),
  so the 15–25 Hz danger band and the entire flicker-rate ruleset are **inapplicable to
  the light** by construction. All audio rates ≤12 Hz.
- **CAUTION 3–60 Hz (§4):** N/A to light (no flicker). Brightness still **ramps in over
  15 s** and **fades out** at the close (≥10 s), capped at ~28%, breathing LFO floored at
  12% (never full black↔white).
- **Binaural rules (§3/§4):** carrier **370 Hz** (<1000 ✓; ASSR-favored alpha carrier),
  L/R offset = beat **≤12 Hz** (<35 ✓). No noise masking required.
- **Color:** dim blue only — no red, no red↔blue alternation. §4 satisfied trivially.
- **Brightness:** first-use ≤50%, capped ~28%; audio ≤~65 dB.

## Channel allocation
- **Audio ch1** — binaural driver. `freq_r` = **fixed 382 Hz**, carrier `frequency` ramps
  370→372 Hz so beat = `freq_r − frequency` = 12→10 Hz (guidelines §5a — `freq_r` is not
  interpolatable, so the carrier is what ramps). `modulation` = 0, `pan` = 0 (center;
  L=freq / R=freq_r), `wave_type` = 0 (sine).
- **Audio ch2 (optional)** — soft pink-noise or nature bed at low volume (`wave_type` 5),
  OR a `BG` line (see Open Questions). Optional; calming bed.
- **LED** — channel mask **15** (four zones), **steady** dim blue (no flicker) + breathing
  brightness LFO. No `pan` for LED.

## Segment table (absolute times)
Beat = `freq_r` (fixed 382) − carrier `frequency`; the carrier `frequency` is what ramps
while `freq_r` stays fixed (guidelines §5a — `freq_r` is not interpolatable). **LED freq =
0 throughout (steady, no strobe).**

| t (mm:ss) | phase | carrier Hz (beat) | freq_r Hz | wave | pan | vol | LED freq Hz | duty % | bright % | RGB | ramp / LFO notes |
|-----------|-------|-------------------|----------|------|-----|-----|-------------|--------|----------|-----|------------------|
| 00:00 | Settle start | 370 (12) | 382 | sine | 0 | 0 | 0 (steady) | 50 | 0 | 0,60,180 (dim blue) | vol fades in; brightness ramps in next rows |
| 00:15 | Settle | 370 (12) | 382 | sine | 0 | →50 | 0 | 50 | `>22` | 0,60,180 | LED brightness 0→22% over 15 s (ramp-in ≥10 s); steady |
| 00:30 | Settle | 370 (12) | 382 | sine | 0 | 50 | 0 | 50 | `~12:28:10000` | 0,60,180 | **breathing LFO engages**: sine 12↔28% over 10 s (≈6 breaths/min) |
| 00:45 | Settle | 370 (12) | 382 | sine | 0 | 50 (fade-in done) | 0 | 50 | (LFO) | 0,60,180 | volume 0→50 over 0:00–0:45 (`>50`) |
| 03:00 | Alpha reached | `>372` (10) | 382 | sine | 0 | 50 | 0 | 50 | (LFO) | 0,60,180 | settle: carrier 370→372 ⇒ beat 12→10 over 3 min (0.67 Hz/min); steady blue |
| 21:00 | Alpha hold end (anchor) | 372 (10) | 382 | sine | 0 | 50 | 0 | 50 | (LFO) | 0,60,180 | **HOLD 03:00–21:00 = 18 min** (dwell ≫6 ✓); steady, breathing |
| 24:30 | Soft exit | 372 (10) | 382 | sine | 0 | 50 | 0 | 50 | (LFO) | 0,60,180 | stay in calm-alert alpha 10 Hz (no down-shift to theta by default) |
| 25:00 | End | 372 (10) | 382 | sine | 0 | `>0` | 0 | 50 | `>0` | 0,60,180 | gentle close: volume→0 and brightness→0 over last 30 s (≥10 s) |

Notes:
- **Anchor at 21:00** bounds the close; the 18-min hold is the gap between the 03:00
  arrival row and the 21:00 anchor (value persists, nothing changes).
- Beat stays at **10 Hz** through the exit (calm-alert alpha) — the user should not be left
  drowsy (it is not a bedtime preset). The session simply fades out from alpha.
- **Optional theta dip variant:** if more depth is wanted, after the alpha hold add a
  carrier `>376` descent (carrier 372→376 vs fixed `freq_r` 382 ⇒ beat 10→6) and extend
  the hold, per research §8 "escalate to 6 Hz theta only if still anxious." Documented
  variant, not default.

## Envelope notes
- **Volume fade-in:** 0 → 50 over the first **45 s** (`>50`), per §3.
- **Exit:** stays in alpha 10 Hz, then a **soft close** — volume and brightness fade to 0
  over the final 30 s. Not a sleep soft-off; ends calm-alert.
- **Breathing texture:** brightness sine LFO 12↔28% / 10 s throughout — paces breathing,
  the only LED motion (no strobe), satisfying "soft, no sharp flicker."

## Illustrative `.ledc` excerpt (format lock — first ~5 real lines)
```
# Calm / Anxiety — 25 min — settle 12->alpha 10 hold -> soft exit (alpha); panic-button preset
# SAFETY: NO visual flicker (LED steady); binaural carrier 370 Hz, offset <=12 Hz; brightness<=28%; ramps>=10s
0 0 50 0 0 60 180 15            # LED steady (freq 0), brightness 0 (ramps in), dim blue, all 4 zones
A 0 370 0 0 0 1 382 0           # ch1: carrier 370 Hz, binaural freq_r 382 (=12 Hz beat), vol 0, sine
15000 0 50 >22 0 60 180 15      # LED brightness 0->22% over 15 s (ramp-in >=10 s); still steady
30000 0 50 ~12:28:10000 0 60 180 15   # breathing LFO: brightness sine 12<->28% over 10 s; no strobe
A 45000 370 0 >50 0 1 382 0     # volume 0->50 over 45 s; binaural still 12 Hz
A 180000 >372 0 50 0 1 382 0    # settle: carrier 370->372 vs fixed freq_r 382 => beat 12->10 over 3 min
```

## Open questions / choices
- **Carrier:** 370 Hz (ASSR alpha carrier). Could use ~250–400 Hz; 370 is the cited sweet
  spot.
- **Theta dip variant:** ship a deeper variant (alpha hold → 6 Hz theta) for users who
  need more, or keep this preset strictly alpha?
- **BG / noise bed:** add `BG <url> 0 25` (placeholder, soft nature/pink loop) or audio ch2
  pink noise at low volume for an enveloping calm bed?
- **Total length:** 25 min specified; the arc compresses to ~15 min or extends to ~30 if
  desired (it is the shortest reliable preset).
</content>
