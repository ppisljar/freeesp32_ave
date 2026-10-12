# Bug sweep after the A/V sync work, 2026-10-12

Found by running the shipped static lint across all 85 library sessions
(`tools/avmeasure/analyze.py lint`) and by a long-capture hardware measurement.
Both firmware bugs below were verified fixed on hardware.

## 1. Out-of-bounds write from an audio channel number (FIXED)

`A <t> <freq> <pan> <vol> <mod> <ch>` takes `ch` from a bare `atoi()` with no
range check, and the generator's `_locked` entry points indexed
`audio_channels[]` with it directly:

    esp_err_t audio_generator_start_channel_locked(int channel, ...) {
        audio_gen_channel_t* ch = &audio_channels[channel];   // no bounds check

`NUM_AUDIO_CHANNELS` is 16, so `A ... 16` wrote a whole `audio_gen_channel_t`
past the end of the array, and a negative channel wrote before it.  Reachable by
any uploaded session; `sessions/library/test.ledc` does it at line 110.

Only the three `_locked` variants were unguarded --
`start_channel_locked`, `update_params_locked`, `start_sweep_locked` -- which is
exactly why it was reachable only through the timeline batch executor (the one
caller that uses them, while holding `audio_gen_mutex`).  Every non-`_locked`
public wrapper already validated.

Fixed in both layers: the three `_locked` entry points return
`ESP_ERR_INVALID_ARG`, and the parser clamps to channel 0 with a log line naming
the offending value, so an author gets a diagnosis instead of silent corruption.

Verified: a session containing `A 0 400 0 70 8 16` now plays and the device stays
up (it previously corrupted memory past the array).

## 2. Audio channels 8..15 silently ignored every ramp (FIXED)

`find_next_audio_for_bit()` took a `uint8_t channel_bit`, and all three call
sites built it as `(uint8_t)(1u << audio->channel)`.  For channels 8..15 that
truncates to 0, nothing ever matched, so the look-ahead that arms a sweep found
no next entry and **every `>` / `*` ramp on those channels stepped instead of
ramping** -- frequency, volume, pan, mod rate, duty, phase and attack alike.
The helper also truncated internally in its comparison.

This was not a theoretical defect: it affected **14 shipped therapeutic
sessions** -- `21_gateway_focus12`, `22_gateway_focus15`, `23_gateway_focus21`,
`24_gateway_focus27`, `25_gateway_journey` and their RGB/SINE/TRAP variants --
i.e. precisely the multi-carrier Gateway sessions that use the high channels.

Fixed by widening the audio helper and its three call sites to `uint16_t`
(1u << 15 = 32768 fits).  The LED helpers stay `uint8_t`: there really are only
8 LED channels.

Verified on hardware -- a `>` frequency ramp on audio channel 8, read back from
`/api/state` (note it numbers audio channels 1-based, so .ledc ch8 is ch9):

    237.7 -> 275.5 -> 311.5 -> 349.2 -> 385.8 -> 400.0 Hz over 8 s

## 3. Long-session stability: no drift, no glitches (MEASURED CLEAN)

An 11-minute constant-10 Hz capture (0.0000% missing samples, 6523 cycles):

* median period **99.999 ms = 10.00007 Hz** (+0.0007%), robust sd 0.061 ms
* A/V offset at 2-62 s / 120-180 s / 300-360 s / 480-540 s:
  **-2.54 / -2.44 / -2.48 / -2.47 ms** -- flat to 0.1 ms over 9 minutes, i.e.
  **< 0.2 ppm** divergence between `esp_timer` (LED clock) and the I2S sample
  clock. Extrapolated over a 60 min session that is well under 1 ms.
* ONE confirmed per-cycle timing event in 11 minutes: a cycle 6.24 ms short
  (6% of a period) at device t = 577.84 s, seen independently by both detectors
  (-6.24 ms and 6.24 ms). Cause not yet identified -- most likely the flicker ISR
  being delayed ~16 ticks by something (RMT transmission, WiFi, a flash op), with
  the phase-locked advance then absorbing it. ~5 per hour, 6% of a period, so low
  priority next to what was fixed, but it IS real.
* 101 single-ISR-tick steps (0.400 ms at 10 Hz), which are expected quantisation
  rather than faults -- see section 4.

## 4. percycle.py cried wolf on long windows (TOOL BUG, FIXED)

Worth recording because it nearly produced a false bug report. Run over 55 s
windows, `percycle.py` flagged 138 "anomalies" across 6000 cycles (2.3%),
including an apparent **-36.8 ms** single-cycle glitch at t=433 s with a smooth
+/-6 ms ramp around it.

Most of it was not real, and the way I nearly got this wrong is the part worth
recording.

**A time-base mistake almost buried a real bug AND almost shipped a false one.**
`percycle.py` reports DEVICE time (measured from the sync marker); I
cross-checked those timestamps by feeding them to a detector working in FILE
time, about 2 s apart on this capture. That made a genuine event look clean, so I
first wrote "no per-cycle glitches" -- wrong. Re-running the cross-check inside
the tool, on the same already-sliced array, confirmed the t=577.84 s event at
-6.24 ms in both detectors. Never compare two measurements until both are in the
same time base, and prefer a cross-check that physically cannot disagree about
the origin.

Three real causes, all now addressed:

1. **pi slip in the phase lattice.** Wherever `|z|` dips, `np.unwrap` can take a
   pi step, shifting every later segment boundary by HALF a cycle -- after which
   `argmax(g)` latches the FALL instead of the RISE. At 50% duty the duty check
   cannot see the swap, so it surfaced as the phantom -36.8 ms at t=433 s. The
   tell was that edges after the event stayed ~54 ms out of step with those
   before: 24.46 cycles between two edges instead of a whole number. Fixed by
   driving the lattice from a robust degree-2 fit to the total phase (still
   tracks a rate sweep; a localised pi step becomes an outlier the fit ignores).

2. **ISR-tick steps counted as faults.** The flicker ISR runs at
   `clamp(f0 * 250, 1000, 10000)` Hz, so a boundary can only land on a tick and
   occasionally slips exactly one. At 10 Hz that is 0.400 ms -- just above the
   0.335 ms threshold, so every slip was a "finding": 12-24 per 55 s window,
   always in adjacent +/- pairs, measured mean 0.402 ms against a one-tick
   0.400 ms. Now counted and reported SEPARATELY rather than suppressed, because
   hiding them would also hide a genuine one-tick fault.

3. **No independent corroboration.** Every timing finding is now cross-checked
   against the amplitude-ranked NMS edge detector in `ratechange.py`, which never
   uses phase and so fails differently. A displacement only one detector sees is
   discarded as an artifact -- but still counted, since a rising discarded count
   means the tool needs work, not that the device is clean.

Net effect on the same 11-minute capture: **138 "anomalies" -> 1 real event,
101 ISR-tick steps, 0 discarded.**

Also hardened while here: `robust_sigma` and `local_resid` are NaN-aware. Without
that, one unmeasurable cycle would make every threshold NaN and the detector
would silently report zero anomalies for ever -- a detector that never fires
looks exactly like a clean device.

## 5. Known issue not changed: omitting waveType resets the channel to sine

`LEDC_WAVE_RESET`, 21 occurrences. An audio entry that omits the `waveType`
column silently resets the channel to wave 0 (sine) rather than leaving it
unchanged, because `audio->wave_type` defaults to 0 while other fields use
`present` bits to preserve the live value. A `-` in the column means "leave
unchanged", but an ABSENT trailing column does not -- an inconsistency.

Deliberately NOT changed: it affects only test files (`test.ledc`, `test2a/b/c`,
`test_iso`), no shipped therapeutic session, and altering the semantics would
change what every existing file plays. Worth fixing together with a pass over
the trailing-column defaults rather than piecemeal.

## 6. Antiphase luminance-flat pair: VERIFIED on hardware

`17_nondual_awareness_RGB` runs the inner zones (mask 9) at `phase 0` and the
outer zones (mask 6) at `phase 180`, with a sine carrier and 8->22% brightness
breathing -- the hardest case for the measurement chain, and the session the
dropped-flick bug lived in.

Measured during the breathing section (t=60..80 s):

| | ch1 sensor | ch2 sensor |
|---|---|---|
| 10 Hz fundamental | -83.2 dBFS | -57.5 dBFS |
| 20 Hz 2nd harmonic | -61.7 dBFS | -59.8 dBFS |
| fundamental vs own rms | -40.1 dB | -16.3 dB |

ch1/ch2 fundamental ratio **-25.8 dB**, relative phase **+178.1 deg** against a
commanded 180.

That is the antiphase design working, not a fault: ch1's sensor happens to sit
where an inner and an outer zone contribute almost equally, so the 10 Hz
fundamental very nearly cancels and the 2nd harmonic dominates (21.5 dB above the
fundamental on that channel). The pair is luminance-flat to within 1.9 deg, which
at 10 Hz is 0.53 ms.

**Measurement consequence.** On an antiphase session a photocell must see ONE
zone. A sensor viewing both measures the residual of a near-cancellation, and
per-cycle analysis of the 10 Hz cycle becomes impossible -- percycle left 154 of
495 cycles ungraded there and raised its new >5%-slip WARNING, whereas ch2
(dominated by a single zone) measured cleanly at 0.86-0.97 ms timing noise with 3
and then 0 anomalies. Before the percycle hardening this configuration produced
59 confident anomalies with claims up to +59.8 ms; it now declines to grade
instead, which is the behaviour that matters.

## 7. capture.sh lost three captures to its own failure path (FIXED)

A refused capture leaves its recorder running. The recorder lives in a Terminal
window (the `open` indirection exists so Terminal, not the tmux server, is the
TCC-responsible process for microphone access), so after this script exits with
an error the recorder keeps holding the interface for the full requested
duration. A prompt retry collides with it and gets a TRUNCATED file, which the
frame-count check then refuses -- leaving yet another recorder running.

Observed: 190 s requested, 46.5 s then 98 s delivered (75.5% and 48.3% "missing"),
and the loop only broke after closing the stale Terminal windows by hand. Note
the symptom reads as sample loss but is really truncation, and that the earlier
665 s capture succeeding is what made it look intermittent rather than causal.

Fixed by clearing any previous recorder at the START of a capture rather than
after a failure.

## 8. env carrier shapes re-measured; TRAPEZOID was a MEASUREMENT ARTIFACT (RESOLVED)

Regression check on the `env` carriers after today's changes, 10 Hz, duty 50,
bright 100, RGB 255 (effective drive 255, inside the linear region), default
attack. Light ch1, signal gate +34..38 dB.

| env | measured h2/h3/h4 | documented |
|---|---|---|
| 0 SQUARE | 0.004 / 0.345 / 0.003 | 0.004 / 0.346 / 0.003 |
| 1 SINE | 0.084 / 0.020 / 0.052 | 0.058 / 0.004 / 0.010 |
| 2 TRIANGLE | 0.043 / 0.106 / 0.034 | 0.061 / 0.139 / 0.022 |
| 3 TRAPEZOID | 0.547 / 0.330 / 0.518 | 0.081 / 0.310 / 0.073 |

SQUARE reproduces the documented values to the third decimal, which is what
establishes that the measurement chain is sound here. The sine-LUT fix holds:
h3 = 0.020 against the 0.145 the old 4x(1-x) parabola produced.

TRAPEZOID is the open item: h2 = 0.547 and h4 = 0.518, roughly 6x the documented
figures, while h3 (0.330 vs 0.310) agrees. Large EVEN harmonics mean the waveform
is asymmetric, and a 50%-duty trapezoid with symmetric edges should have
essentially none -- reading `led_carrier_level_iram`, the rise occupies
`[0, a)` and the fall `(duty-a, duty]` with `a` clamped to `duty/2`, which is
symmetric by construction. At duty 50 and attack 3 ms the clamp is not active
(a = 1966 of 32768), so that is not the explanation either.

NOT diagnosed, and deliberately not guessed at. Possibilities to separate:
the documented row may have been taken at a different `attack` or duty; the
NeoPixel backend's per-tick refresh may interact with the smooth carrier
differently than with a gate; or the trapezoid really is asymmetric somewhere
between `shape_q16` and the strip. Measure h2 against a sweep of `attack` and of
`duty` before touching any code -- if h2 tracks attack, it is the shape; if it
does not, look at the output path.

## 9. Rig note: ch2 sensor connection is intermittent

The ch2 photocell measured cleanly on a mask-15 session earlier in the day
(percycle timing noise 0.063 ms, 0 anomalies) and then, an hour later on the same
mask and a brighter setting, carried no usable flicker at all: the 10 Hz line sat
+1.1..+3.5 dB over its neighbourhood (below the 6 dB "NO SIGNAL" bar) and the
waveform was visibly dominated by 50 Hz mains at +/-0.004 against ch1's +/-0.024.

Analysing that channel without gating produced completely fictional harmonic
ratios (h2/h1 of 3.59 for SINE, 12.84 for TRAPEZOID -- a second harmonic larger
than the fundamental, which is physically impossible for those shapes). This is
the same trap `signalcheck.py` exists to catch, and the lesson is to run the gate
BEFORE interpreting, not after the numbers look odd. Resolder/reseat ch2 and
re-check before trusting any two-channel result.


## 9a. ch2 after reconnection: a FLAT 38 dB optical/series loss (needs hands)

Reconnecting the ch2 cell did not restore it. `signalcheck.py` still says NO
SIGNAL (10 Hz line -1.7 dB over its neighbourhood, versus +32.5 dB on ch1).

Four hypotheses tested and three eliminated, which is worth recording because the
first one looked obvious and was wrong:

| hypothesis | test | result |
|---|---|---|
| missing 1k load -> cell capacitance into 1 Mohm Hi-Z, RC roll-off | flicker at 1 / 3 / 10 Hz | **REFUTED.** ch2-ch1 = -38.7 / -38.1 / -38.6 dB, dead flat across a decade. An RC corner loses ~20 dB/decade. |
| MiniFuse ch2 preamp gain turned down (it dropped ~39 dB once before) | compare 50 Hz hum | **REFUTED.** ch2 hum -51.3 dBFS vs ch1 -53.0, i.e. ch2 amplifies normally; only the photo-signal is missing. |
| cell now faces zones outside `mask 15` | repeat with `mask 255` (all 8 channels) | **REFUTED.** -38.1 dB -> -40.0 dB, unchanged. |
| cell receives almost no light, or a high-resistance joint divides the signal | — | **REMAINING.** |

A flat, frequency-independent loss with intact hum pickup places the attenuation
between the cell and the cable: either the cell is physically not receiving light
(covered, face-down, moved away) or a cold/dirty joint forms a resistive divider
with the 1k load, which attenuates the photocurrent flatly while mains hum still
couples onto the wiring past the joint.

Discriminating test (seconds, needs hands): shine a bright light directly on the
ch2 cell. If the level jumps, it is aim/shading. If it stays ~38 dB down, it is
the joint.


## 8a. TRAPEZOID resolved: no firmware defect

The h2 = 0.547 / h4 = 0.518 figures in section 8 were wrong, and the way they
were wrong is the useful part.

**The trapezoid shape is correct.** Sweeping `attack` at 10 Hz / duty 50 (so the
duty/2 clamp sits at 25 ms), both light channels agreeing:

| attack | ch1 h2/h1 | ch1 h3/h1 | ch2 h2/h1 | ch2 h3/h1 |
|---|---|---|---|---|
| 3 ms | 0.042 | 0.316 | 0.107 | 0.297 |
| 10 ms | 0.221 | 0.221 | 0.271 | 0.196 |
| 20 ms | 0.360 | 0.061 | 0.420 | 0.056 |

h2 rises and h3 falls monotonically with attack, which is exactly what widening
a trapezoid's edges does. At the default attack the numbers sit near the
documented row.

**Two hypotheses tested and refuted before the real cause was found:**

1. *A stale global `attack`.* `attack` is latched from a module global, so a
   session omitting the column might inherit the previous session's value.
   Tested directly -- ran a session setting attack=20 ms, then one with the
   column omitted: h2 came out 0.042, identical to attack=3, not 0.360. It
   defaults correctly; there is no cross-session contamination.

2. *A real asymmetry in the shape computation.* Refuted by re-running the ORIGINAL
   session unchanged after the ch2 wiring was repaired:

       earlier    h1 -34.2 dBFS   h2/h1 0.547   h3/h1 0.330   h4/h1 0.518
       now        h1 -26.8 dBFS   h2/h1 0.044   h3/h1 0.317   h4/h1 0.048

Same .ledc, same analysis, same channel. The fundamental is **7.4 dB stronger**
now, and the even harmonics collapsed by more than a decade. The earlier capture
was taken while the ch2 sensor wiring was faulty, and repairing it changed ch1's
level too -- a shared-wiring/positioning disturbance.

**The generalisable lesson:** a degraded signal path inflates EVEN harmonics
specifically, because whatever attenuates or distorts the photo-signal is not
symmetric within the cycle. So h2/h1 is not safe to compare between runs unless
the FUNDAMENTAL's absolute level is comparable too. Always print h1 in dBFS
beside the ratios; a 7 dB drop in h1 invalidates the ratios built on it. Passing
`signalcheck.py` is necessary but NOT sufficient -- ch1 cleared the +6 dB gate in
both captures and still gave a wrong answer in one of them.
