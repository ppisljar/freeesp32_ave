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
