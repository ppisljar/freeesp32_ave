"""Parse a `.ledc` and build the EXPECTED timeline -- the instrument's ground truth.

THIS IS THE HIGHEST-STAKES FILE IN THE TOOL.
If the expectation is wrong, the comparator confidently reports a bug that is
not there, and someone spends a day chasing it. So every non-obvious behaviour
below carries a file:line citation into the firmware, and where the firmware
has a *bug* we reproduce the bug (with a comment saying so) rather than the
documented intent -- because the device plays the bug, not the documentation.

Reproduced faithfully, including the warts:
  * tokenisation (255-byte line truncation, inline-comment stripping, 16-token
    cap, `A` dispatch that DISCARDS token 0)
  * the `-` sentinel and `present` bits, with live-value substitution
  * animate-on-start ramps: prefix on THIS entry, target from the NEXT entry
  * the ramp-to-zero trap: core-field ramps read the next entry's struct member
    with no present-bit check, so a `-` next entry ramps to 0
  * pulse-field ramps instead scan forward for the next entry that SETS the
    field (the correct rule the core fields do not follow)
  * audio channels 8..15 never ramp (1u<<8 truncated to uint8_t == 0)
  * wave_type is latched unconditionally, so a later entry omitting it resets
    a noise/square channel to sine
  * stable sort by time, three-pass per-timestamp dispatch (audio, LED, other)
  * the 100-entry parse cap and the 50-per-timestamp execution cap
  * first-batch hoisting: the first batch fires at wall-clock 0 whatever its
    timestamp -- which is benign for LED (anchored to the demanded time) and
    NOT benign for audio (started at dispatch)
  * LED freq<=0 means STOP FLICKER, not DC-on; LED freq>100 Hz is rejected
    outright so the entry does nothing at all

Deliberately NOT modelled (and stated in README LIMITATIONS):
  * LED swept values only being recomputed at flicker cycle boundaries (we
    model the continuous value and widen the comparison tolerance instead)
  * audio pulse-shape params updating on a 23.22 ms staircase (same treatment)
  * the EEG-contour waveform's internal shape
"""

from __future__ import annotations

import bisect
import math
import os
from dataclasses import dataclass, field, replace
from typing import Iterable

import numpy as np

import devicemodel as dm
from timeline import (
    DEFAULT_GRID_HZ,
    ExpectedAudioChannel,
    ExpectedLight,
    ExpectedMix,
    Expectation,
    Finding,
    ScheduledEntry,
    make_grid,
)

# ---------------------------------------------------------------------------
# Interpolation / modulation enums -- mirror config_parser.h:44-56
# ---------------------------------------------------------------------------

INTERP_NONE = 0
INTERP_LINEAR = 1
INTERP_QUADRATIC = 2
INTERP_TRIANGLE = 3
INTERP_SINE = 4
INTERP_SAW_UP = 5
INTERP_SAW_DOWN = 6
INTERP_SQUARE = 7

_PREFIX_MAP = {
    ">": INTERP_LINEAR,
    "*": INTERP_QUADRATIC,
    "^": INTERP_TRIANGLE,
    "~": INTERP_SINE,
    "/": INTERP_SAW_UP,
    "\\": INTERP_SAW_DOWN,
    "_": INTERP_SQUARE,
}
_GLYPH = {v: k for k, v in _PREFIX_MAP.items()}

_MODULATIONS = {INTERP_TRIANGLE, INTERP_SINE, INTERP_SAW_UP, INTERP_SAW_DOWN,
                INTERP_SQUARE}


def is_modulation(interp: int) -> bool:
    return interp in _MODULATIONS


def is_ramp(interp: int) -> bool:
    return interp in (INTERP_LINEAR, INTERP_QUADRATIC)


# LED present bits (config_parser.h:70-103)
LED_SET_FREQ = 1 << 0
LED_SET_DUTY = 1 << 1
LED_SET_BRIGHT = 1 << 2
LED_SET_R = 1 << 3
LED_SET_G = 1 << 4
LED_SET_B = 1 << 5
LED_SET_ENV = 1 << 6
LED_SET_PHASE = 1 << 7
LED_SET_ATTACK = 1 << 8
LED_SET_JITTER = 1 << 9
LED_SET_CORE = (LED_SET_FREQ | LED_SET_DUTY | LED_SET_BRIGHT |
                LED_SET_R | LED_SET_G | LED_SET_B)

# Audio present bits
AUD_SET_FREQ = 1 << 0
AUD_SET_PAN = 1 << 1
AUD_SET_VOL = 1 << 2
AUD_SET_MOD = 1 << 3
AUD_SET_FREQR = 1 << 4
AUD_SET_WAVE = 1 << 5
AUD_SET_DUTY = 1 << 6
AUD_SET_ENV = 1 << 7
AUD_SET_PHASE = 1 << 8
AUD_SET_ATTACK = 1 << 9
AUD_SET_JITTER = 1 << 10


# ---------------------------------------------------------------------------
# Primitive conversions -- each one matches a specific C cast
# ---------------------------------------------------------------------------


def c_atof(s: str) -> float:
    """C `atof`: parse the longest numeric prefix, 0.0 on no match.

    Python's float() raises instead, and the difference matters: atof(">250")
    is 0.0, which is exactly how a stray interp glyph in the freqR column
    silently collapses a binaural detune to mono (config_parser.c:1627).
    """
    s = s.strip()
    n = len(s)
    i = 0
    if i < n and s[i] in "+-":
        i += 1
    seen_digit = False
    while i < n and s[i].isdigit():
        i += 1
        seen_digit = True
    if i < n and s[i] == ".":
        i += 1
        while i < n and s[i].isdigit():
            i += 1
            seen_digit = True
    if not seen_digit:
        return 0.0
    j = i
    if i < n and s[i] in "eE":
        k = i + 1
        if k < n and s[k] in "+-":
            k += 1
        if k < n and s[k].isdigit():
            while k < n and s[k].isdigit():
                k += 1
            j = k
    try:
        return float(s[:j])
    except ValueError:
        return 0.0


def c_atol(s: str) -> int:
    """C `atol`: integer prefix only, so "1000.5" -> 1000 and "abc" -> 0."""
    s = s.strip()
    i = 0
    n = len(s)
    if i < n and s[i] in "+-":
        i += 1
    start = i
    while i < n and s[i].isdigit():
        i += 1
    if i == start:
        return 0
    try:
        return int(s[:i])
    except ValueError:
        return 0


def c_u8(v: float) -> int:
    """Unchecked C cast of a float to uint8_t, as the firmware does for LED
    duty/brightness (config_parser.c:1425, 1435) -- NO clamping.

    duty 300 therefore becomes 44 and duty 256 becomes 0. (The standard calls
    out-of-range float->unsigned UB; this modulo behaviour is what the Xtensa
    toolchain emits and what the field reports show.)
    """
    return int(v) & 0xFF


def c_u16(v: float) -> int:
    return int(v) & 0xFFFF


def c_u32_time(v: int) -> int:
    """`(uint32_t)atol(...)`: a negative time wraps to a huge value on device."""
    return v & 0xFFFFFFFF


def clamp_u8_field(v: float) -> int:
    """config_parser.c:1384-1394 -- R/G/B ARE clamped (unlike duty/bright)."""
    iv = int(v)
    return 0 if iv < 0 else (255 if iv > 255 else iv)


def wrap_deg(d: float) -> float:
    r = math.fmod(d, 360.0)
    return r + 360.0 if r < 0.0 else r


def clamp_pct(v: float) -> float:
    return 0.0 if v < 0.0 else (100.0 if v > 100.0 else v)


def tok_is_dash(t: str | None) -> bool:
    """config_parser.c:79-81 -- a LONE '-' is the sentinel. '-50' is a number."""
    return t == "-"


def parse_value_with_interpolation(tok: str) -> tuple[float, int]:
    """config_parser.c:1769-1786."""
    if not tok:
        return 0.0, INTERP_NONE
    interp = _PREFIX_MAP.get(tok[0])
    if interp is None:
        return c_atof(tok), INTERP_NONE
    return c_atof(tok[1:]), interp


def parse_mod_extras(tok: str) -> tuple[float, float]:
    """config_parser.c:1795-1819 -- `prefix start:end:period_ms`.

    Missing `:end`    -> end = start (degenerate, no actual modulation).
    Missing `:period` -> 1000 ms.
    """
    if not tok:
        return 0.0, 1000.0
    body = tok[1:]
    parts = body.split(":")
    end = c_atof(parts[1]) if len(parts) >= 2 else c_atof(parts[0])
    period = c_atof(parts[2]) if len(parts) >= 3 else 1000.0
    return end, period


def parse_jitter_token(tok: str, default_period_ms: float) -> tuple[float, float]:
    """config_parser.c:111-116 routes the amplitude through
    parse_value_with_interpolation, which STRIPS a leading glyph and parses the
    remainder -- so `>5` is 5.0 on the device, not 0.0. The curve itself is
    discarded (jitter is not sweepable), but the magnitude survives."""
    parts = tok.split(":")
    amp, _curve = parse_value_with_interpolation(parts[0])
    period = c_atof(parts[1]) if len(parts) >= 2 else default_period_ms
    return amp, period


# ---------------------------------------------------------------------------
# Entry records
# ---------------------------------------------------------------------------


@dataclass
class Cell:
    """One positional field: value + how it animates."""

    value: float = 0.0
    interp: int = INTERP_NONE
    mod_end: float = 0.0
    mod_period_ms: float = 1000.0


@dataclass
class LedEntry:
    time_ms: int = 0
    present: int = 0
    line_no: int = 0
    raw: str = ""
    freq: Cell = field(default_factory=Cell)
    duty: Cell = field(default_factory=Cell)
    bright: Cell = field(default_factory=Cell)
    r: Cell = field(default_factory=Cell)
    g: Cell = field(default_factory=Cell)
    b: Cell = field(default_factory=Cell)
    mask: int = 0
    env: int = 0
    phase: Cell = field(default_factory=Cell)
    attack: Cell = field(default_factory=Cell)
    jitter_amp_hz: float = 0.0
    jitter_period_ms: float = 45000.0

    def cell(self, name: str) -> Cell:
        return getattr(self, name)


@dataclass
class AudioEntry:
    time_ms: int = 0
    present: int = 0
    line_no: int = 0
    raw: str = ""
    freq: Cell = field(default_factory=Cell)
    pan: Cell = field(default_factory=Cell)
    vol: Cell = field(default_factory=Cell)
    mod: Cell = field(default_factory=Cell)
    channel: int = 0
    freq_r: float = 0.0
    freq_r_had_prefix: bool = False   # see LEDC_FREQR_PREFIX lint
    wave: int = 0
    duty: Cell = field(default_factory=Cell)
    env: int = 4
    phase: Cell = field(default_factory=Cell)
    attack: Cell = field(default_factory=Cell)
    jitter_amp_hz: float = 0.0
    jitter_period_ms: float = 45000.0

    def cell(self, name: str) -> Cell:
        return getattr(self, name)


@dataclass
class SpeechEntry:
    time_ms: int = 0
    line_no: int = 0
    voice: str = "default"
    volume: int = 80
    text: str = ""
    raw: str = ""


@dataclass
class BgEntry:
    url: str = ""
    pan: float = 0.0
    loudness: float = 0.0
    line_no: int = 0


@dataclass
class ParseResult:
    entries: list                  # LedEntry | AudioEntry | SpeechEntry, file order
    bg: BgEntry | None
    lints: list[Finding]
    dropped_over_cap: int
    source: str


# ---------------------------------------------------------------------------
# Lexer + parser
# ---------------------------------------------------------------------------

_BG_SCHEMES = ("http://", "https://", "sdcard://", "push://")


def _tokenize(line: str) -> list[str]:
    """config_parser.c:1332-1361 -- leading space skipped, inline comment from
    the FIRST '#' stripped, split on space/tab, max 16 tokens kept."""
    s = line.lstrip(" \t")
    if not s or s[0] == "#":
        return []
    hash_at = s.find("#")
    if hash_at >= 0:
        s = s[:hash_at]
    toks = s.split()
    return toks[: dm.MAX_TOKENS]


def parse_ledc(path: str, *, keep_speech: bool = False) -> ParseResult:
    """Parse a `.ledc` exactly as main/config_parser.c would.

    `keep_speech=False` (the default) additionally applies the BROWSER's
    transform: the device does not receive the file on disk, it receives
    serialize(parse(file)) with `S` rows stripped
    (web/src/js/gen/serialize.js:159-163, transport.js:15-27). So by default we
    model what the device actually gets. Pass keep_speech=True to model a
    direct POST of the raw file.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    text = blob.decode("utf-8", errors="replace")
    lints: list[Finding] = []
    entries: list = []
    bg: BgEntry | None = None
    dropped = 0

    for line_no, raw_line in enumerate(text.split("\n"), start=1):
        line = raw_line[:-1] if raw_line.endswith("\r") else raw_line

        # config_parser.c:290, 301-304 -- lines >= 256 bytes are TRUNCATED to
        # 255. That drops tail tokens, which changes the token COUNT and can
        # silently reinterpret or reject the line.
        encoded = line.encode("utf-8", errors="replace")
        if len(encoded) >= dm.MAX_LINE_LENGTH:
            line = encoded[: dm.MAX_LINE_LENGTH - 1].decode("utf-8", errors="ignore")
            lints.append(Finding(
                code="LEDC_LINE_TRUNCATED", severity="warning", domain="ledc",
                line_no=line_no,
                message=f"line {line_no} is {len(encoded)} bytes; the device "
                        f"truncates at 255 and loses the tail tokens",
                detail="config_parser.c:290,301-304"))

        stripped = line.lstrip(" \t")
        if not stripped or stripped[0] == "#":
            continue

        # BG is intercepted BEFORE parse_line (config_parser.c:328-389) and is
        # last-wins across the file. It never becomes a timeline entry.
        if (len(stripped) >= 2 and stripped[0] in "Bb" and stripped[1] in "Gg"
                and (len(stripped) == 2 or stripped[2].isspace())):
            toks = _tokenize(line)
            parsed, bg_lint = _parse_bg(toks[1:], line_no)
            if parsed is not None:
                bg = parsed
            if bg_lint:
                lints.append(bg_lint)
            continue

        toks = _tokenize(line)
        if not toks:
            continue

        ent, lint = _parse_line(toks, line, line_no)
        if lint:
            lints.extend(lint if isinstance(lint, list) else [lint])
        if ent is None:
            # A line the device REJECTS never consumes a slot: config_parser.c:
            # 395-420 only increments the entry count when parse_line returned
            # ESP_OK. Counting rejected lines against the cap inflated
            # LEDC_ENTRY_CAP and shifted the cap boundary, so a user chasing
            # "which entries never fire" got a number larger than reality.
            continue
        if isinstance(ent, SpeechEntry) and not keep_speech:
            # The browser strips S rows BEFORE POSTing (serialize.js:159-163),
            # so with keep_speech=False the device never sees this line at all
            # and it cannot occupy a slot either.
            continue
        if len(entries) >= dm.MAX_ENTRIES:
            # config_parser.c:397-420 -- silently dropped at PARSE time, i.e.
            # in FILE order, before the stable sort.
            dropped += 1
            continue
        entries.append(ent)

    if dropped:
        lints.append(Finding(
            code="LEDC_ENTRY_CAP", severity="error", domain="ledc",
            message=f"{dropped} entr{'y' if dropped == 1 else 'ies'} past the "
                    f"{dm.MAX_ENTRIES}-entry limit are silently dropped by the "
                    f"device and will never execute",
            detail="config_parser.h:19, config_parser.c:397-420"))

    return ParseResult(entries=entries, bg=bg, lints=lints,
                       dropped_over_cap=dropped, source=path)


def _parse_bg(toks: list[str], line_no: int) -> tuple[BgEntry | None, Finding | None]:
    # config_parser.c:1707-1710 fails only on `token_count < 3` and IGNORES
    # extras, so `BG http://x 0 50 note` is accepted on the device. Requiring
    # exactly 3 silently dropped the background audio from the expectation,
    # which moves the mix RMS and the dominant-tone prediction.
    if len(toks) < 3:
        return None, Finding(
            code="LEDC_BG_BAD", severity="warning", domain="ledc", line_no=line_no,
            message=f"BG line {line_no} has {len(toks)} fields; at least 3 "
                    f"(url pan loudness) are required, so the device ignores it",
            detail="config_parser.c:1699-1753")
    url = toks[0]
    if not url.startswith(_BG_SCHEMES):
        return None, Finding(
            code="LEDC_BG_SCHEME", severity="warning", domain="ledc", line_no=line_no,
            message=f"BG url scheme not one of {', '.join(_BG_SCHEMES)}; "
                    f"the device rejects the line",
            detail="config_parser.c:1707-1750")
    pan = max(-100.0, min(100.0, c_atof(toks[1]))) / 100.0
    loud = max(0.0, min(100.0, c_atof(toks[2]))) / 100.0
    return BgEntry(url=url, pan=pan, loudness=loud, line_no=line_no), None


def _parse_line(toks: list[str], raw: str, line_no: int):
    """config_parser.c:1368-1380 -- dispatch on the FIRST CHARACTER of token 0."""
    t0 = toks[0]
    if t0[0] in "Aa":
        # NOTE: token 0 is DISCARDED. So `A1500 200 ...` parses as audio with
        # time taken from the NEXT token -- the legacy `A1500 binaural ...`
        # syntax in the root CLAUDE.md misparses silently.
        #
        # The device STILL PARSES THE LINE: config_parser.c:1368-1370 dispatches
        # on the first character only and then hands `tokens + 1` to
        # parse_audio_line, so `A1500 200 0 50 0 0` becomes a real audio entry at
        # t=200 ms with freq=0, pan=50, vol=0, mod=0, ch=0. Modelling it as a
        # DELETED line left the expectation missing an entry the device executes
        # -- typically one that starts a channel at a bogus time.
        if len(t0) > 1:
            ent, sub = _parse_audio(toks[1:], raw, line_no)
            glue = Finding(
                code="LEDC_AUDIO_GLUED_TIME", severity="warning", domain="ledc",
                line_no=line_no,
                message=f"line {line_no}: '{t0}' -- the device discards token 0 "
                        f"entirely, so every field shifts left by one and the "
                        f"time comes from the NEXT field"
                        + (f" (this line executes at t={ent.time_ms} ms, not "
                           f"t={t0[1:]} ms)" if ent is not None else
                           " (and the shifted line is then rejected outright)")
                        + ". Put a space after 'A'.",
                detail="config_parser.c:1368-1370")
            lints = [glue] + ([sub] if sub else [])
            return ent, lints
        return _parse_audio(toks[1:], raw, line_no)
    if t0 in ("S", "s"):
        return _parse_speech(raw, line_no)
    return _parse_led(toks, raw, line_no)


def _parse_led(toks: list[str], raw: str, line_no: int):
    n = len(toks)
    if n != 5 and (n < 8 or n > 12):
        return None, Finding(
            code="LEDC_LED_TOKENS", severity="warning", domain="ledc", line_no=line_no,
            message=f"line {line_no}: LED line has {n} fields; the device accepts "
                    f"only 5 (legacy) or 8..12, so this line is SKIPPED",
            detail="config_parser.c:1405-1408")

    e = LedEntry(time_ms=c_u32_time(c_atol(toks[0])), line_no=line_no, raw=raw.strip())

    def core(idx: int, name: str, bit: int, cast=None):
        tok = toks[idx]
        if tok_is_dash(tok):
            return
        v, interp = parse_value_with_interpolation(tok)
        cell = Cell(value=cast(v) if cast else v, interp=interp)
        if is_modulation(interp):
            end, per = parse_mod_extras(tok)
            cell.mod_end = cast(end) if cast else end
            cell.mod_period_ms = per
        setattr(e, name, cell)
        e.present |= bit

    core(1, "freq", LED_SET_FREQ)
    core(2, "duty", LED_SET_DUTY, c_u8)
    core(3, "bright", LED_SET_BRIGHT, c_u8)

    if n >= 8:
        core(4, "r", LED_SET_R, clamp_u8_field)
        core(5, "g", LED_SET_G, clamp_u8_field)
        core(6, "b", LED_SET_B, clamp_u8_field)
        e.mask = c_u8(c_atol(toks[7]))
    else:
        # Legacy 5-token form: RGB defaults to full white AND the present bits
        # are SET (config_parser.c:1476-1481).
        e.r = Cell(value=255.0)
        e.g = Cell(value=255.0)
        e.b = Cell(value=255.0)
        e.present |= LED_SET_R | LED_SET_G | LED_SET_B
        e.mask = c_u8(c_atol(toks[4]))

    if n >= 9 and not tok_is_dash(toks[8]):
        env_v, _ = parse_value_with_interpolation(toks[8])  # glyph dropped: enum
        env = int(env_v)
        e.env = env if 0 <= env <= 3 else 0
        e.present |= LED_SET_ENV
    if n >= 10 and not tok_is_dash(toks[9]):
        # (uint16_t)wrap_deg(d) -- an INTEGER degree (config_parser.c:1493).
        v, interp = parse_value_with_interpolation(toks[9])
        cell = Cell(value=float(c_u16(wrap_deg(v))), interp=interp)
        if is_modulation(interp):
            end, per = parse_mod_extras(toks[9])
            cell.mod_end = float(c_u16(wrap_deg(end)))
            cell.mod_period_ms = per
        e.phase = cell
        e.present |= LED_SET_PHASE
    if n >= 11 and not tok_is_dash(toks[10]):
        # (uint16_t)(a < 0 ? 0 : a) at config_parser.c:1501, then
        # led_matrix_set_attack caps it at 60 ms (led_matrix_example.c:1435).
        # NOT capped here. led_matrix_set_attack_masked caps at 60 ms
        # (main/led_matrix_example.c:1497) but led_matrix_start_sweep_masked
        # writes spec->attack_start / attack_target straight into s->sw_attack
        # (:2063-2067) with no cap at all, so a RAMPED attack interpolates
        # through uncapped endpoints on the device. The cap is applied on the
        # setter path instead, in _apply_led_pulse_fields.
        v, interp = parse_value_with_interpolation(toks[10])
        cell = Cell(value=float(c_u16(max(0.0, v))), interp=interp)
        if is_modulation(interp):
            end, per = parse_mod_extras(toks[10])
            cell.mod_end = float(c_u16(max(0.0, end)))
            cell.mod_period_ms = per
        e.attack = cell
        e.present |= LED_SET_ATTACK
    if n >= 12 and not tok_is_dash(toks[11]):
        e.jitter_amp_hz, e.jitter_period_ms = parse_jitter_token(toks[11], 45000.0)
        # led_matrix_set_jitter (led_matrix_example.c:1440-1446): amplitude is
        # clamped to 0..5 Hz, and the period is only accepted at >= 1000 ms.
        # (Jitter itself is NOT modelled in the expectation -- see README
        # LIMITATIONS -- but the parsed values must still be the device's.)
        e.jitter_amp_hz = min(5.0, max(0.0, e.jitter_amp_hz))
        if e.jitter_period_ms < 1000.0:
            e.jitter_period_ms = 45000.0
        e.present |= LED_SET_JITTER

    if e.mask == 0:
        # config_parser.c:1513-1516. Note this is also how a '-' in the mask
        # column kills a line: atoi("-") == 0. parse.js disagrees and accepts it.
        return None, Finding(
            code="LEDC_LED_MASK_ZERO", severity="warning", domain="ledc",
            line_no=line_no,
            message=f"line {line_no}: LED channel mask is 0 "
                    f"(a '-' here also reads as 0), so the device SKIPS the line",
            detail="config_parser.c:1474,1513-1516")
    return e, None


def _parse_audio(toks: list[str], raw: str, line_no: int):
    n = len(toks)
    if n < 5:
        return None, Finding(
            code="LEDC_AUDIO_TOKENS", severity="warning", domain="ledc",
            line_no=line_no,
            message=f"line {line_no}: audio line has {n} fields after 'A'; "
                    f"at least 5 (time freq pan vol mod) are required",
            detail="config_parser.c:1583")

    e = AudioEntry(time_ms=c_u32_time(c_atol(toks[0])), line_no=line_no,
                   raw=raw.strip())

    def core(idx: int, name: str, bit: int):
        tok = toks[idx]
        if tok_is_dash(tok):
            return
        v, interp = parse_value_with_interpolation(tok)
        cell = Cell(value=v, interp=interp)
        if is_modulation(interp):
            cell.mod_end, cell.mod_period_ms = parse_mod_extras(tok)
        setattr(e, name, cell)
        e.present |= bit

    core(1, "freq", AUD_SET_FREQ)
    core(2, "pan", AUD_SET_PAN)
    core(3, "vol", AUD_SET_VOL)
    core(4, "mod", AUD_SET_MOD)

    # Selector, not '-'-able: a dash here silently means channel 0.
    e.channel = c_atol(toks[5]) if (n >= 6 and not tok_is_dash(toks[5])) else 0

    if n >= 7 and not tok_is_dash(toks[6]):
        fr = c_atof(toks[6])      # plain atof -- an interp glyph yields 0.0
        e.freq_r_had_prefix = toks[6][0] in _PREFIX_MAP
        e.freq_r = fr if 0.0 < fr <= (dm.AUDIO_SAMPLE_RATE / 2.0) else 0.0
        e.present |= AUD_SET_FREQR
    if n >= 8 and not tok_is_dash(toks[7]):
        wt = c_atol(toks[7])
        e.wave = wt if 0 <= wt < 8 else 0
        e.present |= AUD_SET_WAVE
    if n >= 9 and not tok_is_dash(toks[8]):
        v, interp = parse_value_with_interpolation(toks[8])
        cell = Cell(value=clamp_pct(v), interp=interp)
        if is_modulation(interp):
            end, per = parse_mod_extras(toks[8])
            cell.mod_end, cell.mod_period_ms = clamp_pct(end), per
        e.duty = cell
        e.present |= AUD_SET_DUTY
    if n >= 10 and not tok_is_dash(toks[9]):
        env_v, _ = parse_value_with_interpolation(toks[9])
        env = int(env_v)
        # Out-of-range AUDIO env falls back to 4 (tremolo), not 0 like LED.
        e.env = env if 0 <= env <= 4 else 4
        e.present |= AUD_SET_ENV
    if n >= 11 and not tok_is_dash(toks[10]):
        v, interp = parse_value_with_interpolation(toks[10])
        cell = Cell(value=wrap_deg(v), interp=interp)
        if is_modulation(interp):
            end, per = parse_mod_extras(toks[10])
            cell.mod_end, cell.mod_period_ms = wrap_deg(end), per
        e.phase = cell
        e.present |= AUD_SET_PHASE
    if n >= 12 and not tok_is_dash(toks[11]):
        v, interp = parse_value_with_interpolation(toks[11])
        cell = Cell(value=max(0.0, v), interp=interp)
        if is_modulation(interp):
            end, per = parse_mod_extras(toks[11])
            cell.mod_end, cell.mod_period_ms = max(0.0, end), per
        e.attack = cell
        e.present |= AUD_SET_ATTACK
    if n >= 13 and not tok_is_dash(toks[12]):
        e.jitter_amp_hz, e.jitter_period_ms = parse_jitter_token(toks[12], 45000.0)
        e.present |= AUD_SET_JITTER

    return e, None


def _parse_speech(raw: str, line_no: int):
    """config_parser.c:1528-1578 -- parsed from the RAW line, so a '#' inside
    the quoted text survives. MISSING QUOTES = line rejected."""
    body = raw.strip()
    toks = body.split()
    if len(toks) < 2:
        return None, None
    time_tok = toks[1]
    if not time_tok.lstrip("+-").isdigit():
        return None, Finding(
            code="LEDC_SPEECH_TIME", severity="warning", domain="ledc",
            line_no=line_no,
            message=f"line {line_no}: speech time is not a number; device rejects",
            detail="config_parser.c:1539-1543")
    q1, q2 = body.find('"'), body.rfind('"')
    if q1 < 0 or q2 <= q1:
        return None, Finding(
            code="LEDC_SPEECH_UNQUOTED", severity="warning", domain="ledc",
            line_no=line_no,
            message=f"line {line_no}: speech text is not quoted; the FIRMWARE "
                    f"rejects this line even though the browser parser accepts it",
            detail="config_parser.c:1563-1572 vs parse.js:199")
    voice = toks[2] if len(toks) >= 3 else "default"
    vol = c_atol(toks[3]) if len(toks) >= 4 else 80
    text = body[q1 + 1:q2][: dm.SPEECH_TEXT_MAX]
    return SpeechEntry(time_ms=c_atol(time_tok), line_no=line_no, voice=voice,
                       volume=vol, text=text, raw=body), None


# ---------------------------------------------------------------------------
# Scheduling -- stable sort, batch caps, three-pass dispatch, first-batch hoist
# ---------------------------------------------------------------------------

_KIND_PASS = {"audio": 0, "led": 1, "speech": 2}


def build_schedule(pr: ParseResult) -> tuple[list[ScheduledEntry], list[Finding]]:
    """Reproduce config_parser.c's execution order and batch caps."""
    lints: list[Finding] = []

    def kind_of(e) -> str:
        if isinstance(e, LedEntry):
            return "led"
        if isinstance(e, AudioEntry):
            return "audio"
        return "speech"

    # config_parser.c:443-455 -- STABLE sort by time_ms. File order is preserved
    # only within one timestamp. 35 of 82 shipped sessions are not in file time
    # order and depend on this.
    order = sorted(range(len(pr.entries)), key=lambda i: (pr.entries[i].time_ms, i))

    sched: list[ScheduledEntry] = []
    i = 0
    first_batch = True
    while i < len(order):
        ts = pr.entries[order[i]].time_ms
        j = i
        while j < len(order) and pr.entries[order[j]].time_ms == ts:
            j += 1
        batch = order[i:j]

        # config_parser.c:698-709 / 1942-1952 -- only the first 50 entries at a
        # timestamp ever execute; the rest are stepped over by the
        # "find next strictly later timestamp" scan and NEVER run.
        executed = batch[: dm.MAX_BATCH_SIZE]
        dropped = batch[dm.MAX_BATCH_SIZE:]
        if dropped:
            lints.append(Finding(
                code="LEDC_BATCH_CAP", severity="error", domain="ledc",
                t_ms=float(ts),
                message=f"{len(batch)} entries share timestamp {ts} ms; the device "
                        f"executes only the first {dm.MAX_BATCH_SIZE} and the other "
                        f"{len(dropped)} NEVER FIRE",
                detail="config_parser.c:698-709, 1942-1952"))

        # config_parser.c:696 -- FIRST-BATCH HOISTING: the initial dispatch uses
        # entries[0]'s timestamp and fires IMMEDIATELY, so a timeline whose
        # earliest entry is at t=5000 executes that batch at wall-clock 0.
        dispatch_ms = 0 if first_batch else ts
        if first_batch and ts != 0:
            lints.append(Finding(
                code="LEDC_FIRST_BATCH_HOIST", severity="warning", domain="ledc",
                t_ms=float(ts), expected=float(ts), observed=0.0, delta=-float(ts),
                unit="ms",
                message=f"earliest entry is at t={ts} ms but the device dispatches "
                        f"the first batch IMMEDIATELY at wall-clock 0 "
                        f"({ts} ms early). Audio starts early; LED is unaffected "
                        f"because its cycle anchor is tied to the demanded time.",
                detail="config_parser.c:696-709"))

        # Three passes by TYPE, regardless of file order (config_parser.c:739,
        # 1989-2043): all AUDIO, then all LED, then speech/other.
        ordered = sorted(executed, key=lambda k: _KIND_PASS[kind_of(pr.entries[k])])
        for k in ordered:
            e = pr.entries[k]
            sched.append(ScheduledEntry(
                index=len(sched), line_no=e.line_no, kind=kind_of(e),
                t_demanded_ms=e.time_ms, t_dispatch_ms=dispatch_ms,
                executed=True, drop_reason=None, raw=e))
        for k in dropped:
            e = pr.entries[k]
            sched.append(ScheduledEntry(
                index=len(sched), line_no=e.line_no, kind=kind_of(e),
                t_demanded_ms=e.time_ms, t_dispatch_ms=dispatch_ms,
                executed=False, drop_reason="batch_cap", raw=e))

        first_batch = False
        i = j
    return sched, lints


# ---------------------------------------------------------------------------
# Animation evaluation
# ---------------------------------------------------------------------------


@dataclass
class Sweep:
    """A one-shot ramp: `>` or `*`."""

    v0: float
    v1: float
    curve: int                     # INTERP_LINEAR | INTERP_QUADRATIC
    t0_s: float
    dur_s: float


@dataclass
class Mod:
    """A self-contained periodic modulation: `^ ~ / \\ _`."""

    wave: int
    start: float
    end: float
    period_s: float
    t0_s: float


def sweep_eval(sw: Sweep, t: np.ndarray) -> np.ndarray:
    """audio_generator.c:1972-1989 / led_matrix_example.c:457-471.

    linear    v = s + (t-s)*p
    quadratic ease-in-out: p<0.5 -> 2p^2 else 1-2(1-p)^2, then lerp.
    """
    t = np.asarray(t, dtype=np.float64)
    if sw.dur_s <= 0.0:
        return np.full(t.shape, sw.v1, dtype=np.float64)
    p = (t - sw.t0_s) / sw.dur_s
    p = np.clip(p, 0.0, 1.0)
    if sw.curve == INTERP_QUADRATIC:
        p = np.where(p < 0.5, 2.0 * p * p, 1.0 - 2.0 * (1.0 - p) ** 2)
    return sw.v0 + (sw.v1 - sw.v0) * p


def mod_eval(md: Mod, t: np.ndarray) -> np.ndarray:
    """audio_generator.c:1733-1764 / led_matrix_example.c:509-600.

    One full period is the complete start->end->start cycle for the symmetric
    shapes. Shapes are identical on both engines except that LED's sine uses
    the parabola 4p(1-p) instead of (1-cos 2pi p)/2 -- within ~5%, and we use
    the parabola for LED, the true cosine for audio, so neither engine is
    misrepresented.
    """
    t = np.asarray(t, dtype=np.float64)
    if md.period_s <= 0.0:
        return np.full(t.shape, md.start, dtype=np.float64)
    p = np.mod((t - md.t0_s) / md.period_s, 1.0)
    if md.wave == INTERP_TRIANGLE:
        shape = np.where(p < 0.5, 2.0 * p, 2.0 - 2.0 * p)
    elif md.wave == INTERP_SINE:
        shape = (1.0 - np.cos(2.0 * np.pi * p)) / 2.0
    elif md.wave == INTERP_SAW_UP:
        shape = p
    elif md.wave == INTERP_SAW_DOWN:
        shape = 1.0 - p
    elif md.wave == INTERP_SQUARE:
        shape = np.where(p < 0.5, 0.0, 1.0)
    else:
        shape = np.zeros_like(p)
    return md.start + (md.end - md.start) * shape


def mod_eval_led_sine(md: Mod, t: np.ndarray) -> np.ndarray:
    """LED ISR variant: parabolic sine approximation (led_matrix_example.c:560)."""
    t = np.asarray(t, dtype=np.float64)
    if md.period_s <= 0.0:
        return np.full(t.shape, md.start, dtype=np.float64)
    p = np.mod((t - md.t0_s) / md.period_s, 1.0)
    shape = np.clip(4.0 * p * (1.0 - p), 0.0, 1.0)
    return md.start + (md.end - md.start) * shape


# ---------------------------------------------------------------------------
# Keyframe state machine
# ---------------------------------------------------------------------------

_LED_FIELDS = ("freq", "duty", "bright", "r", "g", "b", "phase", "attack")
_AUD_FIELDS = ("freq", "pan", "vol", "mod", "duty", "phase", "attack")


@dataclass
class Keyframe:
    t_s: float                      # when the written state takes effect
    state: dict
    sweeps: dict
    mods: dict
    is_led: bool

    def value(self, fname: str, t: np.ndarray) -> np.ndarray:
        """Modulation wins over sweep wins over the stepped state value.

        That priority is the firmware's: led_matrix_example.c:827-845 evaluates
        the sweep then lets an active mod override it, and config_parser.c:
        2241-2260 maps every modulation type to curve NONE so a modulated field
        never gets a sweep installed in the first place.
        """
        md = self.mods.get(fname)
        if md is not None:
            if self.is_led and md.wave == INTERP_SINE:
                return mod_eval_led_sine(md, t)
            return mod_eval(md, t)
        sw = self.sweeps.get(fname)
        if sw is not None:
            return sweep_eval(sw, t)
        return np.full(np.shape(t), float(self.state[fname]), dtype=np.float64)


def _led_initial_state() -> dict:
    # led_matrix_example.c:146-173 -- the snapshot defaults for a channel that
    # has never been started. These are what a '-' field resolves to, which is
    # why a '-' freq on a fresh channel means "freq 0" = STOP FLICKER.
    return dict(started=False, active=False, freq=0.0, duty=50.0, bright=100.0,
                r=255.0, g=255.0, b=255.0, env=0, phase=0.0, attack=3.0,
                jitter_amp=0.0, jitter_period=45000.0,
                # When this activation's cycle anchor arrives. The ISR skips
                # the channel entirely until then (led_matrix_example.c:712),
                # so the LED is DARK even though the state is already written.
                # It is PER-ACTIVATION: a channel that is stopped and restarted
                # gets a new anchor, so a single session-wide value would
                # retroactively blank the earlier, perfectly valid, run.
                vis_from=float("inf"))


def _aud_initial_state() -> dict:
    return dict(started=False, active=False, freq=0.0, pan=0.0, vol=0.0, mod=0.0,
                freq_r=0.0, wave=0, duty=50.0, env=4, phase=0.0, attack=3.0)


def _scalar(kf: Keyframe, fname: str, t_s: float) -> float:
    return float(kf.value(fname, np.array([t_s]))[0])


class _Simulator:
    """Walks the schedule, producing per-channel keyframe lists.

    Why keyframes rather than a dense simulation: a 60-minute session has at
    most a few hundred entries, so the state is piecewise-analytic. Evaluating
    analytically keeps the model exact (no integration error) and makes the
    whole expectation cost O(entries + grid) instead of O(samples).
    """

    def __init__(self, sched: list[ScheduledEntry], backend: str):
        self.sched = sched
        self.backend = backend
        self.led_state = {ch: _led_initial_state() for ch in range(dm.NUM_LED_CHANNELS)}
        self.led_sweeps: dict[int, dict] = {ch: {} for ch in range(dm.NUM_LED_CHANNELS)}
        self.led_mods: dict[int, dict] = {ch: {} for ch in range(dm.NUM_LED_CHANNELS)}
        self.led_kfs: dict[int, list[Keyframe]] = {ch: [] for ch in range(dm.NUM_LED_CHANNELS)}
        self.led_visible_from: dict[int, float] = {}

        self.aud_state = {ch: _aud_initial_state() for ch in range(dm.NUM_AUDIO_CHANNELS)}
        self.aud_sweeps: dict[int, dict] = {ch: {} for ch in range(dm.NUM_AUDIO_CHANNELS)}
        self.aud_mods: dict[int, dict] = {ch: {} for ch in range(dm.NUM_AUDIO_CHANNELS)}
        self.aud_kfs: dict[int, list[Keyframe]] = {ch: [] for ch in range(dm.NUM_AUDIO_CHANNELS)}

        # The ISR tick RISES at channel start / sweep start
        # (led_matrix_example.c:1251, callers at :1330, :1941) and RESETS to the
        # floor when the last active channel stops -- see _reset_tick_if_idle.
        # It is pre-warmed to 1000 Hz at boot (:297). A mid-sweep frequency
        # never raises it, which is why a 10->40 Hz ramp lands at 39.68 Hz, not
        # 40.00 Hz.
        self.tick_hz = dm.LED_FLICKER_TICK_MIN
        self.tick_timeline: list[tuple[float, int]] = [(0.0, self.tick_hz)]

        self.events: list[dict] = []
        self.lints: list[Finding] = []
        self.last_dispatch_ms = 0

    # -- helpers ---------------------------------------------------------

    def _live_led(self, ch: int, fname: str, t_s: float) -> float:
        kfs = self.led_kfs[ch]
        if not kfs:
            return float(self.led_state[ch][fname])
        return _scalar(kfs[-1], fname, t_s)

    def _live_aud(self, ch: int, fname: str, t_s: float) -> float:
        kfs = self.aud_kfs[ch]
        if not kfs:
            return float(self.aud_state[ch][fname])
        return _scalar(kfs[-1], fname, t_s)

    def _raise_tick(self, freq_hz: float, t_s: float):
        want = dm.flicker_tick_hz_for(freq_hz)
        if want > self.tick_hz:
            self.tick_hz = want
            self.tick_timeline.append((t_s, want))

    def _reset_tick_if_idle(self, t_s: float):
        """The flicker timer is DELETED when no channel is left running, and
        the next activation re-derives its tick from the floor.

        led_matrix_stop_flicker_masked calls s_maybe_teardown_timer_and_task
        (main/led_matrix_example.c:1550), which returns early if ANY channel is
        still active and otherwise does gptimer_del_timer + `s_flicker_timer =
        NULL` (:1271-1285). The next s_ensure_timer_and_task then takes the
        `if (s_flicker_timer == NULL)` branch and sets `s_flicker_tick_hz =
        LED_FLICKER_TICK_MIN` (:1219) BEFORE the raise step at :1251.

        So the tick does NOT only ever rise, and the difference is measurable:
        the README's own sync-marker recipe is a 40 Hz burst followed by freq 0
        on every channel, which pins the tick at 10 kHz under the old model.
        On sessions/library/90_measure_sync that made the 7 Hz zones predict
        6.99790 Hz where the device emits 6.98324 Hz -- 2093 ppm, and 194 of the
        996 rates in 0.5..100 Hz land in the 30..1500 ppm window where that is
        reported as AV_DIFFERENTIAL_DRIFT rather than excluded as a rate error.
        """
        for ch in range(dm.NUM_LED_CHANNELS):
            if self.led_state[ch]["active"]:
                return
        if self.tick_hz != dm.LED_FLICKER_TICK_MIN:
            self.tick_hz = dm.LED_FLICKER_TICK_MIN
            self.tick_timeline.append((t_s, self.tick_hz))

    def _push_led_kf(self, ch: int, t_s: float):
        self.led_kfs[ch].append(Keyframe(
            t_s=t_s, state=dict(self.led_state[ch]),
            sweeps=dict(self.led_sweeps[ch]), mods=dict(self.led_mods[ch]),
            is_led=True))

    def _push_aud_kf(self, ch: int, t_s: float):
        self.aud_kfs[ch].append(Keyframe(
            t_s=t_s, state=dict(self.aud_state[ch]),
            sweeps=dict(self.aud_sweeps[ch]), mods=dict(self.aud_mods[ch]),
            is_led=False))

    # -- main ------------------------------------------------------------

    def run(self):
        for si, se in enumerate(self.sched):
            if not se.executed:
                continue
            if se.kind == "audio":
                self._exec_audio(si, se)
            elif se.kind == "led":
                self._exec_led(si, se)
            # speech produces no audio/LED observable we can model (and on a
            # non-SD build config_parser.c:2286-2296 errors out anyway)

        # config_parser.c:2104-2111 -- the moment the executor finds no later
        # timestamp it calls led_matrix_stop_flicker_masked(0xFF). That happens
        # in the SAME task iteration as the last batch, i.e. at the last
        # batch's DISPATCH time (no anchor), microseconds after the final LED
        # entry was started. Audio is deliberately left running.
        #
        # Consequence worth knowing: the final LED entry of a session never
        # actually flickers. Its own cycle anchor is 46.439 ms in the future
        # when the stop lands, so the channel goes from "about to start" to
        # "stopped" without ever emitting an edge. Real sessions fade
        # brightness to 0 on that entry so it is a harmless no-op -- but a
        # session that expects its last entry to keep flickering gets darkness.
        # ...but ONLY in the `else` arm of `if (current_entry_index + 1 <
        # current_timeline->count)` (config_parser.c:2102-2111). When the final
        # timestamp carries more than MAX_BATCH_SIZE entries, the capped
        # leftovers keep that condition TRUE, so the inner "no more entries to
        # schedule" branch runs instead and flicker is LEFT RUNNING. That is the
        # exact scenario this tool is meant to diagnose (a batch-cap overflow),
        # and modelling darkness there would have the comparator expect the
        # opposite of what the device does.
        executed = [s for s in self.sched if s.executed]
        if executed:
            last_ms = max(s.t_demanded_ms for s in executed)
            leftovers = any(s.t_demanded_ms == last_ms and not s.executed
                            for s in self.sched)
            if leftovers:
                self.lints.append(Finding(
                    code="LEDC_END_STOP_SKIPPED", severity="warning",
                    domain="led", t_ms=float(last_ms),
                    message=f"the final timestamp t={last_ms} ms carries more "
                            f"than {dm.MAX_BATCH_SIZE} entries, so the capped "
                            f"leftovers keep the executor's "
                            f"'more entries to schedule' branch alive and the "
                            f"end-of-timeline stop_flicker_masked(0xFF) is "
                            f"NEVER reached: the LEDs are left flickering at "
                            f"the end of the session.",
                    detail="config_parser.c:2102-2111"))
                return
            t_end = last_ms / 1000.0
            for ch in range(dm.NUM_LED_CHANNELS):
                st = self.led_state[ch]
                if st["active"]:
                    live_bright = self._live_led(ch, "bright", t_end)
                    if live_bright > 1.0:
                        never_lit = st["vis_from"] > t_end
                        if never_lit:
                            why = (f"starts a FRESH flicker at "
                                   f"{live_bright:.0f}% brightness, but the "
                                   f"end-of-timeline handler stops all flicker "
                                   f"in the same dispatch -- before this "
                                   f"channel's +46.4 ms cycle anchor arrives, so "
                                   f"it never emits a single edge")
                        else:
                            why = (f"leaves the channel flickering at "
                                   f"{live_bright:.0f}% brightness, but the "
                                   f"end-of-timeline handler stops all flicker "
                                   f"in the same dispatch, so that final state "
                                   f"lasts zero time")
                        self.lints.append(Finding(
                            code="LEDC_LAST_ENTRY_CANCELLED", severity="warning",
                            domain="led", channel=ch + 1, t_ms=float(last_ms),
                            observed=live_bright, unit="%",
                            message=f"LED ch{ch + 1}: the final entry at "
                                    f"t={last_ms} ms {why}. End the session with "
                                    f"brightness 0 (or freq 0) to make the intent "
                                    f"explicit.",
                            detail="config_parser.c:2104-2111, "
                                   "led_matrix_example.c:712"))
                    # freq/duty/bright/rgb are RETAINED across a stop
                    # (led_matrix_example.c:1532-1545 clears only led_state,
                    # led_dirty and active), at the value the last cycle
                    # boundary published -- i.e. the LIVE one, not the start of
                    # an interrupted ramp.
                    live = {f: self._live_led(ch, f, t_end)
                            for f in ("freq", "duty", "bright", "r", "g", "b")}
                    st.update(active=False, **live)
                    st["vis_from"] = float("inf")
                    self.led_sweeps[ch] = {}
                    self.led_mods[ch] = {}
                    self._push_led_kf(ch, t_end)
                    self.events.append(dict(
                        kind="led_stop", channel=ch + 1, t_ms=float(last_ms),
                        t_eff_ms=t_end * 1000.0, fresh=False))

    # -- audio -----------------------------------------------------------

    def _exec_audio(self, si: int, se: ScheduledEntry):
        a: AudioEntry = se.raw
        ch = a.channel
        t_s = se.t_dispatch_ms / 1000.0

        if ch < 0 or ch >= dm.NUM_AUDIO_CHANNELS:
            self.lints.append(Finding(
                code="LEDC_AUDIO_CH_RANGE", severity="error", domain="ledc",
                line_no=a.line_no, t_ms=float(a.time_ms), channel=ch,
                message=f"audio channel {ch} is outside 0..15; the timeline path "
                        f"has NO bounds check and writes out of bounds",
                detail="config_parser.c:1623, audio_generator.c:376-377"))
            return

        st = self.aud_state[ch]
        was_active = st["active"]

        # v2 '-' skip: keep the channel's LIVE value so the write is a no-op
        # (config_parser.c:2340-2347). Only possible when the channel is already
        # running; otherwise the parsed default (0) stands.
        eff = {}
        for fname, bit in (("freq", AUD_SET_FREQ), ("pan", AUD_SET_PAN),
                           ("vol", AUD_SET_VOL), ("mod", AUD_SET_MOD)):
            if a.present & bit:
                eff[fname] = a.cell(fname).value
            elif was_active:
                eff[fname] = self._live_aud(ch, fname, t_s)
            else:
                eff[fname] = 0.0
        if a.present & AUD_SET_FREQR:
            eff["freq_r"] = a.freq_r
        elif was_active:
            # config_parser.c:2346 reads the LIVE right carrier
            # (audio_generator_get_current_freq_r_locked -> ch->current_freq_r,
            # audio_generator.c:1792-1796). Two cases, and they differ:
            #
            # BINAURAL (params.frequency_r > 0): every assignment of
            # current_freq_r now writes params.frequency_r itself -- :382, :854,
            # :959, :1011, :1630 -- so it is the ABSOLUTE configured value and a
            # carrier sweep does not move it. The read-back is the stored
            # literal.
            #
            # MONO (params.frequency_r == 0): current_freq_r is set to
            # params.frequency at start (:382) and refreshed on a params latch
            # (:854), but BOTH refresh sites are gated on
            # `params.frequency_r > 0.0f` or skipped while a frequency sweep is
            # armed (:852-855), so while a carrier ramp is in flight
            # current_freq_r is FROZEN at the pre-sweep carrier. A '-' freqR
            # landing mid-ramp therefore latches the PRE-RAMP carrier as freqR
            # and turns the channel binaural with a beat equal to the part of
            # the carrier move completed so far.
            if st["freq_r"] > 0.0:
                eff["freq_r"] = st["freq_r"]
            else:
                sw = self.aud_sweeps[ch].get("freq")
                eff["freq_r"] = (float(sw.v0) if sw is not None
                                 else self._live_aud(ch, "freq", t_s))
        else:
            eff["freq_r"] = 0.0

        st.update(freq=eff["freq"], pan=eff["pan"], vol=eff["vol"], mod=eff["mod"],
                  freq_r=eff["freq_r"])

        # WAVEFORM RESET BUG (config_parser.c:2358 + audio_generator.c:858-859):
        # gen_params.wave_type is filled from the parsed value with NO check of
        # AUD_SET_WAVE and latched unconditionally, so ANY later entry on a
        # running channel that omits waveType silently resets it to SINE.
        st["wave"] = a.wave
        if was_active and not (a.present & AUD_SET_WAVE) and self.aud_kfs[ch]:
            prev = self.aud_kfs[ch][-1].state.get("wave", 0)
            if prev != 0:
                self.lints.append(Finding(
                    code="LEDC_WAVE_RESET", severity="warning", domain="audio",
                    line_no=a.line_no, t_ms=float(a.time_ms), channel=ch,
                    message=f"audio ch{ch} at t={a.time_ms} omits waveType, which "
                            f"silently RESETS the channel from wave {prev} to 0 "
                            f"(sine). Repeat the waveType column to keep it.",
                    detail="config_parser.c:2358, audio_generator.c:858-859"))

        if a.freq_r_had_prefix:
            # ledc_format.md:39 wrongly lists freqR as a prefix-capable cell.
            # Neither parser implements that: atof(">250") is 0.0, which forces
            # the channel to MONO and silently destroys the binaural detune.
            self.lints.append(Finding(
                code="LEDC_FREQR_PREFIX", severity="error", domain="audio",
                line_no=a.line_no, t_ms=float(a.time_ms), channel=ch,
                message=f"audio ch{ch} at t={a.time_ms}: the freqR column carries "
                        f"an interpolation glyph, which parses as 0.0 and collapses "
                        f"the channel to MONO (no binaural beat at all). freqR "
                        f"cannot be ramped -- it can only step.",
                detail="config_parser.c:1627-1631, ledc_format.md:39"))

        if a.present & AUD_SET_ENV:
            st["env"] = a.env
        if a.present & AUD_SET_DUTY:
            # audio_generator.c:211 -- only duty > 0 is written, so `duty 0` is a
            # NO-OP that leaves the previous duty in place.
            if a.duty.value > 0.0:
                st["duty"] = a.duty.value
            else:
                self.lints.append(Finding(
                    code="LEDC_AUDIO_DUTY_ZERO", severity="info", domain="audio",
                    line_no=a.line_no, t_ms=float(a.time_ms), channel=ch,
                    message=f"audio ch{ch} duty 0 is a no-op; the previous duty "
                            f"({st['duty']:.0f}%) stays in force",
                    detail="audio_generator.c:211"))
        if a.present & AUD_SET_PHASE:
            st["phase"] = a.phase.value
        if a.present & AUD_SET_ATTACK:
            st["attack"] = a.attack.value

        # Every new entry preempts whatever animation was running on the
        # channel's fields (config_parser.c:2412-2458), then re-arms.
        self.aud_mods[ch] = {}
        self.aud_sweeps[ch] = {}

        for fname, bit in (("freq", AUD_SET_FREQ), ("pan", AUD_SET_PAN),
                           ("vol", AUD_SET_VOL), ("mod", AUD_SET_MOD),
                           ("duty", AUD_SET_DUTY), ("phase", AUD_SET_PHASE),
                           ("attack", AUD_SET_ATTACK)):
            if not (a.present & bit):
                continue
            cell = a.cell(fname)
            if is_modulation(cell.interp):
                self.aud_mods[ch][fname] = Mod(
                    wave=cell.interp, start=cell.value, end=cell.mod_end,
                    period_s=cell.mod_period_ms / 1000.0, t0_s=t_s)

        # ---- ramps -----------------------------------------------------
        # config_parser.c:2465 -- `uint8_t ch_bit = 1u << audio->channel`.
        # For channel >= 8 that truncates to 0, so find_next_* can never match
        # and NO sweep is ever armed: `>` and `*` silently degrade to steps.
        ch_bit = (1 << ch) & 0xFF
        if ch_bit == 0:
            wants_ramp = any(is_ramp(a.cell(f).interp) for f in _AUD_FIELDS)
            if wants_ramp:
                self.lints.append(Finding(
                    code="LEDC_AUDIO_CH8_NO_RAMP", severity="error", domain="audio",
                    line_no=a.line_no, t_ms=float(a.time_ms), channel=ch,
                    message=f"audio ch{ch} carries a '>' or '*' ramp, but channels "
                            f"8..15 NEVER ramp (1u<<{ch} truncates to 0 in a uint8_t) "
                            f"-- the value steps instead. Use channels 0..7.",
                    detail="config_parser.c:2465"))
        else:
            nxt = self._next_audio(si, ch)
            if nxt is not None:
                window_ms = nxt.time_ms - a.time_ms
                dur_s = window_ms / 1000.0
                # CORE fields: target is the IMMEDIATELY next entry's struct
                # member with NO present-bit check (config_parser.c:2480). If
                # that entry wrote '-', the member is still 0 from the memset,
                # so the ramp drives the value to ZERO instead of holding.
                for fname, bit in (("freq", AUD_SET_FREQ), ("vol", AUD_SET_VOL),
                                   ("pan", AUD_SET_PAN), ("mod", AUD_SET_MOD)):
                    cell = a.cell(fname)
                    if not (a.present & bit) or not is_ramp(cell.interp):
                        continue
                    target = nxt.cell(fname).value if (nxt.present & bit) else 0.0
                    # START VALUE: the firmware DISCARDS the caller's literal
                    # whenever the channel is already running.
                    # audio_generator_start_sweep_locked (audio_generator.c:
                    # 519-551) overwrites `start` with ch->current_freq /
                    # current_amp / current_pan / current_mod_freq, and that wins
                    # permanently: fill_buffer only re-derives current_* from the
                    # latched params while `sweeps[param].duration_samples == 0`
                    # (audio_generator.c:831-855), which an armed sweep makes
                    # false. config_parser.c:2487 does pass audio->frequency, but
                    # it never takes effect.
                    #
                    # This is NOT an asymmetry with the LED engine -- both
                    # substitute the live value, and the C comment at
                    # audio_generator.c:533 says so ("This mirrors the LED-side
                    # fix in led_matrix_start_sweep_masked").
                    #
                    # The fresh-channel case is the sharp one, and it goes the
                    # other way for AMPLITUDE only: start_channel_locked sets
                    # ch->current_amp = 0.0f and ch->active = true
                    # (audio_generator.c:386, :477) BEFORE config_parser arms the
                    # sweep, so `ch->active` is already true and a FIRST-entry
                    # volume ramp starts from SILENCE, not from the file literal.
                    # The other three latch params->X, which IS the literal.
                    if was_active:
                        v0 = self._live_aud(ch, fname, t_s)
                    elif fname == "vol":
                        v0 = 0.0
                    else:
                        v0 = eff[fname]
                    if not (nxt.present & bit):
                        self.lints.append(Finding(
                            code="LEDC_RAMP_TO_ZERO", severity="error",
                            domain="audio", line_no=a.line_no,
                            t_ms=float(a.time_ms), channel=ch,
                            message=f"audio ch{ch} {fname} ramps from "
                                    f"{cell.value:g} at t={a.time_ms} toward the "
                                    f"next entry (t={nxt.time_ms}), which leaves "
                                    f"{fname} as '-' -- so it ramps to ZERO, not "
                                    f"to a hold.",
                            detail="config_parser.c:2480"))
                    self.aud_sweeps[ch][fname] = Sweep(
                        v0=v0, v1=target, curve=cell.interp,
                        t0_s=t_s, dur_s=dur_s)
                    if abs(v0 - cell.value) > max(1e-9, 1e-3 * abs(cell.value)):
                        self.lints.append(Finding(
                            code="LEDC_RAMP_START_SUBSTITUTED", severity="info",
                            domain="audio", line_no=a.line_no,
                            t_ms=float(a.time_ms), channel=ch,
                            expected=cell.value, observed=v0,
                            message=f"audio ch{ch} {fname} at t={a.time_ms} "
                                    f"ramps from {v0:g}, not from the file's "
                                    f"{cell.value:g}: the firmware substitutes "
                                    f"the channel's LIVE value on an already-"
                                    f"running channel (and 0 for a first-entry "
                                    f"volume ramp). The literal in the file is "
                                    f"ignored.",
                            detail="audio_generator.c:519-551, 386, 831-855"))
                    # LEDC_BEAT_NOT_SWEPT USED TO LIVE HERE, and it is
                    # deliberately gone. It said "the firmware recomputes the
                    # right carrier as current_freq + (freqR - freq), so the
                    # DETUNE IS PRESERVED and the beat holds ... for the whole
                    # ramp", and it fired at ERROR severity on 29 of the 83
                    # shipped sessions. main/audio_generator.c:995-1013 now
                    # takes params.frequency_r as the ABSOLUTE right carrier on
                    # the sweep path too -- "29 of 83 sessions were silently not
                    # performing their central move" is that commit's own
                    # wording -- so a carrier ramp DOES glide the beat and every
                    # one of those 29 findings described behaviour that no
                    # longer exists. The opposite authoring mistake, putting a
                    # `>` glyph in the freqR column itself, is still caught:
                    # see LEDC_FREQR_PREFIX above.
                # PULSE fields do it correctly: scan forward for the next entry
                # that actually SETS the field (config_parser.c:2591-2639).
                #
                # AND THEIR START VALUE *IS* THE LITERAL, unlike the core fields
                # above -- do not "fix" this to match them. The live-value
                # substitution in audio_generator.c:541-549 reads back
                # ch->iso_duty / iso_attack_ms / mod_phase_offset_q32, and
                # config_parser.c:2395-2402 has ALREADY written this entry's
                # literals into exactly those fields (via
                # audio_generator_set_iso_channel / _set_phase) before it arms
                # the sweep at :2605. So the read-back returns the literal. The
                # ramp is only reachable when the present bit is set, which is
                # also the condition for that write, so there is no path where
                # the two disagree -- EXCEPT duty 0, which
                # audio_generator.c:210 refuses to write (`duty_pct > 0.0f`), so
                # a `>0` duty ramp starts from the PREVIOUS duty instead.
                for fname, bit in (("duty", AUD_SET_DUTY), ("phase", AUD_SET_PHASE),
                                   ("attack", AUD_SET_ATTACK)):
                    cell = a.cell(fname)
                    if not (a.present & bit) or not is_ramp(cell.interp):
                        continue
                    tgt_e = self._next_audio_with(si, ch, bit)
                    if tgt_e is None:
                        continue
                    v0 = cell.value
                    if fname == "duty" and cell.value <= 0.0:
                        v0 = (self._live_aud(ch, "duty", t_s) if was_active
                              else float(st["duty"]))
                    self.aud_sweeps[ch][fname] = Sweep(
                        v0=v0, v1=tgt_e.cell(fname).value, curve=cell.interp,
                        t0_s=t_s, dur_s=(tgt_e.time_ms - a.time_ms) / 1000.0)
            else:
                orphan = [f for f in _AUD_FIELDS if is_ramp(a.cell(f).interp)]
                if orphan:
                    self.lints.append(Finding(
                        code="LEDC_ORPHAN_RAMP", severity="info", domain="audio",
                        line_no=a.line_no, t_ms=float(a.time_ms), channel=ch,
                        message=f"audio ch{ch} at t={a.time_ms} is the LAST entry "
                                f"on its channel, so its ramp on "
                                f"{', '.join(orphan)} does nothing -- the value "
                                f"just holds.",
                        detail="config_parser.c:2468"))

        if not was_active:
            st["active"] = True
            st["started"] = True
        self._push_aud_kf(ch, t_s)
        self.events.append(dict(
            kind="audio_start" if not was_active else "audio_update",
            channel=ch, t_ms=float(a.time_ms),
            # Audio is dispatched (and thus hoisted) at t_dispatch, and its
            # observable onset is further delayed by the generator block plus
            # the DMA pipeline. Both are added by the comparator's tolerance.
            t_eff_ms=se.t_dispatch_ms, fresh=not was_active, line_no=a.line_no))

    # `s.executed` is deliberately NOT required here, and that is a fidelity
    # point rather than an oversight: find_next_audio_for_bit /
    # find_next_audio_with (config_parser.c:2187-2196, :2227-2235) scan the RAW
    # sorted entry array, so they will happily take a ramp's TARGET from an
    # entry the 50-per-batch cap prevents from ever executing. Requiring
    # `executed` changed the ramp's endpoint outright, not by a rounding step.
    def _next_audio(self, si: int, ch: int) -> AudioEntry | None:
        for k in range(si + 1, len(self.sched)):
            s = self.sched[k]
            if s.kind == "audio" and s.raw.channel == ch:
                return s.raw
        return None

    def _next_audio_with(self, si: int, ch: int, bit: int) -> AudioEntry | None:
        for k in range(si + 1, len(self.sched)):
            s = self.sched[k]
            if s.kind == "audio" and s.raw.channel == ch \
                    and (s.raw.present & bit):
                return s.raw
        return None

    # -- LED -------------------------------------------------------------

    def _exec_led(self, si: int, se: ScheduledEntry):
        led: LedEntry = se.raw
        # config_parser.c:2699-2702 -- the anchor is tied to the DEMANDED time,
        # not the dispatch time, which is exactly what makes the LED path immune
        # to dispatch lag (and to first-batch hoisting).
        t_anchor_s = led.time_ms / 1000.0 + dm.DMA_LAG_US / 1e6
        t_state_s = min(se.t_dispatch_ms / 1000.0, t_anchor_s)

        bits = [b for b in range(dm.NUM_LED_CHANNELS) if led.mask & (1 << b)]
        if not bits:
            return
        if any(b >= 4 for b in bits):
            self.lints.append(Finding(
                code="LEDC_MASK_HIGH_BITS", severity="info", domain="led",
                line_no=led.line_no, t_ms=float(led.time_ms),
                message=f"mask 0x{led.mask:02x} selects channel(s) "
                        f"{[b + 1 for b in bits if b >= 4]}, which light nothing "
                        f"unless the runtime channel map assigns pixels to them",
                detail="Kconfig.projbuild:192-209, led_strip.c:134-143"))

        # v2 '-' substitution reads a snapshot of the LOWEST set bit only
        # (config_parser.c:2651-2676), so a multi-bit mask with divergent live
        # values collapses to the low bit's value.
        low = bits[0]
        eff: dict[str, float] = {}
        for fname, bit in (("freq", LED_SET_FREQ), ("duty", LED_SET_DUTY),
                           ("bright", LED_SET_BRIGHT), ("r", LED_SET_R),
                           ("g", LED_SET_G), ("b", LED_SET_B)):
            if led.present & bit:
                eff[fname] = led.cell(fname).value
            else:
                eff[fname] = self._live_led(low, fname, t_state_s)
                if fname == "freq" and eff[fname] <= 0.0:
                    self.lints.append(Finding(
                        code="LEDC_DASH_FREQ_STOPS", severity="warning",
                        domain="led", line_no=led.line_no, t_ms=float(led.time_ms),
                        message=f"line {led.line_no} leaves freq as '-' but "
                                f"channel {low + 1} has no live rate, so it "
                                f"resolves to 0 = STOP FLICKER (LEDs off)",
                        detail="config_parser.c:2651-2676, 2729-2736"))

        # config_parser.c:2729-2736 -- freq <= 0 means STOP FLICKER, not DC-on.
        #
        # The stop clears ONLY led_state / led_dirty / active
        # (led_matrix_example.c:1532-1545). frequency_milliHz, duty_cycle,
        # brightness and red/green/blue are RETAINED, and those are exactly what
        # led_matrix_get_snapshot (:2184) reports and what config_parser.c:2667
        # substitutes for a '-' column. So a session that stops a channel and
        # later re-enables it with '-' in the freq column restarts at the
        # RETAINED rate on the device. Zeroing `freq` here made the model resolve
        # that '-' to 0 = stop, keep the LEDs modelled dark, and then report the
        # correctly-flickering channel as "never turned on".
        if eff["freq"] <= 0.0:
            for b in bits:
                # WHAT SURVIVES A STOP IS THE LIVE VALUE, NOT THE RAMP'S START.
                # The firmware publishes the interpolated value into
                # s->frequency_milliHz / duty_cycle / brightness / red|green|blue
                # at every cycle boundary (led_matrix_example.c:893-904) and the
                # stop clears only led_state / led_dirty / active (:1532-1545),
                # so a channel interrupted mid-ramp retains where the ramp had
                # GOT TO -- which, since a bit's sweep always ends at the next
                # entry on that bit and that entry is this stop, is the ramp's
                # END. Freezing st at the sweep's v0 (which is what clearing
                # led_sweeps without snapshotting did) left a later '-' column
                # reading the pre-ramp value: on a duty ramp 20 -> 90 ending at
                # the stop, the model said 20% and the device 90%, against an
                # 8-point DUTY threshold.
                live = {f: self._live_led(b, f, t_state_s)
                        for f in ("freq", "duty", "bright", "r", "g", "b")}
                self.led_state[b].update(active=False, **live)
                self.led_sweeps[b] = {}
                self.led_mods[b] = {}
                self._apply_led_pulse_fields(b, led, t_state_s)
                self._push_led_kf(b, t_state_s)
                self.events.append(dict(kind="led_stop", channel=b + 1,
                                        t_ms=float(led.time_ms),
                                        t_eff_ms=t_state_s * 1000.0, fresh=False,
                                        line_no=led.line_no))
            self._reset_tick_if_idle(t_state_s)
            return

        # RANGE REJECTION IS PATH-DEPENDENT, and getting that wrong inverts the
        # whole rest of the channel's timeline.
        #
        # The freq>100 / duty>100 / bright>100 guards exist ONLY in
        # led_matrix_start_flicker_masked (led_matrix_example.c:1314-1325) and
        # led_matrix_update_flicker_params_masked (:1588-1594) -- the no-sweep
        # "orphan" path. led_matrix_start_sweep_masked (:1920-1939) validates
        # nothing except duration_ms != 0 and init_freq_milliHz != 0, and
        # config_parser.c:2836-2896 routes a bit to the sweep path whenever any
        # of its fields carries `>` or `*` and the bit has a later entry. So an
        # out-of-range RAMPED entry DOES take effect on the device: it re-anchors
        # and flickers at the out-of-range rate.
        #
        # Modelling it as a no-op meant every later event on the channel was
        # compared against the wrong state, and LEDC_LED_REJECTED was itself a
        # false positive.
        reject = []
        if eff["freq"] > dm.LED_FREQ_MAX_HZ:
            reject.append(f"freq {eff['freq']:.1f} Hz > 100 Hz")
        if eff["duty"] > 100:
            reject.append(f"duty {int(eff['duty'])}% > 100%")
        if eff["bright"] > 100:
            reject.append(f"bright {int(eff['bright'])}% > 100%")

        # The sweep bucketing: each bit's ramp window aligns to ITS OWN next
        # entry (config_parser.c:2796-2901). Bits with no next entry, or whose
        # bucket has no ramped field, take the immediate flicker path.
        for b in bits:
            st = self.led_state[b]
            was_active = st["active"]
            nxt = self._next_led_for_bit(si, 1 << b)

            takes_sweep = nxt is not None and (
                any(is_ramp(led.cell(f).interp)
                    for f in ("freq", "duty", "bright", "r", "g", "b"))
                or any((led.present & bit) and (nxt.present & bit)
                       and is_ramp(led.cell(f).interp)
                       for f, bit in (("phase", LED_SET_PHASE),
                                      ("attack", LED_SET_ATTACK))))

            if reject and not takes_sweep:
                self.lints.append(Finding(
                    code="LEDC_LED_REJECTED", severity="error", domain="led",
                    line_no=led.line_no, t_ms=float(led.time_ms), channel=b + 1,
                    message=f"line {led.line_no} (t={led.time_ms} ms, ch{b + 1}) "
                            f"is REJECTED by the LED engine "
                            f"({'; '.join(reject)}) -- this entry does nothing "
                            f"at all and the channel keeps doing whatever it "
                            f"was doing. (It is NOT rejected on the sweep path: "
                            f"give the entry a `>` field and it takes effect.)",
                    detail="led_matrix_example.c:1314-1325, 1588-1594; the "
                           "sweep path at :1920-1939 validates nothing"))
                # Pulse fields still land: config_parser.c applies env/phase/
                # attack/jitter in BOTH branches, outside the rejected call.
                # SO DOES THE COLOUR: config_parser.c:2951 calls
                # led_matrix_set_flicker_color_masked(orphan_mask, led->r,
                # led->g, led->b) UNCONDITIONALLY, before the guarded
                # start/update call, and that setter has no range check of its
                # own (main/led_matrix_example.c:1665-1683).
                for _cf, _cbit in (("r", LED_SET_R), ("g", LED_SET_G),
                                   ("b", LED_SET_B)):
                    if led.present & _cbit:
                        st[_cf] = led.cell(_cf).value
                self._apply_led_pulse_fields(b, led, t_state_s)
                self._push_led_kf(b, t_state_s)
                continue
            if reject:
                self.lints.append(Finding(
                    code="LEDC_LED_OUT_OF_RANGE", severity="warning", domain="led",
                    line_no=led.line_no, t_ms=float(led.time_ms), channel=b + 1,
                    message=f"line {led.line_no} (t={led.time_ms} ms, ch{b + 1}) "
                            f"is out of the documented range "
                            f"({'; '.join(reject)}) but carries a ramp, so it "
                            f"takes the SWEEP path, which validates nothing and "
                            f"executes it anyway.",
                    detail="led_matrix_example.c:1920-1939 vs :1314-1325"))

            self.led_mods[b] = {}
            self.led_sweeps[b] = {}

            # Periodic modulations are mask-wide and self-contained.
            for fname, bit in (("freq", LED_SET_FREQ), ("duty", LED_SET_DUTY),
                               ("bright", LED_SET_BRIGHT), ("r", LED_SET_R),
                               ("g", LED_SET_G), ("b", LED_SET_B),
                               ("phase", LED_SET_PHASE), ("attack", LED_SET_ATTACK)):
                if not (led.present & bit):
                    continue
                cell = led.cell(fname)
                if is_modulation(cell.interp):
                    self.led_mods[b][fname] = Mod(
                        wave=cell.interp, start=cell.value, end=cell.mod_end,
                        period_s=cell.mod_period_ms / 1000.0, t0_s=t_state_s)

            ramped = []
            if nxt is not None:
                dur_s = (nxt.time_ms - led.time_ms) / 1000.0
                for fname, bit in (("freq", LED_SET_FREQ), ("duty", LED_SET_DUTY),
                                   ("bright", LED_SET_BRIGHT), ("r", LED_SET_R),
                                   ("g", LED_SET_G), ("b", LED_SET_B)):
                    cell = led.cell(fname)
                    if not is_ramp(cell.interp):
                        continue
                    # Same ramp-to-zero trap as audio: no present-bit check on
                    # the target (config_parser.c:2837-2872).
                    target = nxt.cell(fname).value if (nxt.present & bit) else 0.0
                    # On an ALREADY-RUNNING channel the ramp starts from the LIVE
                    # value, not from the file literal
                    # (led_matrix_example.c:1978-1996).
                    #
                    # This is the SAME rule the audio engine follows, not an
                    # asymmetry -- an earlier version of this comment claimed it
                    # was, which is why the audio path above was left modelling
                    # the literal. audio_generator_start_sweep_locked
                    # (audio_generator.c:519-551) substitutes ch->current_freq /
                    # current_amp / current_pan / current_mod_freq on an active
                    # channel, and its own comment reads "This mirrors the
                    # LED-side fix in led_matrix_start_sweep_masked". The one
                    # real difference is on a FRESH channel, where the audio
                    # engine has already zeroed current_amp before the sweep is
                    # armed, so a first-entry volume ramp starts from silence.
                    v0 = (self._live_led(b, fname, t_state_s) if was_active
                          else cell.value)
                    self.led_sweeps[b][fname] = Sweep(
                        v0=v0, v1=target, curve=cell.interp,
                        t0_s=t_anchor_s, dur_s=dur_s)
                    ramped.append(fname)
                # phase / attack require the present bit on BOTH ends
                # (config_parser.c:2878, 2887) -- the correct rule.
                for fname, bit in (("phase", LED_SET_PHASE),
                                   ("attack", LED_SET_ATTACK)):
                    cell = led.cell(fname)
                    if not (led.present & bit) or not is_ramp(cell.interp):
                        continue
                    if not (nxt.present & bit):
                        continue
                    # LIVE start value on an already-running channel, exactly as
                    # for the core fields above: led_matrix_start_sweep_masked
                    # does `eff_phase_start = (spec->phase_curve !=
                    # LED_INTERP_NONE && already_active) ? (uint16_t)
                    # s->phase_offset_deg : spec->phase_start` and the same for
                    # s->attack_ms (main/led_matrix_example.c:2052-2056), under
                    # a comment reading "Like the params above, a running
                    # channel continues from its LIVE value so an interrupted
                    # ramp does not snap."
                    v0 = (self._live_led(b, fname, t_state_s) if was_active
                          else cell.value)
                    self.led_sweeps[b][fname] = Sweep(
                        v0=v0, v1=nxt.cell(fname).value, curve=cell.interp,
                        t0_s=t_anchor_s, dur_s=dur_s)
                    ramped.append(fname)
            else:
                orphan = [f for f in _LED_FIELDS if is_ramp(led.cell(f).interp)]
                if orphan:
                    self.lints.append(Finding(
                        code="LEDC_ORPHAN_RAMP", severity="info", domain="led",
                        line_no=led.line_no, t_ms=float(led.time_ms),
                        channel=b + 1,
                        message=f"LED ch{b + 1} at t={led.time_ms} is the LAST "
                                f"entry on its channel, so its ramp on "
                                f"{', '.join(orphan)} does nothing -- it holds.",
                        detail="config_parser.c:2800-2802"))

            # Write the stepped values. A ramped field takes its start value
            # here; a non-ramped field takes the literal.
            for fname in ("freq", "duty", "bright", "r", "g", "b"):
                if fname in self.led_sweeps[b]:
                    st[fname] = self.led_sweeps[b][fname].v0
                else:
                    st[fname] = eff[fname]
            self._apply_led_pulse_fields(b, led, t_state_s)

            # THE ISR TICK IS SIZED FROM THE SPEC LITERAL, not from the live
            # value the interpolator ramps from:
            #   init_freq_milliHz = (spec->freq_curve != LED_INTERP_NONE)
            #                       ? spec->freq_milliHz_start
            #                       : spec->freq_milliHz_target
            # (led_matrix_example.c:1936-1938 -> s_ensure_timer_and_task at
            # :1941), and both of those are filled from `led->frequency`
            # (config_parser.c:2838 and the non-swept fill at :2905-2907), i.e.
            # from eff["freq"]. Taking st["freq"] instead read the sweep's v0 --
            # the LIVE value -- so a channel already running at 10 Hz that gets
            # a `>40` entry kept a 2500 Hz tick in the model and expected
            # 39.68 Hz, while the device raises the tick to 10000 Hz and emits
            # 40.000 Hz. A 0.32 Hz error reported against a correct device, on
            # exactly the sub-Hz claim this instrument exists to make.
            #
            # And the tick only rises where the firmware actually calls
            # s_ensure_timer_and_task: the sweep path (:1941) and
            # start_flicker_masked (:1331). update_flicker_params_masked
            # (:1585-1623) does NOT, so a rate RAISE delivered on the no-sweep
            # path to an already-running channel keeps the old tick.
            if takes_sweep or not was_active:
                self._raise_tick(eff["freq"], t_state_s)

            st["active"] = True
            if not was_active:
                st["started"] = True
                st["vis_from"] = t_anchor_s
                self.led_visible_from[b] = t_anchor_s
            self._push_led_kf(b, t_state_s)
            self.events.append(dict(
                kind="led_start" if not was_active else "led_update",
                channel=b + 1, t_ms=float(led.time_ms),
                # Fresh activation is anchored to demanded+46.439 ms by design;
                # an update on a running channel takes effect at dispatch.
                t_eff_ms=(t_anchor_s * 1000.0 if not was_active
                          else se.t_dispatch_ms),
                fresh=not was_active, line_no=led.line_no))

    def _apply_led_pulse_fields(self, b: int, led: LedEntry, t_s: float):
        st = self.led_state[b]
        if led.present & LED_SET_ENV:
            st["env"] = led.env
        if led.present & LED_SET_PHASE:
            st["phase"] = led.phase.value
        if led.present & LED_SET_ATTACK:
            # The SETTER caps it: `if (ms > 60u) ms = 60u`
            # (main/led_matrix_example.c:1497). The sweep path does not.
            st["attack"] = min(60.0, led.attack.value)
        if led.present & LED_SET_JITTER:
            st["jitter_amp"] = led.jitter_amp_hz
            st["jitter_period"] = led.jitter_period_ms

    def _next_led_for_bit(self, si: int, bitmask: int) -> LedEntry | None:
        # See _next_audio: find_next_led_for_bit scans the raw sorted array, so
        # a batch-capped entry still supplies a ramp target.
        for k in range(si + 1, len(self.sched)):
            s = self.sched[k]
            if s.kind == "led" and (s.raw.mask & bitmask):
                return s.raw
        return None


# ---------------------------------------------------------------------------
# Dense sampling
# ---------------------------------------------------------------------------


def _sample_channel(kfs: list[Keyframe], grid: np.ndarray,
                    fields: Iterable[str]) -> dict[str, np.ndarray]:
    out = {f: np.full(grid.size, np.nan) for f in fields}
    out["_active"] = np.zeros(grid.size, dtype=bool)
    out["_env"] = np.zeros(grid.size, dtype=np.int16)
    out["_wave"] = np.zeros(grid.size, dtype=np.int16)
    if not kfs:
        return out
    times = [kf.t_s for kf in kfs]
    for i, kf in enumerate(kfs):
        lo = bisect.bisect_left(grid, kf.t_s)
        hi = bisect.bisect_left(grid, times[i + 1]) if i + 1 < len(kfs) else grid.size
        if hi <= lo:
            continue
        seg = grid[lo:hi]
        for f in fields:
            out[f][lo:hi] = kf.value(f, seg)
        act = bool(kf.state["active"])
        # Per-activation anchor wait (LED only; audio has no such field).
        vis = kf.state.get("vis_from")
        if act and vis is not None:
            out["_active"][lo:hi] = seg >= vis
        else:
            out["_active"][lo:hi] = act
        out["_env"][lo:hi] = int(kf.state.get("env", 0))
        out["_wave"][lo:hi] = int(kf.state.get("wave", 0))
    return out


def _tick_series(tick_timeline: list[tuple[float, int]], grid: np.ndarray) -> np.ndarray:
    out = np.full(grid.size, float(dm.LED_FLICKER_TICK_MIN))
    for t_s, hz in tick_timeline:
        out[grid >= t_s] = float(hz)
    return out


def build_model(entries: list, *, led_backend: str = dm.DEFAULT_LED_BACKEND):
    """Run the state machine over already-parsed entries.

    Exposed so synth.py can render at the audio sample rate from the SAME state
    machine the expectation uses, and so it can first mutate the entry list to
    inject a fault. The alternative -- synth replaying the 20 Hz sampled series
    -- would hide any error in the keyframe evaluation from the test suite.
    """
    pr = ParseResult(entries=entries, bg=None, lints=[], dropped_over_cap=0,
                     source="<in-memory>")
    sched, lints = build_schedule(pr)
    sim = _Simulator(sched, led_backend)
    sim.run()
    return sched, sim, lints


def tick_at(tick_timeline: list[tuple[float, int]], t_s: float) -> int:
    """ISR tick rate in force at `t_s` (the rate only ever rises)."""
    hz = dm.LED_FLICKER_TICK_MIN
    for t0, v in tick_timeline:
        if t_s >= t0:
            hz = v
    return hz


def build_expectation(path: str, *, grid_hz: float = DEFAULT_GRID_HZ,
                      keep_speech: bool = False,
                      duration_s: float | None = None,
                      led_backend: str = dm.DEFAULT_LED_BACKEND) -> Expectation:
    pr = parse_ledc(path, keep_speech=keep_speech)
    sched, sched_lints = build_schedule(pr)
    sim = _Simulator(sched, led_backend)
    sim.run()

    last_ms = max((s.t_demanded_ms for s in sched), default=0)
    if duration_s is None:
        duration_s = last_ms / 1000.0 + 2.0
    grid = make_grid(duration_s, grid_hz)
    tick = _tick_series(sim.tick_timeline, grid)

    # -- light ----------------------------------------------------------
    light: dict[int, ExpectedLight] = {}
    for b in range(dm.NUM_LED_CHANNELS):
        if not sim.led_kfs[b]:
            continue
        s = _sample_channel(sim.led_kfs[b], grid,
                            ("freq", "duty", "bright", "r", "g", "b", "phase"))
        demanded = np.nan_to_num(s["freq"], nan=0.0)
        # `_active` already honours the per-activation anchor wait.
        active = s["_active"]
        emitted = dm.quantized_flicker_hz_vec(demanded, tick)
        light[b + 1] = ExpectedLight(
            channel=b + 1, t=grid, active=active, demanded_hz=demanded,
            emitted_hz=emitted,
            duty_pct=np.floor(np.nan_to_num(s["duty"], nan=0.0)),
            bright_pct=np.floor(np.nan_to_num(s["bright"], nan=0.0)),
            r=np.floor(np.nan_to_num(s["r"], nan=0.0)),
            g=np.floor(np.nan_to_num(s["g"], nan=0.0)),
            b=np.floor(np.nan_to_num(s["b"], nan=0.0)),
            env=s["_env"], phase_deg=np.nan_to_num(s["phase"], nan=0.0),
            tick_hz=tick)

    # -- audio ----------------------------------------------------------
    audio: dict[int, ExpectedAudioChannel] = {}
    for ch in range(dm.NUM_AUDIO_CHANNELS):
        if not sim.aud_kfs[ch]:
            continue
        s = _sample_channel(sim.aud_kfs[ch], grid,
                            ("freq", "pan", "vol", "mod", "duty", "phase"))
        freq_l = np.nan_to_num(s["freq"], nan=0.0)
        # freq_r is stepped only (not sweepable: config_parser.c:1627-1631) and
        # it is the ABSOLUTE right-ear frequency, including while the left
        # carrier is being swept, so SWEEPING THE CARRIER MOVES THE BEAT.
        #
        # This model used to preserve the DETUNE instead
        # (current_freq + (frequency_r - frequency)), which is what the firmware
        # did until main/audio_generator.c:995-1013 was changed: the comment
        # there now reads "freq_r is the ABSOLUTE right-ear frequency, so
        # sweeping the left carrier moves the BEAT. This used to hold the
        # detune constant instead ... Nothing wanted the old behaviour: every
        # carrier-sweep line in the shipped library documents an intended beat
        # change". Verified on hardware: carrier 200->203 with freqR=210 glides
        # the beat 9.847 -> 7.000 Hz with freqR pinned at 210.000.
        fr_state = np.zeros(grid.size)
        kfs = sim.aud_kfs[ch]
        for i, kf in enumerate(kfs):
            lo = bisect.bisect_left(grid, kf.t_s)
            hi = (bisect.bisect_left(grid, kfs[i + 1].t_s) if i + 1 < len(kfs)
                  else grid.size)
            if hi > lo:
                fr_state[lo:hi] = kf.state["freq_r"]
        binaural = (fr_state > 0.0) & (np.abs(fr_state - freq_l) > 1e-9)
        freq_r = np.where(binaural, fr_state, freq_l)
        beat = np.where(binaural, np.abs(freq_r - freq_l), 0.0)
        amp = np.nan_to_num(s["vol"], nan=0.0) / 100.0
        pan = np.nan_to_num(s["pan"], nan=0.0) / 100.0
        # audio_generator.c:1995-2006 equal-power pan law, BYPASSED entirely for
        # a single-channel binaural (audio_generator.c:952-953, 1388-1419).
        ang = (np.clip(pan, -1.0, 1.0) + 1.0) * (np.pi / 4.0)
        gl = np.where(binaural, amp, amp * np.cos(ang))
        gr = np.where(binaural, amp, amp * np.sin(ang))
        audio[ch] = ExpectedAudioChannel(
            channel=ch, t=grid, active=s["_active"], freq_l_hz=freq_l,
            freq_r_hz=freq_r, beat_hz=beat, amp=amp, pan=pan,
            pulse_hz=np.nan_to_num(s["mod"], nan=0.0),
            pulse_duty_pct=np.nan_to_num(s["duty"], nan=50.0),
            env=s["_env"], wave=s["_wave"], gain_l=gl, gain_r=gr)

    mix = _build_mix(audio, grid)

    lints = list(pr.lints) + sched_lints + sim.lints
    # Events are sorted by the time they become OBSERVABLE, not by the demanded
    # time, because that is the order the comparator will find them in the
    # recording (first-batch hoisting can reorder them).
    events = sorted(sim.events, key=lambda e: e["t_eff_ms"])
    return Expectation(source=path, grid_hz=grid_hz, t=grid, schedule=sched,
                       light=light, audio=audio, mix=mix, bg=pr.bg, lints=lints,
                       duration_s=duration_s, led_backend=led_backend,
                       events=events)


def _build_mix(audio: dict[int, ExpectedAudioChannel], grid: np.ndarray) -> ExpectedMix:
    """Collapse the 16 generator slots into what a stereo recording can see.

    A recording cannot separate slots -- they all sum into one DAC pair. So we
    report the DOMINANT tone per ear and leave it NaN whenever no single slot is
    clearly loudest, because an estimator that returns a number there would be
    measuring an arbitrary spectral peak and calling it the carrier.
    """
    n = grid.size
    dom_l = np.full(n, np.nan)
    dom_r = np.full(n, np.nan)
    beat = np.full(n, np.nan)
    pulse = np.full(n, np.nan)
    rms = np.zeros(n)
    nact = np.zeros(n, dtype=np.int16)
    domch = np.full(n, -1, dtype=np.int16)
    if not audio:
        return ExpectedMix(t=grid, dominant_l_hz=dom_l, dominant_r_hz=dom_r,
                           beat_hz=beat, pulse_hz=pulse, rms_rel=rms,
                           active=np.zeros(n, dtype=bool), n_active=nact,
                           dominant_channel=domch,
                           coherent_pair=np.zeros(n, dtype=bool))

    chans = sorted(audio)
    GL = np.stack([audio[c].gain_l for c in chans])
    GR = np.stack([audio[c].gain_r for c in chans])
    FL = np.stack([audio[c].freq_l_hz for c in chans])
    FR = np.stack([audio[c].freq_r_hz for c in chans])
    BT = np.stack([audio[c].beat_hz for c in chans])
    PM = np.stack([audio[c].pulse_hz for c in chans])
    AC = np.stack([audio[c].active for c in chans])

    audible = AC & ((GL > 1e-4) | (GR > 1e-4))
    GLm = np.where(audible, GL, 0.0)
    GRm = np.where(audible, GR, 0.0)
    nact = audible.sum(axis=0).astype(np.int16)
    # Sum of independent sinusoid powers; relative scale only (the absolute
    # level depends on settings.audio_max_volume, which a WAV cannot reveal).
    rms = np.sqrt((GLm ** 2 + GRm ** 2).sum(axis=0) / 2.0)

    il = GLm.argmax(axis=0)
    ir = GRm.argmax(axis=0)
    idx = np.arange(n)
    gl_best = GLm[il, idx]
    gr_best = GRm[ir, idx]
    # "Dominant" means it carries most of the power in that ear. 2x in amplitude
    # (6 dB) over the runner-up is the bar; below that, an FFT peak could be
    # either tone and the comparison would be meaningless.
    def _runner_up(G, best_i):
        G2 = G.copy()
        G2[best_i, idx] = -1.0
        return G2.max(axis=0)

    ru_l = _runner_up(GLm, il) if GLm.shape[0] > 1 else np.zeros(n)
    ru_r = _runner_up(GRm, ir) if GRm.shape[0] > 1 else np.zeros(n)
    ok_l = (gl_best > 1e-4) & (gl_best > 2.0 * np.maximum(ru_l, 0.0))
    ok_r = (gr_best > 1e-4) & (gr_best > 2.0 * np.maximum(ru_r, 0.0))
    dom_l[ok_l] = FL[il, idx][ok_l]
    dom_r[ok_r] = FR[ir, idx][ok_r]
    domch[ok_l] = np.array(chans, dtype=np.int16)[il][ok_l]
    both = ok_l & ok_r & (il == ir)
    beat[both] = BT[il, idx][both]
    pulse[ok_l] = PM[il, idx][ok_l]

    # COHERENT-PAIR DETECTION. `rms` above is an incoherent power sum, which is
    # correct only while the slots' carriers are far enough apart for their
    # cross-term to average out inside a measurement cell. Two slots on
    # near-identical carriers in the SAME ear interfere instead, and the real
    # level swings through deep nulls with period 1/df.
    #
    # This is the common two-channel panned binaural idiom, not a corner case:
    # 04_meditation_theta runs ch1 pan -30 and ch2 pan +30 with both left-ear
    # carriers at 250 Hz ramping to 254 / 253.7, so df goes 0 -> 0.3 Hz, and a
    # clean render produced "[WARN] AMPLITUDE: audio level was -8.1 dB from the
    # expected profile" over 56 s. LEDC_BEAT_NOT_SWEPT actively RECOMMENDS this
    # idiom, so it will keep recurring.
    coh = np.zeros(n, dtype=bool)
    if len(chans) > 1:
        for F, G in ((FL, GLm), (FR, GRm)):
            for i in range(len(chans)):
                for j in range(i + 1, len(chans)):
                    gi, gj = G[i], G[j]
                    loud = (gi > 1e-4) & (gj > 1e-4)
                    if not loud.any():
                        continue
                    # Only matters when the two are within ~10 dB of each other;
                    # a cross-term 20 dB down cannot move a 3 dB threshold.
                    with np.errstate(divide="ignore", invalid="ignore"):
                        ratio = np.where(loud, gi / np.maximum(gj, 1e-12), 1.0)
                    comparable = loud & (ratio > 0.3) & (ratio < 3.0)
                    close = np.abs(F[i] - F[j]) < dm.THRESHOLD_COHERENT_CARRIER_HZ
                    coh |= comparable & close
    return ExpectedMix(t=grid, dominant_l_hz=dom_l, dominant_r_hz=dom_r,
                       beat_hz=beat, pulse_hz=pulse, rms_rel=rms,
                       active=nact > 0, n_active=nact, dominant_channel=domch,
                       coherent_pair=coh)


# ---------------------------------------------------------------------------
# Human-readable dump -- the real defence against a wrong model
# ---------------------------------------------------------------------------


def dump_expectation(exp: Expectation, every_s: float = 60.0) -> str:
    """Print the demanded model so a human can eyeball it against the file.

    Unit tests cannot prove the model matches the firmware; only a human who
    knows the session can. So make it cheap for them to look.
    """
    out: list[str] = []
    out.append(f"# expectation for {os.path.basename(exp.source)}")
    out.append(f"#   {len(exp.schedule)} scheduled entries, "
               f"{exp.duration_s:.1f} s, grid {exp.grid_hz:g} Hz, "
               f"LED backend {exp.led_backend}")
    if exp.bg:
        out.append(f"#   BG {exp.bg.url} pan={exp.bg.pan:+.2f} "
                   f"loudness={exp.bg.loudness:.2f}")
    out.append("")
    out.append("## schedule")
    for se in exp.schedule:
        flag = "" if se.executed else f"  <<< DROPPED ({se.drop_reason})"
        hoist = (f"  [hoisted to {se.t_dispatch_ms}]"
                 if se.t_dispatch_ms != se.t_demanded_ms else "")
        out.append(f"  t={se.t_demanded_ms:>9} ms  {se.kind:<6} "
                   f"line {se.line_no:>3}{hoist}{flag}")
    step = max(1, int(round(every_s * exp.grid_hz)))
    out.append("")
    out.append("## light (sampled)")
    for ch, L in sorted(exp.light.items()):
        out.append(f"  channel {ch}:")
        for i in range(0, L.t.size, step):
            if not L.active[i]:
                out.append(f"    {L.t[i]:>9.1f}s  off")
            else:
                out.append(
                    f"    {L.t[i]:>9.1f}s  {L.demanded_hz[i]:7.3f} Hz demanded"
                    f" -> {L.emitted_hz[i]:7.3f} Hz emitted"
                    f"  duty {L.duty_pct[i]:3.0f}%  bright {L.bright_pct[i]:3.0f}%"
                    f"  rgb({L.r[i]:.0f},{L.g[i]:.0f},{L.b[i]:.0f})"
                    f"  env {L.env[i]}  phase {L.phase_deg[i]:.0f}")
    out.append("")
    out.append("## audio (sampled)")
    for ch, A in sorted(exp.audio.items()):
        out.append(f"  channel {ch}:")
        for i in range(0, A.t.size, step):
            if not A.active[i]:
                out.append(f"    {A.t[i]:>9.1f}s  inactive")
            else:
                out.append(
                    f"    {A.t[i]:>9.1f}s  L {A.freq_l_hz[i]:9.3f} Hz"
                    f"  R {A.freq_r_hz[i]:9.3f} Hz  beat {A.beat_hz[i]:6.3f} Hz"
                    f"  amp {A.amp[i]:5.3f}  pulse {A.pulse_hz[i]:6.3f} Hz"
                    f"  duty {A.pulse_duty_pct[i]:5.1f}%  env {A.env[i]}"
                    f"  wave {A.wave[i]}")
    if exp.lints:
        out.append("")
        out.append("## static .ledc findings")
        for f in exp.lints:
            out.append(f"  [{f.severity}] {f.code}: {f.message}")
    return "\n".join(out)
