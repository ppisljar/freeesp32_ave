# Audio feature verification, 2026-10-12

Measured acoustically through the device headphone jack into an Arturia MiniFuse 4
(TRS splitter, ch3/ch4). Short purpose-built sessions rather than the 30-60 minute
therapeutic ones.

## All 8 carrier wave types: CORRECT

200 Hz carrier, 10 s per wave type, harmonics relative to the fundamental:

| wave | h2 | h3 | h4 | h5 | theory (h2/h3/h4/h5) |
|---|---|---|---|---|---|
| 0 SINE | 0.000 | 0.000 | 0.000 | 0.000 | 0 / 0 / 0 / 0 |
| 1 SQUARE | 0.004 | 0.334 | 0.003 | 0.201 | 0 / .333 / 0 / .200 |
| 2 TRIANGLE | 0.000 | 0.111 | 0.001 | 0.040 | 0 / .111 / 0 / .040 |
| 3 SAWTOOTH | 0.501 | 0.334 | 0.250 | 0.200 | .500 / .333 / .250 / .200 |

Every one matches theory to within 0.001-0.004. Noise generators, spectral slope
fitted over log-spaced bins:

| wave | measured | theory |
|---|---|---|
| 4 WHITE | +0.05 dB/oct | 0 |
| 5 PINK | -3.04 dB/oct | -3 |
| 6 BROWN | -6.07 / -5.69 dB/oct (300 Hz-3 kHz) | -6 |

Wave 7 (EEG contour) is a non-geometric single-cycle LUT with no closed-form
spectrum; measured h1..h6 = 1.000 / 0.500 / 0.331 / 0.221 / 0.140 / 0.091, i.e.
close to a 1/n sawtooth-like series, which is at least self-consistent and
harmonically rich as intended.

**A measurement trap, recorded because I fell into it twice.** A naive wideband
fit of BROWN over 300 Hz-9 kHz gives -4.81 dB/oct and looks like a defect. It is
not: the slope is -6 over 60 Hz-3 kHz and only flattens above ~3 kHz. I first
blamed the capture noise floor (wrong -- there is +19.8 dB of headroom there) and
then clipping (wrong -- the device-side int16 peak is -31 dBFS, nowhere near full
scale). The actual reason is the subject of the next section: everything is 24 dB
down, so brown noise's HF content lands only a few int16 counts above the LSB and
the flat quantisation floor drags the measured slope up. Fit noise slopes only
where the signal is well clear of BOTH the analog floor and the quantisation
floor, and state the band.

## The fixed 1/16 headroom divisor costs 24 dB that no session can recover

`audio_generator.c` (fill_buffer) sets

    float new_target = 1.0f / (float)NUM_AUDIO_CHANNELS;    // 1/16 = -24.1 dB

unconditionally, regardless of how many channels are actually playing. The
rationale in the comment is sound as far as it goes -- it is a hard overflow
guarantee that cannot click, chosen over `min(1, 1/Σamp)` because that bounded
nominal amplitudes while letting instantaneous peaks distort.

The problem is the comment's suggested remedy: "raise per-channel volumes (or the
master-volume cap) to compensate". Both are already at maximum in the measurement
below, so there is nothing left to raise.

Measured device-side int16 peak before output (32767 = full scale),
`audio_max_volume` = 100:

| wave | vol 70 | vol 100 |
|---|---|---|
| SINE | 1013 (-30.2 dBFS) | 1448 (-27.1 dBFS) |
| WHITE | 506 (-36.2 dBFS) | 723 (-33.1 dBFS) |
| PINK | 149 (-46.8 dBFS) | 224 (-43.3 dBFS) |
| BROWN | 540 (-35.7 dBFS) | 900 (-31.2 dBFS) |

A sine at maximum volume on both controls reaches -27 dBFS. About 4 of the 16
bits are unused, which is why brown noise's HF tail sits near the quantisation
floor.

How much is wasted in practice, across the 80 shipped sessions (counting the
maximum number of SIMULTANEOUSLY active audio channels per session):

| simultaneous channels | sessions | headroom wasted |
|---|---|---|
| 1 | 61 | 24.1 dB |
| 2 | 14 | 18.1 dB |
| 4 | 4 | 12.0 dB |

**No shipped session uses more than 4 of the 16 channels.** The divisor is sized
for a case that never occurs, and every session loses at least 12 dB.

### Proposed fix, NOT applied -- this needs a human decision

`new_target = 1.0f / (float)max(1, n_active_channels)` would recover up to 24 dB
and keeps the same worst-case guarantee: each generator is normalised to +/-1.0
peak, so N channels each bounded by their amplitude cannot exceed N, and dividing
by N bounds the sum at 1.0 exactly as the 1/16 version does when all 16 play.
The machinery already exists -- `s_mix_gain_target` / `s_mix_gain_step` /
`s_mix_gain_ramp_left` already slew the mix gain over AUDIO_AMP_RAMP_SAMPLES
"whenever the target gain changes", so a changing divisor is click-safe by
construction. Only the constant is hardcoded.

It is deliberately NOT applied because it would make every existing session up to
**24 dB louder**. This is a head-worn therapy device driving headphones: a user
whose amplifier is set for today's quiet output would get a sudden, possibly
painful jump. Sensible options, in increasing caution:

1. Expose the divisor policy as a setting defaulting to the CURRENT behaviour, so
   nothing changes until it is opted into.
2. Use `1/max(4, n_active)` -- recovers 12 dB, still leaves 12 dB of margin, and
   covers every shipped session without ever exceeding today's loudness by more
   than 12 dB.
3. Apply `1/n_active` and lower `audio_max_volume`'s default to compensate, so
   nominal loudness is unchanged while the dynamic range improves.

Any of these should be verified by re-measuring the int16 peak at vol 100 before
and after, and by confirming no `short_writes` / clipping in `/api/audiostats`.
