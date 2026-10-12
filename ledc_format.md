# `.ledc` / `.led` Format — Authoritative Spec (freeesp32_ave)

> The authoritative parsers are `main/config_parser.c` (device) and
> `web/src/js/gen/parse.js` (browser). **This file is derived from them, not the
> other way round** — revision v3 was written by reading the C line by line after
> `sessions/test/README.md` found ten places where v2 contradicted the code.
> Where the two parsers disagree, the device wins and it is a browser bug.
>
> Revision **v3 (2026-10-12)**. Changes from v2: ten corrections (`waveType`
> 0..7, `ch` 0..15, audio `jitter` scope and target, jitter/attack limits, `duty`
> ignored by env 1/2, the depth that was hardcoded, the path-dependent range
> checks) plus four features marked **NEW v3** below: compound `freqR`, absent
> trailing column = leave unchanged, range guards on the sweep path, and the new
> `depth` column. Anything this file cannot source to a function is marked
> **UNVERIFIED**.
>
> **v3.1 (2026-10-12)**: the LED `freq` column has **three** regimes, not two —
> `0` is a **DC channel** (steady on), and only a **negative** rate stops a mask.
> v3 and earlier documented `<= 0` as the stop, which matched neither the
> firmware nor the 19 shipped sessions that hold a lamp at 0 Hz. See "The three
> rate regimes".

## Model: two layers per channel

Every channel has a **carrier** (what you see/hear) and a **pulse** (the
entrainment rhythm). LED and audio share the *pulse* concepts but have different
*carriers*:

| Layer | LED expresses as | Audio expresses as |
|---|---|---|
| **Carrier** | `R G B` (colour) + `bright` | `freq` `freqR` `waveType` (L pitch, R pitch→binaural, timbre) |
| **Pulse** | `freq`(flicker Hz) `duty` `env` `phase` `attack` `jitter` | `mod`(Hz) `duty` `env` `phase` `attack` `jitter` `depth` |

Note the historical quirk: the entrainment *rate* is `freq` (col 2) on an LED
line but `mod` (col 6) on an audio line — `freq` on an audio line is the carrier
pitch. That is why the two line types keep separate carrier fields but share the
pulse fields.

One asymmetry worth internalising: **an LED pulse is always full depth** (the
envelope goes to 0 between flashes by construction), while an **audio pulse has
a depth** — how far down the gate pulls the carrier. That is the `depth` column,
new in v3; see "depth".

## Line types

```
# comment                                                    (also: inline, from the first '#')
<blank>
<time> <freq> <duty> <bright> <R> <G> <B> <mask> [env] [phase] [attack] [jitter]            # LED, 8..12 tokens
<time> <freq> <duty> <bright> <mask>                                                        # LED legacy, exactly 5
A <time> <freq> <pan> <vol> <mod> [ch] [freqR] [waveType] [duty] [env] [phase] [attack] [jitter] [depth]
BG <url> <pan> <loudness>                                                                   # background audio
S <time> <voice> <volume> "<text>"                                                          # speech
```

Token counts, from `parse_led_line` / `parse_audio_line`:

| line | accepted | rejected |
|---|---|---|
| LED | exactly **5**, or **8..12** | 6, 7, and >12 — logged and skipped |
| audio (after `A`) | **5 or more**; columns past the 14th are ignored | fewer than 5 |

`A` **must be its own token.** `parse_line` tests only the first character and
then hands `tokens + 1` to the audio parser, so `A1500 200 0 50 0` discards
`A1500` *entirely*: the line executes at t=200 ms with `freq` 0, `pan` 50, and
every field shifted left by one. The linter flags this as `LEDC_AUDIO_GLUED_TIME`
(warning). Lower case `a`/`s`/`bg` work. `S` is recognised only as a *one
character* token; anything else that is not `A`/`a`/`S`/`s`/`BG` falls through to
the LED parser.

Other parse-time limits:

| limit | value | source |
|---|---|---|
| line length | 256 bytes, longer lines **truncated** (silently changes the token count) | `CONFIG_PARSER_MAX_LINE_LENGTH` |
| tokens per line | 16; extras dropped | `parse_line`, `tokens[16]` |
| entries per file | 100 | `CONFIG_PARSER_MAX_ENTRIES` |
| entries per timestamp | 50 (the rest of that batch never fires) | `MAX_BATCH_SIZE`, `config_parser_execute_timeline` |

Entries are **stable-sorted by time** at parse (`config_parser_parse_content`),
so authoring order within one timestamp is preserved but grouping by layer is
safe. Each timestamp dispatches in three passes — **all audio, then all LED,
then everything else** — so audio channels in one batch start phase-coherent.

`BG` is a session-level annotation, not a timeline entry: it is intercepted
before `parse_line`, never occupies an entry slot, and **last one wins** (with a
warning). Schemes accepted: `http://`, `https://`, `sdcard://`, `push://`.
`pan` is clamped to ±100 and `loudness` to 0..100 (`parse_bg_line`).

`S` (speech) is **not browser-only** — v2 said it was. The device parses it
always and *plays* it when the firmware is built with `CONFIG_BG_SDCARD_ENABLED`
(set in `sdkconfig.defaults.esp32s3`); without that option the entry parses,
occupies a slot, and does nothing. `voice` defaults to `default` and `volume` to
80 if unparsable; the text is read from the raw line between the first and the
**last** `"`, so inner quotes survive and a trailing comment is ignored.

## Column reference — LED line

| # | field | kind | accepted | default / absent | notes |
|---|---|---|---|---|---|
| 0 | `time` | integer ms | `atol`, no range check | — | not `-`-able (a `-` here reads as 0) |
| 1 | `freq` | compound | **0 .. 100.0 Hz** | live value | `0` is a **DC channel** (steady on, no flicker); **`< 0` means STOP FLICKER** on the mask |
| 2 | `duty` | compound | 0..100 % | live value | cast to `uint8_t` at parse — see "ranges" |
| 3 | `bright` | compound | 0..100 % | live value | ditto |
| 4,5,6 | `R G B` | compound | 0..255, **clamped** with a warning | live value | `clamp_u8_field` |
| 7 | `mask` | selector | 1..255, OR'd bits; bit 0 = LED1 | — | `0` **rejects the line**; `-` is `atoi("-")` = 0 → also rejects it |
| 8 | `env` | enum | 0..3; anything else → **0** | leave unchanged | no interp; a glyph is parsed and dropped |
| 9 | `phase` | compound | degrees, wrapped to 0..359 | leave unchanged | |
| 10 | `attack` | compound | ms, negatives → 0, **capped at 60 ms** | leave unchanged | cap is path-dependent — see "attack" |
| 11 | `jitter` | `amp` or `amp:period_ms` | amp **capped 5 Hz**; period **ignored below 1000 ms** | leave unchanged | period default 45000 ms |

The legacy 5-token line is `time freq duty bright mask` and sets RGB to full
white explicitly. It cannot carry any pulse column.

### The three rate regimes — `freq > 0`, `freq == 0`, `freq < 0`

The `freq` column has **three** meanings, and the middle one is the easy mistake:

| `freq` | meaning | what the LEDs do |
|---|---|---|
| `> 0` | flicker at that rate | square/env-shaped pulse train |
| **`== 0`** | **DC channel** | **steady on** at the row's `bright`/RGB — no flicker |
| `< 0` | **stop** the mask | channel goes inactive; the timer is torn down if no channel is left |

A DC channel is a real feature, not a degenerate case: 19 shipped sessions use it
(`05_focus_smr` holds its lamp at 0 Hz for the whole run, `31_ganzfeld_amber` is a
Ganzfeld field). The ISR has a dedicated branch for it
(`led_matrix_example.c:824`) that still evaluates brightness and colour ramps and
modulations every tick, so a DC channel can breathe and fade — it just never
gates off.

Consequences worth internalising:

- **To turn a channel OFF, write a negative rate** — `-1` is conventional. A row
  of `0` leaves the channel *active*, which keeps the flicker gptimer alive and
  therefore keeps the ISR tick pinned where it was (see "Path-dependent
  behaviour" #5). The sync-marker recipe depends on the teardown, so its stop row
  must be `freq -1`.
- `bright 0` with `freq 0` is dark but **still active**. That is fine as a final
  row (the end-of-timeline handler stops everything anyway) but not as a
  mid-session off switch.
- A rate **ramp that crosses zero** is unrenderable below it. `>` interpolates
  linearly toward the next entry's rate, so a descending glide whose target is a
  negative stop row sweeps the demanded rate down through 0 and into negative
  territory. Pin the ramp with a plain row at its intended endpoint before
  stopping the channel.

## Column reference — audio line (after the `A`)

| # | field | kind | accepted | default / absent | notes |
|---|---|---|---|---|---|
| 0 | `time` | integer ms | `atol` | — | |
| 1 | `freq` | compound | **not validated on any session path** | live value | left-ear carrier Hz; `0` does **not** stop the channel |
| 2 | `pan` | compound | nominally −100..+100, **not validated** | live value | **no effect on a binaural channel** (see `freqR`) |
| 3 | `vol` | compound | nominally 0..100, **not validated** | live value | `vol/100` → amplitude; `0` is how you silence a channel |
| 4 | `mod` | compound | pulse rate Hz; `0` = no pulse at all | live value | |
| 5 | `ch` | selector | **0..15**; out of range → clamped to **0** with a warning | **0** | `-` here also selects 0 |
| 6 | `freqR` | compound **(NEW v3)** | >0 .. 22050 Hz, else 0.0 = mono | live value | `>`/`*` ramp only; a periodic glyph warns and holds |
| 7 | `waveType` | enum | **0..7**; anything else → 0 | leave unchanged **(NEW v3)** | |
| 8 | `duty` | compound | 0..100 %, clamped at parse | leave unchanged | **ignored by env 1 and 2**; a stepped `0` is a no-op |
| 9 | `env` | enum | 0..4; anything else → **4** | leave unchanged | note the fallback is 4 here, 0 on LED |
| 10 | `phase` | compound | degrees, wrapped 0..359 | leave unchanged | per channel |
| 11 | `attack` | compound | ms, negatives → 0, **no upper cap** | leave unchanged | |
| 12 | `jitter` | `amp` or `amp:period_ms` | amp ≥ 0; period **ignored below 1000 ms** | leave unchanged | **GLOBAL**, and it moves the **binaural detune** |
| 13 | `depth` | compound **(NEW v3)** | 0..100 %, clamped at parse | **env-dependent** — see "depth" | `>`/`*` ramp only |

`ch` is **0-based and channel 0 is real** — it is exactly what an omitted column
selects. v2 said "1..16", which is wrong at both ends: 16 is out of range and is
clamped to 0, and the channel an omitted column picks is 0, not 1.

> **`/api/state` reports audio channels 1-BASED** (`state_handler` emits
> `"ch": i + 1`), so `.ledc` `ch 8` appears there as `ch9`. LED channels are also
> emitted 1-based, which matches the usual "mask bit 0 = LED1" convention, so
> only the audio numbering surprises people. This has already cost measurement
> time; when correlating a capture to a file, convert once and write it down.

### `waveType` — eight carriers, not seven

| `waveType` | carrier | measured (200 Hz, 10 s) |
|---|---|---|
| 0 | sine | h2..h5 all 0.000 |
| 1 | square | h3 0.334, h5 0.201 (theory .333 / .200) |
| 2 | triangle | h3 0.111, h5 0.040 (theory .111 / .040) |
| 3 | sawtooth | h2 0.501, h3 0.334, h4 0.250 |
| 4 | white noise | +0.05 dB/oct |
| 5 | pink noise | −3.04 dB/oct |
| 6 | brown noise | −6.07 dB/oct over 60 Hz–3 kHz |
| 7 | **EEG contour** | h1..h6 = 1.000 / 0.500 / 0.331 / 0.221 / 0.140 / 0.091 |

Source: `audio_wave_type_t` (`audio_generator.h`), measurements from
`reports/non_planned_reports/audio_features_measured_2026-10-12.md`.

**Wave 7 is `AUDIO_WAVE_EEG_CONTOUR`** — a non-geometric single-cycle LUT, the
"replicated EEG waveform" of Monroe US 5,213,562. v2 omitted it entirely. It is
a **carrier and needs a valid frequency**; it is *not* noise. The code is careful
about that in two places: the `is_noise` test that waives the frequency check is
a *range* test `WHITE..BROWN`, written that way specifically so 7 (numerically
above the noise types) falls outside it, and `carrier_scale` likewise leaves 7
unscaled by `AUDIO_NOISE_LEVEL_SCALE`, so an EEG channel is ~6 dB hotter than a
noise channel at the same `vol`. Its binaural beat comes from L/R detune exactly
like sine's, because both ears read the same table at their own phase.

Caveat: the frequency check that waiver belongs to lives in
`audio_generator_start_channel`, which no session path calls (see "ranges"), so
in practice an EEG or sine or square channel written with `freq 0` is not
rejected — the phase simply never advances and the channel emits a constant
(silence for sine, a full-scale DC offset for square, and for EEG whatever
`eeg_lut[0]` holds).

Noise channels (4,5,6) are the only wave types for which `freq 0` is meaningful.

## The `-` sentinel, and the absent-trailing-column rule (NEW v3)

```
0    40 25 80 0 128 255 1  1 0            # ch1: 40 Hz, sine env, duty 25, phase 0
0    40 25 80 255 160 40 2  1 180         # ch2: same, phase 180 → antiphase (invisible)
5000 -  -  -  -   -   -  1  -  -  - 0.3   # ch1: change ONLY jitter; keep everything else
```

**One rule, two spellings.** A field written `-`, and an optional trailing column
left off entirely, both mean **"leave this channel's value unchanged"**. The
parser records what the line actually set in `entry->present`; the executor
substitutes the channel's live value for every bit that is clear. A cleared bit
is never treated as 0.

That rule now holds for **every** optional trailing column. The one place it was
broken was audio `waveType`: `gen_params.wave_type` was filled from the parsed
(zero-initialised) value and latched unconditionally, so any later entry that
omitted the column **reset the channel to sine** — the `LEDC_WAVE_RESET` lint
exists for exactly that. v3 reads the live waveform back
(`audio_generator_get_wave_type_locked`) when `AUD_SET_WAVE` is clear.

Four caveats that are *not* sugar:

1. **`mask` and `ch` are selectors, not values.** v2's "any field may be `-`" is
   wrong for them. `-` in the LED `mask` column parses as 0 and the whole line is
   **rejected**; `-` in the audio `ch` column selects **channel 0**. There is no
   "current channel" to leave unchanged.
2. **At channel start there is no live value to keep.** For audio the
   substitution only happens when the channel is already active *and* the entry
   is on the timeline path; otherwise the parsed zero stands, so `-` on `freq` or
   `vol` in a channel's *first* entry means literally 0. For LED the live value
   is read from `led_matrix_get_snapshot`, and an inactive channel reports
   `freq 0` — so `-` on `freq` in the first entry resolves to 0, which is a
   **DC channel** (steady on, no flicker), *not* a stop (lint
   `LEDC_DASH_FREQ_STOPS`). Write a negative rate to stop a channel.
3. **LED `-` reads the lowest bit in the mask.** A multi-bit mask whose channels
   hold different live values collapses them all to the low bit's value for the
   `-` fields.
4. **A ramp target is read from the next entry's struct member, not its present
   bit** — for the *core* fields. So `>` pointing at an entry that writes `-` for
   that field ramps to **0**, not to "hold" (lint `LEDC_RAMP_TO_ZERO`). The pulse
   fields (`duty`/`phase`/`attack`/`freqR`/`depth`) do it correctly: they scan
   forward for the next entry that actually *sets* the field, and hold if there
   is none.

## Compound cells — ramps and modulations

A **compound cell** is a bare number, or a number with one prefix glyph:

| glyph | meaning | needs a next entry? |
|---|---|---|
| `>v` | linear ramp from `v` to the next same-channel entry's value | yes |
| `*v` | quadratic (ease) ramp, same rule | yes |
| `~v:e:p` | sine modulation, `v`↔`e` every `p` ms | no |
| `^v:e:p` | triangle | no |
| `/v:e:p` | saw up | no |
| `\v:e:p` | saw down | no |
| `_v:e:p` | square (half period at `v`, half at `e`) | no |

Missing `:e` → `e` = `v` (degenerate); missing `:p` → **1000 ms**
(`parse_mod_extras`). Compound cells are:

* LED: `freq duty bright R G B phase attack`
* audio: `freq pan vol mod duty phase attack` plus **`freqR` and `depth`** (v3)

Not compound: LED `mask`, audio `ch`, both `env`, both `jitter`, and `time`. A
glyph on an `env` column is parsed and silently discarded by design
(`parse_v2_enum`); on `mask`/`ch` it corrupts the value.

**`freqR` and `depth` take `>` and `*` only.** A periodic glyph on either is
accepted by the tokenizer but **not implemented**: the parser logs a warning,
drops the interp, and holds the start value. Everything else about them is a
normal compound cell.

Two semantics that catch people:

* **Animate-on-start.** The glyph lives on the entry where the ramp *begins*, and
  the target value comes from the *next* entry. `A 0 >200 ...` followed by
  `A 8000 400 ...` glides 200→400 Hz over 8 s.
* **A ramp on a running channel starts from the LIVE value, not from the
  literal in the file** (`audio_generator_start_sweep_locked`,
  `led_matrix_start_sweep_masked`). This is deliberate — starting from the
  literal snaps audibly when the previous ramp was interrupted — but it means a
  file whose literal disagrees with the live state is *lying to its reader*. The
  linter flags it as `LEDC_RAMP_START_SUBSTITUTED`. Write the live value.

`find_next_audio_for_bit` takes a `uint16_t` bit mask — it used to be `uint8_t`,
which truncated every channel ≥8 to 0, so **no ramp on channels 8..15 ever
ran**. Fixed; worth knowing when reading old measurements.

## Ranges, and which path enforces them (NEW v3)

The guards are not in the parser — they are in the engine, and until v3 they were
on only one of the two dispatch paths.

**LED.** `led_matrix_start_flicker_masked` and
`led_matrix_update_flicker_params_masked` (the "no-sweep" path, taken when the
entry has no `>`/`*` field) reject `freq > 100 Hz`, `duty > 100`, `bright > 100`.
`led_matrix_start_sweep_masked` validated **nothing**, so the identical values
executed unchecked when the entry carried a ramp. **v3 adds the same three
guards to the sweep path, checking BOTH endpoints of every ramp.**

Consequence to know: an out-of-range entry is **dropped, not clamped**. The
sweep install returns `ESP_ERR_INVALID_ARG`, the error is logged, and those
channels get neither the sweep nor a fallback flicker.

Also: LED `duty` and `bright` are cast to `uint8_t` *at parse time*, before any
guard sees them. 101..255 is caught by the guards; **above 255 the cast wraps**
(`300` → `44`) and the guard then sees an in-range value. Nothing warns. `R G B`
are safe — `clamp_u8_field` clamps and logs.

**Audio — there is effectively no range check at all.** The validating entry
points (`audio_generator_start_channel`, which rejects `freq <= 0` or
`> 22050`, `amplitude` outside 0..1, `pan` outside ±1) are the *non*-`_locked`
wrappers, and **no session path calls them**: the timeline batch executor and
`/api/patch` both hold the mutex and call `audio_generator_start_channel_locked`
/ `update_params_locked`, which bounds-check the **channel index only**. So, from
a `.ledc` file:

| you write | what happens |
|---|---|
| `vol 500` | amplitude 5.0. It *works* and is ~14 dB louder — the fixed 1/16 divisor means a single centre-panned sine needs roughly `vol 2200` to clip. Rejected by live control, accepted here. |
| `pan 200` | the equal-power law runs past 90°, so the **left output inverts polarity** (`cos` goes negative) while the right falls back toward 0.707. Keep pan inside ±100. |
| `freq 30000` | aliases. No warning. |
| `freq 0`, sine | silence, but the channel stays **active** — an audio line never stops a channel. Use `vol 0`. |
| `freq 0`, square | a constant +1.0 → **DC offset** on the output, because `raw_l` is `phase < 2^31 ? 1 : -1` and the phase never advances. |

This asymmetry (LED guarded, audio unguarded) is not a documented design
decision as far as this file can tell — flagged for a decision rather than
defended. The linter models the LED guards (`LEDC_LED_OUT_OF_RANGE`) and does not
yet model the audio gap.

## `env` — the pulse envelope

Set per entry, never ramped. Every behaviour, including each line's legacy, is an
explicit value:

| `env` | shape | uses `duty`? | uses `attack`? |
|---|---|---|---|
| 0 | **square** gate (unipolar) — LED legacy | **yes** | no |
| 1 | **sine** gate (true raised sine) | **no** | no |
| 2 | **triangle** gate | **no** | no |
| 3 | **trapezoid** gate | **yes** | **yes** |
| 4 | **sine tremolo**, bipolar `×(1+depth·sin)` — audio legacy | no | no |

Default when the column is absent is the line's legacy value: **LED → 0**,
**audio → 4**. Out-of-range falls back differently on the two paths — LED clamps
to **0**, audio to **4** (`parse_led_line`, `parse_audio_line`). LED has no
bipolar meaning, so `env 4` on an LED line is treated as square
(`led_matrix_set_carrier_masked` clamps `> 3` to square).

### `duty` is IGNORED by env 1 and env 2 — on both paths

`iso_gate()` returns `0.5 - 0.5·cos(2πph)` for env 1 and a fixed symmetric
`(ph<0.5) ? 2ph : 2−2ph` for env 2, **neither of which reads `duty`**;
`led_carrier_level_iram()` reads `duty_q16` only in its TRAPEZOID branch. So:

* a `duty` column alongside `env 1`/`env 2` is a **silent no-op**;
* a `duty` **modulation** (`~20:80:4000`) alongside them is also a silent no-op —
  the mod engine dutifully animates a value nothing reads;
* **env 2's on-fraction is always exactly 50 %** (the triangle is symmetric and
  spans the whole cycle);
* env 1 *measures* an apparent duty of 66.67 %, which is just the fraction of a
  raised sine above its own mid-level, not a setting being ignored.

**If `duty` matters, use env 0 or env 3.** Every duty test in `sessions/test`
does. Measured accuracy there: commanded 25 % / 75 % reads 25.08 % / 75.03 %,
gate rate 8.0107 Hz against a commanded 8.000.

### `env` shapes, measured

LED, optical, 10 Hz flicker, after the raised-sine fix (carried over from v2;
same provenance caveat as the brightness table):

| env | measured harmonics (h2/h3/h4) |
|---|---|
| 0 | 0.004 / 0.346 / 0.003 — textbook (theory 0 / 0.333 / 0) |
| 1 | 0.058 / 0.004 / 0.010 |
| 2 | 0.061 / 0.139 / 0.022 (theory 0 / 0.111 / 0) |
| 3 | 0.081 / 0.310 / 0.073 |

Audio, demodulated carrier envelope, 400 Hz gated at 8 Hz, duty 50:

| env | h2 before the fix | h2 after | h3 before | h3 after |
|---|---|---|---|---|
| 0 SQUARE | 0.001 | 0.000 | 0.333 | 0.332 |
| **1 SINE** | **0.251** | **0.002** | **0.112** | **0.001** |
| 2 TRIANGLE | 0.001 | 0.001 | 0.114 | 0.110 |
| 3 TRAPEZOID | 0.076 | 0.076 | 0.321 | 0.321 |
| 4 TREMOLO | 0.000 | 0.002 | 0.000 | 0.001 |

**Both sine envelopes are now TRUE raised sines**, not the `4x(1−x)` parabola
their comments used to advertise. The parabola looks like a similar hump but has
a derivative discontinuity at the cycle boundary, so its harmonics fell as 1/n²:
an 8 Hz alpha gate was simultaneously driving 16 Hz at a quarter of the
fundamental and 24 Hz at a ninth — beta content inside an alpha session. The LED
path measured 0.286 / 0.145 before its fix, the audio path 0.251 / 0.112, which
is how we know it was literally the same shape. Audio h2 is now 0.002.

Measurement note: the 5th harmonic of a 10 Hz flicker lands on 50 Hz, so
mains-hum pickup shows up there and is easy to mistake for real harmonic
content. Check whether h5 moves when you change the flicker rate.

## `depth` — audio pulse depth (NEW v3, 14th column)

```
A <time> <freq> <pan> <vol> <mod> [ch] [freqR] [waveType] [duty] [env] [phase] [attack] [jitter] [depth]
```

Compound cell, **0..100 percent**, appended last so every existing file keeps its
column positions. It is how far the gate pulls the carrier **down**:

```
unipolar gates (env 0..3):   sample *= (1 - depth) + depth * gate
bipolar tremolo (env 4):     sample *= 1 + depth * sin(mod_phase)
```

So on a gate, **100 = the carrier reaches silence between pulses** (a true
isochronic tone) and 0 = no modulation. On env 4 it is the classic tremolo index.

**Why this column exists.** `depth` used to be a hardcoded `0.1f` in
`config_parser.c` with no column and no mention in this file, shared by both
envelope families. Every "isochronic" gate therefore swung the amplitude only
between 0.9 and 1.0 of its carrier. By arithmetic that is an AM index of
0.05/0.95 ≈ 5.3 %; measured ≈5.8 % (figure from the `depth_pct` comment in
`config_parser.h` — this file could not locate the raw capture). Either way it is
a barely-perceptible tremolo rather than discrete pulses. It is the single
parameter that sets isochronic effect size, and it was invisible.

**Default when the column is absent depends on the envelope**, because the two
families want opposite things:

| `env` column | `depth` column | effective depth | why |
|---|---|---|---|
| absent | absent | **10 %** | legacy: `iso_depth` stays 0 and the generator falls back to `params.mod_depth` = 0.1, so pre-v3 files are byte-for-byte unchanged |
| present, 0..3 | absent | **100 %** | a unipolar gate is meant to be a discrete pulse |
| present, 4 | absent | **10 %** | legacy bipolar tremolo is meant to be a gentle wobble |
| any | present | the column | clamped 0..100 at parse |

Source: `execute_timeline_entry_ctx` (the `depth_arg` block) and
`audio_generator_set_iso_channel`.

Three things to plan for when you turn depth up:

* **Full depth is quieter.** A 50 %-duty gate at depth 100 is **−3.01 dB in mean
  power** against the same tone ungated — the carrier really is off half the
  time. Perceived loudness drops with it, so `vol` usually needs raising. (At the
  old 10 % depth the difference was ~0.2 dB, which is why nobody noticed.)
* **Full depth on a square gate makes clicks by construction** — a step
  discontinuity at every edge. The click-free route to full depth is **`env 3`
  (trapezoid) with `attack` 3..10 ms**, which rounds both edges; `iso_trapezoid_env`
  additionally clamps the edge to half the on-time, so a short gate degrades to a
  triangle rather than misbehaving.
* **`depth 0` means no modulation at all**, and so does ramping depth down to 0.
  This was briefly broken when the column was introduced: the generator used
  `iso_depth > 0` as its "a depth was set" test while initialising `iso_depth` to
  0 as "unset", so `depth 0` fell back to the hardcoded 0.1 and produced 10 %.
  The "unset" marker is `-1` now (`audio_generator.c`, `s_iso_depth`) and the
  test is `>= 0`. Measured after the fix: `depth 0` leaves the gate's line in the
  carrier envelope only +2.5 dB above its own neighbourhood — i.e. nothing —
  against +58.4 dB for a full gate.

## `jitter` — anti-habituation wander

One token, `<amp>` or `<amp>:<period_ms>`. `0` or absent = off. Amplitude is in
Hz, period is the full cycle, default 45000 ms. **The two line types do
genuinely different things**, which v2 got wrong:

| | LED | audio |
|---|---|---|
| scope | **per channel-mask** (`led_matrix_set_jitter_masked`) | **GLOBAL** (`audio_generator_set_beat_jitter` takes no channel) |
| what it moves | the **flicker rate** | the **binaural detune** — `current_freq_r = params.frequency_r + offset`, gated on `frequency_r > 0` |
| shape | bipolar triangle LFO | bipolar sine LFO |
| amp limit | **clamped to 5 Hz** | none |
| period limit | **< 1000 ms silently ignored** (previous value stays) | **< 1000 ms silently ignored** |

Three consequences on the audio side: the **last** audio line in the file that
carries a jitter column sets it for the *whole session*; a channel that sets no
jitter column still wanders if some other line turned it on; and a **mono**
channel never wanders at all, because the offset is only ever added to the right
carrier. It never touches the isochronic `mod` rate — if you want the pulse rate
to wander, modulate `mod` with `~`/`^` instead.

End a session by writing jitter explicitly back to `0`, or it leaks into whatever
runs next (the global is not reset between sessions).

A jittered LED channel is also excluded from the cycle-lattice re-anchor, by
design — there is no fixed lattice to snap to.

## `phase` is a LEAD, not a lag

`phase` advances the cycle: the firmware adds `phase × period / 360` to elapsed
time (`led_flicker_timer_callback`, "V-E2"; the audio side adds
`mod_phase_offset_q32` to the mod phase, which is the same sign convention), so
the edge arrives EARLIER, matching `sin(ωt + φ)` rather than "start this many
degrees later".

Measured on hardware at 10 Hz (100 ms period), timing LED-on edges:

| `phase` | if it were a lag | actually measured |
|---|---|---|
| 0   |  0 ms |  0.00 ms |
| 90  | 25 ms | 74.76 ms |
| 180 | 50 ms | 49.93 ms |
| 270 | 75 ms | 24.69 ms |

0 and 180 are symmetric under negation, so they are identical either way — and
across `sessions/library` the phase column takes only those two values (265
occurrences of 0, 210 of 180; re-counted for this revision).
That is why the convention has never mattered in practice, and why it is written
down here before someone reaches for 90. **To delay by N degrees, write
`360 − N`.**

Provenance: these four numbers are carried over from v2 and are independently
reproduced in `sessions/test/README.md` (t06); no report file in `reports/`
holds the raw capture, so they are corroborated but not re-derivable from this
tree. The *sign convention* itself is verified in
`led_flicker_timer_callback` (`eff_elapsed += off`).

Audio phase is per channel and stored as a fraction of the mod cycle, so it is
frequency-independent by construction and survives `mod` ramps. LED phase is
applied at use-time from the live period, so it stays a fixed phase through
frequency ramps. `180` on one channel of a complementary-colour pair sharing a
start time gives antiphase → luminance-flat "invisible" flicker (the LED carrier
shapes are luminance-linear, with no perceptual gamma, specifically so a
180°-antiphase pair of raised sines sums to a constant).

## `attack` — trapezoid edge width

Milliseconds. Only `env 3` reads it. Negatives become 0. Defaults to **3 ms** on
a fresh LED channel (`s_flicker_attack_ms`) and **3 ms** on a fresh audio channel
(`s_iso_attack_ms`). Click-safe is ≥2 ms.

**The upper cap is path-dependent, and v2 documented no cap at all:**

| path | cap |
|---|---|
| LED, stepped (`led_matrix_set_attack_masked`) | **60 ms** |
| LED, ramped (`led_matrix_start_sweep_masked`, and the ISR's per-cycle interpolation) | **not capped in the stored value** — a ramp can leave `attack_ms` above 60 |
| LED, rendered | the ISR caps the *effective* edge at 60 ms (`attack_us > 60000`) and `led_carrier_level_iram` further clamps it to half the on-time |
| audio (`audio_generator_set_iso_channel`, `AUDIO_PARAM_ISO_ATTACK`) | **no cap**; `iso_trapezoid_env` clamps the effective edge to half the on-time |

So a ramped LED attack can exceed a stepped one in the stored and reported state
while rendering identically above 60 ms. If you need a long edge, you are really
asking for `env 1` (sine).

## `bright` is linear only above ~40 % effective drive

Light output is proportional to `bright × RGB/255` (the "effective drive"), but
only in the upper part of the range. Measured optically, light per unit drive:

| effective drive | light / drive | vs linear |
|---|---|---|
| 12 | 0.135 | 68 % |
| 24 | 0.153 | 77 % |
| 36 | 0.175 | 88 % |
| 48–60 | 0.183–0.188 | 92–95 % |
| 102 | 0.200 | 100 % |
| 153–255 | 0.197–0.202 | 100 % |

Above drive ~100 it is linear to about ±1 %. Below that it rolls off, reaching
68 % of linear at drive 12. This is the LED's own behaviour at low PWM, not a
firmware scaling bug — but it is worth knowing because the therapeutic sessions
live in exactly that region. `17_nondual_awareness` breathes `~8:22` on blue
200, i.e. effective drive 16..44, so its brightness LFO is compressed at both
ends and compressed UNEVENLY — the dim end loses more than the bright end, so
the shape of the breathing is distorted, not just its depth.

If an accurate low-brightness ramp matters, raise the RGB value and lower
`bright` to hit the same light at a higher effective drive.

Validated as a device property rather than a sensor artifact: two sweeps at
different RGB (255 and 60) overlap in effective drive (48/51/60) and land on the
same curve there (0.183/0.186/0.188), so the measurement chain is linear across
the range.

Provenance: carried over from v2 and reproduced in `sessions/test/README.md`
(t03); same status as the phase table — corroborated, raw capture not in-tree.
The `17_nondual_awareness` figure was re-checked for this revision: its LED rows
are `~8:22:12000` on blue 200, giving effective drive 16..44.

## The mixing model — why `vol` is an absolute quantity

`audio_generator_fill_buffer` scales **every** channel by a fixed
`1 / NUM_AUDIO_CHANNELS` = **1/16 = −24.1 dB**, unconditionally — never by the
number of channels that happen to be active.

Think of **sixteen violins in a room**. Each has its own volume. Maximum
loudness is all sixteen playing at once. If fifteen stop, the remaining one does
not get louder. That is exactly the model: per-channel `vol` is an **absolute**
quantity, not a share of a mix, and it is a hard overflow guarantee that cannot
click when channels start or stop (the alternative, `min(1, 1/Σamp)`, bounded
nominal amplitudes but let instantaneous peaks distort).

The cost is level. A single centre-panned sine at `vol 100` with
`audio_max_volume` 100 peaks at **−27.1 dBFS** (measured int16 peak 1448):
`1.0 × 0.7071` for the equal-power pan law × `0.0625`. A binaural channel skips
the pan law entirely, so it peaks at −24.1 dBFS. Noise channels are 6 dB below
tones again (`AUDIO_NOISE_LEVEL_SCALE` 0.5), and pink noise at `vol 100`
measured −43.3 dBFS. About 4 of the 16 bits go unused, which is why brown
noise's HF tail sits near the quantisation floor.

This is **deliberate, not a bug**, and it is also *not currently adjustable from
a session*: `reports/non_planned_reports/audio_features_measured_2026-10-12.md`
proposes replacing the divisor with `1/max(1, n_active)` and deliberately does
not apply it, because every existing session would get up to 24 dB louder on a
head-worn device. Downstream of the divisor there is one more gain — the runtime
`audio_max_volume` setting (0–100 %) — and then a hard clamp to ±1.0 before the
int16 conversion.

## Binaural: two idioms, and `pan` does nothing in one of them

1. **One channel with `freqR` set.** The generator drives the left carrier at
   `freq` and the right at `freqR` and routes them straight to L and R —
   "binaural bypass". When `params.frequency_r > 0` *and* `!= frequency`, **the
   pan stage is skipped entirely**, so `pan` has no effect on such a channel.
   (The equal-power law would cross-mix the two carriers between ears and at any
   non-centre pan attenuate one to zero, collapsing the beat.) Balance the ears
   with two channels instead, or with `vol`.
2. **Two channels hard-panned −100 / +100** with slightly different carriers.
   Both go through the normal pan law, so each ear gets its own carrier and
   >40 dB of separation from the other's.

Either way the beat **is** the frequency difference between the two spectral
lines; nothing in the recorded signal beats, so never look for a beat line in a
mono sum.

`freqR` is **absolute**, not a detune offset. Ramping it (`>`, v3) is the direct
way to glide a beat and now actually works — a bare `atof(">250")` used to
evaluate to 0.0 and silently collapse the channel to mono, which for a binaural
device is the worst possible failure mode (lint `LEDC_FREQR_PREFIX`, error). The
equivalent older idiom — ramp the LEFT carrier and leave `freqR` pinned — still
works and is what the shipped library does; the beat then moves because `freqR`
is absolute.

Because `freqR` is absolute and "absent" means "keep the live right carrier",
**an entry that moves `freq` and omits `freqR` can turn a mono channel into an
unintended binaural pair** — see "Path-dependent behaviour", item 7. Restate
`freqR` (`0` for mono) whenever you move a carrier.

## Path-dependent behaviour worth knowing

Collected because every one of them has cost somebody a measurement session.
These are observations about the code, not format rules.

1. **A fresh LED start re-latches `env`, `attack` and `jitter` from module-global
   defaults.** `led_matrix_start_flicker_masked` writes
   `s->carrier_waveform = s_flicker_carrier` (square), `s->attack_ms = 3`, and
   the jitter globals *unconditionally*, and the timeline calls it for any mask
   that is not **already fully active** (`led_matrix_is_flickering_masked` is ALL,
   not ANY). So a row that restarts a channel after a `freq < 0` stop, or a
   multi-bit mask where one bit is idle, **resets env/attack/jitter to the
   defaults on every bit in the mask** unless the row restates them — defeating
   "absent = leave unchanged" for those three columns. `phase` survives (it is
   not re-latched), and the *sweep* path does not re-latch at all, so whether the
   reset happens depends on whether the row carries a ramp. Restate `env` on
   every row that restarts a channel. **Not yet covered by a lint.**
2. **A fresh channel started via the sweep path gets `attack` 0, not 3 ms** —
   `led_matrix_start_sweep_masked` never reads `s_flicker_attack_ms`, and the
   static initialiser leaves `attack_ms` at 0. Only matters for `env 3`.
3. **A stepped audio `duty 0` is a no-op** — `audio_generator_set_iso_channel`
   only writes `duty_pct > 0`, so the previous duty stays (lint
   `LEDC_AUDIO_DUTY_ZERO`, info). A *ramped* or modulated duty can reach 0,
   because the per-block evaluation uses `iso_duty_from_pct`, which allows it.
4. **`env`/`phase`/`attack`/`jitter` are applied on a stop (`freq < 0`) LED row
   too** —
   the pulse-field writes sit outside the stop/start branch, so a stop row can
   change the pulse shape. Observable only at the next start (and then subject
   to 1 above).
5. **The flicker ISR tick is not monotonic across a session.** The gptimer is
   deleted when no channel is left active and the next activation re-derives the
   tick from the 1000 Hz floor — so the standard sync marker (40 Hz burst, then
   **`freq -1`** everywhere) *resets* it, and the first post-marker rate then fixes
   the grid for everything after it. This is why every file in `sessions/test`
   carries a "tick primer" row that keeps one invisible channel alive.
6. **The first batch is hoisted.** It fires at wall-clock 0 whatever its
   timestamp — benign for LED (anchored to the demanded time) and *not* benign
   for audio (started at dispatch).
7. **Changing `freq` without restating `freqR` can create an accidental binaural
   beat.** "Absent `freqR`" resolves to the channel's *live right carrier*
   (`audio_generator_get_current_freq_r_locked`), and the live right carrier of a
   mono channel equals its left one. So a later entry that moves `freq` and omits
   the `freqR` column leaves the right ear at the OLD pitch:
   `params.frequency_r > 0 && != frequency` then satisfies the binaural-bypass
   test, so the channel plays two different carriers, beats at the size of the
   carrier change, and **silently ignores `pan`**. Scanning `sessions/` for it
   finds 29 audibly-affected lines across four files — all of them the
   `test*.ledc` scratch sessions; every numbered therapeutic session writes the
   8-token form with an explicit `freqR`, which is why this has stayed hidden.
   **Restate `freqR` (`0` for mono) on any entry that moves an audio carrier.**
   Not covered by a lint.

## Backward-compatibility contract

* 5-token and 8-token LED lines, and 5..8-token audio lines, parse exactly as
  before. `-` and every trailing column are purely additive.
* Pre-v3 files are unaffected by the v3 changes: `depth` is a new trailing
  column whose absence reproduces the old hardcoded 10 %; `freqR` only gains
  behaviour for prefixes that previously evaluated to 0.0 (i.e. were broken);
  the absent-`waveType` fix only stops a reset that nothing wanted; the new
  sweep-path range guards only reject values the same entry would already have
  been refused for without a ramp.
* The `sessions/library/*.ledc` set MUST still serialize byte-identically
  (regression gate). Serialization re-emits `-` for fields the source marked
  unchanged, and omits a never-set trailing field.
* `web/src/js/gen/parse.js` mirrors the device parser and must be updated in
  lockstep. As of this revision its `freqR` is still a bare `parseFloat` and it
  has no `depth` column — **the browser is behind the device on both v3 audio
  changes.**
