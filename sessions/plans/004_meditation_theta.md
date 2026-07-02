# Plan 004 — Meditation (Theta)

Status: **Planned (authoring spec).** Precise enough that transcription to
`sessions/library/04_meditation_theta.ledc` is mechanical. No `.ledc` authored yet.

## Header
- **Name:** Meditation (Theta)
- **Goal / persona:** A seated meditator who wants a long, flat theta plateau to sink
  into. Gentle alpha induction, a slow descent to theta, a long stable hold (the
  defining feature of a meditation arc — long and *flat*), then a soft return to alpha.
- **Total duration:** ~35 min (2,100,000 ms).
- **Target bands & rationale:**
  - Induction **alpha ~10 Hz → 8 Hz** (ease in) — guidelines §1; arc §3 "Settling 10→8".
  - **Theta plateau 6.5 Hz** (6–7 Hz core). A 6 Hz beat induced cortex-wide theta within
    ~10 min of a 30-min exposure (NIH PMC) — `session_arc_design_by_usecase.md §3 [11]`,
    `000_SYNTHESIS.md §2` (meditation theta 5.5–7).
  - **7.83 Hz "Schumann" variant** — the theta/alpha "twilight" border, a popular hold
    target. Confidence **[W]** (marketing/weak). Offered as a one-line swap, not default.
  - **Return to alpha 10 Hz** (soft close) — guidelines §1 Exit (meditation is not sleep).
- **Driver:** **binaural**. Theta is a gentle/low state; guidelines §3 reserve
  binaural for "gentle/low/sleep content," and the long flat hold suits the smooth
  binaural beat. Dim blue/violet LED flicker tracks the beat 1:1 (all ≤10 Hz, safe).
  **Beat-slide technique (guidelines §5a):** `freq_r` is NOT interpolatable, so the beat
  is slid by holding `freq_r` **fixed** and ramping the carrier `frequency`; beat =
  `freq_r − frequency` then changes smoothly. Here `freq_r` is fixed at **260 Hz** and the
  carrier ramps 250→253.5 Hz across the arc (an inaudible <4 Hz carrier shift).

## Safety classification (§4 rules applied)
- **No danger-band content:** every entrainment rate is **≤10 Hz**, well below the
  15–25 Hz photosensitive band. The strongest content is 10 Hz flicker during induction —
  still below 15 Hz. §4 primary rule satisfied by design.
- **CAUTION 3–60 Hz (§4):** flicker (6.5–10 Hz) runs at **low brightness (≤30%)** with a
  breathing LFO floored at 10% (never full black↔white); light **ramps in over 15 s** and
  **fades out** at the close (both ≥10 s); never stopped abruptly.
- **Binaural rules (§3/§4):** carrier **250 Hz** (<1000 ✓; the ASSR-favored theta carrier),
  L/R offset = beat **≤10 Hz** (<35 ✓). No noise masking needed.
- **Color:** blue/violet only — **no saturated red**, **no red↔blue per-flicker
  alternation** (color is steady, slow). §4 satisfied.
- **Brightness:** first-use ≤50%, capped ≤30% here; audio ≤~65 dB.

## Channel allocation
- **Audio ch1** — binaural driver. `freq_r` = **fixed 260 Hz**, carrier `frequency` ramps
  250→253.5 Hz so beat = `freq_r − frequency` = 10→6.5 Hz (guidelines §5a — `freq_r` is not
  interpolatable, so the carrier is what ramps). `modulation` = 0, `pan` = 0 (center;
  channel splits L=freq / R=freq_r internally), `wave_type` = 0 (sine).
- **Audio ch2 (optional)** — soft drone bed via brown noise (`wave_type` 6) at low volume,
  OR use a `BG` line instead (see Open Questions).
- **LED** — channel mask **15** (four zones), single flicker pattern. Blue/violet, dim,
  with a breathing brightness LFO.

## Segment table (absolute times)
Beat = `freq_r` (fixed 260) − carrier `frequency`; the carrier `frequency` is what ramps
while `freq_r` stays fixed (guidelines §5a — `freq_r` is not interpolatable). LED freq
tracks the beat 1:1.

| t (mm:ss) | phase | carrier Hz (beat) | freq_r Hz | wave | pan | vol | LED freq Hz | duty % | bright % | RGB | ramp / LFO notes |
|-----------|-------|-------------------|----------|------|-----|-----|-------------|--------|----------|-----|------------------|
| 00:00 | Settling start | 250 (10) | 260 | sine | 0 | 0 | 10 | 50 | 0 | 90,0,200 (violet) | vol fades in; brightness ramps in next rows |
| 00:15 | Settling | 250 (10) | 260 | sine | 0 | →50 | 10 | 50 | `>20` | 90,0,200 | LED brightness 0→20% over 15 s (ramp-in ≥10 s) |
| 00:30 | Settling | 250 (10) | 260 | sine | 0 | 50 | 10 | 50 | `~10:30:10000` | 90,0,200 | **breathing LFO engages**: sine 10↔30% over 10 s (≈6 breaths/min) |
| 01:00 | Settling | 250 (10) | 260 | sine | 0 | 50 (fade-in done) | 10 | 50 | (LFO) | 90,0,200 | volume 0→50 over 0:00–1:00 (`>50`) |
| 05:00 | Descent → theta | `>252` (8) | 260 | sine | 0 | 50 | `>8` | 50 | (LFO) | 90,0,200 | carrier 250→252 ⇒ beat 10→8 over induction (0.4 Hz/min); LED 10→8 lockstep |
| 09:00 | Theta plateau begins | `>253.5` (6.5) | 260 | sine | 0 | 50 | `>6.5` | 50 | (LFO) | 60,0,180 (deep violet-blue) | carrier 252→253.5 ⇒ beat 8→6.5 over 4 min (0.375 Hz/min); slight color deepen |
| 31:00 | Plateau end (anchor) | 253.5 (6.5) | 260 | sine | 0 | 50 | 6.5 | 50 | (LFO) | 60,0,180 | **HOLD 09:00–31:00 = 22 min, flat** (dwell ≫6 ✓); minimal change |
| 34:30 | Return | `>250` (10) | 260 | sine | 0 | 50 | `>10` | 50 | (LFO) | 90,0,200 | carrier 253.5→250 ⇒ beat 6.5→10 over 31:00–34:30 (return to alpha, soft) |
| 35:00 | Soft close | 250 (10) | 260 | sine | 0 | `>0` | 10 | 50 | `>0` | 90,0,200 | gentle close: volume→0 and brightness→0 over last 30 s (≥10 s) |

Notes:
- **Anchor at 31:00** bounds the return `>` ramp to the 31:00–34:30 window; the 22-min
  hold is the gap between the 09:00 arrival row and the 31:00 anchor (value persists).
- The breathing brightness LFO (`~10:30:10000`) runs continuously from 00:30 through the
  hold; it is preempted by the explicit `>0` brightness fade at 35:00.
- **7.83 Schumann variant:** for the hold, replace carrier `253.5 (6.5)` with
  `252.17 (7.83)` (= fixed `freq_r` 260 − 7.83) and the LED freq `6.5` with `7.83` (still
  <15 Hz, safe); `freq_r` stays fixed 260. One-line swaps; everything else identical.

## Envelope notes
- **Volume fade-in:** 0 → 50 over the first **60 s** (`>50`), per §3.
- **Exit:** meditation returns to alpha 10 Hz, then a **soft close** — volume and
  brightness fade to 0 over the final 30 s. (Not a sleep soft-off; user ends calm-alert.)
- **Breathing texture:** brightness sine LFO 10↔30% / 10 s period throughout the body of
  the session for breath pacing (guidelines §3 "breathing = brightness LFO ~10 s").

## Illustrative `.ledc` excerpt (format lock — first ~5 real lines)
```
# Meditation (Theta) — 35 min — alpha 10->8 induction -> theta 6.5 long plateau -> return 10
# SAFETY: all rates <=10 Hz (below 15-25 band); binaural carrier 250 Hz, offset <=10 Hz; brightness<=30%; ramps>=10s
0 10 50 0 90 0 200 15           # LED flicker 10 Hz, brightness 0 (ramps in), violet, all 4 zones
A 0 250 0 0 0 1 260 0           # ch1: carrier 250 Hz, binaural freq_r 260 (=10 Hz beat), vol 0, sine
15000 10 50 >20 90 0 200 15     # LED brightness 0->20% over 15 s (ramp-in >=10 s)
30000 10 50 ~10:30:10000 90 0 200 15   # breathing LFO: brightness sine 10<->30% over 10 s
A 60000 250 0 >50 0 1 260 0     # volume 0->50 over 60 s; binaural still 10 Hz
A 300000 >252 0 50 0 1 260 0    # descent: carrier 250->252 vs fixed freq_r 260 => beat 10->8 over induction
```

## Open questions / choices
- **6.5 Hz vs 7.83 Hz hold:** default 6.5 Hz (evidence-leaning); ship a 7.83 "Schumann"
  variant as a sibling file or documented swap?
- **Plateau length:** 22 min specified; meditation arcs tolerate 20–40 min — extend if the
  total budget grows.
- **BG drone bed:** add `BG <url> 0 30` (centered, 30% loudness) — placeholder URL for a
  low ambient drone/pad. Alternatively use audio ch2 brown noise at low volume.
- **Carrier:** 250 Hz (ASSR theta carrier). 7.83 variant could nudge carrier slightly but
  250 Hz works for both.
</content>
