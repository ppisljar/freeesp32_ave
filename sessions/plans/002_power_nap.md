# Session Plan 002 — Power Nap

> Authoring plan for the `descent` family. Precise enough that transcription to
> `002_power_nap.ledc` is mechanical. Complies with
> `sessions/SESSION_DESIGN_GUIDELINES.md` and the research in
> `reports/session_research/`. **This is a plan, not the `.ledc`.**

---

## 1. Header

| Field | Value |
|---|---|
| **Name** | Power Nap |
| **Goal / persona** | A short restorative nap that drops the user into light delta rest, holds briefly, then **deliberately re-ascends to wake them before sleep inertia sets in**. NASA-style 20-min nap. |
| **Total duration** | ~22:30 (kept under ~25 min to avoid sleep-inertia grogginess) |
| **Target bands** | descend alpha 10 → delta ~3.4 Hz over ~16 min → brief hold → **re-ascent** 3.4 → 7 → 12 Hz, then audio 12→14 Hz to wake |
| **Arc type** | **Descent + brief hold + Re-ascent (wake)** — one of the few presets that legitimately up-ramps |

**Rationale (cited):** `session_arc_design_by_usecase.md` §6 (Power Nap): drop-in 10→4, delta rest 4→3.4, then re-ascent 3.4→7→12 with rising brightness/volume, wake finish 12→14; total ~20–25 min. Guidelines §2 row "Power nap": delta→wake, ↓~3.4 then ↑~12, 20–25 min, *dim → rise on wake*. Synthesis §6 item 2. The re-ascent is the whole point; total is deliberately short.

---

## 2. Safety classification (§4 of guidelines — mandatory)

| §4 rule | Applies? | How this plan satisfies it |
|---|---|---|
| AVOID bright full-field flicker **15–25 Hz** | Yes (the wake stage approaches it) | **LED flicker is capped at 12 Hz.** When the audio beat ramps 12→14 Hz to wake, the LED flicker is **held at 12 Hz** — the entrainment past 12 Hz is carried by **audio only**. Never enters 15–25 Hz visually. |
| CAUTION **3–60 Hz** (caps, ramp in/out ≥10 s, no abrupt stop) | Yes (3.4–12 Hz flicker) | Brightness **capped ≤50%**; LED brightness **ramps in over 10 s** (0→25% across 0:00–0:10) and **fades off over 2 min** (4:00→6:00) — no abrupt edges; re-ascent brightness rises gradually over 4 min. |
| SAFER zones <3 Hz / >65 Hz | partial | Delta floor (~3.4 Hz) sits just above the <3 Hz line, inside the managed 3–60 Hz CAUTION regime (dim/off during rest). |
| No saturated-red strobe in 3–60 Hz; no red↔blue alternation | Yes | Color is **warm amber (255,100,0)** descending, **off** during rest, then **amber→warm white (255,200,120)** on wake — no saturated red, no blue alternation. |
| Brightness floor / first-use ≤50% / audio ≤~65 dB | Yes | Peak brightness **50%** (at the cap); audio peaks at **65 / 100**; warm dim field, no black↔white strobe. |
| Up-ramp into beta during sleep is forbidden **except** a deliberate wake | This IS the sanctioned exception | This is a *nap* (not sleep-through). The re-ascent is an intentional, gradual wake — exactly the documented power-nap exception. A true sleep preset (see 001) must never do this. |
| Contraindication screening | UI-enforced | Handled by Firmware UI cap + acknowledgement (out of `.ledc` scope). |

---

## 3. Channel allocation

| Resource | Channel | Use | Pan layout |
|---|---|---|---|
| Audio ch **1** | binaural beat | Left ear = `frequency` (ramped); Right ear = `freq_r` (held 255 Hz). Perceived beat = `freq_r − frequency`. wave_type 0 (sine). | **pan 0** (centered; `freq_r` provides the L/R split) |
| LED ch **1–8** | full-field warm flicker | mask **255** (all 8). Flicker tracks the beat 1:1 **but capped at 12 Hz**. | n/a |
| Background (optional) | `BG` bed | optional thin ambient under the tones during descent/rest | pan 0 |

**Encoding note (same as 001):** `freq_r` is not interpolatable, so the beat is slid by ramping the **left** `frequency` with `freq_r` held at 255 Hz. Perceived beat = `255 − frequency`. The nap beat goes *down then up*, so the left frequency rises (descent) then falls (re-ascent):

| Beat (Hz) | Left `frequency` (Hz) |
|---|---|
| 10 | 245 |
| 4 | 251 |
| 3.4 | 251.6 |
| 7 | 248 |
| 12 | 243 |
| 14 | 241 |

Both ears stay 241–255 Hz (carrier ≈248 Hz, <1000 Hz; max offset 14 Hz < 35 Hz — satisfies §3 binaural rules).

---

## 4. Segment table (absolute times)

LED line is `mask 255`, `duty 50%` throughout. Audio binaural is ch1, sine, pan 0, mod 0, `freq_r=255`. `(→)` = value unchanged / mid-ramp.

| t (mm:ss) | phase | carrier L (Hz) | freq_r R (Hz) | beat (Hz) | wave | pan | volume | LED freq (Hz) | duty % | bright % | RGB | ramp / LFO notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 00:00 | Drop-in (start) | 245 | 255 | 10 | sine | 0 | `>0` | 10 | 50 | `>0` | 255,100,0 | vol fade-in start; LED brightness ramp-in start (0→25% over 10 s) |
| 00:10 | Drop-in | ≈245 | 255 | ≈10 | sine | 0 | (→) | 10 | 50 | `>25` | 255,100,0 | brightness reaches 25% (then held) |
| 00:30 | Drop-in | 245.5 | 255 | 9.5 | sine | 0 | **50** | (→) | 50 | 25 | 255,100,0 | volume fade-in complete (target 50) |
| 04:00 | Delta glide (start) | 251 | 255 | 4 | sine | 0 | `>50` | 4 | 50 | `>25` | 255,100,0 | beat 10→4 done; begin vol 50→30 and **light fade toward off** |
| 06:00 | Delta rest | 251.1 | 255 | ≈3.9 | sine | 0 | 30 | 3.9 | 50 | `>0` | 255,80,0 | light essentially **OFF**; quiet rest; beat glides on toward 3.4 |
| 16:00 | Delta floor | 251.6 | 255 | 3.4 | sine | 0 | 30 | 3.4 | 50 | 0 | (off) | deepest point; dark; brief floor/hold |
| 17:00 | Re-ascent (start) | 251.6 | 255 | 3.4 | sine | 0 | `>30` | 3.4 | 50 | `>0` | 255,100,0 | **wake begins**: freq, brightness, volume all start rising; amber returns |
| 19:00 | Re-ascent (mid) | 248 | 255 | 7 | sine | 0 | `>45` | 7 | 50 | (rising) | 255,100,0 | theta pass-through |
| 21:00 | Re-ascent | 243 | 255 | 12 | sine | 0 | `>60` | **12 (CAP)** | 50 | `>50` | `>255 >150 >60` | LED flicker reaches the **12 Hz cap**; color warming toward white |
| 22:30 | Wake finish | 241 | 255 | **14** | sine | 0 | 65 | **12 (held)** | 50 | 50 | 255,200,120 | audio beat 14 Hz; **LED held at 12 Hz** (audio-only above cap); bright warm-white; awake. **No fade-out.** |

**Ramp-rate check (§3):**
- Drop-in 10→4 over 4 min = 1.5 Hz/min (a brisk but gentle nap entry — acceptable; this is a nap, not deep sleep onset).
- Delta glide 4→3.4 over 12 min = 0.05 Hz/min (very slow — the restful core).
- Re-ascent 3.4→12 over 4 min = ~2.1 Hz/min (deliberate wake; §3 allows up to ~5 Hz/min for wake ascents).
- Wake finish 12→14 over 1.5 min = 1.3 Hz/min.

**Dwell:** delta rest spans 06:00→16:00 (~10 min ≤4 Hz) plus the 16:00 floor — the restorative core comfortably exceeds the ≥5–6 min dwell rule.

---

## 5. Envelope notes

- **Volume fade-in:** ch1 `>0` at t=0 → `50` at 0:30 (30 s, within 30–60 s rule).
- **Rest dip:** volume eases 50→30 over 04:00–06:00 and holds 30 through the delta rest (quiet anchor, not silence).
- **Wake rise (NOT a fade-out):** volume `>30`→45→`>60`→65 across 17:00→22:30; LED brightness `>0`→50% across 17:00→21:00; LED freq 3.4→12. All three rise together to wake. Session **ends bright and awake** — there is deliberately **no Soft Off** here.
- **Light off during rest:** LED brightness `>25`→0 over 04:00→06:00 (2 min fade, ≥10 s ramp-out rule), held 0 until 17:00.
- **Brightness cap:** 50% peak; **flicker cap 12 Hz** (audio carries 14 Hz).
- **Optional wake cue:** a soft chime at ~22:00 could be added on a spare audio channel; not required.

---

## 6. Illustrative `.ledc` excerpt (format-lock — first ~5 lines, NOT the whole file)

```
# 002_power_nap.ledc — Power Nap (~22.5 min) — descend to delta ~3.4, re-ascend to wake
# Safety: LED flicker capped ≤12 Hz (audio carries the 14 Hz wake); bright ≤50%; ramp in/out ≥10 s; warm amber→warm white.
# t=0 — drop-in: 10 Hz warm amber, brightness ramp-in; binaural beat 10 Hz (L245 / R255)
0      >10 50 >0   255 100 0 255             # LED all-ch: 10 Hz, brightness 0→ (10 s ramp-in), amber
10000  >10 50 >25  255 100 0 255             # brightness up to 25%
A 0     >245   0 >0  0 1 255 0               # ch1 binaural: L carrier 245, R(freq_r) 255 = 10 Hz beat; vol fade-in
A 30000 >245.5 0 50  0 1 255 0               # vol fade-in complete (50); carrier glide continues toward 251
```
*(LED field order: `time freq duty bright R G B mask`. Audio field order: `A time freq pan volume mod channel freq_r wave_type`. Ramp convention: prefix on the START line; target = next same-channel entry's value.)*

---

## 7. Open questions / choices

- **Carrier:** ~250 Hz (left 241→254, right `freq_r`=255), same family as 001 for a consistent timbre. Adjustable.
- **`freq_r` cannot be ramped** (plain `atof`); beat slide encoded by ramping the left `frequency` (§3 note). Because the nap goes down *then* up, the left frequency rises then falls.
- **12 Hz LED cap:** chosen to stay clear of the 15–25 Hz danger band while still giving a visible wake pulse. The final 12→14 Hz of the wake is **audio-only**. Could alternatively cap at 10 Hz for extra margin (then audio carries 10→14).
- **Isochronic wake option:** for a stronger arousal, the re-ascent could add an audio `modulation` at the beat Hz (sharp isochronic pulse, the harder waking driver) instead of pure binaural — not included here to keep the nap gentle. Worth A/B testing.
- **BG bed (optional):** thin ambient only, e.g. `BG http://<your-host>/soft_ambient.mp3 0 20`, faded out before the wake; needs a real reachable URL.
- **LED mask 255** assumes all 8 channels are mapped; fall back to `15` where 5–8 are unmapped.
- **Total length** is ~22:30; trim/extend the delta rest (06:00–16:00) to land on a preferred 20 or 25 min nap.
