# Plan 003 — Deep Relaxation / Stress Melt

Status: **Planned (authoring spec).** Precise enough that transcription to
`sessions/library/03_deep_relaxation.ledc` is mechanical. No `.ledc` authored yet.

## Header
- **Name:** Deep Relaxation / Stress Melt
- **Goal / persona:** A keyed-up, stressed user who wants to decompress without
  falling asleep. Iso-principle start near a waking low-beta state, glide down to a
  theta "melt," then return to calm-alert alpha so the user is relaxed but not groggy.
- **Total duration:** ~30 min (1,800,000 ms).
- **Target bands & rationale:**
  - Start **low-beta ~14 Hz** (iso-principle: begin near the awake/keyed-up state,
    never jump straight to the target) — `session_arc_design_by_usecase.md §2`,
    Iso-Principle `[9][10]`; guidelines §1 Induction.
  - Glide **alpha 10 Hz** → **theta 6 Hz hold** (the therapeutic core; theta = deep
    relaxation, SNU study used 5 Hz theta 30 min) — research §2 "Deep relaxation
    beta→theta 14→10→6", `000_SYNTHESIS.md §2`.
  - **Exit back to ~10 Hz alpha** (NOT beta) so the user ends calm-alert — guidelines
    §1 Exit; arc table row "Exit (gentle) 6→10 … ends in alpha so user isn't groggy".
- **Driver:** **isochronic** (audio `modulation`) is the primary entrainment driver
  (the harder driver for waking states — guidelines §3). Low-Hz LED flicker is allowed
  but kept **dim**, and is suppressed entirely during the 14 Hz portion (see Safety).

## Safety classification (§4 rules applied)
- **14 Hz start is just below / adjacent to the 15–25 Hz photosensitive danger band.**
  Mitigation: during the induction (beat 14 → ~10 Hz, t 0:00–5:00) the **LED does NOT
  flicker** — flicker frequency is held at **0 (steady dim light)** and the **audio
  isochronic pulse carries the beat**. Visual flicker only begins once the beat has
  descended to **≤10 Hz** (t = 5:00), far below the danger band. Satisfies §4 "for
  SMR/beta let AUDIO carry the beat and keep light steady/<12 Hz/dim."
- **CAUTION 3–60 Hz (§4):** once flicker is active (6–10 Hz) brightness is **capped at
  30%** (≤50% first-use), light **ramps in over 15 s** and **fades out gently** at the
  end (both ≥10 s); flicker is never stopped abruptly (it ramps with the beat).
- **Color:** cool-blue → warm-amber slow shift over minutes; **no saturated-red strobe**
  and **no per-flicker red↔blue alternation** (color changes are slow ramps, not
  flicker-rate alternation). Amber hold is orange/amber, not saturated red. §4 satisfied.
- **Brightness floor:** never full black↔white; dim 30% ceiling. First-use ≤50%, audio ≤~65 dB.
- **No binaural here** (isochronic-driven), so the binaural carrier/offset rules are N/A.

## Channel allocation
- **Audio ch1** — carrier + isochronic driver. `frequency` = carrier (200 Hz),
  `modulation` = beat Hz, `freq_r` = 0, `pan` = 0 (center, both ears), `wave_type` = 0 (sine).
- **Audio ch2 (optional bed)** — soft brown/pink-noise pad, `wave_type` 6 (brown) or 5
  (pink), `pan` 0, low volume (~20). Optional; see Open Questions.
- **LED** — channel mask **15** (the four legacy zones r1–r4), single flicker pattern.
  `pan` concept N/A for LED. All zones share the same flicker/brightness/color.

## Segment table (absolute times)
Beat = the entrainment rate, carried by audio `modulation` (isochronic). LED freq tracks
the beat 1:1 **only from 5:00 onward**; before that LED freq = 0 (steady).

| t (mm:ss) | phase | carrier Hz | beat (mod) Hz | wave | pan | vol | LED freq Hz | duty % | bright % | RGB | ramp / LFO notes |
|-----------|-------|-----------|---------------|------|-----|-----|-------------|--------|----------|-----|------------------|
| 00:00 | Induction start | 200 | 14 | sine | 0 | 0 | 0 (steady) | 50 | 0 | 0,80,180 (cool blue) | vol fades in; brightness ramps in next row |
| 00:15 | Induction | 200 | 14 | sine | 0 | →55 | 0 (steady) | 50 | `>30` | 0,80,180 | LED brightness 0→30% over 15 s (ramp-in ≥10 s) |
| 00:45 | Induction | 200 | 14 | sine | 0 | 55 (fade-in done) | 0 | 50 | 30 | 0,80,180 | volume 0→55 over 0:00–0:45 (`>55`) |
| 05:00 | Alpha reached / flicker on | 200 | `>10` | sine | 0 | 55 | 10 (step from 0) | 50 | 30 | 0,80,180 | isochronic 14→10 over induction; LED flicker begins at 10 Hz |
| 12:00 | Deepening → theta | 200 | `>6` | sine | 0 | 55 | `>6` | 50 | 30 | `>255` `>140` `>30` (→amber) | beat 10→6 (0.57 Hz/min); LED freq 10→6 lockstep; color blue→amber |
| 24:00 | Theta hold (anchor, hold end) | 200 | 6 | sine | 0 | 55 | 6 | 50 | 30 | 255,140,30 (amber) | **HOLD 12:00–24:00 = 12 min** (dwell ≥6 ✓); steady; optional breathing LFO (see notes) |
| 27:00 | Exit | 200 | `>10` | sine | 0 | 55 | `>10` | 50 | 28 | 255,160,60 | beat 6→10 over 3 min (exit to calm-alert alpha, NOT beta) |
| 29:30 | Settle (anchor) | 200 | 10 | sine | 0 | 55 | 10 | 50 | 28 | 255,160,60 | hold alpha; begin gentle close |
| 30:00 | End | 200 | 10 | sine | 0 | `>0` | 10 | 50 | `>0` | 255,160,60 | gentle fade: volume→0 and brightness→0 over last 30 s (≥10 s) |

Notes:
- **Anchor rows at 24:00 and 29:30** exist so the `>` ramps that follow are bounded to
  the exit window (a `>` ramp interpolates from the previous entry on that field). The
  hold is the gap between the 12:00 arrival row and the 24:00 anchor — no entries = value
  persists.
- **Optional breathing during hold:** replace the 24:00 brightness `30` with a slow sine
  LFO `~24:32:12000` (12 s period ≈ 5 breaths/min) for a subtle breath pace. Off by
  default to keep the hold flat.

## Envelope notes
- **Volume fade-in:** 0 → 55 over the first **45 s** (`>55`), per §3 (30–60 s always).
- **Exit:** relaxation ends with a **gentle close** — beat returns to alpha 10 Hz, then
  volume and brightness both fade to 0 over the final 30 s (a short, soft exit; this is
  NOT a sleep "soft-off," the user stays calm-alert in alpha up to the fade).

## Illustrative `.ledc` excerpt (format lock — first ~5 real lines)
```
# Deep Relaxation / Stress Melt — 30 min — iso 14→alpha 10→theta 6 hold→exit 10
# SAFETY: 14 Hz portion is audio-isochronic-driven; LED steady (no flicker) until beat<=10 Hz; brightness<=30%; ramps>=10s
0 0 50 0 0 80 180 15            # LED steady (freq 0), brightness 0, cool blue, all 4 zones
A 0 200 0 0 14 1 0 0            # ch1: carrier 200 Hz, isochronic mod 14 Hz, vol 0 (fades in), freq_r 0, sine
15000 0 50 >30 0 80 180 15      # LED brightness 0->30% over 15 s (ramp-in >=10 s); still steady
A 45000 200 0 >55 14 1 0 0      # volume 0->55 over 45 s; isochronic still 14 Hz
A 300000 200 0 55 >10 1 0 0     # induction: isochronic 14->10 Hz over 5 min
300000 10 50 30 0 80 180 15     # LED flicker begins at 10 Hz (beat now <=10, safe), dim cool blue
```

## Open questions / choices
- **Carrier choice:** 200 Hz is a neutral isochronic carrier; could use 150–250 Hz for a
  softer/deeper timbre. Isochronic carrier is just the audible tone (not ASSR-tuned).
- **Optional BG bed:** add `BG <url> 0 25` (centered, 25% loudness) — placeholder URL,
  e.g. a low ambient drone or brown-noise loop. Alternatively use audio ch2 brown noise.
- **Breathing LFO during hold:** include the `~24:32:12000` brightness LFO or keep flat?
- **Hold length:** 12 min specified; could extend to 15–20 if total budget grows.
</content>
</invoke>
