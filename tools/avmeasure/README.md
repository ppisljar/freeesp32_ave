# avmeasure — did the device actually play what the `.ledc` prescribed?

A measurement instrument, not a test suite. You give it a session `.ledc` and an
observation of the device playing it; it gives you a timestamped list of
discrepancies you can act on:

```
[ERROR  ]  20:00.000 EVENT_LATE ch1
                     LED ch1: the entry at t=1200000 ms fired 41 ms LATE (flicker rate step).
[ERROR  ]   5:12.400 FLICKER_RATE ch2
                     LED channel 2 flickered at 8.121 Hz where 7.818 Hz was expected (+3.88%)
                     from t=312.4 s to t=420.0 s.
[ERROR  ]            AV_CONST_OFFSET
                     audio lags light by a CONSTANT 138.2 ms = +132.0 ms measured residual
                     + 6.2 ms modelled pipeline, outside the expected 0..40 ms band.
```

It exists because "the device sometimes doesn't play what the `.ledc` says" is
too vague to chase by guessing at fixes. So: express what the file *demands*,
express what was *observed*, and diff them with timestamps.

**Dependencies: `python3` + `numpy`. Nothing else, deliberately** — it has to run
on a clean machine at a bench, possibly offline. WAVs are read with the stdlib
`wave` module; all DSP is numpy (FFT, Goertzel, convolution). No scipy, no
soundfile, no matplotlib.

---

## 1. Quick start

```bash
cd tools/avmeasure

# 1. Prove the engine works, with no hardware at all: render a synthetic
#    recording from a session, then analyse it. Should report nothing.
python3 analyze.py synth   --ledc tests/selftest.ledc --out /tmp/clean.wav
python3 analyze.py analyze --ledc tests/selftest.ledc --wav /tmp/clean.wav \
        --map "audioL=1,audioR=2,light1=3,light2=4" --sync-tone 3000

# 2. Inject a known fault and watch it get caught, with the right magnitude.
python3 analyze.py synth   --ledc tests/selftest.ledc --out /tmp/bad.wav \
        --fault offset:120 --fault late:30000:60
python3 analyze.py analyze --ledc tests/selftest.ledc --wav /tmp/bad.wav \
        --map "audioL=1,audioR=2,light1=3,light2=4"

# 3. A real recording.
python3 analyze.py analyze --ledc ../../sessions/library/07_genus_40hz.ledc \
        --wav rig_recording.wav \
        --map "audioL=1,audioR=2,light1=3,light2=4,light3=5,light4=6" \
        --sync-tone 3000 --json report.json
```

### The four subcommands

| command | what it does | needs hardware? |
|---|---|---|
| `analyze` | diff a recording against a `.ledc` — the main job | a recording |
| `synth` | render a synthetic recording *from* a `.ledc`, with optional injected faults | no |
| `expect` | print the demanded model, for human review | no |
| `lint` | static `.ledc` problems (dropped entries, firmware traps) | no |

`lint` is worth running on its own before you ever record. It finds the
`.ledc`-authoring traps that *look* like device bugs — a 9th entry at one
timestamp past the batch cap, a `>` ramp on audio channel 8 that silently cannot
ramp, a final entry whose light is cancelled by the end-of-timeline handler:

```bash
python3 analyze.py lint --ledc ../../sessions/library/01_sleep_onset.ledc
```

### Exit codes

| code | meaning |
|---|---|
| `0` | nothing to report |
| `1` | at least one **error** — or, with `--strict`, any finding at all |
| `2` | **operator error**: a path that does not exist, a bad `--map`, bad arguments |
| `3` | sync failed, so nothing below `t0` could be judged |

`2` is deliberately separate from `1`: a mistyped filename is not a device
fault, and a script must be able to tell them apart. (It used to print a
traceback and exit `1`, the same code as "the device is wrong".)

`EVENT_LATE`, `EVENT_EARLY` and `AV_CONST_OFFSET` are **errors**, because they
are the faults the tool primarily exists to find — the quick-start fault demo
above exits `1`. They are still demoted to warnings automatically when `t0`
itself is too weak to support them, with the reason printed; `--strict` then
escalates those too.

---

## 2. The channel map (`--map`)

The tool cannot guess which WAV channel is which sensor, and guessing wrong
makes every number meaningless, so `--map` is required.

**Explicit form** (recommended — order-independent, self-documenting):

```
--map "audioL=1,audioR=2,light1=3,light2=4,light3=5,light4=6"
```

Channel numbers are **1-based**, matching how interfaces and DAWs label inputs.

**Positional form** — token *i* names the role of WAV channel *i*:

```
--map "AL,AR,L1,L4"
```

### Roles

| role | aliases | meaning |
|---|---|---|
| `audioL` | `AL`, `L`, `left`, `audio` | device audio, left ear |
| `audioR` | `AR`, `R`, `right` | device audio, right ear |
| `light1`..`light8` | `L1`..`L8`, `photo3`, `pd3` | photosensor watching LED channel *N* |
| *(skip)* | `-`, `x`, `skip`, `unused`, `none` | channel exists but carries nothing |

`lightN` is the **`.ledc` LED channel number, 1–8** — i.e. mask bit *N−1*.
Mask `15` drives channels 1,2,3,4; mask `9` drives 1 and 4. If you map `light1`
to a sensor that is physically watching zone 4, the tool will report channel 1
as dead and channel 4 as unexpected, which is exactly the right complaint — but
about your map, not the firmware.

### Inverting a channel

Prefix the index with `!` for a front-end that pulls **down** on light (a
transimpedance amp into a virtual ground, or a pull-up + phototransistor to
GND):

```
--map "audioL=1,audioR=2,light1=!3"
```

### Degrading gracefully

- **Audio only** (no light sensors): tone and amplitude are measured; the
  audio-vs-light offset is reported as *not measurable*, and t0 confidence drops
  to `low` because the audio onset carries the full dispatch jitter. The report
  says so explicitly.
- **One audio channel** instead of two: tone and amplitude still work; the
  binaural beat is reported `BEAT_UNAVAILABLE` rather than guessed.
- **Light only**: flicker rate/duty/brightness are graded; everything audio is
  skipped.

---

## 3. Sync: the recording's `t=0`

**This is the part to actually read.** The recording starts whenever you pressed
record; the session's `T0` is whenever the POST handler captured
`esp_timer_get_time()` (`main/config_parser.c:655`). Nothing in the audio or the
WAV header relates the two. **A wrong t0 makes every timing number downstream
meaningless**, so the report always prints t0 first, with its method,
confidence and uncertainty, and never hides it.

Three methods, in descending precision:

1. **`--t0 SECONDS`** — you know it (hardware trigger, clapper, logged timestamp).
2. **`marker`** — a sync marker authored into the `.ledc` (below). Recommended.
3. **`xcorr`** — cross-correlate the whole session's activity envelope, then
   refine against the first strong event and the flicker phase.

### Why you should author a marker

The fallback works well on a session that *starts with a step*. It works badly —
and this is the normal case — on a session that fades brightness in over 10–30 s
and then holds one rate, because the activity envelope is then a near-flat
plateau. Two flat plateaus cross-correlate at 0.9997 at *any* small lag: the
correlation peak is high and completely uninformative.

The tool measures how well the correlation actually localises the lag and
reports that as the uncertainty, so this case comes out as
`confidence LOW (+/-4750 ms)` with an explanation, rather than a confident lie.
But ±4.75 s is useless for finding a 41 ms late event. **On a real 60-minute
session, with no marker, t0 is simply not determinable to better than seconds.**

And the consequence is not merely imprecision. The A/V offset is the
*difference* of two origins, and the audio origin is searched around the light
one; on a session whose level only fades in, the onset detector returns a point
whose distance from the window centre is fixed, so the measurement stops
tracking the truth altogether. Measured on `tests/fadein.ledc`: injecting a
300 ms audio delay moved the reported offset by **0.0004 ms**, and −1000 ms read
as a healthy +11 ms. So the offset is now **withheld** (`AV_NOT_MEASURABLE`)
whenever either origin is not a measured step or t0 is coarser than the
0…40 ms band — rather than printed as a number that carries no information.

The two origins are also reported separately and labelled
`t0 from light` / `t0 from audio`, with `NOT MEASURED` spelled out when one of
them is unavailable, and the headline offset prints its own arithmetic
(`measured residual + modelled pipeline`) so it reconciles with them.

### Sync-marker recipe

Prepend a short, bright, loud burst and shift the rest of the session. The
marker is 600 ms of full-brightness flicker plus a steady tone on an otherwise
unused audio channel:

```
# --- sync marker: 600 ms of full-brightness light + a 3 kHz tone ---
0     40 50 100 255 255 255 15     # all four zones, 100% bright, sharp edges
A 0   3000 0 70 0 15               # 3 kHz tone, vol 70, audio channel 15
600   0 50 0 0 0 0 15              # freq 0 => stop flicker
A 600 3000 0 0 0 15                # volume 0
                                   # ... then your session, every timestamp +1000 ms
```

Then analyse with `--sync-tone 3000`, which tells the tool to demodulate that
exact frequency. Measured effect on a 60-minute session that otherwise cannot be
synced at all:

| | without marker | with marker |
|---|---|---|
| t0 error | **2000 ms** | **0.0 ms** |
| confidence | `low (±4750 ms)` | `high (±5 ms)` |
| findings | 12, all phantom | 0 |

Four rules, each of which matters:

1. **Shift every original timestamp** by the marker length + a gap (1000 ms
   works). `.ledc` timestamps are absolute, so the marker cannot overlap the
   session's own `t=0` entries. Keep this as a *measurement copy* of the
   session — do not ship it.
2. **Set the marker's flicker rate equal to the session's highest flicker
   rate** (40 Hz above). The flicker ISR tick is
   `clamp(floor(f_Hz)*250, 1000, 10000)` Hz and rises -- never falls -- *while
   at least one channel is flickering* (`main/led_matrix_example.c:1251`); it
   RESETS to the 1000 Hz floor once every channel has stopped, because
   `s_maybe_teardown_timer_and_task` deletes the timer (:1271-1285) and the next
   activation re-initialises `s_flicker_tick_hz` (:1219). So a 50 Hz marker
   followed by a real dark gap (rule 3) does *not* pin the tick for the rest of
   the run -- but a marker that overlaps the session's own first entries does.
   The tool models both, so this rule is about measuring the session you ship
   rather than about the tool.
3. **Full brightness, and a real dark gap afterwards.** The sharp edge out of
   darkness is the whole signal.
4. **Put the tone on an audio channel the session does not use** (15 above) so
   it cannot disturb the session's own generator slots.

`tests/selftest.ledc` is a working, commented example.

---

## 4. The hardware rig

### You cannot use this Mac's built-in microphone. Two independent reasons.

1. **It is one channel.** Measuring audio-vs-light offset requires audio *and*
   light simultaneously, in the same file, on the same clock. That is ≥3 inputs
   (L, R, one photosensor), and realistically 6 for four LED zones.
2. **It is a microphone, so it cannot see the flicker at all.** Flicker is
   1–40 Hz. A mic capsule plus its preamp is high-passed somewhere around
   50–100 Hz and is acoustically useless below that. A 10 Hz optical envelope
   arrives attenuated into nothing. There is no amount of post-processing that
   recovers it.

**You need a USB audio interface with ≥3 simultaneous line inputs.** Known-good
shapes: Behringer UMC404HD (4 in), MOTU M4 (4 in), Focusrite Scarlett 18i8,
Zoom H6 (4-track recorder, records to SD with no computer). Set inputs to
**LINE** (not MIC/INSTRUMENT), **disable any high-pass / low-cut switch**, and
disable "voice isolation"-style OS processing.

### Photosensor — parts list (one per LED zone)

| # | part | example MPN | note |
|---|---|---|---|
| 1 | phototransistor, visible-light | Vishay **TEPT5600** | 5 mm, peak ~570 nm, rise/fall ~15 µs — fast enough for a 40 Hz flicker and its kHz PWM carrier |
| 2 | load resistor | 10 kΩ ¼ W | trim 4.7 k–100 k to suit ambient brightness |
| 3 | series/output resistor | 1 kΩ ¼ W | pads into a line input, protects the device |
| 4 | coupling capacitor | 1 µF film | only if your input is DC-coupled; most are not |
| 5 | battery holder + cell | 2×AA (3 V) or 3×AA (4.5 V) | **battery, not USB** — see ground loops below |
| 6 | shielded cable + plug | TS ¼" or TRS 3.5 mm to suit the interface | one per sensor |
| 7 | opaque tube / heatshrink | ~10 mm long, black | collimator, see crosstalk below |

Alternatives that work: Everlight PT331C, Osram SFH 300. A plain **photodiode**
(BPW34) also works but needs a transimpedance op-amp — more parts, better
linearity. The phototransistor is the right trade here because we care about
*edge timing and relative level*, not absolute photometry.

### Wiring (per sensor)

```
        +3 V (battery +)
           |
           C        phototransistor TEPT5600
          [|]       (collector to +V, emitter to R)
           E
           |
           +------[ 1 kΩ ]------> tip  (interface line input N)
           |
         [ 10 kΩ ]  load
           |
          GND ------------------> sleeve (input ground)
```

- The phototransistor acts as a current source proportional to illumination; the
  10 kΩ load converts it to a voltage, and the emitter voltage follows the light.
- Output swing should be a few hundred mV to ~1 V peak-to-peak with the LED at
  full brightness at your working distance. If it clips, raise the series
  resistor or back the sensor off; if it is in the noise, raise the load resistor.
- **Share one battery ground across all sensors and tie it to the interface
  input ground.** Do not power the sensors from the same USB bus as the
  interface or the ESP32: a ground loop injects the LED strip's switching
  current into your measurement, and it looks exactly like flicker.
- **Collimate each sensor.** Slide black heatshrink or a short opaque tube over
  the phototransistor so it sees one LED zone only. Without this, zone 2's light
  reaches zone 1's sensor and "channel 2 never turned on" becomes unreliable —
  the tool's `CHANNEL_NEVER_ON` check depends on per-zone optical isolation.
- Aim each sensor at its zone from a few cm, mechanically fixed. A sensor that
  moves during a 60-minute run produces a slow level drift the tool will
  faithfully report as a brightness discrepancy.

### Recording

Record **one WAV file**, all channels, 44.1 or 48 kHz, 16- or 24-bit (float
WAVs are fine). Channels must be in one file so they share a clock — separate
files from separate devices defeats the entire measurement. Start recording
*before* you POST the session and stop *after* it ends; the tool reports
`RECORDING_INCOMPLETE` for events outside the captured span rather than calling
them missing.

**Stay under 4 GB.** That is a hard limit of the RIFF container, not of this
tool: 60 min / 6 ch / 24-bit / 48 kHz is 3.1 GB and 90 min is 4.7 GB. Past it
interfaces either write **RF64** or **Wave64** — which this tool detects and
names, but cannot read — or silently split the take. Record 16-bit (which halves
the size and costs nothing here, since every amplitude is checked as a ratio),
or split the session.

---

## 5. What it reports

The report has three fixed sections: **SYNC** (t0, method, confidence),
**GLOBAL OFFSET AND CLOCK**, then **FINDINGS**.

### The two global quantities, kept separate on purpose

- **Constant audio-vs-light offset** — estimated once, reported as *one*
  finding. A constant offset is a different bug from a drift, and reporting it
  as 400 individual late events would bury the actual signal.
- **Clock drift** — split into the part **common** to audio and light (that is
  your recorder's crystal, typically ±50–100 ppm ≈ 120 ms over 20 min; it is
  fitted out and reported for information) and the **differential** part (audio
  vs light), which is the only one that can be a device bug. Audio and LED both
  derive from the same 40 MHz crystal (`sdkconfig:1164`), so the device cannot
  drift internally by more than a few ppm.

`expected` is **not** `demanded`. The `.ledc` demands 7.83 Hz; the firmware
emits 7.8184 Hz, because its flicker cycle re-anchors to the ISR tick instant
every cycle instead of to `anchor + k*period`
(`main/led_matrix_example.c:891`). That quantization is one-sided — the emitted
rate is always ≤ demanded, never above — and the tool compares against the
quantized value while printing both. A comparator that flagged the difference
would cry wolf on every low-rate session ever recorded.

### Main finding codes

| code | severity | meaning |
|---|---|---|
| `SYNC_FAILED` / `SYNC_LOW_CONFIDENCE` | error / warn | t0 unusable or weak — read this before anything else |
| `EVENT_LATE` / `EVENT_EARLY` | error | an entry fired outside its timing budget. The budget always contains the estimator's **own** stated half-width, and for an audio level trajectory that half-width is the *larger* of two numbers the finding prints: the SSE valley (the precision the fit would have if its residual were white) and a leave-one-sixth-out jackknife (the precision it actually reproduces). The jackknife is what separates a real shift from one the fit invented to absorb an isochronic AM or a noise bed's own wander — on three bit-perfect renders it covered apparent errors of −237 ms, −430 ms and +1639 ms, while a 300 ms injected delay reproduced to ±36 ms |
| `EVENT_MISSING` | error | a cold activation should have produced something observable and did not |
| `EVENT_NOT_LOCATED` | error | an *update* produced no matching change anywhere in the search window — either it never took effect or it is further out than the window. Still an error when the search had to stop short of the lateness budget: **present but unquantifiable** is still *present* |
| `EVENT_COMMON_OFFSET` | error | a quarter or more of the located events are out by the *same* amount, by more than 3× the stated t0 uncertainty. Either t0 is wrong by that much (an entry near the top of the session — the marker? — never executed) or the timeline really ran that late. Deliberately **not** demoted by a low sync confidence: it is the evidence that the confidence is understated |
| `EVENT_NOT_OBSERVABLE` | info | this entry's **timing** is not graded, and the reason is given: it demands ~0% brightness / ~0 volume, or it changes only the *slope* of a ramp already in flight (a kink, whose instant no estimator can pin), or nothing it touches moves in a way a recording can localise. Its **values** are still graded |
| `EVENT_DRIFT` | error | lateness *grows* over the session (the scheduler uses absolute deadlines, so this means a clock problem) |
| `EVENT_DRIFT_NOT_TESTED` | info | the residuals tilt, but a gate failed (baseline too short, slope inside its own standard error, or inside the already-fitted clock ratio) — the gate that failed is named |
| `EVENTS_BEFORE_RECORDING` | info | events predicted before the first sample: the capture started late, or t0 is early |
| `FLICKER_RATE` | error | per-channel flicker rate wrong over a sustained run. Primary estimator is a least-squares fit of the *edge grid* over every rise in a rate-steady span — hundreds of edges, tens of ppm, and the finding quotes the fit's own half-width. The 10-cycle rolling-mean series is kept only as a **gross** (>15%) detector for spans the grid fit declines |
| `DUTY` | error | on-time / period wrong. Error, not a warning: duty comes from the optical *edge pattern*, so it has no sensor-gain or AC-coupling excuse |
| `BRIGHTNESS` | warning | the measured brightness *profile* departs from the demanded one. A warning because a single session-wide sensor gain, ambient drift and AC coupling can all produce it — the finding's own detail names each |
| `BRIGHTNESS_MODULATION` | error | the `~a:b:period` brightness **breathing** is absent where it is demanded (measured depth under a quarter of the demanded depth over the following 15 s), or present where a steady brightness is demanded. A presence test, not a depth measurement: the per-cycle ON-level estimator under-reports the dim half of an LFO by up to 2x, so the printed depths are indicative and anything between a quarter and the full demand is left ungraded. The window looks *forward*, so the finding starts at the entry that broke the modulation |
| `LIGHT_DARK_WHEN_DEMANDED` | error | a zone was **dark** for a sustained span while the `.ledc` demanded measurable light. Decided from the normalised per-cycle modulation depth, so no sensor gain, ambient level or AC coupling can account for it |
| `CHANNEL_NEVER_ON` | error | the `.ledc` asks for light and the sensor saw none. Decided against this sensor's **own** noise floor, so it fires even when *every* mapped channel is dark |
| `LIGHT_NOT_GRADED` | info | brightness demanded below the measurable floor; coverage lost, stated not hidden |
| `TONE_FREQ`, `BEAT_FREQ`, `PULSE_RATE`, `BEAT_DRIFT` | error | audio value wrong. The carrier is graded against the set of carriers *this ear* was asked for, and only where one audible slot dominates by 6 dB **or** every audible slot agrees on the same carrier — a recording cannot separate two comparable tones 5 Hz apart, and the peak then lands between them |
| `AMPLITUDE` | warning | the audio level *profile* departs from the demanded one |
| `AUDIO_LEVEL_ABSENT` | error | the level is 12 dB or more below the demanded profile for a sustained span — four times down in amplitude, which no single-gain calibration can absorb |
| `AUDIO_PEAK_SHORT` | error | on a span where two slots interfere, the measured envelope **peak** falls short of the sum of the demanded amplitudes. That bound survives the coherence even though the mean level does not, so the two-slot panned binaural idiom is still graded |
| `PULSE_NOT_GRADED` | info | the demanded isochronic rate is below what the AM analysis window can resolve (fewer than four cycles in it) |
| `AMPLITUDE_NOT_GRADED` | info | two audible slots share a carrier there, so the summed level is a coherent interference the incoherent power-sum model cannot predict |
| `BEAT_UNAVAILABLE`, `PULSE_NOT_MEASURED` | info | not measurable with this front-end/map |
| `AV_CONST_OFFSET` | error | constant audio-vs-light offset above +40 ms |
| `AV_AUDIO_LEADS_LIGHT` | error | audio *precedes* light by more than the measurement floor — impossible if both +46.439 ms corrections are applied |
| `AV_OFFSET_MARGINAL` | info | slightly negative, but inside this instrument's own floor (the real fault is −46 ms or −93 ms) |
| `AV_NOT_MEASURABLE` | info | the offset is **not reported**: one of the two origins could not be measured, or t0 is coarser than the whole 0…40 ms band |
| `AV_DIFFERENTIAL_DRIFT` | error | audio and light timebases diverge, past this recording's own estimator floor — a real device bug, and the finding names *which* domain moved |
| `AV_DIFFERENTIAL_DRIFT_NOT_TESTED` | info | past the nominal threshold but inside the estimator floor for this recording's run lengths |
| `CLOCK_UNSEPARABLE` | info | only one domain had a usable rate reference, so "recorder crystal" vs "device-wide timebase error" is an assumption, not a measurement |
| `LEDC_*` | varies | static `.ledc` problems, from `lint`; no recording needed |

Findings that are purely *relative to t0* are automatically demoted from error
to warning when t0 itself is weak, and say so — a sync error larger than the
discrepancy is not evidence of a device fault.

**Where something cannot be measured, the number is withheld and the reason is
printed.** `AV_NOT_MEASURABLE`, `AMPLITUDE_NOT_GRADED`, `LIGHT_NOT_GRADED`,
`EVENT_NOT_OBSERVABLE`, `EVENT_DRIFT_NOT_TESTED` and `CLOCK_UNSEPARABLE` all
exist for that: "unmeasurable here, and why" is a valid output, and a number
that carries no information is not.

### Verdict thresholds

Deliberately loose. A false positive costs a human hours chasing a bug that does
not exist, which is strictly worse than missing a marginal real one. All live in
`devicemodel.py` with a `file:line` citation each, so a firmware change is a
one-file edit:

- event lateness: **25 ms** (budget: ±0.5 ms timer grid + ~7.6 ms for one INFO
  log line at 115200 baud + prio-4-on-core-0 wait behind WiFi), plus **2 flicker
  periods** for an LED value change timed off the edge intervals, or **10** when
  it had to be timed off a 10-cycle smoothed series (duty, brightness) — the
  finding says which
- the realisation **search window tracks that tolerance** instead of being
  fixed, so any lateness the verdict would flag is inside the search. It is
  clamped by the distance to the neighbouring entry on the same channel, and
  when the clamp bites, lateness beyond it is reported as *present but
  unquantifiable* rather than not reported
- audio-vs-light offset: the healthy band is **0…+40 ms** and **audio is
  expected to lag light** by ~13 ms. Audio *leading* light is a bug, but it is
  only an **error** past a **15 ms measurement floor** (widened further by t0's
  own uncertainty) — on bit-perfect renders the raw residual already spans
  −1.5…+2.3 ms from estimator bias alone, and the fault this catches is −46 ms
  or −93 ms. Between the floor and 0 it is `AV_OFFSET_MARGINAL` info
- flicker rate: 0.5% beyond the computed `ceil()` quantization, and a rate that
  is *low* by less than that is never flagged
- drift: 20 ppm common, 30 ppm differential — but the differential is also gated
  on **this recording's own estimator floor** (3σ of the per-domain fits). The
  light clock is fitted from edge times, so its uncertainty scales as
  1/(rate × run length): a 20 s run at 4 Hz is 80 edges and cannot resolve
  30 ppm, and saying so beats reporting the noise
- a **drift** claim additionally needs a ≥300 s baseline, ≥8 residuals weighted
  by their own tolerances, a slope beating 3× its own standard error, and
  agreement with the independently fitted clock ratio
- first LED edge: a ≥50 ms grace window, because the cycle anchor sits
  +46.439 ms in the future by design (`main/led_matrix_example.c:712`)

---

## 6. Architecture

Four separable pieces, so the planned on-device trace front-end
(`plans/onboard_av_trace_plan.md`) drops in without touching the comparator —
**but it would not measure the same thing.** See §8: everything between
"the executor dispatched the entry" and "photons/sound left the device" is
invisible to an on-device trace and is exactly what the external rig is for.
The two front-ends are interchangeable in CODE, not in COVERAGE.

```
ledc_expect.py ──┐
  the .ledc's     │
  demanded model  ├──> compare.py ──> findings
                  │      (front-end agnostic)
observe_wav.py ──┘
  observed model        synth.py — renders test recordings + injects faults
  (WAV front-end)       devicemodel.py — firmware constants & quantization
                        timeline.py — the shared contract
```

`timeline.py` defines the contract. **`compare.py` is forbidden from touching
anything audio-specific** — no sample rates, no FFTs, no WAV channel indices. A
front-end that cannot measure a field leaves it NaN and sets the matching
`available` flag, and the comparator reports "unavailable" instead of guessing.
A guess from a measurement instrument is a lie.

`ledc_expect.py` is the highest-stakes file: it is the ground truth. It
implements the firmware's parser and executor semantics — the animate-on-start
interpolation convention, the stable sort by timestamp, the audio-then-LED batch
passes, the 50-per-batch and 100-entry caps, first-batch hoisting, the `-`
sentinel's live-value substitution — each with a `file:line` citation to the
firmware it mirrors.

### Memory

A 60-minute 6-channel 44.1 kHz WAV is **1.8 GB**; it is never loaded into RAM.
Audio is processed in streaming blocks (`--block-frames`, default 32768 ≈ 0.74 s).

Measured peak RSS:

| recording | peak RSS |
|---|---|
| 5 min, 4 ch | **81 MB** |
| 60 min, 6 ch, 1.8 GB file, 40 Hz flicker on 4 zones | **299 MB** |

What actually scales is **not** file size but the number of detected optical
edges: 40 Hz × 3600 s × 4 zones ≈ 576 k edges, plus two 1 kHz float32 series
(the fine onset-timing level and the marker envelope, ~14 MB each per hour).
A lower flicker rate or fewer light channels costs proportionally less.
Shrinking `--block-frames` does **not** reduce the peak — it slightly increases
it — because the block is not what dominates. Measured on the 60-minute file:
297,959,424 B at the 32768 default against 301,629,440 B at 4096. The CLI help
for that flag used to claim the opposite (and to be wrong by 8.6×); it now says
this.

---

## 7. Self-test

```bash
cd tools/avmeasure
python3 -m unittest discover -s tests -t . -v      # the -t . is required
python3 tests/test_avmeasure.py -v                 # same thing
```

82 tests, stdlib + numpy only, ~176 s, no hardware. Four parts:

- **Cry-wolf tests** — a clean synthetic recording must produce **zero errors
  and zero warnings**, with and without a sync marker, and the measured
  quantities must match the session (not merely "not fail"). This is the most
  important test in the file: an instrument that reports phantom bugs gets
  ignored, and then it may as well not exist.
- **Fault tests** — each injects *one* quantified defect and asserts both that
  the right code fires *and* that the reported magnitude is right to a stated
  tolerance. Asserting only "something fired" would let the detector be wrong by
  10× and still pass.
- **Silent-miss tests** (`TestSilentMisses`) — the worst failure class, so it
  gets its own section. Each case is one where the tool used to print "Nothing
  to report: the recording matches the `.ledc` within every threshold" with a
  real injected fault in the recording: an audio step 700 ms or 1500 ms late, an
  LED rate step 300 ms or 800 ms late, every mapped light channel dark, a
  dropped entry. Sensitivity is asserted to be **monotone** — a worse fault must
  never be more likely to be missed than a mild one.

Covered faults: constant A/V offset, audio leading light, recorder clock drift
(must **not** be called a device bug), differential drift (must be), late event
at four magnitudes, dropped event, wrong flicker frequency, wrong audio
frequency, dead light channel (one, and *all*), beat drift, added noise,
AC-coupled sensor, single-audio-channel and no-light degradation, and —
separately — ~38 firmware-semantics assertions encoding specific `.ledc`
behaviours as hand-written expectations.

### Injectable faults (`synth --fault`, repeatable)

```
offset:MS              audio delayed MS ms relative to light (negative = leads)
drift:PPM              recorder clock error, applied to BOTH domains
audio-drift:PPM        differential drift, audio only (a device-side bug)
drop:T_MS[,T_MS...]    entries at those timestamps never fire
late:T_MS:MS           entries at T_MS fire MS ms late
light-freq:CH:FACTOR   LED channel CH flickers at FACTOR x the right rate
audio-freq:FACTOR      every audio carrier scaled by FACTOR
dead-light:CH[,CH...]  LED channel(s) never light up
noise:DBFS             white noise at DBFS added to every channel
ac-couple:HZ           simulate an AC-coupled sensor input (high-pass)
beat-drift:HZ_PER_1000S  the binaural detune grows, so the beat drifts
```

The suite also renders **four shipped sessions** (a fade-in with an 18-minute
rate glide, a steady audio isochronic over a DC lamp, a two-channel panned
binaural with a brightness LFO, and a 40 Hz four-zone session) and asserts zero
errors and zero warnings on each. Rendering only `tests/selftest.ledc` was how
three error-severity phantoms — a 10× wrong `PULSE_RATE`, an `EVENT_DRIFT` off a
31 s baseline, and an `AMPLITUDE` null from two slots sharing a carrier — all
survived a green suite.

---

### What is deliberately NOT graded, and why

Every one of these prints an `info` finding naming the span and the reason —
silent loss of coverage is the failure mode this tool is written against.

* **The clock ratio of a smooth-carrier light channel.** A threshold detector
  crosses the firmware's sine/triangle/trapezoid carrier a fixed fraction of a
  cycle after the cycle origin *only while the brightness is constant*; any
  level movement walks the crossing inside the cycle and that walk is
  indistinguishable from a period error. Measured at +221 ppm on a zero-drift
  render whose true rate was right to +2 ppm. Square carriers are unaffected
  (the crossing *is* the step), and the rate check — three orders of magnitude
  coarser than a clock fit — keeps smooth carriers.
* **An audio level corner, to better than its own reproducibility.** The level
  trajectory match grades the corner where a volume ramp starts or ends — that
  is how a 300 ms late ramp-end is caught at all — but the same fit, on a clean
  recording, slides by hundreds of milliseconds to absorb a model/mix residual
  that is smooth on the scale of the shift. Its stated half-width is therefore
  the wider of the SSE valley and a leave-one-block-out jackknife, and on a
  30-minute session with a noise bed that is routinely ±0.3–2.5 s. Lateness
  smaller than that is **not graded** on those entries, and the finding says
  which of the two numbers set the budget. A STEP is unaffected (a 6 dB volume
  step reproduces to ±80 ms, and its 180 ms lateness reads 212 ms).
* **The instant of a "kink".** An entry that only changes the *slope* of a ramp
  already in flight has no locatable instant: measured 3.0 s and 0.3 s of
  apparent lateness on two bit-perfect recordings. Its values are graded; its
  timing is not.
* **The isochronic rate where the mix has an envelope of its own.** Two audible
  carriers in one ear beat at their difference frequency, which is a real, deep
  amplitude modulation of the recording. A measured AM rate that matches such a
  beat is not flagged.
* **A carrier where several comparable tones are audible in the same ear.** See
  `TONE_FREQ` above.
* **The DEPTH of a brightness LFO.** The level series is the mean of the
  envelope over each detected ON interval, and the Schmitt trigger's thresholds
  come from 4 s percentile blocks, so in the dim half of a `~a:b:period` swing
  the upper trigger goes unreachable and the ON segment's mean is pulled up by
  the brighter cycles around it. Measured on a clean `~10:30:10000`: the series
  floors at the equivalent of 16.5% where 10% was demanded, i.e. 0.45-0.65 of
  the true depth on a perfect render. So the modulation is graded as PRESENT or
  ABSENT (`BRIGHTNESS_MODULATION`, under a quarter of the demand) and its depth
  is not graded at all. A device whose LFO is half as deep as asked is not
  caught by this instrument.

## 8. LIMITATIONS — what this cannot see

Read this before trusting a result.

**The self-tests do not validate `ledc_expect.py`.** `synth.py` renders from the
same keyframe state machine the expectation uses, so the round trip validates
`observe_wav.py` and `compare.py` but is *blind to a shared misunderstanding of
the firmware*. If `ledc_expect.py` mis-models a `.ledc` rule, synth will render
the same mistake and the tests will pass. That fidelity rests on the `file:line`
citations, on the ~38 hand-written semantic tests, and on a human reading
`analyze.py expect --dump`. **This is the tool's single biggest risk.**

Two consequences of that were found and fixed by re-reading the C rather than
by any test: the audio engine *discards* a ramp's start literal and substitutes
the channel's live value (`main/audio_generator.c:519-551`) — and zero, not the
literal, on a first-entry volume ramp (`:386`) — and the LED ISR tick is sized
from the **spec literal**, not from the live value the interpolator ramps from
(`main/led_matrix_example.c:1936-1938`). Both were modelled the other way
round, both corrupted a headline number, and `synth.py` shares the state machine
so all tests passed. The semantic tests now assert those start values and tick
rates *directly* rather than only round-tripping them.

**Absolute levels are unknowable.** The audio chain includes a fixed 1/16 mix
headroom (`main/audio_generator.c:705`) and a runtime `settings.audio_max_volume`
that no recording can reveal; optical gain depends on distance and ambient
light. Everything amplitude-related is therefore checked as a **ratio** after
fitting one scale factor per channel — the *shape* of a profile, never its
absolute value. A session played at half volume throughout reads as correct.

**Audio channels cannot be separated.** All 16 generator slots sum into one
stereo pair. The tool checks the *dominant* tone per ear plus the total level,
and only checks a beat when a single binaural channel dominates. Two
simultaneous carriers of similar level are not individually measurable.

**Timeline AM depth is ~10%, not full gating.** `mod_depth` is hard-coded to
0.1 for every timeline entry (`main/config_parser.c:2357`), so the isochronic
gate swings amplitude only between 0.9 and 1.0 of the carrier (~0.9 dB). Pulse
measurements are correspondingly delicate, and a noisy recording may report
`PULSE_NOT_MEASURED`.

**Low brightness is not measurable.** Below ~15% demanded brightness the optical
envelope cannot be resolved into edges, and the rate estimator collapses toward
a submultiple. Those spans are excluded and reported as `LIGHT_NOT_GRADED`
rather than graded wrongly — which means a fault that occurs *only* during a
fade-in or fade-out will be missed.

**The demanded trajectory is sampled on a grid, and the grid is coarse on a
long session.** `ledc_expect` samples the model at a step chosen to bound
memory for a 60-minute capture, so what the comparator interpolates is a
*sampled* trajectory whose breakpoints generally fall between samples. That
alone made two bit-perfect renders look like device faults (an `EVENT_EARLY` of
−252 ms on `21_gateway_focus12_TRAP`, −445 ms on `25_gateway_journey_TRAP`),
because a ramp's end does not reach its final level in the interpolation until
the first sample at or after it. The comparator now reinserts the breakpoint by
extrapolating both sides to the entry's own timestamp (`_grid_sharpen`), which
is exact for a step, a corner, a ramp start and a ramp end — but only AT the
entry being realised. Any other feature in the window is still grid-limited.

**AC-coupled inputs distort level, not timing.** Every line input high-passes
somewhere around 5–20 Hz. Edge *times* survive this (there is a test for it), so
rate and duty are fine, but the recovered brightness *level* of a slow flicker
is attenuated and phase-shifted. **A sub-1 Hz flicker, or a DC-on channel, is
effectively invisible to an audio interface** — that needs a DC-coupled ADC,
which no USB audio interface is.

That attenuation is **rate-dependent**, and the brightness check fits one sensor
gain per channel for the whole session, so a session that changes a channel's
flicker rate partway through can produce a `BRIGHTNESS` warning on the later
half from the input alone. MEASURED with a 12 Hz one-pole high-pass on an
otherwise bit-perfect render of `tests/selftest.ledc`: 0 dB on the 10 Hz first
half and **+3.7 dB** on the 20 Hz second half. This is NOT fixed — the finding's
`detail` says so instead, because fitting a gain per rate segment would silently
remove brightness coverage from every session with a continuous rate glide
(`01_sleep_onset` glides 10 → 2 Hz for 18 minutes), and silent loss of coverage
is the worse failure. Build the DC-coupled front-end in §4.

**Without a sync marker, t0 on a real session is good to seconds, not
milliseconds.** See §3. The tool tells you when this is the case; it does not
magically fix it.

**It cannot distinguish "the firmware mis-scheduled it" from "the OS delayed
the HTTP POST".** T0 is captured inside the POST handler, so anything that
happened before that is invisible.

**Timing on NEOPIXEL is quantized to ~2.5 ms.** 74 LEDs × 24 bits × 1.25 µs +
280 µs latch = 2.50 ms per frame, and the refresh blocks on the previous
transmission (`main/led_strip.c:517`). Pass `--led-backend direct` if the
device is running the LEDC backend instead (~40 µs), which changes the verdict
thresholds by ~5 ms.

**The browser does not POST the file on disk.** It POSTs
`serialize(parse(file))` with `S` (speech) rows stripped, and when speech is
present it rewrites the `BG` line to `push://` and streams a bounced WAV — which
also *defers* T0 until 2000 ms of audio is buffered
(`web/src/js/gen/serialize.js:159`, `main/config_parser.c:636-648`). The tool
applies the same transform by default (`--keep-speech` disables it), but if you
want certainty, capture the actual POST body.

**An on-device trace could not replace this rig for the A/V offset — it is
blind to exactly the terms the offset is made of.** Everything between "the
executor dispatched the entry" and "sound/photons left the device" is invisible
to a trace and fully visible to an external recording:

| term | size | source |
|---|---|---|
| `fill_buffer` latch | U(0, 23.22 ms), mean 11.61 ms | a channel start or param update is only picked up at the next generator-block boundary, **once per buffer, not per sample** (`main/audio_generator.c:813-821`) |
| true DMA transit | [40.6, 46.4] ms | 7–8 descriptors of 256 frames (`devicemodel.py:55-65`) |
| the firmware's own over-statement | ~2.90 ms | it uses the single upper-bound constant 46439 µs for both corrections |
| codec group delay | unknown | not observable from inside |
| NeoPixel strip write | 2.5 ms | 74 px × 24 bit × 1.25 µs + 280 µs latch (`main/led_strip.c:517`) |

A device trace would report an A/V offset of ~0 ms for a device whose DMA
pipeline was genuinely misaligned, because both of its timestamps come from
*before* the pipeline. §6's claim that the trace front-end "drops in without
touching the comparator" is true of the code and false of the measurement.

**Flicker-rate jitter is not modelled.** The `.ledc` jitter column perturbs the
emitted rate at the ISR (`main/led_matrix_example.c:845-851`), while the
expectation treats the rate as constant, so a session that uses that column will
show rate wander the model calls an error. Neither the 5 Hz amplitude cap nor
the `period_ms >= 1000` guard (`:1440-1446`) is reproduced. Do not use the
jitter column on a session you intend to measure.

**4 GB is a hard ceiling on the recording.** Plain RIFF/WAVE cannot address
past it: 60 min / 6 ch / 24-bit / 48 kHz is 3.1 GB and 90 min is 4.7 GB, at
which point interfaces write RF64 or Wave64 (which this tool names and refuses,
rather than mis-parsing) or silently split the take. Record 16-bit, or split the
session.

**Not supported:** the on-device trace front-end (designed for, not built),
per-pixel/RGB colour verification beyond the per-channel R/G/B values, and
anything about what the session *should* therapeutically do.
