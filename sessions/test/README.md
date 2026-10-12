# `sessions/test/` — feature-verification sessions

Fourteen short `.ledc` files that between them exercise every feature of the
`.ledc` format. **Total play time 12 min 32 s.** The shipped therapeutic
sessions in `sessions/library/` run 30–60 minutes each, which makes them
useless for verifying a change: these are 13–71 s apiece, one feature family
per file, so a failure is isolated by filename and a re-run costs under a
minute.

Every file is designed to be **measured**, not merely played. The tables below
state, for every block, what to expect **numerically** and which instrument
produces that number. A session nobody knows how to grade is worthless.

---

## Running them

```
# 1a. always first — did the RECORDING survive? (see "Capture integrity" below)
python3 tools/avmeasure/captureintegrity.py rec.wav --carrier <HZ> --audio-ch 1

# 1b. also first — a disconnected sensor produces confident, fictional results
python3 tools/avmeasure/signalcheck.py rec.wav <CH> <FLICKER_HZ> [start_s] [dur_s]

# 2. whole-session diff against the file
python3 tools/avmeasure/analyze.py analyze --ledc sessions/test/tNN_*.ledc \
    --wav rec.wav --map "audioL=1,audioR=2,light1=3,light4=4" --sync-tone 3000

# 3. targeted instruments
python3 tools/avmeasure/avsync.py     rec.wav --light 3 --audio 1 --carrier 400 \
                                      --marker-hz 3000 --rates F:T0:T1[,...]
python3 tools/avmeasure/ratechange.py rec.wav --light 3 --audio 1 \
                                      --f-old F --f-new F --entry S --win S --marker-hz 3000
python3 tools/avmeasure/percycle.py   rec.wav <CH> <F0> <START_S> <DUR_S>

# 4. static check — no hardware needed
python3 tools/avmeasure/analyze.py lint --ledc sessions/test/tNN_*.ledc
```

`percycle.py` and `avsync.py` both take **device** time (measured from the sync
marker), `avsync.py`/`ratechange.py` channels are **1-based**. Mixing device
time and file time is how a genuine single-cycle event was once written up as
"no per-cycle glitches": the two time bases sat ~2 s apart on that capture.

### Rig

| capture ch | source |
|---|---|
| 1, 2 | device headphone jack L / R via a TRS splitter, into **Hi-Z / instrument** inputs |
| 3 | photocell on the eye that holds **LED3+LED4** (`mask 12`) |
| 4 | photocell on the eye that holds **LED1+LED2** (`mask 3`) |

All four on one interface so they share one ADC clock. A photocell on a *line*
input (~20 kΩ) differentiates through its 100 nF cap and the harmonic ladder
goes flat (1 / 0.91 / 0.86 … instead of 1 / 0.33 / 0.20 …), which makes every
amplitude and shape grade in this document meaningless. Use a **line input, not
a microphone**, for the audio: a room adds reverb and ~3 ms per metre.

Blocks that need **two** photocells are marked "2 sensors" in the tables. With
one sensor you still grade every single-lane block and half of every two-lane
block.

### Capture integrity — run this before believing any per-cycle number

A recorder can lose a block of frames mid-capture and still write a file of
**exactly the right length**: the missing *time* is simply absent rather than
padded, so the frame-count check `capture.sh` performs is blind to it. Five such
steps were found in one 665 s capture (~1 per 2.2 min, largest 268 frames =
6.08 ms), and one of them was written up as a firmware defect — and survived a
day of scrutiny — because *both* optical sensors agreed on it. They agreed
because the whole recording stepped, not because the LED did.

At ~1 step per 2.2 minutes, a 13–71 s file has a real chance of carrying one.
Every session here therefore guarantees a **continuous sine** on a known audio
channel so `captureintegrity.py` can use the device's own I²S-clocked carrier as
a reference:

| file | command |
|---|---|
| `t01` | `captureintegrity.py rec.wav --carrier 400 --audio-ch 1` (ch1's gated 400 Hz sine; a 10 %-deep gate never shuts) |
| `t08` | `--carrier 311 --audio-ch 1` (ch1, continuous, LEFT from B3 on) |
| `t12` | `--carrier 500 --audio-ch 1` (ch3, continuous; volume and gate rate change, carrier frequency does not) |
| `t11` | `--carrier 1900 --audio-ch 1` (**ch0**, dedicated, hard LEFT — ch15 is needed by the channel map) |
| all others | `--carrier 1900 --audio-ch 1` (**ch15**, dedicated, hard LEFT) |

The dedicated carrier is hard-panned LEFT so the RIGHT capture channel stays
clean for envelope and `avsync.py` work. `t01`, `t08` and `t12` deliberately get
*no* extra tone: an ungated 1900 Hz carrier would add a DC pedestal to the `|x|`
envelopes those files are graded from, cutting the gate contrast from ~7 % to
~4.5 % and roughly doubling the per-edge scatter.

The tool needs a **sine**. A session cycling square / sawtooth / noise
"carriers" gives ~369 µs of block-to-block phase noise against ~1 µs for a
sine, which inflates its self-calibrating threshold into a rubber stamp — it
then returns "could not verify" rather than a worthless pass. The marker window
(0–600 ms) is not covered by the dedicated carrier; check it separately with
`--carrier 3000`.

**What a timebase step does and does not invalidate.** All input channels step
*together*, so every **differential** measurement is immune: the audio-vs-LED
offset, the inter-eye offset, and every harmonic ratio. What it corrupts is
**absolute per-cycle timing** — exactly `percycle.py`'s timing axis. Medians are
robust (a dropout spoils one period, not the median); single-cycle claims are
not.

**The 101 one-tick steps are device-real and benign.** Every excursion is a ± pair
summing to ~0, clustering at a ~1 s cadence, which points at a periodic timer
(lwIP / WiFi / stats) rather than RMT or flash. With the tick primer those steps
are 0.100 ms rather than 0.400 ms and fall below `percycle.py`'s threshold
entirely.

---

## Conventions every file shares

**Sync marker** — the first four rows of every file are copied verbatim from
`sessions/library/90_measure_sync.ledc`: 600 ms of full-brightness white on all
four zones plus a 3 kHz tone on audio ch1. Every tool locates device `t=0` from
it (±5 ms, versus ±2 s from bare cross-correlation). Always pass
`--sync-tone 3000`.

**ISR tick primer** — row five of every file except `t13` is
`1000 40 50 0 0 0 0 16 0 0`. This is load-bearing, not decoration. The flicker
gptimer is **deleted** whenever no channel is left running and the next
activation re-derives its tick from the 1000 Hz floor, so the marker's `freq 0`
stop at 600 ms *resets* the tick — the 40 Hz marker does **not** pin it. And
because an update on an already-running channel never calls
`s_ensure_timer_and_task`, whatever the first post-marker block sets is what
every later block gets. The primer keeps one channel alive at 40 Hz for the
whole file, pinning the tick at `clamp(40×250,1000,10000) = 10000 Hz` — a
**100 µs grid everywhere**. Two consequences worth knowing:

* a one-tick ISR slip is now **0.100 ms**, below `percycle.py`'s 0.335 ms
  threshold, so the tick steps that used to dominate its output (101 of them in
  one 11-minute capture at 0.400 ms) disappear;
* `mask 16` = LED5, which neither sensor can see (−78 / −74 dBFS, the noise
  floor), and brightness is 0 regardless, so the primer cannot contaminate a
  measurement on any unit. It raises `LEDC_MASK_HIGH_BITS` (info) by design.

**The two lanes** — the measured eye split is `{LED1,LED2}` vs `{LED3,LED4}`,
so the per-eye masks are **3** and **12** (not 9 and 6, which each take one LED
from *each* eye — that is why `17_nondual_awareness_RGB` cancelled by 25.8 dB).

* **lane A** = `mask 3` → capture ch4
* **lane B** = `mask 12` → capture ch3

The two lanes always carry **different flicker rates** in the same block, so a
spectrum names which lane a sensor is actually watching. That trick identified
the channel map in one capture.

**Brightness** — anything whose amplitude is being graded uses `RGB 255` and
`bright 100`, i.e. effective drive 255. Light is linear in drive only above
drive ~100; at drive 12 it is 68 % of linear, and the roll-off is *uneven*, so a
dim LFO's shape is distorted and not merely shallow. `t03` is the one file that
deliberately works down in the non-linear region, because measuring that curve
is its job.

**Phase 180** — never set between two zones that one sensor can see. It cancels
the fundamental (measured −25.8 dB) and makes per-cycle analysis impossible.
`t06` B3 is the only antiphase block and it is in *different eyes*, so each
sensor sees one phase; it is still marked "2 sensors".

**Audio gate depth is 0.1 and not settable.** `config_parser.c:2376` hardcodes
`mod_depth = 0.1f`. So a unipolar gate (`env 0..3`) swings the envelope
**0.90 → 1.00** and a tremolo (`env 4`) swings it **0.90 → 1.10**. Expect
sidebands around 30 dB below the carrier, never an audible on/off.

**Hardware facts respected throughout:** LED5 emits nothing; LED6 is visible to
lane A's eye but addressed by no shipped mask (both are probed deliberately in
`t13` B1); audio channels are 0..15 in the file but **1-based in `/api/state`**,
so `.ledc` ch8 reads back as ch9 there.

**Reading harmonic ratios** — always print the fundamental's absolute level in
dBFS beside any ratio. A degraded signal path inflates **even** harmonics
specifically: the same session measured h2/h1 = 0.547 with a faulty sensor
wire and 0.044 after it was repaired, with the fundamental 7.4 dB stronger.
Passing `signalcheck.py` is necessary but **not** sufficient. Also, h5 of a
10 Hz flicker lands on 50 Hz mains — check that h5 *moves* when the rate
changes before believing it.

---

## t01_avsync.ledc — 56 s — audio-vs-LED sync

Rates are 4 / 8 / 16 Hz, all exact divisors of the 10 kHz tick (2500 / 1250 /
625 ticks), so LED rate quantisation contributes **exactly zero** here. LED gate
and audio gate are commanded at the same rate, duty 50 and phase 0, so the
offset between their envelopes *is* the sync error. Running three rates
separates a real latency (constant **time** offset) from an envelope-shape
artifact (constant **phase**).

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–16 s | A/V offset at 4 Hz, 60 cycles | **−2.5 … −3.5 ms** (audio early); per-edge scatter 0.23–0.31 ms | `avsync.py --rates 4:2:15` |
| B2 | 16–26 s | A/V offset at 8 Hz, 80 cycles | same **−2.5 … −3.5 ms** | `avsync.py --rates 8:17:25` |
| B3 | 26–46 s | A/V offset at 16 Hz, 320 cycles | same **−2.5 … −3.5 ms**. Constant-latency fit across B1–B3 should close to **<0.1 ms**; a constant-*phase* fit should need ~40° of spread. That contrast is the result. | `avsync.py --rates 16:27:45` |
| B4 | 46.03–56 s | rate change 16 → 8 Hz at an **unaligned** instant (20 030 ms = 320.48 old periods after the 16 Hz entry) | change lands **≤ +1 cycle** after the entry, and the block's A/V offset equals B2's to within 1 ms. Pre-V-S4 this block ran up to ±31 ms out (half an old period); pre-V-S3 the LED grid jumped `new_period − old_period`. | `ratechange.py --f-old 16 --f-new 8 --entry 46.03 --win 6` then `avsync.py --rates 8:47:55` |
| — | all | LED rate accuracy, all three rates exact on the tick lattice | period within **±0.01 %**; robust sd ≲0.11 ms | `percycle.py rec.wav 3 <f> <start> <dur>` |

`avsync.py` only knows the offset **modulo the period**, which is useless once
the error exceeds half a period — that is why B4's landing instant is graded by
`ratechange.py` and only its residual by `avsync.py`.

---

## t02_led_rate_duty.ledc — 63 s — channel map, flicker rate, duty

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–13 s | **channel map**: LED1/2/3/4 at 5 / 7 / 9 / 11 Hz | each sensor's spectrum lists only its own eye's two rates, **30–43 dB** above the other eye's. ch4 → 5 and 7 Hz; ch3 → 9 and 11 Hz. Re-run this block whenever the rig is touched. | spectrum (2 s windows), 2 sensors |
| B1 | 1–13 s | **rate accuracy off the tick lattice** | 5 Hz exact. 7 / 9 / 11 Hz are *not* exact divisors of 10 kHz; the two candidate predictions are **quantised** 6.99790 / 8.99281 / 10.98901 Hz (−300 / −799 / −999 ppm) or **exact** if the phase-locked cycle origin removes the quantisation. This block decides which, and that question is still open (see `92_verify_7hz.ledc`). | `percycle.py` median period |
| B2 | 13–23 s | **duty 25 (lane A, 10 Hz) vs 75 (lane B, 7 Hz)** | on-fraction **25.0 ± 0.5 pp** and **75.0 ± 0.5 pp**. percycle's duty floor on a square carrier is 0.06–0.14 pp; `analyze.py`'s own gate is ±8 pp. | `percycle.py` duty axis, 2 sensors |
| B3 | 23–33 s | **duty 10 vs 90** — the extremes the engine still resolves | **10.0 ± 0.5 pp** / **90.0 ± 0.5 pp**. At 10 % of a 100 ms period the ON window is 10 ms = 100 ISR ticks, so quantisation is 1 %. | `percycle.py` duty axis, 2 sensors |
| B4 | 33–43 s | **rate extremes**: 40 Hz (lane A, 400 cycles) vs 2 Hz (lane B, 20 cycles) | both exact on the lattice: **40.000** and **2.000 Hz** within ±0.01 %. 2 Hz gets only 20 cycles — grade it as a period measurement, not a spectrum. | `percycle.py`, 2 sensors |
| B5 | 43–63 s | **rate limits**: 100 Hz = `LED_FREQ_MAX_HZ` (lane A, 2000 cycles) vs 1 Hz (lane B, 20 cycles) | **100.000** and **1.000 Hz** within ±0.01 %; both exact on the lattice. ⚠ 100 Hz is above flicker fusion and is not a therapeutic rate — bench only, do not stare at it. | `percycle.py`, 2 sensors |

---

## t03_led_bright_rgb.ledc — 71 s — brightness linearity and the R/G/B fields

Lane B holds a **fixed reference** (7 Hz, RGB 255, bright 100) for the whole
file; lane A steps through the drive curve at 10 Hz. Every lane-A amplitude is
quoted as a **ratio to lane B in the same window**, which divides out slow
sensor and supply drift. Never compare the two *sensors* to each other: ch3 and
ch4 are not gain-matched.

Reference curve (light per unit drive, measured optically): drive 12 → 0.135
(68 % of linear), 24 → 0.153 (77 %), 36 → 0.175 (88 %), 48–60 → 0.183–0.188
(92–95 %), 102 → 0.200, 153–255 → 0.197–0.202.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–11 s | drive **255** (bright 100 × RGB 255) | the 1.000 anchor for everything below | spectrum amplitude of the 10 Hz line |
| B2 | 11–21 s | drive **51** (bright 20) | light **0.183–0.188×** B1, against a linear prediction of 0.200 → **92–94 % of linear** | spectrum amplitude |
| B3 | 21–31 s | drive **12.75** (bright 5) | light **≈0.034×** B1 against a linear 0.050 → **≈68 % of linear**. This is the worst point on the curve. | spectrum amplitude |
| B4 | 31–41 s | **equal-drive cross-check, part 1**: bright 24 × RGB 255 = drive 61.2 | light ≈ **0.226×** B1 | spectrum amplitude |
| B5 | 41–51 s | **equal-drive cross-check, part 2**: bright 100 × RGB 60 = drive 60 | light ≈ **0.221×** B1, i.e. within **2–3 %** of B4 — the 2 % difference is the drive difference, not a non-linearity. Two paths to the same drive landing on the same light is what proves the roll-off is the LED's own PWM behaviour and not a measurement-chain artifact. | spectrum amplitude |
| B6 | 51–61 s | **colour fields**: lane A pure red (255,0,0), lane B pure green (0,255,0) | both lanes emit; record each sensor's level as a per-colour baseline. A photocell is broadband and not photometric, so absolute R:G:B ratios are rig constants, not device claims — what is being tested is that each colour field *drives independently*. | spectrum amplitude, 2 sensors |
| B7 | 61–71 s | lane A pure blue (0,0,255), lane B back to the white reference | lane A emits on blue only; lane B returns to its B1 level within 1 % (proving nothing drifted across the file) | spectrum amplitude, 2 sensors |

---

## t04_led_env_attack.ledc — 55 s — LED `env` carrier shapes and `attack`

The LED parser accepts `env` 0..3 and maps anything else (including 4) to 0.
Documented ladders at 10 Hz / duty 50 / drive 255:

| env | h2/h1 | h3/h1 | h4/h1 |
|---|---|---|---|
| 0 SQUARE | 0.004 | 0.345 | 0.003 |
| 1 SINE | 0.084 | 0.020 | 0.052 |
| 2 TRIANGLE | 0.043 | 0.106 | 0.034 |
| 3 TRAPEZOID, attack 3 ms | 0.042 | 0.316 | 0.048 |

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–11 s | **env 0** square (lane A, 10 Hz) vs **env 1** sine (lane B, 7 Hz) | square: h3/h1 **0.33–0.35**, h2 and h4 **<0.01**. sine: h3/h1 **<0.03** — the raised-sine LUT. The old `4x(1−x)` parabola gave h3 = 0.145, so a value near that means the LUT regressed. | spectrum, h1 in dBFS beside every ratio, 2 sensors |
| B2 | 11–21 s | **env 2** triangle (lane A) vs **env 3** trapezoid, attack 3 ms (lane B) | triangle: h3/h1 **≈0.11** (1/n² law), h2 **<0.06**. trapezoid at the default edge sits near the square row: h2 **≈0.04**, h3 **≈0.32**. | spectrum, 2 sensors |
| B3 | 21–31 s | **attack 10 ms** (lane A) vs **20 ms** (lane B), both env 3, both un-clamped (duty/2 = 25 ms at 10 Hz) | h2 **rises** and h3 **falls** monotonically: 10 ms → h2 ≈ 0.22, h3 ≈ 0.22; 20 ms → h2 ≈ 0.36, h3 ≈ 0.06. **The monotonic trend is the test**, not any single ratio. | spectrum, 2 sensors |
| B4 | 31–43 s | **attack 40 ms** at 4 Hz (lane A) vs **60 ms** at 3 Hz (lane B). Rates drop so duty/2 (62.5 and 83 ms) exceeds the edge and neither is clamped. | the ladder continues the B3 trend at the lower rates; both produce a visibly trapezoidal envelope with a measurable rise time of 40 ± 4 ms and 60 ± 6 ms | time-domain envelope rise time, 2 sensors |
| B5 | 43–55 s | **two clamps**: lane A attack **80 ms** at 4 Hz; lane B **env 4** on an LED line at 3 Hz | lane A must measure a **60 ms** rise, not 80: `led_matrix_set_attack_masked` caps at 60 ms, so B5 lane A is indistinguishable from B4 lane B scaled. lane B must measure as **SQUARE** (same ladder as B1 lane A): the parser maps env 4 → 0. | time-domain rise time (lane A), spectrum (lane B), 2 sensors |

Do **not** grade env 1/2/3 blocks with `percycle.py`'s timing axis. A smooth
carrier has no sharp edge, so its timing floor is ~10× worse (1.263 ms measured
on a sine carrier versus 0.113 ms on a square); use the fundamental's phase, or
grade those blocks on harmonics only.

---

## t05_led_interp.ledc — 57 s — all seven interpolation prefixes, LED side

LED compound cells are `freq duty bright R G B phase attack`. `mask`, `env` and
`jitter` are **not** compound — a prefix there is silently discarded. Every
brightness LFO swings 40 → 100 at RGB 255 (drive 102 → 255), entirely inside
the linear region, so the measured LFO shape *is* the commanded shape.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–13 s | **`>` linear** freq ramp 6 → 10 Hz (lane A); **`*` quadratic** freq ramp 12 → 7 Hz (lane B) | lane A: a straight ridge, +0.333 Hz/s. lane B: an S-curve with zero slope at both ends and −1.25 Hz/s at the midpoint. A rate sweep deliberately skips the V-S4 lattice snap (no fixed lattice exists), so grade with percycle's quadratic tracker, not an edge count. | spectrogram ridge + `percycle.py` (timing floor rises to ~0.22 ms during a sweep), 2 sensors |
| B2 | 13–23 s | **`~` sine** on brightness, 40↔100, 2 s (lane A); **`~` sine** on duty, 25↔75, 2 s (lane B) | lane A: the 10 Hz line's amplitude traces a sine with 5 full cycles, peak/trough ratio **2.50 ± 0.05** (drive 255/102, both linear). lane B: on-fraction traces a sine 25 → 75 pp, 5 cycles. | amplitude envelope of the carrier line; `percycle.py` duty series, 2 sensors |
| B3 | 23–33 s | **`^` triangle** on brightness (lane A); **`/` saw-up** on brightness (lane B) | same 2.50 peak/trough, but the envelope is a linear up-down triangle (lane A) and a linear ramp with an instantaneous reset (lane B). The reset edge is the discriminator between `/` and `^`. | amplitude envelope, 2 sensors |
| B4 | 33–43 s | **`\` saw-down** (lane A); **`_` square** (lane B), both on brightness | lane A: ramp 100 → 40 then jump back up. lane B: a two-level envelope, 1 s at bright 40 and 1 s at bright 100, with step transitions. | amplitude envelope, 2 sensors |
| B5 | 43–53 s | **`~` sine on a COLOUR cell**: lane A G swings 0 → 255 over 2.5 s (R 0, B 255) so the colour runs blue → white → blue; **`>` duty ramp** 20 → 80 (lane B) | lane A: the 10 Hz amplitude on a broadband photocell rises and falls with total drive (255 → 510 → 255), 4 cycles. lane B: on-fraction ramps linearly 20 → 80 pp at 6 pp/s. | amplitude envelope; `percycle.py` duty series, 2 sensors |
| B6 | 53–57 s | ramp target / settle — **not a measured block**, it exists so B5's duty ramp has an explicit endpoint | lane B duty reaches and holds **80 pp** | `percycle.py` duty |

---

## t06_led_phase_jitter.ledc — 68 s — LED `phase` and `jitter`

`phase` is a **LEAD**: the firmware adds `phase × period / 360` to elapsed time,
so the edge arrives **earlier**. Measured at 10 Hz: phase 0 → 0.00 ms, 90 →
74.76 ms, 180 → 49.93 ms, 270 → 24.69 ms (i.e. 90 and 270 are *not* 25 and
75 ms). To delay by N degrees, write `360 − N`.

Both lanes run at **10 Hz** in the phase blocks, which is the point — phase is
only meaningful relative to something at the same rate — and it is safe because
the lanes are in different eyes (30–43 dB apart), so neither sensor sees a
cancellation.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–11 s | phase **0** (lane A) vs phase **90** (lane B), same 10 Hz | lane B's rising edge **25.0 ± 1 ms EARLIER** than lane A's, equivalently +75.0 ms; cross-spectral phase **+90 ± 2°**. If you measure 25 ms *late*, the lead convention has been inverted. | edge-time difference between ch3 and ch4; cross-spectrum at 10 Hz. **2 sensors required** |
| B2 | 11–21 s | phase **0** vs phase **270** | lane B's edge **75.0 ± 1 ms earlier** = 25.0 ms later; cross-spectral phase **+270 ± 2°** | same. **2 sensors required** |
| B3 | 21–31 s | phase **0** vs phase **180** — antiphase, isolated | relative phase **180 ± 2°** (2° = 0.55 ms at 10 Hz); 178.1° was measured on the hardest shipped case. **One sensor per eye is mandatory**: a sensor seeing both lanes measures the residual of a near-cancellation (−25.8 dB fundamental, 2nd harmonic 21.5 dB *above* it) and percycle will decline to grade ~⅓ of the cycles. | cross-spectrum. **2 sensors required, one per eye** |
| B4 | 31–43 s | **phase ramp** 180 → 0 on lane B over 12 s, lane A holds 0 | lane B's edge **retards 4.17 ms per second** (−15 °/s), total 50 ms of walk; the rate stays 10.000 Hz throughout. The literal in the file is written as `>180`, not `>0`, because a running channel starts a phase ramp from its **live** value — writing `>0` would have ramped from 180 anyway and the file would have lied. | edge-time difference vs lane A, tracked over the block. **2 sensors required** |
| B5 | 43–53 s | **`~` attack modulation** on lane A, 3 ↔ 20 ms, 2 s period, trapezoid carrier (lane B holds phase 0 as B4's target) | lane A's harmonic ladder slides between the attack-3 and attack-20 rows of t04 (h2 0.04 ↔ 0.36, h3 0.32 ↔ 0.06) at 0.5 Hz, 5 cycles | short-window spectra across the block (h1 in dBFS each time) |
| B6 | 53–68 s | **jitter** 1.0 Hz amplitude, 5 s period on lane A at 10 Hz; lane B un-jittered at 7 Hz | lane A's instantaneous period wanders sinusoidally between **90.9 ms (11 Hz)** and **111.1 ms (9 Hz)**, 3 full cycles in 15 s; lane B's period stays flat. Engine limits **not in `ledc_format.md`**: amplitude is clamped to 5 Hz and a period below 1000 ms is silently ignored. A jittered channel is also excluded from the V-S4 lattice snap, by design — there is no fixed lattice. | `percycle.py` per-cycle period series (the sinusoid is the signal, not an anomaly), 2 sensors |

---

## t07_audio_carrier.ledc — 71 s — all eight wave types, pan, volume

The parser accepts waveType **0..7** (`AUDIO_WAVE_COUNT` is 8).
`ledc_format.md` documents only 0..6 and omits 7.

The four geometric waves run **simultaneously** on mutually non-harmonic
carriers (311 / 503 / 701 / 1103 Hz), so one spectrum grades all four — the
audio twin of the distinct-flicker-rate trick. Noise types cannot share a block
and are sequential, each with the previous explicitly silenced. A steady 10 Hz
LED reference runs for the whole file so the light channel still supplies `t=0`.

This file's **integrity carrier (ch15, 1900 Hz, hard LEFT, vol 40)** appears in
every block's spectrum. 1900 Hz was chosen to clear everything measured here:
it is not a multiple of 200 (the EEG-contour carrier in B5, whose harmonics land
on 2000), and it misses 311×6 = 1866, 503×4 = 2012, 701×3 = 2103 and
1103×2 = 2206. Grade the RIGHT capture channel wherever you can and it does not
appear at all.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–11 s | **wave 0/1/2/3** at once: sine 311, square 503, triangle 701, saw 1103 | sine: no harmonic above the floor. square: **odd only**, h3 0.333, h5 0.200, h7 0.143, h2 ≈ 0. triangle: **odd only**, h3 0.111, h5 0.040 (1/n²). saw: **all**, h2 0.500, h3 0.333, h4 0.250 (1/n). Carrier frequencies within **1 %** of nominal. | spectrum (0.25 s windows ⇒ 4 Hz bins; no two ladders collide) |
| B2 | 11–21 s | **wave 4 white noise**, ch5, alone | flat spectral density, **0 ± 1 dB/octave** over 200 Hz–10 kHz. **Exclude a ±50 Hz band around 1900 Hz from the fit** — that is this file's integrity carrier, on the LEFT channel; fit the RIGHT channel and the question does not arise. | spectrum, octave-band fit |
| B3 | 21–31 s | **wave 5 pink noise**, ch6, alone | **−3 ± 1 dB/octave** | spectrum, octave-band fit |
| B4 | 31–41 s | **wave 6 brown noise**, ch7, alone | **−6 ± 1 dB/octave** | spectrum, octave-band fit |
| B5 | 41–51 s | **wave 7 EEG contour** at 200 Hz on **channel 8** (also puts the block on the high half of the channel array) | a periodic 200 Hz carrier whose ladder matches **neither** 1/n nor 1/n² nor a sine. There is no published reference, so record the ladder as the baseline and assert only reproducibility against it on later runs. | spectrum (store the ladder) |
| B6 | 51–61 s | **pan** −100 / 0 / +100 on three carriers at once | equal-power law: 311 Hz only in L (R ≥ 40 dB down), 701 Hz only in R, 503 Hz **−3.01 ± 0.3 dB** in each channel | per-channel spectrum, both audio captures |
| B7 | 61–71 s | **volume** 80 / 40 / 20 / 10 on four carriers at once | three successive **−6.02 ± 0.3 dB** steps: 311 → 503 → 701 → 1103 | per-carrier spectrum level |

---

## t08_audio_pulse.ledc — 61 s — isochronic gating

Predicted **first sideband** relative to that channel's own carrier, with
`mod_depth` fixed at 0.1:

| env | envelope | first sideband | carrier |
|---|---|---|---|
| 4 sine tremolo | 0.90 … 1.10 | **−26.0 dB** | 0.00 dB |
| 0 square gate, duty 50 | 0.90 … 1.00 | **−29.5 dB** | −0.45 dB |
| 1 sine gate | 0.90 … 1.00 | **−31.6 dB** | −0.45 dB |
| 2 triangle gate | 0.90 … 1.00 | **−33.4 dB** | −0.45 dB |

Higher sidebands are the shape fingerprint: square falls as 1/n (odd only),
triangle as 1/n² (odd only), sine has none, trapezoid sits between and moves
with `attack`. A steady **8 Hz** LED reference runs for the whole file; 8 Hz is
not one of the mod rates in B1–B4, and it is also B6's mod rate.

`percycle.py` has no envelope detector — it segments by demodulated phase at the
flicker rate, so it grades **light**, not an AM audio channel. The audio gate is
graded either from the sideband ladder (multi-carrier blocks) or from a smoothed
`|x|` envelope (one-variant-per-ear blocks), which is what `avsync.py` builds
internally.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–11 s | **env 0/1/2/3** at once, mod 10 Hz, duty 50, four carriers | first sidebands at carrier ±10 Hz: **−29.5 / −31.6 / −33.4 / ≈−29.6 dB** respectively; the higher-sideband laws above distinguish the shapes. **The sharpest assertion here is ch2 (env 1):** that gate was `4·ph·(1−ph)` — a parabola, not a sine — until today, measuring h2/h1 = 0.251 and h3/h1 = 0.112 at 8 Hz, i.e. an alpha gate also driving 16 and 24 Hz. A true raised sine has only DC and the fundamental, so **no second or third sideband above the floor**. `duty 50` on ch2/ch3 is inert — see note 10. | spectrum, 2 s windows (0.5 Hz bins) |
| B2 | 11–21 s | **env 4 tremolo** (311) vs **env 1 sine gate** (503) vs **mod 0** unmodulated reference (701) | sideband −26.0 dB (env 4) vs −31.6 dB (env 1) — a clean **5.6 dB** discriminator; and env 1/env 4 carriers sit **−0.45 dB / 0.00 dB** against the mod-0 reference. The reference channel is what makes that 0.45 dB a measurement rather than an article of faith. | spectrum |
| B3 | 21–31 s | **duty 25 (hard LEFT) vs 75 (hard RIGHT)**, env 0, mod 8 Hz | envelope on-fraction **25.0 ± 1 pp** (L) and **75.0 ± 1 pp** (R) of a 125 ms period. Duty 25 and 75 of a square have the *same magnitude spectrum* (time-reverses), so only the time-domain on-fraction can tell them apart — hence the hard panning. Duty 50 is already covered by B1 ch1. | smoothed `|x|` envelope per audio channel; **both audio captures required** |
| B4 | 31–41 s | **mod rate 4 / 10 / 16 Hz** on three carriers at once | sideband **spacing** around 311 / 503 / 701 Hz measures 4.00 / 10.00 / 16.00 Hz ± 0.05 | spectrum, 2 s windows |
| B5 | 41–51 s | **attack 3 ms (LEFT) vs 25 ms (RIGHT)**, env 3, mod 8 Hz duty 50 (duty/2 clamp = 31.25 ms, so neither is clamped) | 2nd sideband **rises** and 3rd **falls** with attack, matching the optical trend (3/10/20 ms → h2 0.042/0.221/0.360, h3 0.316/0.221/0.061). Envelope rise time **3 ± 1 ms** and **25 ± 3 ms**. | spectrum + envelope rise time; **both audio captures required** |
| B6 | 51–61 s | **audio pulse phase 0 (LEFT) vs 90 (RIGHT)** against the 8 Hz LED gate | phase 0 → the established **−2.5 … −3.5 ms**. phase 90 → **31.25 ms earlier** than that (90/360 of a 125 ms period), because audio phase is the same LEAD convention as LED phase (the offset is *added* to the mod accumulator). | `avsync.py --light 3 --audio 1` then `--audio 2`, `--rates 8:52:60`; **both audio captures required** |

---

## t09_binaural.ledc — 53 s — both binaural idioms, beat glide, beat jitter

The two idioms are **not** equivalent. With `freqR` set on one channel, the
generator routes L and R carriers straight to the outputs and **skips the pan
stage entirely** (`binaural bypass`, when `frequency_r > 0` and `≠ frequency`),
so `pan` has no effect on such a channel — which `ledc_format.md` does not say.
With two hard-panned channels, the normal equal-power pan applies.

Both are measured the same way: **the frequency difference between the L and R
spectral lines is the beat**. Nothing in the recorded signal beats — the beat
happens in the listener — so never look for a beat line in a mono sum.
LED reference: steady 16 Hz, exact on the tick lattice.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–13 s | **idiom (a)**: ch1 L240/R250, ch2 L400/R406 | L capture shows lines at **240.0** and **400.0**, R at **250.0** and **406.0**, all within 0.1 Hz ⇒ beats **10.000** and **6.000 Hz**. Two different beats so the spectrum cannot confuse the channels. | spectrum, 2 s windows, both audio captures |
| B2 | 13–25 s | **idiom (b)**: pair ch3/ch4 at 300 L / 307 R, pair ch5/ch6 at 500 L / 504 R | L shows **300.0** and **500.0** only, R shows **307.0** and **504.0** only, channel separation **≥40 dB** ⇒ beats **7.000** and **4.000 Hz** | spectrum, both audio captures |
| B3 | 25–37 s | **beat glide**: left carrier ramps 240 → 243 with `freqR` pinned at 250 | the L line slides 240 → 243 Hz linearly (**0.250 Hz/s**) while the R line stays 250.0, so the beat glides **10.000 → 7.000 Hz** and *keeps going* — it must not snap back when the ramp ends. 29 of 83 shipped sessions were silently not performing this move until `freqR` was made the absolute right carrier on the sweep path. | spectrogram of both audio captures; line-difference series |
| B4 | 37–53 s | **beat jitter**, amp 1.0 Hz / period 5 s, set on ch1 only | ch1's R line wanders **249.0 … 251.0 Hz** sinusoidally, 3.2 cycles in 16 s, while its L line stays 243.0 ⇒ beat 7 ± 1 Hz. Three undocumented properties verified here: (1) it is **global** — ch2 sets no jitter column and must wander too; (2) it moves **only** the binaural detune (gated on `frequency_r > 0`), so ch7's mono 10 Hz gate must stay **rock steady**; (3) a period below 1000 ms is silently ignored. | spectrogram with interpolated peak tracking, 2 s windows; ch7 graded by sideband spacing stability |

---

## t10_audio_interp.ledc — 57 s — all seven prefixes, audio side

Audio compound cells are `freq pan vol mod duty phase attack`. **Not** `freqR`,
`ch`, `waveType`, `env` or `jitter`.

Two start-value rules, and they differ by field class:

* **core** (`freq pan vol mod`): on an already-running channel the firmware
  **discards the literal** and starts from the live value. On a fresh channel
  the literal is used — except `vol`, where the channel's amp is already 0 when
  the sweep is armed, so a first-entry volume ramp always starts from silence.
* **pulse** (`duty phase attack`): the start value **is** the literal. The one
  exception is `duty 0`, which the setter refuses, so a `>0` duty ramp silently
  starts from the previous duty.

Every ramp in this file lives on a channel whose live value already equals its
literal, so the numbers below are what the **device** does. Channel 1 is
deliberately unused for ramps: the marker leaves it active at freq 0 / vol 0.
LED reference: steady 20 Hz, exact on the lattice.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–13 s | **`>` linear** carrier ramp ch3 311 → 900 Hz; **`*` quadratic** ch4 1103 → 500 Hz | ch3: a straight ridge, **+49.08 Hz/s**. ch4: an S with zero slope at both ends. Both arrive at their B2 literals within 1 %. | spectrogram ridge fit |
| B2 | 13–23 s | **all five periodic modulations on `vol`, at once**: `~` 900, `^` 500, `/` 701, `\` 1301, `_` 1709, each 20↔80 over 2000 ms | each carrier's level traces its own LFO shape, 5 cycles, swinging **−14.0 dB ↔ −1.9 dB** (12.0 dB range). Sine vs triangle vs saw-up vs saw-down vs square are distinguished by envelope shape; the saws by which end resets. | per-carrier level envelope (0.1 s windows) |
| B3 | 23–35 s | **`~` pan** sine auto-pan ch3 −100↔+100 over 3 s; **`>` pan** linear ch4 0 → +100 over 12 s; **`~` mod rate** ch5 4↔16 Hz over 3 s | ch3: the 900 Hz line's L/R ratio traces an equal-power sine, 4 cycles. ch4: 500 Hz walks from centre (−3.01 dB each side) to hard right at **8.33 pan-units/s**. ch5: the sideband spacing around 701 Hz breathes **4 ↔ 16 Hz**, 4 cycles. | per-channel level ratio; sliding spectrum for ch5 |
| B4 | 35–47 s | **pulse-field ramps**: `>` duty 20 → 80 (ch3, mod 10 Hz); `>` attack 3 → 25 ms (ch4, mod 8 Hz, duty 50 ⇒ clamp 31.25 ms); `>` phase 0 → 180° (ch5, mod 10 Hz) | ch3 on-fraction ramps **5 pp/s**. ch4 envelope rise time ramps **1.83 ms/s**. ch5's gate edge advances **15 °/s = 4.17 ms/s** against the 20 Hz LED reference. | envelope on-fraction / rise time; `avsync.py --audio 2` for ch5's edge walk — use the **RIGHT** capture channel, since this file's integrity carrier sits in LEFT and would add a DC pedestal to the envelope |
| B5 | 47–57 s | the three ramp targets, held | duty **80 pp**, attack **25 ms**, phase **180°**, each as a static reading | as B4 |

---

## t11_audio_multichannel.ledc — 35 s — all 16 channels, and the ch8..15 ramp regression

Every channel carries a **sine** (no harmonics at all, so no two channels can
collide) at **f = 300 + 50 × ch** Hz. The channel index is therefore readable
straight off the spectrum as `(f − 300)/50`, which is what makes a 16-way block
gradeable. `vol 20` each: 16 × 0.20 × the 1/16 per-channel headroom = 0.20 peak,
no clipping. LED reference: steady 25 Hz.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–13 s | **all 16 channels sounding at once** — fifteen entries at one timestamp (inside the 50-per-timestamp cap) plus **channel 0**, armed at t=700 as the integrity carrier. ch0 is a real channel and the one an omitted `ch` column selects, so it serves both purposes. | **fifteen** lines at 350, 400, …, 1050 Hz, each within **1 %** of nominal and within **3 dB** of each other, **plus ch0's 1900 Hz line** (hard LEFT, vol 40, so it is louder and one-sided by design) = sixteen channels proven. A missing line names the channel that failed. Cross-check against `/api/state` remembering it is 1-based: `.ledc` ch8 reads back as ch9. | spectrum, 0.25 s windows |
| B2 | 13–25 s | **`>` frequency ramps on ch8..ch15 only**, at eight different slopes, with monotonically ordered start and end frequencies so the ridges can never cross. ch0..7 silenced. | eight **clean fanning ridges** at **4.17 / 8.33 / 12.50 / 16.67 / 20.83 / 25.00 / 29.17 / 33.33 Hz/s** (700→750, 750→850, 800→950, 850→1050, 900→1150, 950→1250, 1000→1350, 1050→1450). A **vertical jump** instead of a ridge on any channel is the `(uint8_t)(1u << ch)` truncation bug returning — it silently degraded every `>`/`*` ramp on channels 8..15 to a step and hit 14 shipped Gateway sessions. | spectrogram ridge fit, per channel |
| B3 | 25–35 s | ramp endpoints, held | the eight target frequencies within 1 %; no ramp left orphaned | spectrum |

---

## t12_format_edges.ledc — 51 s — the `-` sentinel, legacy LED lines, BG, S

`-` clears a field's present bit; the execute path substitutes the channel's
**live** value, so the write is a no-op for that field. Two sharp edges:
`-` in the LED `freq` column on a channel with **no** live rate resolves to 0 =
**stop flicker** (every `-` freq here lands on an already-flickering channel);
and the substitution reads a snapshot of the **lowest set bit** of the mask
only, so a multi-bit mask whose bits have diverged collapses to the low bit's
value (masks 3 and 12 are used and both bits always carry identical values, so
that cannot confuse the reading here).

An **absent trailing column is not the same as `-`**: omitting `waveType` on a
running audio channel resets it to sine, while `-` holds it. That inconsistency
is known and deliberately unfixed.

**BG**: the `sdcard://bg/test_bed.wav` path almost certainly does not exist on
your card — point it at a real asset or expect a "BG fetch failed" log line and
grade the rest of the file anyway. Do **not** substitute a `push://` URL: a push
URL makes the executor *hold the timeline* until the stream has buffered (or a
timeout fires), which moves `t=0`.

**S**: browser-only. The browser strips S rows before POSTing, so `analyze.py`
strips them too unless you pass `--keep-speech`, and the device's own parser
errors out on them on a non-SD build. Grade speech only when the session is
played **through the web UI**.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| — | session | **`BG` row**, `sdcard://`, pan 0, loudness 30 | a stereo bed at 30 % loudness, centre-panned, present from t=0; or a "BG fetch failed" log line if the asset is absent | serial log + audio level, `analyze.py analyze` |
| — | 1.5 s | **`S` row**, voice `default`, volume 60 | spoken text audible at t≈1.5 s **when played through the web UI**; nothing at all when POSTed to the device | listen / serial log; `lint --keep-speech` for the static side |
| B1 | 1–11 s | **5-token legacy LED lines**: `time freq duty bright mask`, no RGB | RGB defaults to **full white** and is marked explicitly present, so effective drive is 255 and these are directly comparable with the 8-token white blocks in t02/t03: lane A 10.000 Hz, lane B 7 Hz, both duty 50, both at the t02-B2 amplitude | `percycle.py` + spectrum amplitude, 2 sensors |
| B2 | 11–21 s | **`-` changes ONLY duty** | rate, brightness and colour **bit-identical to B1**; on-fraction moves to **25.0 pp** (lane A) and **75.0 pp** (lane B). Any change in amplitude or rate here is a sentinel bug. | `percycle.py` all three axes, 2 sensors |
| B3 | 21–31 s | **`-` changes ONLY colour** | rate unchanged **and the duty set in B2 survives** (25 / 75 pp); lane A white → red, lane B white → blue, with amplitude following the photocell's colour response only | spectrum amplitude + `percycle.py` duty, 2 sensors |
| B4 | 31–41 s | **`-` changes ONLY jitter** (LED, `ledc_format.md`'s own worked example) and **ONLY volume** (audio) | lane A's period starts wandering ±1.0 Hz with a 5 s cycle while its colour, duty and brightness hold; lane B's period stays flat; audio drops **70 → 40 vol = −4.9 ± 0.3 dB** with carrier, pan and gate rate unchanged | `percycle.py` period series; audio line level, 2 sensors |
| B5 | 41–51 s | **`-` changes ONLY the audio mod rate**; LED jitter back off | audio sideband spacing moves **10 Hz → 4 Hz** with the level still at −4.9 dB; lane A's period returns to flat 10.000 Hz with colour and duty still held | spectrum sideband spacing; `percycle.py`, 2 sensors |

---

## t13_rejects.ledc — 41 s — the lint findings ARE the test

⚠ **This file is supposed to trip the linter.** Every other file here lints
clean (bar the primer's info); this one carries one instance of each range or
syntax fault the device must **diagnose rather than crash on**. Grade it twice:

1. `analyze.py lint --ledc t13_rejects.ledc` must report **exactly** the
   findings in the table below — no more, no fewer.
2. play it with the serial monitor attached: the device must emit the matching
   `config_parser` warning for each, **and stay up**. Fault 4 was an
   out-of-bounds write of a whole `audio_gen_channel_t` past the end of
   `audio_channels[]` until it was fixed, so "the device stays up" is the real
   assertion.

No tick primer here, because the primer lives on `mask 16` and `mask 16` is the
thing under test in B1. The tick therefore follows the file's own rates: 1000 Hz
floor after the marker, 3500 Hz from the 14 Hz entry, 10000 Hz from the 150 Hz
sweep. That matters only for rate-accuracy claims, which this file does not
make.

| block | device t | fault | expected lint | expected device behaviour / measurement | instrument |
|---|---|---|---|---|---|
| F1 | 1 s | **`mask 0`** | `LEDC_LED_MASK_ZERO` (warning), 1× | rejected at **parse** time, so the entry never exists at all; log `channel_mask=0 — skipping` | lint + serial log |
| B1 | 1–13 s | **`mask 16`** (LED5) at 6 Hz and **`mask 32`** (LED6) at 14 Hz | `LEDC_MASK_HIGH_BITS` (info), 1× deduped | **no 6 Hz line on either sensor** (LED5 sits at the noise floor, −78 / −74 dBFS, probably unpopulated); a **14 Hz line on ch4 only at ≈−41.5 dBFS**, ~8 dB below LED1/LED2, despite LED6 being addressed by no shipped session | spectrum, 2 sensors |
| B2 | 13–25 s | **LED freq 150 Hz, two paths**: lane A without a ramp, lane B with `>` | `LEDC_LED_REJECTED` (error) ×2 — one per bit of mask 3; `LEDC_LED_OUT_OF_RANGE` (warning) ×2 for mask 12 | lane A **stays dark**: the no-sweep path's `freq>100` guard rejects the entry outright and the channel keeps doing whatever it was doing. lane B **actually flickers at ≈150 Hz**: the sweep path validates nothing. Measuring both in one block is what makes the asymmetry undeniable. | spectrum, 2 sensors |
| B3 | 25–37 s | **`R = 300`** on lane A | none (the model clamps silently) | clamped to **255** with a log line naming the field; lane A emits red at full drive | serial log + spectrum |
| B3 | 25–37 s | **audio `ch 16`** | `LEDC_AUDIO_CH_RANGE` (error) | clamped to **channel 0** with a log line naming the bad value, so 400 Hz plays on ch0 and **not where the file says**; the device must not reboot | serial log + spectrum (400 Hz line present) |
| B3 | 25–37 s | **`freqR` with a `>` prefix** on ch3 | `LEDC_FREQR_PREFIX` (error) | `atof(">250")` is 0.0, so the channel collapses to **MONO**: one 500 Hz line in **both** ears and **no beat** — instead of 500/250. This is the trap `ledc_format.md` invites by listing freqR as a compound cell. | spectrum, both audio captures |
| B3 | 25–37 s | **audio `duty 0`** on ch4 | `LEDC_AUDIO_DUTY_ZERO` (info) | a **no-op**: the setter only writes `duty > 0`, so the channel keeps the 50 % default, not 0 %. Sidebands around 600 Hz match a 50 % square gate. | spectrum |
| B3/B4 | 25–41 s | **`>` ramp whose target entry writes `-`** on ch5 | `LEDC_RAMP_TO_ZERO` (error) | the target is read from the next entry's struct member with **no present-bit check**, so it is still 0 from the memset: a ridge sliding from **700 Hz down toward DC** over 12 s instead of holding | spectrogram |
| B4 | 37–41 s | **`waveType` omitted** on a channel running wave 1 | `LEDC_WAVE_RESET` (warning) | the channel is silently reset to **wave 0 (sine)**: the 500 Hz channel's odd-harmonic ladder **disappears at t=37 s** | spectrum before/after 37 s |

Expected lint total: **5 errors, 4 warnings, 2 infos = 11 findings**, exit code 1.

---

## t14_batch_cap.ledc — 13 s — the 50-entries-per-timestamp cap

⚠ Trips `LEDC_BATCH_CAP` (error) on purpose; the cap **is** the thing under
test. Only the first 50 entries sharing a timestamp ever execute — the "find the
next strictly later timestamp" scan walks straight past the leftovers, so they
never fire, **silently, with no log line at all**. It is the one device limit
with no runtime diagnostic, which is why a reproducer is worth having.

54 entries share t=1000, in file order: the tick primer (slot 1), 49 identical
`mask 3` @ 10 Hz rows (slots 2–50), then 4 `mask 12` @ 7 Hz rows that are
**dropped**.

| block | device t | feature | expected measurement | instrument |
|---|---|---|---|---|
| B1 | 1–13 s | batch cap at 54 entries on one timestamp | **lane A (ch4) flickers at 10.000 Hz; lane B (ch3) stays DARK** with no 7 Hz line anywhere, ≥30 dB down. If mask 12 flickers, the cap has moved and every "why did my event not fire" diagnosis built on it needs revisiting. | spectrum + `signalcheck.py` on ch3 (which should report NO SIGNAL), 2 sensors |

Expected lint: **1 error + 1 info**, exit code 1.

---

## Feature-coverage checklist

### LED line — `<time> <freq> <duty> <bright> <R> <G> <B> <mask> [env] [phase] [attack] [jitter]`

| feature | values covered | where |
|---|---|---|
| 5-token legacy form | ✔ | t12 B1 |
| 8-token canonical form | ✔ | everywhere |
| 9–12-token v2 forms | ✔ all four lengths | t04 (9,10,11), t06 B6 (12) |
| `-` sentinel | ✔ in freq, duty, bright, R, G, B, env, phase, attack | t12 B2–B5 |
| `freq` | ✔ 0 (stop), 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 14, 16, 20, 25, 40, 100, 150 (out of range) | t01, t02, t04, t13 |
| `duty` | ✔ 10, 25, 50, 75, 90 | t02 B2–B3, t12 |
| `bright` | ✔ 0, 5, 20, 24, 50, 100 | t03 |
| `R`,`G`,`B` | ✔ 0, 60, 255, 300 (clamped) | t03 B6–B7, t05 B5, t13 B3 |
| `mask` | ✔ 0 (rejected), 1, 2, 3, 4, 8, 12, 15, 16, 32 | t02 B1, t13 F1/B1, all |
| `env` | ✔ 0, 1, 2, 3, 4 (→ square) | t04 |
| `duty` is a no-op under `env 1`/`env 2` | ✔ asserted, not merely avoided | t04 B1–B2 (see note 10) |
| `phase` | ✔ 0, 90, 180, 270 + ramp | t06 B1–B4 |
| `attack` | ✔ 3, 10, 20, 25, 40, 60, 80 (→ capped 60) + `~` mod | t04, t06 B5 |
| `jitter` | ✔ `amp` and `amp:period` forms, 0 and 1.0 | t06 B6, t12 B4 |
| `>` linear | ✔ on freq, duty, bright, phase | t05 B1/B5, t06 B4, t13 B2 |
| `*` quadratic | ✔ on freq | t05 B1 |
| `~` sine mod | ✔ on bright, duty, G, attack | t05 B2/B5, t06 B5 |
| `^` triangle mod | ✔ on bright | t05 B3 |
| `/` saw-up mod | ✔ on bright | t05 B3 |
| `\` saw-down mod | ✔ on bright | t05 B4 |
| `_` square mod | ✔ on bright | t05 B4 |

### Audio line — `A <time> <freq> <pan> <vol> <mod> [ch] [freqR] [waveType] [duty] [env] [phase] [attack] [jitter]`

| feature | values covered | where |
|---|---|---|
| 5/6/7/8-token short forms | ✔ 6 (marker), 7 (t13 B4), 8 (t07, t09, t11) | — |
| 9–13-token v2 forms | ✔ 11 (t01, t08), 12 (t10 B4), 13 (t09 B4) | — |
| `-` sentinel | ✔ in freq, pan, vol, mod | t12 B4–B5, t13 B4 |
| `freq` | ✔ 0 (noise), 200, 240…1709, 3000 | t07, t09, t10, t11 |
| `pan` | ✔ −100, 0, +100 + `~` and `>` | t07 B6, t08 B3/B5/B6, t09 B2, t10 B3 |
| `vol` | ✔ 0, 10, 20, 40, 60, 70, 80 + all five mods | t07 B7, t10 B2 |
| `mod` | ✔ 0, 4, 8, 10, 16 + `~` mod | t08 B4, t10 B3 |
| `ch` | ✔ **0 … 15** and 16 (clamped) | t11 B1, t13 B3 |
| `freqR` | ✔ 0 (mono), 250, 406 + prefix-abuse case | t09 B1, t13 B3 |
| `waveType` | ✔ **0, 1, 2, 3, 4, 5, 6, 7** | t07 B1–B5 |
| `duty` | ✔ 0 (no-op), 25, 50, 75, 80 + `>` ramp | t08 B1/B3, t10 B4, t13 B3 |
| `env` | ✔ 0, 1, 2, 3, 4 | t08 B1–B2 |
| `phase` | ✔ 0, 90, 180 + `>` ramp | t08 B6, t10 B4 |
| `attack` | ✔ 3, 25 + `>` ramp | t08 B1/B5, t10 B4 |
| `jitter` | ✔ `amp:period` and 0; globality and binaural-only gating both probed | t09 B4 |
| `>` / `*` | ✔ on freq (both), pan, vol, duty, phase, attack | t09 B3, t10 B1/B3/B4, t11 B2 |
| `~ ^ / \ _` | ✔ all five on vol; `~` also on pan and mod | t10 B2–B3 |
| isochronic gating | ✔ all four gate shapes + tremolo | t08 |
| binaural, idiom (a) `freqR` | ✔ | t09 B1, B3, B4 |
| binaural, idiom (b) panned pair | ✔ two pairs | t09 B2 |
| noise types | ✔ white, pink, brown | t07 B2–B4 |
| channels 8..15 ramping | ✔ all eight, eight slopes | t11 B2 |

### Other line types and structures

| feature | covered | where |
|---|---|---|
| `#` full-line comment | ✔ | every file |
| inline `#` comment | ✔ | every file |
| blank lines | ✔ | every file |
| `BG <url> <pan> <loudness>` | ✔ `sdcard://` | t12 |
| `S <time> <voice> <vol> "<text>"` | ✔ | t12 |
| sync-marker idiom | ✔ | every file |
| ISR-tick behaviour across a marker stop | ✔ | every file (primer) |
| continuous-sine capture-integrity reference | ✔ | every file (ch15, or ch0 in `t11`, or an existing carrier in `t01`/`t08`/`t12`) |
| 50-per-timestamp batch cap | ✔ | t14 |
| per-eye masks 3 / 12 | ✔ | t02–t06, t12, t14 |
| LED5 invisible / LED6 visible | ✔ | t13 B1 |
| rate change on an active channel at an unaligned instant | ✔ | t01 B4 |

---

## Not covered, and why

| feature | why not |
|---|---|
| **`BG` with `http://`, `https://`, `push://`** | Only `sdcard://` is exercised. `http(s)://` needs a reachable host and turns a bench test into a network test; `push://` makes the executor **hold the timeline** until the pushed stream buffers, which moves `t=0` and would break every timing grade in the suite. Both are worth a separate, non-timing session. |
| **`LEDC_FIRST_BATCH_HOIST`** (earliest entry not at t=0 ⇒ first batch fires immediately at wall-clock 0) | Mutually exclusive with a `t=0` sync marker, which every file here needs. A reproducer must be a marker-less file, and it would then have no reliable `t=0` to measure against. |
| **`LEDC_END_STOP_SKIPPED`** (final timestamp over the batch cap ⇒ end-of-timeline stop never runs ⇒ LEDs flicker for ever) | A real failure mode, but a test session that leaves the LEDs on indefinitely is unsafe to ship. `t14` puts the over-capped timestamp in the middle for exactly this reason. |
| **`LEDC_ENTRY_CAP`** (entries past 100 silently dropped at parse) | Would need a >100-line file, i.e. the whole suite's line budget in one unmeasurable session, and the dropped entries produce no observable at all. Verifiable by `lint` alone on a throwaway file. |
| **`MAX_LINE_LENGTH` 256 / `MAX_TOKENS` 16 truncation** | Token truncation changes how a line is *interpreted*, so a file testing it is by construction a file whose meaning is ambiguous. `lint` reports `LEDC_LINE_TRUNCATED` statically; not worth a play-session. |
| **`mod_depth`** | Not a `.ledc` field at all — hardcoded `0.1f` in `config_parser.c:2376`. Only reachable via `/api/iso-env`, which is an API test, not a format test. |
| **LED `jitter` amplitude clamp at 5 Hz** | Documented in `t06`'s header and in this README, but not exercised: a >5 Hz wander on a 10 Hz flicker would make the rate non-monotonic and ungradeable. |
| **`env` on a *stopped* channel** | `config_parser.c` applies env/phase/attack/jitter in both the stop and start branches, so a `freq 0` row can still change the pulse shape. Observable only on the *next* start; needs a dedicated three-block session and is low-value. |
| **Sub-Hz flicker (<1 Hz)** | `t02` B5 goes down to 1 Hz (20 cycles). 0.85 Hz, which `29_noise_sleep_pulsed` uses, would need 70 s for 60 cycles — a quarter of the whole suite's budget for one rate. |
| **≥60 flicker cycles in every block** | Honoured everywhere except: `t02` B4 lane B (2 Hz, 20 cycles), `t02` B5 lane B (1 Hz, 20 cycles), `t04` B4/B5 lane A (4 Hz, 48 cycles) and lane B (3 Hz, 36 cycles). All four are graded as **period or envelope-shape** measurements rather than spectra, which is what those cycle counts support. |

---

## `ledc_format.md` vs `config_parser.c` — ten disagreements found

The parser is authoritative. All nine are reflected in the sessions' comments
and in the expectations above.

| # | `ledc_format.md` says | the code does | severity |
|---|---|---|---|
| 1 | `waveType`: "`0` sine `1` square `2` triangle `3` saw `4` white `5` pink `6` brown" | **0..7**. `AUDIO_WAVE_COUNT` is 8 and `config_parser.c:1655` accepts `wt < AUDIO_WAVE_COUNT`; **wave 7 = `AUDIO_WAVE_EEG_CONTOUR`**, a non-geometric single-cycle LUT carrier ("replicated EEG waveform", Monroe US 5,213,562). The doc omits a whole wave type. | **doc missing a feature** |
| 2 | `ch` (audio): "**1..16** (or omitted)" | **0..15** (`config_parser.c:1636`, `NUM_AUDIO_CHANNELS` 16). Channel **0** is real and is the one an omitted column selects; 16 is out of range and clamped to 0. Separately, `/api/state` reports audio channels **1-based**, so the doc's numbering matches neither the file nor the API consistently. | **doc wrong, off-by-one both ends** |
| 3 | `freqR` is listed among the "compound cells — support interpolation prefixes" | **plain `atof` only** (`config_parser.c:1648-1651`). `atof(">250")` is 0.0, which forces the channel to MONO and silently destroys the binaural detune. The linter raises `LEDC_FREQR_PREFIX` at **error** severity precisely because the doc invites this. | **doc wrong, actively harmful** |
| 4 | `jitter`: "slow anti-habituation wander of the pulse rate (LED flicker / **audio mod**)" | on the audio side it is **neither** per-line **nor** a mod-rate wander. `audio_generator_set_beat_jitter(amp, period)` is **global** (no channel argument) and the offset is only ever added to `current_freq_r`, gated on `params.frequency_r > 0` — so it moves the **binaural detune** of every binaural channel and never touches the isochronic mod rate. The LED side *is* per-mask (`led_matrix_set_jitter_masked`). | **doc wrong on scope and on target** |
| 5 | `jitter`: "`<amp>` or `<amp>:<period_ms>` … default period 45000" | a period **below 1000 ms is silently ignored** on both sides (`audio_generator.c:236`, `led_matrix_example.c:1701`) — the previous period stays in force rather than the value you wrote. The LED side additionally **clamps amp to 5 Hz**. Neither limit is documented. | **undocumented limits** |
| 6 | `attack`: "Raised-edge duration … (click-safe ≥2 ms; default 3)" | the LED setter **caps it at 60 ms** (`led_matrix_example.c:1497`) — and the *sweep* path does not, so a ramped attack can exceed a stepped one. No upper bound is documented. | **undocumented cap, path-dependent** |
| 7 | `env`: "Unipolar gates apply `×((1−depth)+depth·g)`" | `depth` is **hardcoded 0.1** (`config_parser.c:2376`) with no `.ledc` field and no mention in the doc. Every gate is a 10 % envelope, not an on/off. Readers reasonably expect a deep isochronic pulse and get sidebands 30 dB down. | **doc omits the one number that sets the effect size** |
| 8 | `bright` is described as a value field like any other; the brightness table is the only hint | `freq>100` / `duty>100` / `bright>100` are rejected **only on the no-sweep path** (`led_matrix_example.c:1314-1325`, `:1588-1594`). `led_matrix_start_sweep_masked` (`:1920-1939`) validates nothing, so an out-of-range **ramped** entry executes. The doc states no range at all for `freq` and does not mention the asymmetry. | **doc silent on a path-dependent range check** |
| 9 | "The `-` sentinel — 'leave unchanged' … Any field may be `-`" and "Trailing fields left off entirely = default (equivalent to `-` at channel start)" | for audio `waveType` an **absent** trailing column is **not** equivalent to `-`: `gen_params.wave_type` is filled from the parsed value with no `AUD_SET_WAVE` check and latched unconditionally, so any later entry omitting the column **resets the channel to sine**. 21 occurrences across the test files in `sessions/library/`; `LEDC_WAVE_RESET` exists to catch it. Known and deliberately unfixed. | **doc wrong for one column** |

| 10 | `env`: only `3 trapezoid` is annotated "uses `duty`+`attack`", which reads as if the others might honour `duty` too | **`duty` is ignored by `env 1` (sine) and `env 2` (triangle) on BOTH paths.** `iso_gate` returns `0.5-0.5*cos` and a fixed symmetric `(ph<0.5)?2ph:2-2ph` with no reference to `duty` (`audio_generator.c:175-205`); `led_carrier_level_iram` likewise reads `duty_q16` only in its TRAPEZOID branch (`led_matrix_example.c:681-700`). So `env 1` or `env 2` plus a `duty` column — or a `duty` **modulation** — is a silent no-op, and `env 2`'s on-fraction is always exactly 50 %. Every duty test in this suite therefore uses `env 0` or `env 3`. | **doc misleads by omission** |

Two further notes that are not doc-vs-parser conflicts but are not written down
anywhere either:

* **the flicker ISR tick is not monotonic within a session.** The gptimer is
  deleted when no channel is left running and the next activation re-derives the
  tick from the 1000 Hz floor, so the standard sync marker (40 Hz burst then
  `freq 0` on every channel) *resets* it. `92_verify_7hz.ledc`'s header still
  says "the tick is never lowered within a session", which is true only while at
  least one channel stays active. This is why every file here carries a tick
  primer.
* **whether the phase-locked cycle origin removes the rate quantisation is
  still open.** `92_verify_7hz.ledc` predicts "all five should now read exact"
  but records only the pre-fix measurements; the device model in
  `tools/avmeasure` still predicts the quantised rate. `t02` B1 decides it, at
  7 / 9 / 11 Hz on a 100 µs grid, where the two predictions differ by
  300–999 ppm.

---

## Lint status

| file | runtime | lint | findings |
|---|---|---|---|
| `t01_avsync.ledc` | 56 s | clean (exit 0) | 1 info: `LEDC_MASK_HIGH_BITS` (tick primer, by design) |
| `t02_led_rate_duty.ledc` | 63 s | clean | 1 info, as above |
| `t03_led_bright_rgb.ledc` | 71 s | clean | 1 info, as above |
| `t04_led_env_attack.ledc` | 55 s | clean | 1 info, as above |
| `t05_led_interp.ledc` | 57 s | clean | 1 info, as above |
| `t06_led_phase_jitter.ledc` | 68 s | clean | 1 info, as above |
| `t07_audio_carrier.ledc` | 71 s | clean | 1 info, as above |
| `t08_audio_pulse.ledc` | 61 s | clean | 1 info, as above |
| `t09_binaural.ledc` | 53 s | clean | 1 info, as above |
| `t10_audio_interp.ledc` | 57 s | clean | 1 info, as above |
| `t11_audio_multichannel.ledc` | 35 s | clean | 1 info, as above |
| `t12_format_edges.ledc` | 51 s | clean (also with `--keep-speech`) | 1 info, as above |
| `t13_rejects.ledc` | 41 s | **exit 1 BY DESIGN** | 5 errors, 4 warnings, 2 infos — the exact list in the t13 table |
| `t14_batch_cap.ledc` | 13 s | **exit 1 BY DESIGN** | 1 error (`LEDC_BATCH_CAP`) + 1 info |
| **total** | **12 min 32 s** | | |

`LEDC_MASK_HIGH_BITS` is an **info**, so `lint` still exits 0 on the twelve
clean files. It is the unavoidable cost of the tick primer: there is no way to
hold a channel active without lighting something, except on a channel that
lights nothing, and the linter cannot know that `mask 16` is deliberate.

Every file is also under the 100-entry parse cap (largest: `t14` at 60 lines,
`t11` at 56) and under the 50-per-timestamp batch cap, except `t14`, where
exceeding it is the test.
