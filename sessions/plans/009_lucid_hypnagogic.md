# Session Plan 009 — Lucid / Hypnagogic  ⚠️ EXPERIMENTAL [W]-tier

> Authoring plan. Transcribe mechanically to `sessions/library/009_lucid_hypnagogic.ledc`.
> Rulebook: `sessions/SESSION_DESIGN_GUIDELINES.md`. Evidence: `reports/session_research/`.
> **Confidence: [W] (weak / exploratory).** The theta substrate is evidence-based; the
> "lucid / REM-cue" framing is not validated on this device — label it experimental in
> the library and UI.

## 1. Header
- **Name:** Lucid / Hypnagogic (experimental)
- **Goal / persona:** A user lying down at sleep onset who wants a long, dim, mostly
  audio-driven **theta plateau** with gentle ambient "dream cues" to encourage
  hypnagogic imagery / lucid-dream onset, ending in a **Soft Off** so they can drift
  into sleep undisturbed.
- **Total duration:** 60:00 (mm:ss)
- **Target band:** theta, **hold ~6 Hz** (4–7 Hz region) — a long plateau, no exit ramp.
- **Rationale:** Theta (4–8 Hz) is linked to meditation, drowsiness, hypnagogia and
  memory (`scientific_research_protocols.md` §Theta, **[moderate]**); binaural at low
  freq with headphones is the gentle tool of choice (guidelines §3). Lucid-dream
  *cueing* (Remee/NovaDreamer-style light flashes) requires REM detection we cannot do
  (§7 out-of-scope), so cues here are **timed ambient approximations** — hence the
  [W]/experimental tag (synthesis §6 item 8 lists this as optional/exploratory).

## 2. Safety classification (§4 — mandatory)
- **"AVOID bright flicker 15–25 Hz."** → N/A by design: the only frequencies present are
  theta (6 Hz) and alpha (10 Hz), both **<12 Hz**, and the light is kept **very dim**
  (≤15 %). No content anywhere near the danger band.
- **"CAUTION 3–60 Hz: ramp light in/out ≥10 s, never stop abruptly; brightness caps."**
  → 6 Hz dim flicker is within the caution band; brightness ramps in 0→12 % over 15 s,
  is capped ≤15 %, and at the end fades to 0 over ~5 min (Soft Off) — never abrupt.
- **"Sleep/lucid sessions skip the Exit and end in a Soft Off."** → Light brightness and
  **all** audio volume fade to 0 **simultaneously** over the final 5 min; no
  alpha/SMR exit ramp.
- **Warm amber colour for low/sleep content is fine** (flicker is <4–12 Hz, dim). →
  RGB (255,120,30). No saturated-red strobe (dim warm, not a bright red strobe).
- **Binaural carrier rule (§3):** carrier <1000 Hz, L/R offset <35 Hz. → 250 Hz carrier,
  6 Hz offset. ✔
- **Audio ≤ ~65 dB; brightness floor.** → volumes ≤40; brightness dim with a floor.

## 3. Channel allocation
**Audio (16 ch):**
- **ch1 — isochronic theta driver (sweepable).** Carrier **220 Hz** sine
  (`wave_type 0`), `pan 0`, beat = `modulation`, ramped 10→6 then held 6 Hz. Used for the
  descent because **`modulation` can ramp** (see §7 constraint).
- **ch2 — binaural theta layer (fixed beat).** `frequency 250`, **`freq_r 256` (= 6 Hz
  binaural beat)**, `pan 0`, low volume (~25), stepped in at the plateau. Gentle,
  headphone-expected. Fixed because `freq_r` is **not** sweepable (§7).
- **ch3 — brown-noise ambiance + auto-pan (REM-ish spaciousness).** `wave_type 6`
  (brown), low volume (~18), **slow sine pan LFO** for a drifting, oceanic field.
- **(optional) BG bed** — ambient dream soundscape via `BG <url> 0 25`.

**LED (8 ch):**
- **mask 15** (full frame; `255` if all 8 mapped). **Very dim warm amber (255,120,30)**,
  6 Hz theta flicker (matched 1:1 to the beat), duty 50, with a **breathing brightness
  sine LFO** during the plateau (the ambient "dream cue"). Soft-off to 0.

**Pan layout:** ch1/ch2 centred; ch3 auto-pans slowly (±60) for ambience.

## 4. Segment table (absolute times)
Warm amber RGB = (255,120,30). 60 min = 3,600,000 ms.

| t (mm:ss) | phase | audio carrier Hz | beat (mod / freq_r) Hz | wave | pan | vol | LED freq Hz | duty % | bright % | RGB | ramp / LFO notes |
|-----------|-------|------------------|------------------------|------|-----|-----|-------------|--------|----------|-----|------------------|
| 00:00 | induction start | 220 (ch1) | **mod 10** | sine | 0 | **0 → (>)** | **10** | 50 | **0 → (>)** | 255,120,30 | vol fade-in (60 s); dim warm 10 Hz flicker; brightness ramp-in |
| 00:15 | induction | 220 | mod 10 | sine | 0 | — | 10 | 50 | **12** | 255,120,30 | brightness reaches 12 % (≥10 s ramp-in) |
| 01:00 | induction | 220 | mod 10 | sine | 0 | **40** | 10 | 50 | 12 | 255,120,30 | vol fade-in complete |
| 02:00 | deepening | 220 | **mod 10 → (>) → 6** | sine | 0 | 40 | **10 → (>) → 6** | 50 | 12 | 255,120,30 | beat + flicker descend 10→6 over 6 min (~0.7 Hz/min); LED freq 1:1 with beat |
| 08:00 | **plateau start** | 220 | **mod 6** | sine | 0 | 38 | **6** | 50 | **LFO 6↔15** | 255,120,30 | theta hold; brightness **sine LFO 6↔15 % / 10 s** = breathing "dream cue" |
| 08:00 | + binaural layer | 250 (ch2) | **freq_r 256 (6 Hz)** | sine | 0 | **0 → (>) → 25** | — | — | — | — | ch2 binaural fades in (fixed 6 Hz beat) |
| 08:00 | + ambiance | 200 (ch3) | n/a | brown | **LFO ±60** | **0 → (>) → 18** | — | — | — | — | ch3 brown noise; **pan sine LFO −60↔+60 / 30 s** (REM-ish drift) |
| 55:00 | **Soft Off start** | 220 / 250 / 200 | hold | — | 0 / 0 / LFO | **all → (>) → 0** | 6 | 50 | **15 → (>) → 0** | 255,120,30 | light + ALL audio fade to 0 simultaneously over 5 min |
| 60:00 | end (off) | — | — | — | — | **0** | 6 | 50 | **0** | 255,120,30 | fully off — Soft Off complete (no wake ramp) |

- **Plateau dwell:** 6 Hz held 08:00–55:00 = **47 min** (≫ §3; theta/sleep content
  wants long holds). ✔
- **Descent rate:** 10→6 Hz over 6 min ≈ 0.7 Hz/min (gentle, sleep-appropriate). ✔
- **REM-ish cues = ambient only:** brightness breathing LFO (6↔15 % / 10 s) + ch3 pan
  LFO (±60 / 30 s). These approximate dream cueing; **true REM-triggered light flashes
  need EEG/eye detection and are out of scope (§7).** State this in the header comment.

## 5. Envelope notes
- **Volume fade-in:** ch1 0 → 40 over ~60 s (§3). ch2/ch3 fade in at plateau start. ✔
- **Soft Off (no Exit):** 55:00–60:00, LED brightness (15→0) and **every** audio channel
  volume (→0) ramp down **together** to zero. No alpha/SMR exit ramp — the user drifts
  into sleep. ✔ (§1/§4 sleep+lucid rule).

## 6. Illustrative `.ledc` excerpt (first ~5 real lines)
Ramp convention (verified in `config_parser.c` ~L1995): `>` prefix on the **earlier**
entry, value = **start**; **target** = next same-channel entry's value. LFOs are
self-contained: `PREFIXstart:end:period_ms` (`~` = sine).

```
# 009 Lucid / Hypnagogic (EXPERIMENTAL [W]) — theta 6 Hz plateau, dim warm, Soft Off ending.
# REM "cues" are ambient LFO approximations only (true REM-triggered flashes need EEG, out of scope).
# LED: time freq duty bright R G B mask   |   Audio: A time freq pan vol mod ch [freq_r] [wave]
0      10 50 >0 255 120 30 15        # dim warm 10 Hz flicker, brightness fade-in from 0
15000  10 50 12 255 120 30 15        # brightness 12% (15 s ramp-in, ≥10 s rule)
120000 >10 50 12 255 120 30 15       # t=2:00 LED freq descend START 10 ...
480000 6 50 ~6:15:10000 255 120 30 15 # t=8:00 freq 6 Hz; brightness sine LFO 6<->15% /10s (breathing cue)
A 0     220 0 >0 10 1                # ch1 isochronic: 220 Hz carrier, vol fade-in, beat 10 Hz
A 120000 220 0 40 >10 1             # t=2:00 beat ramp START 10 Hz ...
A 480000 220 0 38 6 1               # t=8:00 ... TARGET 6 Hz (theta plateau)
A 480000 250 0 >0 0 2 256 0         # t=8:00 ch2 binaural fade-in: 250 Hz L / 256 Hz R = 6 Hz beat (FIXED)
A 480000 200 ~-60:60:30000 >0 0 3 0 6 # t=8:00 ch3 brown noise, pan sine LFO ±60 /30s, vol fade-in
```

## 7. Open questions / choices
- **Grammar constraint — `freq_r` is NOT sweepable.** `config_audio_entry_t` has no
  `freq_r` interp field, so the binaural beat (ch2) can only be **stepped**, not ramped.
  That is why the *descent* uses isochronic `modulation` (ch1, which **is** sweepable)
  and binaural (ch2) only joins as a **fixed** 6 Hz layer at the plateau. Confirm this is
  acceptable; alternative is stepwise binaural-beat changes.
- **Lucid cueing is unvalidated** on this hardware — keep the [W]/experimental label;
  the brightness/pan LFOs are mood, not a proven lucidity trigger.
- **Plateau frequency:** 6 Hz chosen; 7 Hz (or 7.83 "Schumann", [W]) are alternatives.
- **Light during plateau:** dim 6 Hz flicker chosen (matched 1:1 to the beat, <12 Hz,
  dim, safe). A fully steady dim lamp is an even gentler alternative if any flicker
  disturbs sleep onset.
- **LED freq-descent + brightness-LFO on the same channel:** the 02:00 entry ramps
  `freq` while the 08:00 entry switches `brightness` to an LFO — verify the engine
  handles a per-field ramp followed by a per-field LFO cleanly (it should, fields are
  independent).
- **BG bed:** optional ambient dream soundscape; needs a user URL.
```
