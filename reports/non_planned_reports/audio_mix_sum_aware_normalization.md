# Audio Mix: Sum-Aware Headroom (replaces 1/N_active)

**Date:** 2026-07-18
**Type:** unplanned bug fix (user-reported)
**Status:** implemented, firmware build clean (app `0x1357c0`→`0x1357d0`).
Not yet flashed / hardware-verified.

## Bug

User playing `09_lucid_hypnagogic.ledc` reported a "large loudness shift" around
t=480000 (8:00) where a binaural beat seems to die. At exactly 8:00 the timeline
enables ch2 (250/256 Hz binaural, fading in) and ch3 (brown noise, fading in)
while ch1 (220 Hz binaural) is already at its plateau.

## Root cause

`audio_generator_fill_buffer` scaled **every** channel by `1/N_active` (count of
active channels) — `audio_generator.c` ~657. When ch2+ch3 activated, `N` went
1→3, so the global gain dropped 1.0→0.333, attenuating the established ch1 by
**20·log₁₀(3) ≈ 9.5 dB** in ~5 ms. ch2's new beat was still near-zero (fading in),
so perceptually the strong beat ducked out. Exactly the user's report.

`1/N` is a count-based worst-case normalizer (assumes every channel at full scale,
same-panned). Authored content sets deliberate sub-unity volumes, so `1/N` throws
away headroom and couples channels that should be independent.

## Fix

Sum-aware headroom: `gain = min(1, 1/Σamp)` where `Σamp` is the sum of active
channels' live envelope amplitudes (`current_amp`). Attenuate **only** when the
mix would actually exceed ±1.0.

- `09` sums to 0.81 → gain 1.0 → **no attenuation, no pumping**; ch1 holds its
  authored 0.38 as ch2/ch3 fade in. Bug fixed.
- Multi-carrier Gateway sessions (see below) sum up to ~5× → attenuated by exactly
  what's needed to stay clip-safe, by actual level not channel count.

Reuses the existing cross-buffer smoothing ramp (renamed `s_inv_n_*` →
`s_mix_gain_*`, `inv_n_per_sample` → `mix_gain_per_sample`) so a mix crossing the
±1 ceiling slews over `AUDIO_AMP_RAMP_SAMPLES` instead of clicking. The downstream
hard clamp in `audio_test.c:152-153` (after BG mix + master gain) remains the
final safety net.

## Why not simply remove 1/N

A library scan of the true simultaneous `Σ(target vol)` over each timeline found
**8 of 36 sessions sum past 1.0** — the Monroe patent-upgrade "septon stack"
Gateway files stack 13–14 carriers:

| Session | Peak concurrent Σvol |
|---|---|
| `24_gateway_focus27` | 5.12 (13 ch @ 9:00) |
| `25_gateway_journey` | 3.68 (14 ch) |
| `21`/`22`/`23_gateway_focus…` | 1.9–2.2 |
| `36_shamanic_trance_drum` | 1.75 |
| `20_gateway_focus10`, `34_gateway_obe` | 1.6 |
| `09_lucid_hypnagogic` (reported) | 0.81 |

These rely on headroom division; plain "no divide + clamp" would hard-clip them
into distortion. Sum-aware keeps them clip-safe with the minimum necessary gain
reduction while leaving the 28 under-unity sessions (incl. `09`) untouched.

## Files

- `main/audio_generator.c`: sum-aware target + variable rename + comments (the
  count loop, ramp arming, per-sample precompute, and the mix apply site).

## Verify on device

- `09_lucid_hypnagogic`: confirm NO loudness drop / beat-duck at t=8:00 when ch2/ch3
  fade in; ch1 stays steady.
- A Gateway stack (e.g. `24_gateway_focus27`): confirm no clipping/distortion at
  its busiest section (~9:00, 13 carriers) — the mix should attenuate smoothly.
- General: no clicks on channel start/stop (the smoothing ramp is preserved).

## Possible future refinement

`Σamp` is a mono (pan-blind) bound — two hard-panned channels don't actually sum on
the same output channel, so a pan-aware per-L/R `Σ` would attenuate slightly less
for hard-panned content. The mono sum is a safe conservative bound; refine only if
hard-panned multi-channel content proves over-attenuated.
