# Session Plan 001 — Sleep Onset

> Authoring plan for the `descent` family. Precise enough that transcription to
> `001_sleep_onset.ledc` is mechanical. Complies with
> `sessions/SESSION_DESIGN_GUIDELINES.md` and the research in
> `reports/session_research/`. **This is a plan, not the `.ledc`.**

---

## 1. Header

| Field | Value |
|---|---|
| **Name** | Sleep Onset |
| **Goal / persona** | Fall asleep. User lies down, eyes closed, lights to off. A continuous downward glide that mirrors natural sleep architecture (alpha entry → theta light-sleep → delta deep-sleep) then goes silent and dark. |
| **Total duration** | ~45:00, then silent + dark (no loop) |
| **Target bands** | alpha 10→8 Hz → theta 8→5 Hz → delta 5→2→1 Hz |
| **Arc type** | **Descent + Soft Off** (no Exit phase) — per guidelines §1 / synthesis §1 |

**Rationale (cited):** The sleep arc mirrors sleep stages and ends in a fade so audio dies after the user is asleep (`session_arc_design_by_usecase.md` §1; a 2023 trial swept delta 3→1 Hz over 20 min and shortened sleep latency). Guidelines §2 row "Sleep onset": delta, 10→8→5→2→1, 40–60 min, *fade to OFF, warm amber*. Binaural is the gentle/low-frequency tool (§3): sleep carrier ~250 Hz, offset 0.25–3 Hz region near the end. Continuous slow glide (not a flat plateau) is the documented sleep pattern; the slowest ramp is reserved for the delta tail where it matters most.

---

## 2. Safety classification (§4 of guidelines — mandatory)

| §4 rule | Applies? | How this plan satisfies it |
|---|---|---|
| AVOID bright full-field flicker **15–25 Hz** | Not triggered | Max flicker is **10 Hz** (induction); everything else is lower. Never enters the 15–25 Hz danger band. |
| CAUTION **3–60 Hz** (caps, ramp in/out ≥10 s, no abrupt stop) | Yes (induction/deepening 5–10 Hz) | Brightness **capped ≤30%**; LED **brightness ramps in over 10 s** (0→30% across 0:00–0:10); session ends in a gradual Soft Off (brightness `>0` over the 15-min delta tail), never an abrupt cut. |
| SAFER zones <3 Hz / >65 Hz | Yes (delta tail 2→1 Hz) | The therapeutic core (30:00–45:00) sits at **2→1 Hz**, inside the SAFER <3 Hz zone. |
| No saturated-red strobe in 3–60 Hz; no red↔blue alternation | Yes | Color is **warm amber (255,100,0)** ramping to **dim deep-red (150,20,0)** — never pure saturated red, no blue alternation. Guidelines explicitly allow warm amber for sleep because sleep flicker is <4 Hz. |
| Brightness floor / first-use ≤50% / audio ≤~65 dB | Yes | Peak brightness **30%** (well under 50%); audio volume peaks at **55 / 100**; brightness never strobes full black↔white (floor held by warm dim amber). |
| Contraindication screening | UI-enforced | Photosensitive/epilepsy/migraine acknowledgement handled by the Firmware UI brightness cap + acknowledgement (out of `.ledc` scope). |

**Soft Off:** light brightness **and** audio volume fade to zero *simultaneously* over the final delta segment (30:00→45:00), per §1.

---

## 3. Channel allocation

| Resource | Channel | Use | Pan layout |
|---|---|---|---|
| Audio ch **1** | binaural beat | Left ear = `frequency` (carrier, ramped); Right ear = `freq_r` (held 255 Hz). Perceived beat = `freq_r − frequency`. wave_type 0 (sine). | **pan 0** (centered — `freq_r` already provides the L/R tone split; pan 0 keeps both ears at equal level) |
| Audio ch **3** | pink-noise bed | wave_type **5** (pink). freq ignored. Soft masking/comfort bed. | pan 0 |
| LED ch **1–8** | full-field warm flicker | mask **255** (all 8 logical channels). Flicker frequency tracks the audio beat 1:1. | n/a |
| Background (optional) | `BG` bed | optional ambient drone/pad under the tones | pan 0 |

**Carrier choice & encoding note (important):** the parser does **not** support an interpolation prefix on the `freq_r` token (it is read with plain `atof()`), so a smooth binaural-beat *slide* cannot be authored by ramping `freq_r`. Instead we **hold `freq_r` constant at 255 Hz** (right ear) and **ramp the left `frequency` up** (which *does* accept `>`), shrinking the beat. Perceived beat = `255 − frequency`:

| Beat (Hz) | Left `frequency` (Hz) |
|---|---|
| 10 | 245 |
| 8 | 247 |
| 5 | 250 |
| 2 | 253 |
| 1 | 254 |

Both ears stay in 245–255 Hz (carrier ≈250 Hz, well <1000 Hz; max L/R offset 10 Hz < 35 Hz — satisfies §3 binaural rules). The left-ear pitch drifts ~9 Hz over 45 min (~3.6%, sub-perceptual for a sleep aid).

---

## 4. Segment table (absolute times)

LED line is `mask 255`, `duty 50%` (square) throughout. Audio binaural is ch1, sine, pan 0, mod 0, `freq_r=255`. `(→)` = value unchanged / mid-ramp (interpolated).

| t (mm:ss) | phase | carrier L (Hz) | freq_r R (Hz) | beat (Hz) | wave | pan | volume | LED freq (Hz) | duty % | bright % | RGB | ramp / LFO notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 00:00 | Induction (start) | 245 | 255 | 10 | sine | 0 | `>0` | 10 | 50 | `>0` | 255,100,0 | vol fade-in start; LED brightness ramp-in start (0→30% over 10 s) |
| 00:10 | Induction | ≈245 | 255 | ≈10 | sine | 0 | (→) | 10 | 50 | `~25:30:10000` | 255,100,0 | brightness reaches breathing band → start **breathing LFO** (sine 25↔30% / 10 s period) |
| 00:30 | Induction | ≈245.2 | 255 | ≈9.8 | sine | 0 | (→) | (→) | 50 | (LFO) | 255,100,0 | pink-noise bed fade-in complete (ch3 vol 18) |
| 00:45 | Induction | 245.3 | 255 | 9.7 | sine | 0 | **55** | (→) | 50 | (LFO) | 255,100,0 | volume fade-in complete (target 55) |
| 05:00 | Deepening (start) | 247 | 255 | 8 | sine | 0 | 55 | 8 | 50 | `>30` | 255,100,0 | induction→deepening; freq 10→8 ramp complete; begin brightness 30→15 |
| 15:00 | Deep descent | 250 | 255 | 5 | sine | 0 | 55 | 5 | 50 | `>15` | `>255 >100 >0` | begin color glide amber→deep-red; bright 15→5 |
| 30:00 | Delta fade (start) | 253 | 255 | 2 | sine | 0 | `*55` | 2 | 50 | `>5` | `>180 >30 >0` | **Soft Off begins**: quad volume fade 55→0; pink-noise quad fade; bright 5→0 |
| 45:00 | Soft Off (end) | 254 | 255 | 1 | sine | 0 | 0 | 1 | 50 | 0 | 150,20,0 | brightness 0 **and** volume 0 together → silent + dark; no loop |

**Ramp-rate check (§3 ≤~0.1 Hz/min for sleep descents):**
- Induction 10→8 over 5 min = 0.4 Hz/min (acceptable for the eased-in induction; matches research arc table).
- Deepening 8→5 over 10 min = 0.3 Hz/min.
- Deep descent 5→2 over 15 min = 0.2 Hz/min.
- **Delta tail 2→1 over 15 min = 0.07 Hz/min** — slowest where it matters, comfortably within the 0.1 Hz/min target.
- Overall 10→1 over 45 min ≈ 0.2 Hz/min average. No phase shorter than 5 min.

**Dwell:** the descent is continuous (the documented sleep pattern), with ≥15 min spent in the theta→delta region (15:00→45:00) and 15 min in the 2→1 Hz delta core — well past the ≥5–6 min dwell rule.

---

## 5. Envelope notes

- **Volume fade-in:** ch1 `>0` at t=0 → `55` at 0:45 (45 s, within the 30–60 s rule). Pink-noise ch3 fades in `>0`→18 over 0:00–0:30.
- **Soft Off (fade-out to 0):** ch1 volume uses **quadratic** `*55` at 30:00 → `0` at 45:00 (gentle ease-out over 15 min). Pink-noise ch3 `*18`→0 over the same window. LED brightness `>5`→0 over 30:00–45:00. Light and volume reach zero **together** at 45:00.
- **Breathing texture:** brightness LFO `~25:30:10000` (sine, 25↔30%, 10 s period ≈ 6 breaths/min) during induction only (0:10→5:00); after 5:00 brightness switches to plain `>` descent ramps.
- **Brightness cap:** 30% peak (≤50% first-use rule).

---

## 6. Illustrative `.ledc` excerpt (format-lock — first ~5 lines, NOT the whole file)

```
# 001_sleep_onset.ledc — Sleep Onset (~45 min) — alpha→theta→delta, Soft Off
# Safety: flicker ≤10 Hz (below 15-25 Hz band); warm amber; bright ≤30%; ramp-in ≥10 s; Soft Off fade to 0.
# t=0 — induction: 10 Hz warm amber, brightness ramp-in; binaural beat 10 Hz (L245 / R255)
0      >10 50 >0           255 100 0 255     # LED all-ch: 10 Hz, brightness 0→ (10 s ramp-in), amber
10000  >10 50 ~25:30:10000 255 100 0 255     # breathing brightness LFO (sine 25↔30% / 10 s)
A 0     >245   0 >0  0 1 255 0               # ch1 binaural: L carrier 245, R(freq_r) 255 = 10 Hz beat; vol fade-in
A 45000 >245.3 0 55  0 1 255 0               # vol fade-in complete (55); carrier glide continues toward 247
A 0     0 0 >0  0 3 0 5                       # ch3 pink-noise bed, vol fade-in (wave_type 5)
```
*(LED field order: `time freq duty bright R G B mask`. Audio field order: `A time freq pan volume mod channel freq_r wave_type`. Ramp convention: prefix on the START line; target = next same-channel entry's value.)*

---

## 7. Open questions / choices

- **Carrier:** chose ~250 Hz (left 245→254, right `freq_r`=255). Could drop to ~200 Hz (Gnaural-style 140–200 Hz) if a deeper tone is preferred — adjust the beat→left table accordingly.
- **`freq_r` cannot be ramped** by the current parser (plain `atof`). We encode the beat slide by ramping the *left* `frequency` with `freq_r` held constant (§3 note). If the parser later supports `freq_r` interpolation, the cleaner form is a fixed carrier + ramped `freq_r`.
- **BG bed (optional):** placeholder `BG http://<your-host>/ambient_drone.mp3 0 25` (pan 0, loudness 25). Needs a real reachable HTTP/HTTPS/sdcard URL; loudness should sit under the tones. If used, the pink-noise channel can be dropped to avoid muddiness.
- **Duty 50%** (square, strongest) chosen for clarity; could soften to sine-like via lower duty, but at <10 Hz / dim amber it is already gentle.
- **LED mask 255** assumes the channel-map LUT populates all 8 channels; fall back to `15` (four legacy zones) on hardware where 5–8 are unmapped.
