# `.ledc` / `.led` Format — Authoritative Spec (freeesp32_ave)

> This supersedes the stale parent `ledc_spec.md` (which still documents the removed
> `W` column). The authoritative parsers are `main/config_parser.c` (device) and
> `web/src/js/gen/parse.js` (browser) — they MUST agree with this doc.
> Revision **v2 (2026-07-03)** adds the shared "pulse" fields + the `-` sentinel.

## Model: two layers per channel

Every channel has a **carrier** (what you see/hear) and a **pulse** (the entrainment
rhythm). LED and audio share the *pulse* concepts but have different *carriers*:

| Layer | LED expresses as | Audio expresses as |
|---|---|---|
| **Carrier** | `R G B` (colour) | `freq` `freqR` `waveType` (a tone: L pitch, R pitch→binaural, timbre) |
| **Pulse** | `freq`(flicker Hz) `duty` `env` `phase` `attack` `jitter` | `mod`(Hz) `duty` `env` `phase` `attack` `jitter` |

Note the historical quirk: the entrainment *rate* is `freq` (col 2) on an LED line but
`mod` (col 6) on an audio line — `freq` on an audio line is the carrier pitch. That's
why the two line types keep separate carrier fields but share the pulse fields.

## Line types

```
# comment
<blank>
<time> <freq> <duty> <bright> <R> <G> <B> <mask> [env] [phase] [attack] [jitter]     # LED
A <time> <freq> <pan> <vol> <mod> [ch] [freqR] [waveType] [duty] [env] [phase] [attack] [jitter]  # AUDIO
BG <url> <pan> <loudness>                                                             # background audio
S <time> <voice> <volume> "<text>"                                                   # speech (browser-only)
```

Legacy LED (5-field) `<time> <freq> <duty> <bright> <mask>` still parses (RGB defaults
white). Audio still accepts 5..8 core tokens before the new pulse fields.

## Fields

### `bright` is linear only above ~40% effective drive

Light output is proportional to `bright x RGB/255` (the "effective drive"), but
only in the upper part of the range. Measured optically, light per unit drive:

| effective drive | light / drive | vs linear |
|---|---|---|
| 12 | 0.135 | 68% |
| 24 | 0.153 | 77% |
| 36 | 0.175 | 88% |
| 48-60 | 0.183-0.188 | 92-95% |
| 102 | 0.200 | 100% |
| 153-255 | 0.197-0.202 | 100% |

Above drive ~100 it is linear to about +/-1%. Below that it rolls off, reaching
68% of linear at drive 12. This is the LED's own behaviour at low PWM, not a
firmware scaling bug -- but it is worth knowing because the therapeutic sessions
live in exactly that region. `17_nondual_awareness` breathes `~8:22` on blue
200, i.e. effective drive 16..44, so its brightness LFO is compressed at both
ends and compressed UNEVENLY -- the dim end loses more than the bright end, so
the shape of the breathing is distorted, not just its depth.

If an accurate low-brightness ramp matters, raise the RGB value and lower
`bright` to hit the same light at a higher effective drive.

Validated as a device property rather than a sensor artifact: two sweeps at
different RGB (255 and 60) overlap in effective drive (48/51/60) and land on
the same curve there (0.183/0.186/0.188), so the measurement chain is linear
across the range.

### `phase` is a LEAD, not a lag

`phase` advances the cycle: the firmware adds `phase x period / 360` to elapsed
time (`led_matrix_example.c`, "V-E2"), so the edge arrives EARLIER, matching the
`sin(wt + phi)` convention rather than "start this many degrees later".

Measured on hardware at 10 Hz (100 ms period), timing LED-on edges:

| `phase` | if it were a lag | actually measured |
|---|---|---|
| 0   |  0 ms |  0.00 ms |
| 90  | 25 ms | 74.76 ms |
| 180 | 50 ms | 49.93 ms |
| 270 | 75 ms | 24.69 ms |

0 and 180 are symmetric under negation, so they are identical either way -- and
every shipped session uses only those two values (244 uses of 0, 189 of 180).
That is why the convention has never mattered in practice, and why it is written
down here before someone reaches for 90.

To delay by N degrees, write `360 - N`.

### Value fields (compound cells — support interpolation prefixes)
`freq duty bright R G B` (LED) and `freq pan vol mod freqR duty phase attack` (audio)
and LED `phase attack` are **compound cells**: a bare number, or one of the existing
prefixes:
- `>v` linear ramp to the next same-channel entry's value
- `*v` quadratic (ease-in-out) ramp
- `~v:e:p` / `^` / `/` / `\` / `_` periodic modulation (sine/triangle/sawup/sawdn/square),
  oscillating `v`↔`e` every `p` ms.

So **"modulate pulse duty 10→80% over 5 s"** is just `~10:80:5000` in the `duty` field.

### Enum / discrete fields
- `mask` (LED): OR'd 8-bit channel bitmask (1..255), `0` rejected.
- `ch` (audio): 1..16 (or omitted).
- `waveType` (audio carrier timbre): `0` sine `1` square `2` triangle `3` saw `4` white
  `5` pink `6` brown.
- **`env`** (NEW, pulse envelope shape — both lines): set per entry (not ramped), like
  `waveType`. Every behaviour — including each line's legacy — is an explicit value:
  - `0` **square** gate (unipolar) — **LED legacy** (hard on/off)
  - `1` **sine** gate (unipolar)
  - `2` **triangle** gate (unipolar)
  - `3` **trapezoid** gate (unipolar, uses `duty`+`attack`)
  - `4` **sine tremolo** (bipolar `×(1+depth·sin)`, amplifies above unity) — **audio legacy**

  Unipolar gates apply `×((1−depth)+depth·g)`, `g∈[0,1]` (pulse *down* from full — modern
  isochronic). The **default** (field absent) is the line's legacy value: **LED→`0`**,
  **audio→`4`** — both explicitly selectable, so "absent" is pure sugar for the default and
  existing sessions round-trip byte-identically. (LED has no bipolar meaning — `env=4` on an
  LED line is treated as square. Audio may use any of 0..4.)

### `jitter` (NEW, both lines)
One token: `<amp>` or `<amp>:<period_ms>` — slow anti-habituation wander of the pulse
rate (LED flicker / audio mod), amplitude in Hz, default period 45000. `0` or absent = off.
On an audio **binaural** channel it wanders the beat (`freqR` offset).

### `phase` (NEW, both lines) — compound cell, degrees 0..359
Pulse phase offset. `180` on one channel of a complementary-colour pair (sharing the
same start time) = antiphase → luminance-flat "invisible" flicker. Applied from the
live period, so it stays a fixed phase through frequency ramps.

### `attack` (NEW, both lines) — compound cell, milliseconds
Raised-edge duration of the `env=trapezoid` gate (click-safe ≥2 ms; default 3). Ignored
for square/sine/triangle.

## The `-` sentinel — "leave unchanged"

Any field may be `-` meaning **keep the channel's current value for that field** (at
channel start, `-` = the field default). This lets a mid-timeline entry change one field
without restating the rest, and removes positional-placeholder noise for the new trailing
fields.

```
0    40 25 80 0 128 255 1  1 0            # ch1: 40 Hz, sine(env=1), duty 25, phase 0
0    40 25 80 255 160 40 2  1 180         # ch2: same, phase 180 → antiphase (invisible)
5000 -  -  -  -   -   -  1  -  -  - 0.3   # ch1: change ONLY jitter to 0.3 Hz; keep all else
```

Rules:
- A **bare number** sets the value (exactly as today) — so **all existing sessions parse
  identically**; `-` is purely additive.
- `-` is allowed in any positional field, including existing ones (`freq duty bright R G B`
  …). Trailing fields left off entirely = default (equivalent to `-` at channel start).
- Serialization re-emits `-` for fields the source marked unchanged; a never-set new field
  is omitted (trailing) so default rows round-trip byte-identically.

## Backward compatibility contract
- 5-field and 8-field LED lines, and 5–8-token audio lines, parse exactly as before.
- New pulse fields default to the legacy behaviour: `env=0` (square/sine-tremolo),
  `phase=0`, `attack=3`, `jitter=off`, audio `duty=50`.
- The 25 `sessions/library/*.ledc` MUST serialize byte-identically (regression gate).
