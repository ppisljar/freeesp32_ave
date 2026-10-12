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
* no per-cycle glitches: max inter-edge deviation 1.16 ms, against 0.52 ms at a
  control point.

## 4. percycle.py cries wolf on long windows (TOOL BUG, OPEN)

Worth recording because it nearly produced a false bug report. Run over 55 s
windows, `percycle.py` flagged 138 "anomalies" across 6000 cycles (2.3%),
including an apparent **-36.8 ms** single-cycle glitch at t=433 s with a smooth
+/-6 ms ramp around it.

It is not real. Dumping the raw waveform at that instant showed clean 100 ms
cycles, and the independent NMS edge detector in `ratechange.py` measured a
maximum deviation of 1.16 ms there versus 0.52 ms at a control point. The
signature -- one huge value flanked by a smooth ramp of residuals -- is the
sliding-quadratic reference reacting to a segmentation slip, not a device fault.

The cause is `percycle.py`'s phase segmentation: it demodulates against the
nominal rate over the whole window, so on a long window the accumulated phase
can slip past a segment boundary and one segment captures two edges or none.
Until it is fixed, keep percycle windows short (<= 20 s, where it measured 0
anomalies repeatedly) and corroborate any single-cycle finding against
`ratechange.py`'s edge intervals before believing it.

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
