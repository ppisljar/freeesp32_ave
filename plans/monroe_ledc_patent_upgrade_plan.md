# Plan — Patent-Upgrade the Monroe Gateway `.ledc` Sessions

Apply the Monroe-patent techniques (from `reports/session_research/monroe_patents_guide.md`)
to each existing Gateway Focus session. **`.ledc`-only — no firmware, no code changes.**

## Scope
Update these 6 files in `freeesp32_ave/sessions/library/` **in place**:
`20_gateway_focus10` · `21_gateway_focus12` · `22_gateway_focus15` ·
`23_gateway_focus21` · `24_gateway_focus27` · `25_gateway_journey`.

Do NOT touch files 00–19 (those are non-Monroe). Do NOT create new files here (the
`26_sleep_descent` 90-min schedule session is a SEPARATE follow-up, not part of this plan).

## What we ARE applying (and the ONE thing we're not)
| # | Technique | Applying? | How |
|---|---|---|---|
| 2 | Phased Pink Sound bed | ✅ | pink-noise channel, pan-LFO L↔R, −18 dB under beats |
| 3 | Septon multi-carrier stack | ✅ | split the ONE dominant theta layer into 3 detuned carriers |
| 4a | 275 Hz / 200–212 band anchor | ✅ | septon + bed live in the patent's optimal band; measured signature carriers UNCHANGED |
| 4b | Layer amplitude staging (−10/−15/−20 dB) | ✅ | recompute layer vols to the patent mix convention |
| 4c | 40 s / −10 dB voice cadence | ✅ (light) | keep existing S-rows; only adjust if spacing is wildly off |
| — | **EEG-contour (Fourier) waveform** | ❌ **CANNOT** | engine has no arbitrary-contour wave; leave carriers sine |

## The `.ledc` audio row (reference)
`A <time_ms> <carrier> <pan> <vol> <mod> <channel> <freq_r> [<wave>]`
- binaural beat = `freq_r − carrier`; pan 0 = center (range −100..100); vol 0..100.
- `<wave>`: 0 sine (default) · 1 square · 2 triangle · 3 saw · **4 white · 5 PINK · 6 brown**.
- Sweep/LFO glyphs on any numeric cell: `>v`/`*v` = linear/quad ramp to v;
  `~start:end:period` = **sine LFO** (also `^ / \ _` = tri/saw-up/saw-down/square).

---

## Transformation rules (apply to each file)

### Rule A — Add ONE phased pink-noise bed channel
- Add a single new channel using the **next free channel index** in that file.
- Row shape (steady-state): `A <t> 200 ~-100:100:6000 <VOL_BED> 0 <ch> 0 5`
  - `wave = 5` (pink). `freq_r = 0` (no binaural — it's noise). `carrier = 200` (ignored for noise; keep it in-band).
  - `pan = ~-100:100:6000` → smooth L↔R sine pan, **6.0 s cycle** (the patent's "phased" character).
  - `VOL_BED` = **−18 dB below the session's dominant layer vol** (see dB→vol table). For a dominant vol of 65 → bed vol ≈ **8**.
- **Envelope the bed to the existing arc:** it must fade IN during the prep phase and fade OUT during the return, exactly like the other layers — never an abrupt noise burst. Use the same timed-row / vol-ramp pattern the file already uses for its layers (low/zero vol at t=0, up during prep; down to 0 in the return). Keep the pan-LFO on every bed row so panning is continuous.

### Rule B — Septon-stack the single DOMINANT theta layer
Pick the one **4 Hz theta layer that carries the session** (for most files that's the primary
`200[4.0]` / `250[4.0]` / `300[4.0]` bed tone — choose the lowest-carrier 4 Hz layer, typically the 200 Hz one).
Replace that ONE channel with a **3-carrier septon stack** on three consecutive free channels:
- ch a: carrier **200**, freq_r **204**
- ch b: carrier **204**, freq_r **208**
- ch c: carrier **208**, freq_r **212**
(L ear hears 200/204/208, R hears 204/208/212 → 4 Hz **binaural AND monaural** beats; speaker-robust.)
- **Vol:** each septon channel = **round(dominant_vol / 3)** (e.g. 65 → 22). This preserves loudness and avoids clipping.
- Keep pan 0, wave 0 (sine) on the septon channels. Follow the SAME arc envelope (fade-in/hold/fade-out timings) the replaced layer had.
- **Only ONE layer per file gets septon-stacked.** Do not stack multiple layers (channel budget).
- **Skip Rule B entirely for `25_gateway_journey`** (it already uses 13 channels; no room). Journey gets the pink bed only.

### Rule C — 275 Hz / band anchoring (NON-DESTRUCTIVE)
- **Do NOT overwrite the measured signature carriers** (100/250/300/400/500/600/750/900 etc.) — those are the empirically-correct fingerprint of each Focus level. Preserve them exactly.
- The septon (200–212) and the pink bed (200) already sit in the patent's ~200–275 "most effective" band. That is where we honor 275 Hz — additively, in the layers we ADD, not by mutating the signature.
- (Rationale in guide §2: 275 Hz is the optimum for a *single* carrier; our sessions are deliberately multi-carrier to match the measured tapes. Adding band-optimal layers > destroying the measured map.)

### Rule D — Layer amplitude staging (patent mix convention)
Re-balance the layer vols to the US 5,356,368 mix table, RELATIVE to the dominant layer (0 dB ref):
- **Dominant state layer** (the septon theta bed): reference, keep near its current vol (~65).
- **Secondary state layers** (delta / alpha / other theta): **−10 to −15 dB** → vol ~20–37.
- **Signature high-carrier layers** (the alpha-10 / theta-7 / beta-16 add-ons): keep them subtle, **−15 to −20 dB** → vol ~12–20 (they were already quieter at ~35–45; nudge toward this if it doesn't break the intended audibility).
- **Pink bed:** −18 to −20 dB → vol ~7–10.
dB→vol (linear %, ref 65): −10 dB→20 · −15 dB→12 · −18 dB→8 · −20 dB→6.5. General: `vol = ref * 10^(dB/20)`.
Use judgment — don't make a signature layer inaudible; the goal is the patent's *relative staging*, not literal dead reproduction.

### Rule E — Voice cadence (light touch)
Leave the existing `S` narration markers. Only if two markers are absurdly close or a long
stretch (>8 min) has none, nudge spacing. Do NOT rewrite narration text.

---

## HARD invariants (must not break — these fail the build/safety review)
1. **LED rows UNCHANGED.** Do not touch any LED (non-A/S) row. LED flicker stays **4 Hz** everywhere — the alpha-10 / theta-7 / **beta-16** beats remain **AUDIO-ONLY**. (Independently re-checked after edits: max LED flicker must be 4 Hz.)
2. **Channel index ≤ 16.** Count total distinct channel indices after edits; must be ≤ 16. (focus10→~7, focus12/15→~10, focus21→~9, focus27→~13, journey→~14 with bed only.)
3. **Preserve the narrative arc + measured signature carriers.** Prep→base→signature→hold→return timing and the measured carrier/beat map stay intact.
4. **Headphones/binaural header note stays.** Add one line to each header documenting the new pink bed + septon (cite guide).
5. **`node sessions/validate_session.mjs library/<file>` → 0 errors, 0 warnings.** Iterate until clean. If the validator warns about a pink/noise channel or the pan-LFO syntax, fix the syntax (don't disable the feature) and note it.

## Validation gate (each file)
```
cd freeesp32_ave/sessions
node validate_session.mjs library/<file>.ledc     # must be 0 errors / 0 warnings
```
Then report: channels before→after, which layer was septon-stacked, bed vol, and confirm LED still 4 Hz.

## Subagent split (3 agents × 2 files)
- Agent 1: `20_gateway_focus10`, `21_gateway_focus12`
- Agent 2: `22_gateway_focus15`, `23_gateway_focus21`
- Agent 3: `24_gateway_focus27`, `25_gateway_journey` (journey = bed only, NO septon)

Each agent reads: this plan + `reports/session_research/monroe_patents_guide.md` +
`freeesp32_ave/sessions/SESSION_DESIGN_GUIDELINES.md` + its target files.
**Subagents do NOT build/flash/push** (orchestrator validates + pushes after).
