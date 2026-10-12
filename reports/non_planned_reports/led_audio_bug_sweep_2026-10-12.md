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
* ZERO device-side timing faults in 6523 cycles. **Corrected below** -- the
  "-6.24 ms event" originally reported here was the RECORDING, not the device.
  See section 3a.
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

## 10. Measured LED-channel-to-eye map (both sensors healthy)

With ch2 repaired, each LED channel was driven at its own rate in one session
(bit0..bit5 -> 5/7/9/11/13/15 Hz) so each sensor's spectrum names the channels it
can see. Separation 30-43 dB, so this is unambiguous:

| LED ch | ch1 sensor | ch2 sensor | belongs to |
|---|---|---|---|
| LED1 (5 Hz) | -68.2 | **-32.7** | ch2's eye |
| LED2 (7 Hz) | -72.2 | **-34.1** | ch2's eye |
| LED3 (9 Hz) | **-33.5** | -63.5 | ch1's eye |
| LED4 (11 Hz) | **-32.2** | -75.2 | ch1's eye |
| LED5 (13 Hz) | -78.2 | -73.5 | neither (noise floor) |
| LED6 (15 Hz) | -65.3 | -41.5 | ch2's eye, ~8 dB down |

**The eye split is {LED1, LED2} vs {LED3, LED4}.** So the per-eye masks are
**3** and **12** -- which the shipped library uses only 9 times each, versus
masks 9 and 6 at 216 uses apiece.

That also explains the earlier antiphase cancellation. Masks 9 (LED 1,4) and 6
(LED 2,3) each take ONE LED from EACH eye, so a session setting them antiphase
gives every eye one phase-0 and one phase-180 source, which cancel within that
eye -- the 25.8 dB fundamental cancellation and 178.1 deg measured on
`17_nondual_awareness_RGB`. This is consistent with the intended "inner ring of
both eyes vs outer ring of both eyes" provided the per-eye channel order is
mirrored (eye B = inner,outer = 1,2 while eye A = outer,inner = 3,4), which a
mirror-image daisy chain would produce. A single sensor per eye cannot separate
inner from outer within that eye, so this measurement confirms the EYE split only
-- the ring assignment still rests on the wiring.

Two loose ends for whoever knows the harness:

* **LED5 produces no light either sensor can see** (-78.2 / -73.5 dBFS, i.e. the
  noise floor). The library never uses a mask above 15, so a 4-zone build is
  consistent and this is probably unpopulated rather than faulty -- worth
  confirming rather than assuming.
* **LED6 IS visible** to ch2's eye at -41.5 dBFS (~8 dB below LED1/LED2), despite
  never being addressed by any shipped session. If channels map onto ranges of
  one physical NeoPixel chain, channel 6 may alias onto pixels in that eye.

Practical note for writing sessions: to drive one eye independently use mask 3 or
mask 12. To check a measurement rig, drive each channel at a distinct rate and
read the spectrum -- it identifies the map in one capture and needs no knowledge
of the wiring.


## 3a. CORRECTION: the "-6.24 ms single-cycle fault" was the recording

It is not a firmware defect. I reported it as one, twice, and it survived a day
of scrutiny because two independent optical detectors agreed on it. They agreed
because the whole recording stepped, not because the LED did.

**What settles it, in order of strength:**

1. **Sign structure.** With `cycle_start_time_us += ended_cycle_us` the origin
   stays on an absolute grid, so an ISR served late yields one LONG cycle and
   then a compensating SHORT one -- the grid is restored. The measured event is
   ONE-SIDED: the lattice steps and stays there for the rest of the capture. Only
   a WRITE to the origin can do that, and no such write was reachable (the
   session had a single flicker entry at t=2000 ms, so V-S4's
   `pending_anchor_us` was 0 from t~2.05 s onward; the stall path needs
   `elapsed >= 2*period` = 200 ms and the longest interval anywhere near the
   event is 100.4 ms). So EVERY ISR-delay story was structurally impossible
   before any other evidence was considered -- including the one this report
   originally offered.
2. **The device's audio carrier steps at the same instant.** That 400 Hz sine is
   clocked by I2S and the codec; nothing in the LED path, in `esp_timer`, or in
   the optical chain can move it. Demodulated in 5 ms blocks (block-to-block
   phase noise ~1 us), it steps +1079.1 us at the event -- independently
   reproduced.
3. **Arithmetic.** The only integer frame count consistent with BOTH the carrier
   step and the ~6.1 ms light step is **268 frames = 6.0771 ms**, matching the
   carrier step to 0.3 us (267 and 269 are 22 us off). Both solar cells measure
   the light step as -6.122 and -6.138 ms, agreeing to 16 us.

The same applies to the t=433 s event that the percycle pi-slip hardening
suppressed: that was a genuine ~-52 ms CAPTURE break (the two eyes disagreed by
1.6 ms on edges that normally agree to 0.33 ms, and an unused ADC input burst to
126 LSB against a baseline of 1). The hardening produced the right outcome for
the wrong reason.

Total: **five timebase discontinuities in 665 s** (~1 per 2.2 min), and **zero
device-side timing faults in 6523 cycles**.

**Why this went undetected:** `capture.sh` asserted the frame COUNT, and the
file was exactly the right length -- a dropout removes time rather than padding
it. `tools/avmeasure/captureintegrity.py` now closes that hole by using the
device's own audio carrier as a clock, and `capture.sh` refuses a capture whose
timebase stepped by more than 10 frames (set `AVM_CARRIER_HZ`). It needs a
continuous SINE reference tone: a session cycling square/sawtooth/noise
"carriers" gave 369 us of phase noise against ~1 us for a sine, which inflated
the self-calibrating threshold into a rubber stamp -- the tool now returns "could
not verify" rather than a worthless pass.

**Which earlier conclusions survive.** Differential measurements are immune,
because all four input channels step TOGETHER: the audio-vs-LED offset, the
inter-eye offset, and every harmonic ratio are unaffected, which is also why the
A/V offset read -2.54 / -2.44 / -2.48 / -2.47 ms across the same capture. Absolute
per-cycle timing is what was corrupted, and only at the five instants above --
the median period (99.999 ms, 10.00007 Hz) is robust because a dropout spoils one
period, not the median.

**The 101 one-tick steps ARE device-real and benign.** Every excursion is a
+/- pair summing to ~0 (+0.455/-0.446, +0.408/-0.402, ...): a boundary served one
tick early and then restored, because the `cycle_start + period` threshold sits
just above a tick instant and ISR-entry latency occasionally crosses it. They
cluster at times ending .899/.999, i.e. a ~1 s cadence, which points at a
periodic timer (lwIP/WiFi/stats) rather than RMT or flash.


## 11. A rate change on an active channel kept the OLD rate's ISR tick (FIXED, V-S5)

The flicker ISR tick is `clamp(freq * 250, 1000, 10000)` Hz, raised by
`s_ensure_timer_and_task`. Three LED entry points exist;
`start_flicker_masked` and `start_sweep_masked` both called it and
**`led_matrix_update_flicker_params_masked` never did** -- which is the path the
timeline uses for an entry applied to an already-flickering channel
(config_parser routes a no-swept-fields entry through `orphan_mask`).

So a session that began at a low rate kept that rate's coarse tick for every
later rate, however high. Real sessions do exactly this.

Measured, rebooting between runs so the tick genuinely starts at the 1 kHz floor:

| | tick | measured rate | error | jitter sd |
|---|---|---|---|---|
| 2 Hz then 23 Hz (update path), before | stuck 1 kHz | 23.1681 Hz | **+0.731%** | 0.517 ms |
| same, after V-S5 | raised | 23.0173 Hz | +0.075% | 0.142 ms |
| 23 Hz from a fresh start (control) | 5750 Hz | 23.0024 Hz | +0.010% | 0.115 ms |

+0.731% is roughly 18x the project's +/-0.01 Hz beat-accuracy target.

**Two things made this easy to miss, both worth remembering.**

First, *the choice of test rate hides it completely.* An earlier attempt used
40 Hz and found nothing, because 40 Hz is 25.000 ms = exactly 25 ticks of a 1 kHz
tick. Any rate that divides the tick period evenly is immune. 23 Hz was chosen
because 43.478 ms is deliberately NOT a whole number of ticks.

Second, *the tick is a global that survives a session.* `s_flicker_tick_hz` is
only reset to the floor when the timer is deleted (no channel left running), so a
previous session that ran at a high rate leaves the tick high and the next
session silently inherits good timing. The first version of this test measured
B after A in the same boot and so measured A's 10 kHz tick -- which is why the
runs above reboot the device between them. Any timing test of this subsystem must
control for boot state.

Fixed by calling `s_ensure_timer_and_task(new_freq_milliHz)` on that path too.
Safe unconditionally: it only ever RAISES the tick within a session, so no other
channel's timing can be coarsened.

## 12. Repeating a modulation restarted it (FIXED)

Reported from listening: in `17_nondual_awareness_RGB` the LEDs breathe on a 12 s
cycle, and around the second spoken line they sat at FULL brightness and then
dropped hard to the floor.

The speech was a red herring. There is an LED entry at the same timestamp:

    300000 >10 50 ~16:44:12000 ...      # inner: 10->7 lockstep
    300000 >10 50 ~16:44:12000 ...      # outer
    S 300000 ... "Let awareness open and rest wherever it is..."

`s_apply_mod()` set `start_time_us = now` unconditionally, and that field is the
LFO's phase origin, so re-arming snapped the value back to `start_q` = 16%, the
floor. The format's way to carry a modulation across an entry that changes
something else is to repeat the same `~a:b:p` cell, so this fired on every such
entry (also t=1560000 and t=1740000, matching "and the one after that as well").

**Why it was at maximum first**, which is the detail that confirms the mechanism:
the mod is armed at t=30 s with a 12 s period, so by t=300 s it has run 270 s =
exactly **22.5 cycles** = phase 0.5 = the peak. The reported "at max, then sharp
drop to lowest" is precisely peak -> start_q.

### The first attempt was a no-op, and only measuring caught it

The fix compared the new spec against the slot's and kept the phase origin when
they matched -- but gated on the slot being `.active`. The timeline dispatcher
STOPS every LED mod and then re-arms the ones the entry carries
(`config_parser.c`: six `mod_engine_stop_led` calls, then `mod_engine_start_led`),
so the slot is always inactive by the time `s_apply_mod` runs and the guard never
fired. Re-measuring showed the identical 2.00 s interval as before the change.

What makes the comparison possible across that stop is that
`led_matrix_clear_mod_masked` only clears the flag and RETAINS the spec and the
phase origin. The test is now spec-equality plus `period_us != 0`, the latter so a
zero-initialised slot on a fresh channel cannot "match" an all-zero arm and
inherit a phase origin of 0.

The audio generator had the same bug and the same trap in
`audio_generator_set_mod_locked`. It is live content:
`36_shamanic_trance_drum` repeats `~-100:100:8000` on ch4's pan at t=60000, so its
8 s rotating pan snapped back to hard-left there.

### Verification

Controlled test, 3.00 s breathing with the same modulation repeated 8 s in
(phase 0.667, so a restart is unmistakable), both light channels agreeing:

| | peak intervals | worst deviation |
|---|---|---|
| before | 3.00 3.00 **2.00** 3.00 3.00 | 1.000 s |
| after | 3.00 3.00 3.00 3.00 3.00 | **0.000 s** |

Then on the real session, reading the device's own live brightness at 0.5 s
intervals (24 samples per 12 s cycle) across the reported moment:

    298.8  34      300.7  43      302.7  38
    299.3  38      301.4  43      303.4  34
    300.0  42  <-- t=300 row      304.1  30

It sails through the peak. A restart would read 16. Troughs at 295.2 / 307.0 /
319.3 give 11.8 and 12.3 s spacing, continuous across the entry, and the peak
lands at t≈300 exactly as 22.5 cycles predicts -- which independently confirms
the phase origin is still t=30 s. `freq` simultaneously shows the 10->7 ramp
engaging (10.00 -> 9.97 -> 9.95), so rate ramp and modulation now coexist.

**Measurement note.** The optical envelope estimator could NOT resolve this on the
real session: it breathes 16..44% on RGB(60,30,200), i.e. effective drive 12..34,
deep in the non-linear part of the brightness curve, and the rate ramps after
t=300 so a fixed-window peak-to-peak also tracks cycle count. The two light
channels disagreed by 1.5 s vs 3.5 s -- the estimator failing, not the device.
Reading the modulated value from `/api/state` is the right instrument for a slow
LFO: it is the exact quantity, noise-free, and needs no rig.
