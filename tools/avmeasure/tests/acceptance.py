#!/usr/bin/env python3
"""avmeasure ACCEPTANCE HARNESS -- the bar the tool is actually judged against.

WHY THIS FILE EXISTS
--------------------
Rounds 1 and 2 of the avmeasure fix effort both ended in FAIL, and both failed
the same way: the fixer verified on tests/selftest.ledc plus a couple of
hand-picked sessions, declared success with real measured numbers, and then
adversaries rendered FULL-LENGTH captures of many different SHIPPED sessions
and immediately found both error-severity false positives on clean recordings
and silent misses on real faults. Round 2 fixed 44 issues and still produced
error-severity findings on 14 of 41 clean shipped sessions, plus silence on a
genuine 0.9%-slow flicker rate.

So the defect was in the PROCESS: there was no acceptance gate that matched how
the tool gets judged. This file is that gate. It is deliberately written BEFORE
any analyzer bug is fixed, so that "done" means the same thing to the fixer and
to the adversaries, and so that the starting point is on the record.

THE TWO GATES
-------------
A. CLEAN SWEEP -- the cry-wolf gate.
   Every .ledc in sessions/library, sync-marked with the shipped marker.py,
   rendered at FULL LENGTH by the shipped renderer, analysed through the
   shipped CLI. Zero error-severity findings required. Then the same sweep
   again with RECORDER imperfections that are not device faults (crystal
   error, broadband noise, an AC-coupled input, a DC offset) -- also zero.

B. FAULT SWEEP -- the silent-miss gate.
   One injected fault per case, across every fault class synth.py supports and
   across magnitudes chosen to include round 2's actual misses: both signs of a
   sub-1% rate error, lateness far outside any search window, and faults late
   in a long session. Each must be DETECTED, with the right magnitude, and the
   PROCESS EXIT CODE must be non-zero.

HOW THE TWO HALVES ARE COUNTED (read this before quoting a number)
------------------------------------------------------------------
* "error" means a finding whose severity is "error" AND whose code does not
  start with LEDC_. The LEDC_* codes are STATIC lints about the session file,
  reproducible by `analyze.py lint` with no recording at all; the shipped
  sessions genuinely contain some of what they name. They are counted and
  reported separately, because they also reach `$?` and so they do matter --
  see the LEDC_BEAT_NOT_SWEPT headline, which the firmware delta made stale.
* `drift`, `noise` and `ac-couple` are RECORDER-domain injections. synth.py's
  own docstrings say so ("this is what a real recorder does and is NOT a
  device bug", synth.py:56-59; ac-couple at :97-99). Asserting that those must
  raise an error would directly contradict gate A's imperfection variant, so
  for those three the assertion is inverted: zero errors AND the recorder-side
  quantity must be recovered with the right magnitude. They are reported in
  their own bucket and are NOT part of the "injected faults not detected"
  headline. This is a deliberate reading of the brief and is called out in the
  report so nobody has to guess.

WHAT THIS HARNESS CANNOT PROVE
------------------------------
synth.py renders from the SAME keyframe state machine that ledc_expect.py
builds the expectation from. So a round trip here validates observe_wav.py and
compare.py but is blind, by construction, to a shared misunderstanding of the
firmware (round 2's ISR-tick and phase-column findings were exactly that, and
all 82 unit tests were blind to both). Model fidelity still rests on reading
the C. Do not read a green run here as "the model is right".

RUNNING IT
----------
    python3 tests/acceptance.py                  # both gates, full length
    python3 tests/acceptance.py --mode faults    # the silent-miss gate only
    python3 tests/acceptance.py --mode clean --sessions rep
    python3 tests/acceptance.py --jobs 4 --max-ms 180000   # fast, NOT the gate

Exit code: 0 only if both gates pass. 1 if either fails. 2 on a harness/input
problem. Deterministic: every render uses a fixed seed and every added noise
stream is seeded from the case name, so two runs of the same tree agree.

Caching: renders land in the scratchpad keyed by a fingerprint of the RENDER
side of the tool (synth/ledc_expect/timeline/devicemodel/wavio/marker) and
results are keyed additionally by the ANALYSIS side (compare/observe_wav/
analyze). Editing compare.py therefore re-analyses without re-rendering, which
is the loop this round will actually spend its time in.

WHAT IT COSTS (measured on an M1 Pro, 10 cores, while otherwise in use)
-----------------------------------------------------------------------
Rendering dominates and does NOT parallelise well: 4 concurrent renders of
heavy shipped sessions came to 8 MB/s of WAV in total, against 6.5 MB/s for one
alone. A full-length library is ~2400 minutes of audio = ~76 GB of 6-channel
16-bit WAV, so the complete clean sweep is a couple of hours of render whatever
--jobs says; the fault sweep adds ~24 GB. Analysis is roughly 5x cheaper than
render per byte, and the imperfection post-processing is ~8 s per GB.

Consequences, all of them already wired up:
  * --jobs defaults to a modest number because more does not help.
  * The WAV cache is capped (--budget-gb) and evicted least-recently-used, with
    a floor on free disk, because 76 GB does not fit everywhere. Nothing in it
    is irreplaceable.
  * --cached-only reports exactly what has run and NAMES what has not, so a
    sweep interrupted after an hour still yields a self-consistent, honestly
    labelled partial result instead of a shrunken gate.
  * The cache directory is marked .metadata_never_index: Spotlight was taking
    25% of a core to index gigabytes of WAV nobody will ever search.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass, field
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(TOOL))
LIB = os.path.join(REPO, "sessions", "library")

sys.path.insert(0, TOOL)

import numpy as np  # noqa: E402

import wavio  # noqa: E402

# Modules whose contents change what a RENDER looks like. The WAV cache key.
RENDER_MODULES = ("synth.py", "ledc_expect.py", "timeline.py", "devicemodel.py",
                  "wavio.py", "marker.py")
# Modules whose contents change what an ANALYSIS says. The result cache key.
ANALYZE_MODULES = ("compare.py", "observe_wav.py", "ledc_expect.py",
                   "timeline.py", "devicemodel.py", "wavio.py", "analyze.py")

SYNC_TONE = 3000.0
#: marker.py's own default; the marker burst is 600 ms + a 400 ms gap, so every
#: original timestamp moves +1000 ms and device t=0 is the marker's rising edge.
MARKER_SHIFT_MS = 1000


def _scratch_root() -> str:
    env = os.environ.get("AVM_ACCEPT_CACHE")
    if env:
        return env
    # The session scratchpad if we are running inside one, else a sibling of
    # the tool. Never inside the repo: these are gigabytes of WAV.
    for cand in (os.environ.get("CLAUDE_SCRATCHPAD"),):
        if cand and os.path.isdir(cand):
            return os.path.join(cand, "avmeasure_acceptance")
    return os.path.join(
        os.environ.get("TMPDIR", "/tmp").rstrip("/"), "avmeasure_acceptance")


# ---------------------------------------------------------------------------
# Session shape classification
#
# The brief requires the full-length set to cover every shape present in the
# library. Rather than assert that by hand (and be wrong when the library
# grows), tag every session from its own text and PRINT the coverage matrix, so
# a reader can check the claim instead of believing it.
# ---------------------------------------------------------------------------

SHAPE_TAGS = (
    "fade-in",          # a > or * ramp on brightness or volume
    "rate-glide",       # a > or * ramp on the LED frequency column
    "steady-iso",       # audio AM/isochronic rate > 0 held, no carrier ramp
    "binaural",         # freq_r set, so the channel is binaural
    "binaural-sweep",   # carrier ramped while freq_r is set
    "bright-lfo",       # ~a:b:period brightness breathing
    "phase",            # non-zero LED phase column
    "antiphase",        # LED phase column == 180
    "multi-mask",       # more than one distinct LED mask
    "noise-bed",        # audio wave column 5 (pink) or 6 (brown)
    "gamma40",          # a flicker or AM rate at or above 35 Hz
    "dc-lamp",          # freq 0 LED, i.e. a steady lamp with no flicker
    "env-smooth",       # env column 1 (sine) or 2 (triangle)
    "env-trap",         # env column 3 (trapezoid), usually with `attack`
    "rgb",              # the 8+ column LED form with a non-grey colour
    "speech",           # S rows (stripped before POST, but they shape the file)
    "ramp-to-zero",     # a > or * ramp whose target entry is zero
)


def _num(tok: str) -> float | None:
    try:
        return float(tok.lstrip(">*~").split(":")[0])
    except ValueError:
        return None


def classify(path: str) -> tuple[set[str], int]:
    """Return (shape tags, last timestamp in ms) for one .ledc."""
    tags: set[str] = set()
    masks: set[int] = set()
    last_ms = 0
    led_rows: list[list[str]] = []
    aud_rows: list[list[str]] = []
    for raw in open(path, errors="replace"):
        s = raw.split("#")[0].strip()
        if not s:
            continue
        t = s.split()
        if t[0] in ("BG", "bg"):
            continue
        if t[0] in ("S", "s"):
            tags.add("speech")
            continue
        if t[0] in ("A", "a"):
            if len(t) < 2:
                continue
            aud_rows.append(t[1:])
            v = _num(t[1])
            if v is not None:
                last_ms = max(last_ms, int(v))
            continue
        # glued audio form, e.g. "A1500 ..."
        if t[0][:1] in ("A", "a") and t[0][1:].isdigit():
            aud_rows.append([t[0][1:]] + t[1:])
            last_ms = max(last_ms, int(t[0][1:]))
            continue
        v = _num(t[0])
        if v is None:
            continue
        last_ms = max(last_ms, int(v))
        led_rows.append(t)

    # LED: time freq duty bright mask            (5 columns)
    #   or time freq duty bright R G B mask [env] [phase] [attack]
    for t in led_rows:
        if len(t) == 5:
            freq, duty, bright, mask = t[1], t[2], t[3], t[4]
            rgb, env, phase = None, None, None
        elif len(t) >= 8:
            freq, duty, bright = t[1], t[2], t[3]
            rgb = (t[4], t[5], t[6])
            mask = t[7]
            env = t[8] if len(t) > 8 else None
            phase = t[9] if len(t) > 9 else None
            if len(t) > 10:
                tags.add("env-trap")
        else:
            continue
        try:
            masks.add(int(mask))
        except ValueError:
            pass
        if freq[:1] in (">", "*"):
            tags.add("rate-glide")
        if bright[:1] in (">", "*"):
            tags.add("fade-in")
        if bright[:1] == "~":
            tags.add("bright-lfo")
        f = _num(freq)
        if f is not None and f >= 35:
            tags.add("gamma40")
        if f == 0 and freq[:1] not in (">", "*"):
            tags.add("dc-lamp")
        b = _num(bright)
        if b == 0 and bright[:1] not in (">", "*", "~"):
            tags.add("ramp-to-zero")
        if rgb and len(set(rgb)) > 1:
            tags.add("rgb")
        if env is not None:
            e = _num(env)
            if e in (1.0, 2.0):
                tags.add("env-smooth")
            if e == 3.0:
                tags.add("env-trap")
        if phase is not None:
            p = _num(phase)
            if p:
                tags.add("phase")
            if p == 180.0:
                tags.add("antiphase")
    if len(masks) > 1:
        tags.add("multi-mask")

    # Audio: time freq pan vol mod [ch] [freq_r] [wave]
    for t in aud_rows:
        freq = t[1] if len(t) > 1 else "0"
        vol = t[3] if len(t) > 3 else "0"
        mod = t[4] if len(t) > 4 else "0"
        fr = t[6] if len(t) > 6 else "0"
        wave = t[7] if len(t) > 7 else "0"
        if vol[:1] in (">", "*"):
            tags.add("fade-in")
        m = _num(mod)
        if m and m > 0:
            tags.add("steady-iso")
        if m and m >= 35:
            tags.add("gamma40")
        r = _num(fr)
        if r and r > 0:
            tags.add("binaural")
            if freq[:1] in (">", "*"):
                tags.add("binaural-sweep")
        w = _num(wave)
        if w in (5.0, 6.0):
            tags.add("noise-bed")
    return tags, last_ms


# ---------------------------------------------------------------------------
# The documented session sets
# ---------------------------------------------------------------------------

#: The documented representative subset, selected by SHAPE: it is what
#: `--sessions rep` runs when a full-length sweep of the whole library does not
#: fit (the brief allows a documented subset of at least 20 covering every shape
#: present; this is 27). The DEFAULT is still every .ledc in the library at full
#: length, because rendering short was exactly what let rounds 1 and 2 pass
#: their own tests. The shape coverage of whatever a run actually renders is
#: computed against the whole library and printed every time, and a shape
#: present in the library but missing from the run is called out, so the subset
#: cannot silently stop covering something.
REP_SESSIONS = (
    "01_sleep_onset",            # 25 min: brightness fade-in + 10->2 Hz glide
    "01_sleep_onset_RGB",        # the same, antiphase (phase 180) RGB banks
    "04_meditation_theta",       # two-slot panned binaural + brightness LFO
    "05_focus_smr",              # DC lamp (freq 0) + audio isochronic
    "07_genus_40hz",             # 60 min of 40 Hz gamma on four zones
    "09_lucid_hypnagogic",       # 60 min, the longest session in the library
    "11_lucid_wbtb",             # 12 min, 10->6 Hz glide via > ramps
    "13_astral_deep",            # 50 min
    "15_ganzflicker_imagery",    # env=1 sine carrier, 20 min
    "18_jhana_absorption",       # 35 min
    "20_gateway_focus10",        # ~a:b:period LFO handover, 30 min
    "21_gateway_focus12_SINE",   # sine carrier + LFO
    "21_gateway_focus12_TRAP",   # trapezoid carrier + attack + phase column
    "25_gateway_journey_TRAP",   # 40 min, trapezoid
    "26_alpha_breath",
    "28_noise_sleep_bed",        # pink-noise bed, steady (freq 0) light
    "29_noise_sleep_pulsed",     # noise bed with a trapezoid AM
    "30_splitfield_lucid",       # different rate per eye, masks 3/12/15
    "31_ganzfeld_amber",
    "32_hypnagogic_visions",
    "33_ego_dissolution_gamma",  # gamma
    "33_ego_dissolution_swap",
    "34_gateway_obe",
    "35_wbtb_lucid_gamma",       # masks 3/5/10/12/15
    "36_shamanic_trance_drum",   # ends with a > ramp into zero on every zone
    "90_measure_sync",           # the session the README tells you to record
    "91_measure_17_uniform",
)

#: Hosts for the fault sweep. Chosen for SHAPE and for LENGTH spread, and
#: deliberately NOT including the two 60-minute sessions: a fault case needs its
#: own render (the fault changes the signal), and 07_genus_40hz alone would cost
#: ~170 s of render per case. 27_genus_40hz_dim carries the 40 Hz gamma shape at
#: half the length, which is what the gamma fault cases actually need.
FAULT_HOSTS = {
    "90_measure_sync": "5 min, the README's own measurement session",
    "11_lucid_wbtb": "12 min, 10->6 Hz rate glide",
    "30_splitfield_lucid": "15 min, a different rate per eye (masks 3/12)",
    "04_meditation_theta": "20 min, two-slot panned binaural + brightness LFO",
    "01_sleep_onset": "25 min, brightness fade-in + 18-minute rate glide",
    "20_gateway_focus10": "30 min, ~a:b:period brightness LFO",
    "27_genus_40hz_dim": "30 min, 40 Hz gamma on four zones",
    "36_shamanic_trance_drum": "30 min, every zone ends on a ramp into zero",
}


# ---------------------------------------------------------------------------
# Recorder-imperfection profiles for gate A's second half.
#
# All of these are applied by POST-PROCESSING the cached clean render, which is
# exactly where a recorder sits in the chain (after the device) and keeps a
# variant case at ~1/4 the cost of a re-render. The crystal error is applied by
# retuning the WAV header rather than resampling the samples: a crystal error IS
# a mismatch between the rate the file was captured at and the rate it claims,
# and doing it this way injects ZERO signal artefact. Resampling by linear
# interpolation would have added a ~2% amplitude ripple at 3 kHz sweeping at
# 2.2 Hz, i.e. a synthetic AM that the PULSE_RATE detector would have been
# entitled to report -- the harness would have been manufacturing the fault it
# was testing for.
#
# The integer-sample-rate grid is 1/44100 = 22.7 ppm wide, so "+/-50 ppm"
# lands on +/-45.4 ppm. The achieved value is printed, not rounded away.
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Imperfection:
    name: str
    why: str
    ppm: float = 0.0            # crystal error, via header retune
    noise_dbfs: float | None = None   # broadband noise, all channels
    hp_hz: float = 0.0          # AC-coupled input, light channels only
    dc: float = 0.0             # constant offset, full-scale units


IMPERFECTIONS = (
    Imperfection("ppm+50", "recorder crystal fast by ~50 ppm", ppm=+50.0),
    Imperfection("ppm-50", "recorder crystal slow by ~50 ppm", ppm=-50.0),
    Imperfection("noise-60", "broadband noise at -60 dBFS (below any real "
                             "converter's floor)", noise_dbfs=-60.0),
    Imperfection("noise-40", "broadband noise at -40 dBFS (a realistic "
                             "photodiode front end)", noise_dbfs=-40.0),
    # MEASURED: a 5 Hz high-pass removes enough of the marker's optical step
    # that the solver drops to LOW confidence and DEMOTES every timing finding
    # to a warning. That is the honest behaviour the brief says to preserve, but
    # it also means this profile cannot prove anything about the detectors -- it
    # tests the demotion machinery. The report counts low-confidence cases
    # separately for exactly that reason, and `rig` deliberately leaves the
    # high-pass out so that the combined profile still exercises the detectors.
    Imperfection("ac5", "AC-coupled sensor input, one-pole high-pass at 5 Hz",
                 hp_hz=5.0),
    Imperfection("dc", "a small constant DC offset on every channel", dc=0.02),
    Imperfection("rig", "one plausible capture rig at once: +50 ppm, noise at "
                        "-45 dBFS and a DC offset (DC-coupled, see ac5)",
                 ppm=+50.0, noise_dbfs=-45.0, dc=0.015),
)


# ---------------------------------------------------------------------------
# Fault matrix for gate B
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class FaultCase:
    cls: str                 # fault class name, as --fault spells it
    host: str                # session basename
    spec: tuple[str, ...]    # literal --fault arguments, {T} resolved later
    expect: tuple[str, ...]  # finding codes, ANY of which counts as detection
    kind: str = "device"     # "device" (must error) or "recorder" (must not)
    pick: str | None = None  # entry-time selector for {T}
    mag: tuple[str, float, float] | None = None
    """(what, value, tol) -- the magnitude assertion. `what` is one of:
         delta      finding.observed - finding.expected, in the finding's unit
         ratio      finding.observed / finding.expected
         rel        (observed - expected) / expected
         diff_ppm   report.drift_differential_ppm
         common_ppm report.drift_common_ppm
         av_ms      report.av_offset_ms, minus the modelled pipeline term
         t_ms       finding.t_ms, against the resolved entry time
    """
    note: str = ""


def _fault_matrix() -> list[FaultCase]:
    """The fault cases, with WHY each magnitude was chosen.

    Every magnitude here is either (a) a round-1/round-2 actual miss, (b) just
    above a stated detection floor, or (c) far outside any search window. A
    matrix of comfortable mid-range magnitudes is what let two rounds pass
    their own tests and fail review.
    """
    C: list[FaultCase] = []

    # --- offset: a constant A/V desync. The tool's headline measurement. ---
    C += [
        FaultCase("offset", "27_genus_40hz_dim", ("offset:120",),
                  ("AV_CONST_OFFSET",), mag=("av_ms", 120.0, 25.0),
                  note="the README's own quick-start fault demo"),
        FaultCase("offset", "90_measure_sync", ("offset:45",),
                  ("AV_CONST_OFFSET", "AV_OFFSET_MARGINAL"),
                  mag=("av_ms", 45.0, 20.0),
                  note="just outside the 0..40 ms healthy band: the SMALL end"),
        FaultCase("offset", "90_measure_sync", ("offset:-80",),
                  ("AV_AUDIO_LEADS_LIGHT", "AV_CONST_OFFSET"),
                  mag=("av_ms", -80.0, 25.0),
                  note="audio LEADING light is physically impossible on a "
                       "healthy device, so it must never be absorbed"),
    ]

    # --- audio-drift: a device-side differential drift. ---
    C += [
        FaultCase("audio-drift", "27_genus_40hz_dim", ("audio-drift:60",),
                  ("AV_DIFFERENTIAL_DRIFT",), mag=("diff_ppm", 60.0, 20.0)),
        FaultCase("audio-drift", "01_sleep_onset", ("audio-drift:-40",),
                  ("AV_DIFFERENTIAL_DRIFT",), mag=("diff_ppm", -40.0, 18.0),
                  note="the NEGATIVE side: round 2 found a rate threshold that "
                       "discarded one sign entirely"),
    ]

    # --- drift: a RECORDER crystal error. Must be fitted out, not accused. ---
    C += [
        FaultCase("drift", "90_measure_sync", ("drift:50",), ("CLOCK_DRIFT_RECORDER",),
                  kind="recorder", mag=("common_ppm", 50.0, 20.0)),
        FaultCase("drift", "90_measure_sync", ("drift:-50",), ("CLOCK_DRIFT_RECORDER",),
                  kind="recorder", mag=("common_ppm", -50.0, 20.0)),
        FaultCase("drift", "04_meditation_theta", ("drift:300",),
                  ("CLOCK_DRIFT_RECORDER",), kind="recorder",
                  mag=("common_ppm", 300.0, 60.0),
                  note="far outside any crystal spec, still not a device fault"),
    ]

    # --- drop: an entry that never fires. ---
    #
    # ROUND-3 NOTE ON THESE `expect` LISTS. They name which finding CODES count
    # as having detected the fault, and they were written against the
    # vocabulary the tool had before the round-3 fix pass. Round 3 added codes
    # that report exactly these faults more precisely --
    # LIGHT_DARK_WHEN_DEMANDED ("this zone was dark for 5 s while the .ledc
    # demanded 20% brightness", decided from the normalised modulation depth so
    # no sensor gain can explain it) and AUDIO_LEVEL_ABSENT ("the level is 12 dB
    # under the demanded profile", four times down in amplitude) -- so they are
    # added here. The severity, exit-code and MAGNITUDE assertions are
    # untouched: the case still has to produce an error-severity finding, a
    # non-zero `$?`, and a timestamp within the stated tolerance of the entry.
    _DROP = ("EVENT_MISSING", "EVENT_NOT_LOCATED", "FLICKER_RATE",
             "BRIGHTNESS", "DUTY", "LIGHT_DARK_WHEN_DEMANDED",
             "AUDIO_LEVEL_ABSENT", "AUDIO_PEAK_SHORT",
             "BRIGHTNESS_MODULATION")
    C += [
        FaultCase("drop", "20_gateway_focus10", ("drop:{T}",), _DROP,
                  pick="first_body", mag=("t_ms", 0.0, 4000.0)),
        FaultCase("drop", "27_genus_40hz_dim", ("drop:{T}",), _DROP,
                  pick="mid", mag=("t_ms", 0.0, 6000.0)),
        FaultCase("drop", "36_shamanic_trance_drum", ("drop:{T}",),
                  _DROP + ("AMPLITUDE",), pick="late_body",
                  mag=("t_ms", 0.0, 8000.0),
                  note="late in a 30-minute session, where round 2's drift "
                       "machinery had the least leverage"),
        FaultCase("drop", "90_measure_sync", ("drop:0",),
                  ("EVENT_MISSING", "EVENT_NOT_LOCATED", "SYNC_LOW_CONFIDENCE",
                   "SYNC_FAILED", "CHANNEL_NEVER_ON"), pick="marker",
                  note="the marker burst itself never fires: round 2 found this "
                       "demotes itself to a warning and exits 0"),
    ]

    # --- late: the primary quantity the instrument exists to measure. ---
    C += [
        FaultCase("late", "90_measure_sync", ("late:{T}:60",), ("EVENT_LATE",),
                  pick="first_body", mag=("delta", 60.0, 40.0),
                  note="just above the stated lateness threshold: the SMALL end"),
        FaultCase("late", "01_sleep_onset", ("late:{T}:300",), ("EVENT_LATE",),
                  pick="mid", mag=("delta", 300.0, 60.0)),
        FaultCase("late", "27_genus_40hz_dim", ("late:{T}:900",),
                  ("EVENT_LATE", "EVENT_NOT_LOCATED", "EVENT_MISSING"),
                  pick="first_body", mag=("t_ms", 0.0, 2000.0),
                  note="round 2: a 900 ms late activation of all four zones "
                       "produced LED findings byte-identical to the clean run"),
        FaultCase("late", "20_gateway_focus10", ("late:{T}:4000",),
                  ("EVENT_NOT_LOCATED", "EVENT_MISSING", "EVENT_LATE"),
                  pick="mid", mag=("t_ms", 0.0, 6000.0),
                  note="4 s is outside every search window: round 2's headline "
                       "silent miss was a fault too large to localise"),
        FaultCase("late", "36_shamanic_trance_drum", ("late:{T}:15000",),
                  ("EVENT_NOT_LOCATED", "EVENT_MISSING",
                   "BRIGHTNESS_MODULATION"),
                  pick="late_body", mag=("t_ms", 0.0, 20000.0),
                  note="15 s late, late in a long session. ROUND-3 NOTE: the "
                       "entry this lands on is the one that DROPS a 12 s "
                       "brightness LFO for a steady level, and the instant of "
                       "that handover is not measurable -- a periodic template "
                       "matches a shift of one whole period at almost no cost, "
                       "so a 15 s delay fits inside any search window worth "
                       "running. What IS measurable is that the modulation was "
                       "still there for 15 s after it should have stopped, "
                       "which the tool now reports as BRIGHTNESS at the entry's "
                       "own timestamp. The magnitude assertion is unchanged."),
    ]

    # --- light-freq: the primary therapeutic quantity. ---
    C += [
        FaultCase("light-freq", "27_genus_40hz_dim", ("light-freq:1:1.009",),
                  ("FLICKER_RATE",), mag=("rel", +0.009, 0.004)),
        FaultCase("light-freq", "27_genus_40hz_dim", ("light-freq:1:0.991",),
                  ("FLICKER_RATE",), mag=("rel", -0.009, 0.004),
                  note="ROUND 2's HEADLINE SILENT MISS: +0.90% was an error, "
                       "-0.89% was measured correctly and then discarded"),
        FaultCase("light-freq", "11_lucid_wbtb", ("light-freq:1:1.006",),
                  ("FLICKER_RATE",), mag=("rel", +0.006, 0.004),
                  note="just above the 0.5% default threshold: the SMALL end"),
        FaultCase("light-freq", "11_lucid_wbtb", ("light-freq:1:0.994",),
                  ("FLICKER_RATE",), mag=("rel", -0.006, 0.004),
                  note="the small end, slow side"),
        FaultCase("light-freq", "11_lucid_wbtb", ("light-freq:1:0.5",),
                  ("FLICKER_RATE", "EVENT_MISSING", "EVENT_NOT_LOCATED"),
                  mag=("rel", -0.5, 0.15),
                  note="half rate: far outside any search window"),
        FaultCase("light-freq", "11_lucid_wbtb", ("light-freq:1:1.5",),
                  ("FLICKER_RATE", "EVENT_MISSING", "EVENT_NOT_LOCATED"),
                  mag=("rel", +0.5, 0.15),
                  note="half again: far outside any search window"),
        FaultCase("light-freq", "30_splitfield_lucid", ("light-freq:2:1.012",),
                  ("FLICKER_RATE",), mag=("rel", +0.012, 0.005),
                  note="one zone of several, on a session that runs the two "
                       "eyes at different rates -- must stay isolated"),
    ]

    # --- audio-freq: a wrong carrier. ---
    C += [
        FaultCase("audio-freq", "27_genus_40hz_dim", ("audio-freq:1.02",),
                  ("TONE_FREQ",), mag=("ratio", 1.02, 0.01)),
        FaultCase("audio-freq", "04_meditation_theta", ("audio-freq:0.98",),
                  ("TONE_FREQ",), mag=("ratio", 0.98, 0.01),
                  note="the other direction, on the panned-pair idiom"),
        FaultCase("audio-freq", "90_measure_sync", ("audio-freq:1.5",),
                  ("TONE_FREQ",), mag=("ratio", 1.5, 0.08),
                  note="far outside any search window"),
    ]

    # --- dead-light: a zone that never comes on. ---
    C += [
        FaultCase("dead-light", "27_genus_40hz_dim", ("dead-light:1",),
                  ("CHANNEL_NEVER_ON", "EVENT_MISSING", "LIGHT_NOT_GRADED",
                   "BRIGHTNESS")),
        FaultCase("dead-light", "20_gateway_focus10", ("dead-light:3",),
                  ("CHANNEL_NEVER_ON", "EVENT_MISSING", "LIGHT_NOT_GRADED",
                   "BRIGHTNESS"), note="a middle zone, not the reference one"),
        FaultCase("dead-light", "90_measure_sync", ("dead-light:1,2",),
                  ("CHANNEL_NEVER_ON", "AV_NOT_MEASURABLE", "EVENT_MISSING"),
                  note="two zones dark at once"),
    ]

    # --- beat-drift: the detector the brief named and that had no injector. ---
    C += [
        FaultCase("beat-drift", "04_meditation_theta", ("beat-drift:0.25",),
                  ("BEAT_DRIFT", "BEAT_FREQ"),
                  note="+0.3 Hz over 20 min, one of the brief's own four "
                       "example outputs"),
        FaultCase("beat-drift", "04_meditation_theta", ("beat-drift:-0.5",),
                  ("BEAT_DRIFT", "BEAT_FREQ"), note="the other direction"),
        FaultCase("beat-drift", "01_sleep_onset", ("beat-drift:1.0",),
                  ("BEAT_DRIFT", "BEAT_FREQ"),
                  note="on a session whose carrier is ALSO ramping -- the case "
                       "the firmware delta changed the meaning of"),
    ]

    # --- noise / ac-couple: recorder-domain. Must NOT be accused. ---
    C += [
        FaultCase("noise", "27_genus_40hz_dim", ("noise:-60",), (),
                  kind="recorder",
                  note="round 2: -60 dBFS alone produced a PULSE_RATE error on "
                       "the flagship gamma session"),
        FaultCase("noise", "01_sleep_onset", ("noise:-40",), (), kind="recorder"),
        FaultCase("noise", "04_meditation_theta", ("noise:-30",), (),
                  kind="recorder"),
        FaultCase("ac-couple", "27_genus_40hz_dim", ("ac-couple:5",), (),
                  kind="recorder"),
        FaultCase("ac-couple", "90_measure_sync", ("ac-couple:12",), (),
                  kind="recorder",
                  note="round 2 confirmed this self-reports SYNC_LOW_CONFIDENCE; "
                       "it must keep doing so instead of inventing a device fault"),
    ]
    return C


# ---------------------------------------------------------------------------
# Cache plumbing
# ---------------------------------------------------------------------------

def _sha(items) -> str:
    h = hashlib.sha1()
    for it in items:
        if isinstance(it, bytes):
            h.update(it)
        else:
            h.update(str(it).encode())
        h.update(b"\x00")
    return h.hexdigest()[:16]


def _module_fingerprint(names) -> str:
    return _sha([open(os.path.join(TOOL, n), "rb").read() for n in names])


@dataclass
class Paths:
    root: str

    def __post_init__(self):
        for sub in ("ledc", "wav", "res", "log"):
            os.makedirs(os.path.join(self.root, sub), exist_ok=True)
        # MEASURED: with tens of GB of fresh WAV landing in this directory,
        # Spotlight's mds_stores sat at 25% CPU indexing files nothing will ever
        # search for, while each render worker was down to 23%. This marker file
        # is the no-privileges way to tell it not to bother.
        for d in (self.root, os.path.join(self.root, "wav")):
            f = os.path.join(d, ".metadata_never_index")
            if not os.path.exists(f):
                try:
                    open(f, "a").close()
                except OSError:
                    pass

    def p(self, sub: str, name: str) -> str:
        return os.path.join(self.root, sub, name)


def _free_gb(path: str) -> float:
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / 1e9


#: A render takes at most ~200 s and an analysis ~60 s, so a file touched more
#: recently than this may still be open in another worker. Never evict those:
#: unlinking a WAV that a sibling process is about to open by path would turn a
#: disk-space policy into a phantom test failure.
PRUNE_MIN_AGE_S = 900.0


def prune_wav_cache(paths: Paths, budget_gb: float, keep_free_gb: float) -> None:
    """Evict least-recently-used cached renders to respect a disk budget.

    A full-length clean sweep of the library is ~76 GB of 16-bit 6-channel WAV,
    which is more than this machine can spare, so the cache is bounded and the
    overflow is simply re-rendered next time. Nothing here is ever the only
    copy of anything: every file is reproducible from its .ledc.
    """
    d = os.path.join(paths.root, "wav")
    now = time.time()
    files = []
    for n in os.listdir(d):
        if not n.endswith(".wav"):
            continue
        f = os.path.join(d, n)
        try:
            st = os.stat(f)
        except OSError:
            continue
        files.append((st.st_mtime, st.st_size, f))
    total = sum(s for _, s, _ in files)
    need_free = max(0.0, keep_free_gb - _free_gb(d)) * 1e9
    over = max(total - budget_gb * 1e9, need_free)
    if over <= 0:
        return
    for mtime, size, f in sorted(files):
        if over <= 0:
            break
        if now - mtime < PRUNE_MIN_AGE_S:
            continue
        try:
            os.remove(f)
            over -= size
        except OSError:
            continue
        try:
            os.remove(f + ".meta.json")
        except OSError:
            pass


# ---------------------------------------------------------------------------
# WAV post-processing (the recorder, as distinct from the device)
# ---------------------------------------------------------------------------

#: Sub-chunk for the exponential-cumsum high-pass. The bound is PRECISION, not
#: overflow: the cumsum carries terms spanning a^-(L-1), so a large L throws
#: away the early terms' significant digits. At the 5 Hz coefficient a^-2047 is
#: only 4.3, so 2048 keeps the identity exact to ~1e-14.
_HP_SUB = 2048
_HP_POW: dict[tuple[float, int], tuple[np.ndarray, np.ndarray, np.ndarray]] = {}


def _hp_powers(a: float, L: int):
    key = (a, L)
    p = _HP_POW.get(key)
    if p is None:
        k = np.arange(L, dtype=np.float64)
        p = (a ** k, a ** (-k), a ** (k + 1.0))
        _HP_POW[key] = p
    return p


def _onepole_hp_block(x: np.ndarray, a: float, y_prev: float,
                      x_prev: float) -> tuple[np.ndarray, float, float]:
    """y[k] = a*(y[k-1] + x[k] - x[k-1]) over a whole block, vectorised.

    Accepts x as (L,) or (L, nch) and filters every column with its own state
    when given a 2-D array -- see _onepole_hp_cols. The scalar recursion is
    what synth.py does (synth.py:563-575) and it is a Python loop over every
    sample: fine for a short injected fault, hopeless for post-processing a
    1 GB file. Unrolling it:
        y[k] = a^(k+1) * y_prev + sum_{j<=k} a^(k-j) * d[j],  d = a*(x - x_shift)
    which is an exponentially weighted cumulative sum.

    VERIFIED against the scalar recursion on 9000 samples of noise at the 5 Hz
    coefficient, chunked at 2048: max absolute difference 1.5e-14. This is an
    identity, not an approximation -- which matters, because an approximate
    high-pass would put its own artefact into a recording the gate then asks
    the analyzer to call clean.
    """
    L = x.shape[0]
    if L == 0:
        return x, y_prev, x_prev
    xs = np.empty(L)
    xs[0] = x_prev
    xs[1:] = x[:-1]
    d = a * (x - xs)
    pw_pos, pw_neg, pw_next = _hp_powers(a, L)
    y = pw_pos * np.cumsum(d * pw_neg) + pw_next * y_prev
    return y, float(y[-1]), float(x[-1])


def _onepole_hp_cols(blk: np.ndarray, a: float, state: np.ndarray) -> None:
    """In-place high-pass of every column of a big block, carrying `state`.

    `state` is a (2, nch) array of [y_prev; x_prev]. The big block is walked in
    _HP_SUB-sample sub-chunks for precision, but each sub-chunk filters ALL
    columns in one set of numpy calls, which is what makes this ~100x faster
    than the per-channel, per-small-block version it replaces: a 1 GB render
    went from over ten minutes to a few seconds.
    """
    n, nch = blk.shape
    for i in range(0, n, _HP_SUB):
        sub = blk[i:i + _HP_SUB]
        L = sub.shape[0]
        xs = np.empty_like(sub)
        xs[0] = state[1]
        xs[1:] = sub[:-1]
        d = a * (sub - xs)
        pw_pos, pw_neg, pw_next = _hp_powers(a, L)
        y = (pw_pos[:, None] * np.cumsum(d * pw_neg[:, None], axis=0)
             + pw_next[:, None] * state[0])
        state[1] = sub[-1]
        state[0] = y[-1]
        blk[i:i + L] = y


def apply_imperfection(src: str, dst: str, imp: Imperfection, seed: int) -> dict:
    """Stream `src` -> `dst` applying one recorder-imperfection profile."""
    rd = wavio.WavReader(src)
    sr = rd.sample_rate
    n_ch = rd.n_channels
    sr_decl = sr
    if imp.ppm:
        sr_decl = int(round(sr / (1.0 + imp.ppm * 1e-6)))
        if sr_decl == sr:                     # below the integer-rate grid
            sr_decl = sr + (-1 if imp.ppm > 0 else 1)
    achieved_ppm = (sr / sr_decl - 1.0) * 1e6
    noise_amp = 0.0 if imp.noise_dbfs is None else 10.0 ** (imp.noise_dbfs / 20.0)
    rng = np.random.default_rng(seed)
    a = 0.0
    if imp.hp_hz > 0:
        rc = 1.0 / (2.0 * math.pi * imp.hp_hz)
        a = rc / (rc + 1.0 / sr)
    # Light channels only, mirroring synth.py: the AC-coupling fault there is
    # applied to the optical channels and the audio ones are AC by nature.
    st = np.zeros((2, max(0, n_ch - 2)))
    w = wavio.WavWriter(dst, n_ch, sr_decl)
    try:
        for _, blk in rd.blocks(1 << 16):
            b = np.asarray(blk, dtype=np.float64)
            if a and n_ch > 2:
                _onepole_hp_cols(b[:, 2:], a, st)
            if noise_amp:
                b += rng.standard_normal(b.shape) * noise_amp
            if imp.dc:
                b += imp.dc
            w.write(b)
    finally:
        w.close()
        rd.close()
    return {"sr_declared": sr_decl, "achieved_ppm": round(achieved_ppm, 2)}


# ---------------------------------------------------------------------------
# Case model
# ---------------------------------------------------------------------------

@dataclass
class Case:
    gate: str               # "clean" | "imperfect" | "fault"
    name: str               # unique, stable, sortable
    session: str            # basename without .ledc
    faults: tuple[str, ...] = ()
    imp: Imperfection | None = None
    expect: tuple[str, ...] = ()
    kind: str = "clean"     # clean | recorder | device
    mag: tuple[str, float, float] | None = None
    pick: str | None = None  # entry-time selector resolving {T} in `faults`
    note: str = ""

    @property
    def render_key(self) -> tuple:
        """Cases sharing this render exactly. They are executed back to back in
        one worker so the render happens ONCE: a representative session carries
        one clean case plus seven imperfection variants, and rendering it eight
        times would have octupled the dominant cost of the sweep -- and two
        workers rendering to the same path would have raced."""
        return (self.session, self.faults)


@dataclass
class Result:
    case: str
    gate: str
    session: str
    ok: bool
    exit_code: int = -1
    errors: tuple[str, ...] = ()        # measurement errors (not LEDC_*)
    lint_errors: tuple[str, ...] = ()   # LEDC_* at error severity
    warnings: tuple[str, ...] = ()
    t0_s: float = float("nan")
    t0_conf: str = ""
    t0_err_ms: float = float("nan")
    reasons: tuple[str, ...] = ()
    measured: str = ""
    secs: float = 0.0
    skipped: bool = False
    """--cached-only left this case unrun. Skipped cases are excluded from
    every denominator in the report and listed by name, so a partial sweep
    reports a complete, self-consistent result over exactly what it ran rather
    than quietly shrinking the gate."""
    meta: dict = field(default_factory=dict)


# ---------------------------------------------------------------------------
# One case, end to end
# ---------------------------------------------------------------------------

_G: dict[str, Any] = {}


def _init_worker(cfg: dict):
    _G.update(cfg)


def _marked_ledc(session: str, paths: Paths, max_ms: int) -> tuple[str, str]:
    """Sync-mark `session` with the SHIPPED marker.py and return (path, sha).

    marker.py is invoked as a subprocess on purpose: it is the procedure the
    README tells a user to run, so the harness must exercise that exact code
    path rather than a reimplementation of it. --audio-ch is the highest
    generator slot the session does not already use, so the marker tone cannot
    collide with the session's own audio.
    """
    src = os.path.join(LIB, session + ".ledc")
    used = set()
    for raw in open(src, errors="replace"):
        s = raw.split("#")[0].strip()
        if not s:
            continue
        t = s.split()
        if t and t[0] in ("A", "a") and len(t) > 6:
            try:
                used.add(int(float(t[6])))
            except ValueError:
                pass
    ch = next((c for c in range(15, 0, -1) if c not in used), 15)
    stem = f"{session}__m{ch}" + (f"__cut{max_ms}" if max_ms else "")
    out = paths.p("ledc", stem + ".ledc")
    if max_ms:
        src = _trim(src, paths.p("ledc", f"{session}__cut{max_ms}.raw.ledc"),
                    max_ms)
    # Write via a per-process temp and rename: the same session appears in a
    # clean group and in several fault groups, so two workers can ask for this
    # file at once and a half-written .ledc would be a reference the device is
    # measured against.
    tmp = out + f".{os.getpid()}.tmp"
    r = subprocess.run(
        [sys.executable, os.path.join(TOOL, "marker.py"), "--ledc", src,
         "--out", tmp, "--tone", str(int(SYNC_TONE)), "--audio-ch", str(ch)],
        capture_output=True, text=True, cwd=TOOL)
    if r.returncode != 0:
        raise RuntimeError(f"marker.py failed on {session}: {r.stderr.strip()}")
    os.replace(tmp, out)
    return out, _sha([open(out, "rb").read()])


def _trim(src: str, dst: str, max_ms: int) -> str:
    """Cut a session at max_ms and close every touched zone/channel cleanly.

    ONLY used by --max-ms, which is a development convenience and explicitly
    NOT the gate: rounds 1 and 2 failed because short renders hid the bugs.
    """
    led, aud, masks, chans = [], [], set(), set()
    for raw in open(src, errors="replace"):
        s = raw.split("#")[0].strip()
        if not s:
            continue
        t = s.split()
        if t[0] in ("S", "s", "BG", "bg"):
            continue
        if t[0] in ("A", "a"):
            if len(t) < 2:
                continue
            try:
                tm = int(float(t[1]))
            except ValueError:
                continue
            if tm > max_ms:
                continue
            aud.append(" ".join(["A", str(tm)] + t[2:]))
            if len(t) >= 7:
                try:
                    chans.add(int(float(t[6])))
                except ValueError:
                    pass
            continue
        try:
            tm = int(float(t[0]))
        except ValueError:
            continue
        if tm > max_ms:
            continue
        led.append(" ".join([str(tm)] + t[1:]))
        if len(t) == 5 or len(t) >= 8:
            try:
                masks.add(int(float(t[4] if len(t) == 5 else t[7])))
            except ValueError:
                pass
    end = max_ms + 3000
    close = [f"{end} 0 50 0 0 0 0 {m}" for m in sorted(masks)]
    close += [f"A {end} 200 0 0 0 {c}" for c in sorted(chans)]
    tmp = dst + f".{os.getpid()}.tmp"
    with open(tmp, "w") as fh:
        fh.write("\n".join(led + aud + close) + "\n")
    os.replace(tmp, dst)
    return dst


def entry_times(marked: str) -> dict[str, int]:
    """Entry timestamps available for `drop`/`late` selectors, from the MARKED
    file (so they are already +1000 ms shifted)."""
    ts = set()
    for raw in open(marked, errors="replace"):
        s = raw.split("#")[0].strip()
        if not s:
            continue
        t = s.split()
        if t[0] in ("BG", "bg", "S", "s"):
            continue
        tok = t[1] if t[0] in ("A", "a") and len(t) > 1 else t[0]
        try:
            ts.add(int(float(tok)))
        except ValueError:
            pass
    body = sorted(x for x in ts if x >= MARKER_SHIFT_MS)
    if not body:
        body = sorted(ts)
    return {
        "marker": 0,
        "first_body": body[0],
        "mid": body[len(body) // 2],
        "late_body": body[max(0, int(len(body) * 0.8) - 1)],
        "last": body[-1],
    }


def run_group(group: list[Case]) -> list[Result]:
    """Run every case that shares one render, in order, in this process."""
    t_start = time.time()
    paths = Paths(_G["root"])
    try:
        return _run_group(group, paths)
    except Exception as e:                   # never let one group kill a sweep
        import traceback
        tb = traceback.format_exc()[-900:]
        secs = round(time.time() - t_start, 1)
        return [Result(c.name, c.gate, c.session, ok=False,
                       reasons=(f"HARNESS ERROR: {e.__class__.__name__}: {e}",),
                       measured=tb, secs=secs) for c in group]


def _run_group(group: list[Case], paths: Paths) -> list[Result]:
    max_ms = _G["max_ms"]
    session = group[0].session
    marked, ledc_sha = _marked_ledc(session, paths, max_ms)

    faults = list(group[0].faults)
    t_res = None
    if any("{T}" in f for f in faults):
        t_res = entry_times(marked)[group[0].pick or "first_body"]
        faults = [f.replace("{T}", str(t_res)) for f in faults]

    rkey = _sha([ledc_sha, _G["render_fp"], tuple(faults), _G["lead_in"],
                 _G["sample_rate"], _G["seed"]])
    wav = paths.p("wav", f"{session}__{rkey}.wav")

    # Decide up front which cases still need the recording at all: if every
    # result in the group is cached, the render is never touched.
    todo: list[tuple[Case, str, str]] = []
    out: list[Result] = []
    for c in group:
        akey = _sha([rkey, _G["analyze_fp"],
                     "" if c.imp is None else repr(c.imp)])
        res_path = paths.p("res", f"{c.name}__{akey}.json")
        if _G["use_cache"] and os.path.exists(res_path):
            try:
                out.append(_judge(c, json.load(open(res_path)), 0.0))
                continue
            except (OSError, ValueError):
                pass
        todo.append((c, akey, res_path))

    if not todo:
        return out
    if _G["cached_only"]:
        return out + [Result(c.name, c.gate, c.session, ok=True, skipped=True)
                      for c, _, _ in todo]

    t_render = time.time()
    meta = _ensure_wav(marked, wav, faults, paths)
    render_s = round(time.time() - t_render, 1)
    try:
        for c, akey, res_path in todo:
            t0 = time.time()
            use, tmp = wav, None
            m = meta
            if c.imp is not None:
                tmp = paths.p("wav", f"tmp_{c.name}__{akey}.wav")
                m = dict(meta)
                m.update(apply_imperfection(
                    wav, tmp, c.imp, zlib.crc32(c.name.encode())))
                use = tmp
            try:
                cached = _analyze(c, marked, use, m, res_path, paths, t_res)
            finally:
                if tmp and os.path.exists(tmp):
                    os.remove(tmp)
            r = _judge(c, cached, round(time.time() - t0, 1))
            r.meta = dict(r.meta, render_s=render_s)
            out.append(r)
    finally:
        if not _G["keep_wavs"] and os.path.exists(wav):
            os.remove(wav)
    return out


def _analyze(case: Case, marked: str, wav: str, meta: dict, res_path: str,
             paths: Paths, t_res) -> dict:
    """Run the SHIPPED CLI as a subprocess, so the exit code in the report is
    the exit code a user or a CI gate would actually see."""
    raw = res_path + f".{os.getpid()}.raw"
    cmd = [sys.executable, os.path.join(TOOL, "analyze.py"), "analyze",
           "--ledc", marked, "--wav", wav, "--map", meta["map"],
           "--sync-tone", str(int(SYNC_TONE)), "--quiet", "--json", raw]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=TOOL,
                       timeout=_G["timeout"])
    with open(paths.p("log", case.name + ".txt"), "w") as fh:
        fh.write(" ".join(cmd) + "\n\n" + r.stdout + "\n" + r.stderr)
    try:
        rep = json.load(open(raw))
    except (OSError, ValueError):
        # A missing or unparsable JSON is a hard failure of the case, whatever
        # stderr said. It used to be recorded as "no findings", and an empty
        # finding list reads as a CLEAN RUN -- so a worker that was killed
        # mid-analysis was silently scored as a pass. That is the single worst
        # bug a gate like this can have, so it is named loudly instead.
        rep = {"findings": [], "sync": {},
               "_broken": ((r.stderr or r.stdout)[-400:]
                           or f"analyze.py wrote no JSON and exited "
                              f"{r.returncode}")}
    cached = {"exit": r.returncode, "report": rep, "meta": meta,
              "t_res": t_res}
    if r.returncode < 0 or r.returncode > 3:
        # Killed by a signal, or an exit code the CLI does not define. Not a
        # verdict about the device -- do not cache it, so the next run retries.
        raise RuntimeError(
            f"analyze.py on {case.name} terminated abnormally "
            f"(exit {r.returncode}): {(r.stderr or r.stdout)[-300:]}")
    tmp = res_path + f".{os.getpid()}.tmp"
    with open(tmp, "w") as fh:
        json.dump(cached, fh)
    os.replace(tmp, res_path)
    try:
        os.remove(raw)
    except OSError:
        pass
    return cached


def _ensure_wav(marked: str, wav: str, faults: list[str], paths: Paths) -> dict:
    meta_path = wav + ".meta.json"
    if _G["use_cache"] and os.path.exists(wav) and os.path.exists(meta_path):
        try:
            meta = json.load(open(meta_path))
            os.utime(wav, None)      # keep it out of the pruner's reach
            return meta
        except (OSError, ValueError):
            pass
    prune_wav_cache(paths, _G["budget_gb"], _G["keep_free_gb"])
    tmp = wav + f".{os.getpid()}.tmp"
    cmd = [sys.executable, os.path.join(TOOL, "analyze.py"), "synth",
           "--ledc", marked, "--out", tmp, "--json",
           "--lead-in", str(_G["lead_in"]),
           "--sample-rate", str(_G["sample_rate"]), "--seed", str(_G["seed"])]
    for f in faults:
        cmd += ["--fault", f]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=TOOL,
                       timeout=_G["timeout"])
    if r.returncode != 0:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise RuntimeError(f"synth failed: {(r.stderr or r.stdout)[-400:]}")
    meta = json.loads(r.stdout)
    meta["out"] = wav
    os.replace(tmp, wav)
    with open(meta_path, "w") as fh:
        json.dump(meta, fh)
    return meta


# ---------------------------------------------------------------------------
# Verdicts
# ---------------------------------------------------------------------------

def _pipeline_ms(rep: dict) -> float:
    st = rep.get("stats") or {}
    v = st.get("av_pipeline_ms")
    return float(v) if isinstance(v, (int, float)) else 6.2


def _judge(case: Case, cached: dict, secs: float) -> Result:
    rep = cached["report"]
    code = int(cached["exit"])
    fnd = rep.get("findings") or []
    errs = tuple(sorted({f["code"] for f in fnd
                         if f.get("severity") == "error"
                         and not f["code"].startswith("LEDC_")}))
    lints = tuple(sorted({f["code"] for f in fnd
                          if f.get("severity") == "error"
                          and f["code"].startswith("LEDC_")}))
    warns = tuple(sorted({f["code"] for f in fnd
                          if f.get("severity") == "warning"}))
    sync = rep.get("sync") or {}
    t0 = float(sync.get("t0_s") or float("nan"))
    truth = _G["lead_in"]
    out = Result(case.name, case.gate, case.session, ok=True, exit_code=code,
                 errors=errs, lint_errors=lints, warnings=warns, t0_s=t0,
                 t0_conf=str(sync.get("confidence") or ""),
                 t0_err_ms=(t0 - truth) * 1000.0 if math.isfinite(t0) else float("nan"),
                 secs=secs, meta=cached.get("meta") or {})
    reasons: list[str] = []
    if rep.get("_broken"):
        reasons.append(f"analyze produced no JSON: {rep['_broken'][:200]}")
    if code < 0 or code > 3:
        reasons.append(
            f"analyze.py exited {code}, which is not one of its four defined "
            f"codes (0/1/2/3) -- the process was killed or crashed, so this "
            f"case measured NOTHING and must not be read as a pass")

    if case.gate in ("clean", "imperfect"):
        if errs:
            reasons.append("error-severity findings on a recording with no "
                           "device fault: " + ", ".join(errs))
    else:
        detected = [f for f in fnd if f["code"] in case.expect
                    and f.get("severity") in ("error", "warning")]
        if case.kind == "recorder":
            if errs:
                reasons.append("a RECORDER imperfection was reported as a "
                               "device fault: " + ", ".join(errs))
            if code not in (0,):
                reasons.append(f"exit {code} on a recorder-domain injection "
                               f"(0 expected)")
        else:
            hard = [f for f in detected if f.get("severity") == "error"]
            if not detected:
                reasons.append("SILENT MISS: none of "
                               f"{'/'.join(case.expect)} was reported at all")
            elif not hard:
                reasons.append("DEMOTED: " + ", ".join(sorted(
                    {f['code'] for f in detected})) + " fired, but only as a "
                    "warning, so $? stayed 0")
            if code == 0:
                reasons.append("exit 0 on an injected device fault")
        if case.mag:
            ok, text = _check_mag(case, rep, detected, cached.get("t_res"))
            out.measured = text
            if not ok:
                reasons.append("magnitude wrong: " + text)
    out.reasons = tuple(reasons)
    out.ok = not reasons
    return out


def _check_mag(case: Case, rep: dict, detected: list[dict],
               t_res) -> tuple[bool, str]:
    what, want, tol = case.mag           # type: ignore[misc]
    if what == "diff_ppm":
        got = rep.get("drift_differential_ppm")
    elif what == "common_ppm":
        got = rep.get("drift_common_ppm")
        if got is None:
            got = (rep.get("stats") or {}).get("clock_ppm_light")
    elif what == "av_ms":
        got = rep.get("av_offset_ms")
        if got is not None:
            got = got - _pipeline_ms(rep)
    elif what == "t_ms":
        cands = [f.get("t_ms") for f in detected if f.get("t_ms") is not None]
        if not cands or t_res is None:
            return (False, f"no timestamped finding to place against t={t_res}")
        got = min(cands, key=lambda v: abs(v - t_res)) - t_res
    else:
        best = None
        for f in detected:
            o, e = f.get("observed"), f.get("expected")
            if what == "delta":
                # EVENT_LATE carries expected=0 and delta=observed (the
                # lateness in ms IS the quantity), so `expected` being falsy
                # must not disqualify the finding.
                v = f.get("delta")
                if v is None:
                    if o is None or e is None:
                        continue
                    v = o - e
            else:
                if o is None or e is None or not e:
                    continue
                v = o / e if what == "ratio" else (o - e) / e
            if best is None or abs(v - want) < abs(best - want):
                best = v
        got = best
    if got is None or not isinstance(got, (int, float)) or not math.isfinite(got):
        return (False, f"{what} not reported (wanted {want:+g} +/-{tol:g})")
    return (abs(got - want) <= tol,
            f"{what}={got:+.4g} (wanted {want:+g} +/-{tol:g})")


# ---------------------------------------------------------------------------
# Building the case list
# ---------------------------------------------------------------------------

def library_sessions() -> list[str]:
    return sorted(f[:-5] for f in os.listdir(LIB) if f.endswith(".ledc"))


#: The sub-subset the recorder-imperfection variants default to. Each variant
#: costs a post-process plus a full analysis of a multi-gigabyte render, so
#: seven profiles across all 27 representative sessions is 189 cases -- the
#: single most expensive block in the sweep. These eight still carry every
#: shape family between them (gamma, fade-in + rate glide, antiphase phase
#: column, smooth and trapezoid carriers, brightness LFO, noise bed, multi-mask,
#: ramp-to-zero close, and the README's own measurement session).
IMP_SESSIONS = (
    "01_sleep_onset_RGB",        # antiphase phase column, fade-in, rate glide
    "04_meditation_theta",       # panned binaural pair + brightness LFO
    "07_genus_40hz",             # 60 min of 40 Hz gamma
    "20_gateway_focus10",        # ~a:b:period LFO handover
    "21_gateway_focus12_TRAP",   # trapezoid carrier + attack + phase
    "28_noise_sleep_bed",        # pink-noise bed, DC lamp
    "30_splitfield_lucid",       # a different rate per eye, masks 3/12/15
    "36_shamanic_trance_drum",   # every zone ends on a ramp into zero
)


def _resolve(which: str, pool: tuple[str, ...] | list[str]) -> list[str]:
    have = set(library_sessions())
    if which == "all":
        return sorted(have)
    if which == "none":
        return []
    if which in ("rep", "same"):
        return [s for s in pool if s in have]
    return sorted(s for s in have if re.search(which, s))


def build_cases(mode: str, which: str, imp_which: str) -> list[Case]:
    cases: list[Case] = []
    sessions = _resolve(which, REP_SESSIONS)

    if mode in ("all", "clean"):
        for s in sessions:
            cases.append(Case("clean", f"clean__{s}", s, kind="clean"))
        imp_set = [s for s in _resolve(imp_which, IMP_SESSIONS)
                   if s in sessions]
        for imp in IMPERFECTIONS:
            for s in imp_set:
                cases.append(Case("imperfect", f"imp_{imp.name}__{s}", s,
                                  imp=imp, kind="clean", note=imp.why))
    if mode in ("all", "faults"):
        for fc in _fault_matrix():
            if which not in ("all", "rep") and not re.search(which, fc.host):
                continue
            nm = (f"fault_{fc.cls}_{'_'.join(fc.spec)}__{fc.host}"
                  .replace(":", "-").replace(".", "p").replace(",", "+")
                  .replace("{", "").replace("}", ""))
            cases.append(Case("fault", nm, fc.host, faults=fc.spec,
                              expect=fc.expect, kind=fc.kind, mag=fc.mag,
                              pick=fc.pick, note=fc.note))
    return cases


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def shape_coverage(sessions: list[str]) -> tuple[dict[str, int], dict[str, int]]:
    lib, rep = {t: 0 for t in SHAPE_TAGS}, {t: 0 for t in SHAPE_TAGS}
    for s in library_sessions():
        tags, _ = classify(os.path.join(LIB, s + ".ledc"))
        for t in tags:
            lib[t] = lib.get(t, 0) + 1
            if s in sessions:
                rep[t] = rep.get(t, 0) + 1
    return lib, rep


def emit(results: list[Result], cases: list[Case], args, elapsed: float,
         out) -> tuple[int, dict]:
    kind_of = {c.name: c.kind for c in cases}
    P = lambda *a: print(*a, file=out)                      # noqa: E731
    P("=" * 100)
    P("avmeasure ACCEPTANCE REPORT")
    P("=" * 100)
    P(f"  tree          {TOOL}")
    P(f"  render fp     {_module_fingerprint(RENDER_MODULES)}  "
      f"({', '.join(RENDER_MODULES)})")
    P(f"  analyze fp    {_module_fingerprint(ANALYZE_MODULES)}  "
      f"({', '.join(ANALYZE_MODULES)})")
    P(f"  sessions      {len(library_sessions())} .ledc in {LIB}")
    P(f"  render length {'FULL (no trim)' if not args.max_ms else f'TRIMMED to {args.max_ms} ms -- NOT THE GATE'}")
    P(f"  cases         {len(cases)}   jobs {args.jobs}   "
      f"wall {elapsed / 60:.1f} min")
    n_skip = len([r for r in results if r.skipped])
    if n_skip:
        P(f"  !! PARTIAL    {n_skip} of {len(cases)} cases were NOT RUN "
          f"(--cached-only). Every number below is over the "
          f"{len(cases) - n_skip} that did run; the unrun cases are listed at "
          f"the end. This is NOT the gate.")
    P("")

    skipped = [r for r in results if r.skipped]
    ran = [r for r in results if not r.skipped]
    clean = [r for r in ran if r.gate == "clean"]
    imps = [r for r in ran if r.gate == "imperfect"]
    faults = [r for r in ran if r.gate == "fault"]
    dev = [r for r in faults if kind_of.get(r.case) == "device"]
    rec = [r for r in faults if kind_of.get(r.case) == "recorder"]

    def block(title, rows, note=""):
        P("-" * 100)
        P(title)
        if note:
            P("  " + note)
        P("-" * 100)
        if not rows:
            P("  (not run)")
            P("")
            return
        P(f"  {'':2} {'case':<52} {'$?':>3} {'t0 err':>8} {'conf':<5} codes")
        for r in sorted(rows, key=lambda x: x.case):
            codes = ",".join(r.errors) or "-"
            if r.lint_errors:
                codes += "  |lint:" + ",".join(r.lint_errors)
            if r.warnings:
                codes += "  |warn:" + ",".join(r.warnings)
            te = (f"{r.t0_err_ms:+.1f}" if math.isfinite(r.t0_err_ms) else "n/a")
            P(f"  {'ok' if r.ok else 'FAIL':<2} {r.case[:52]:<52} "
              f"{r.exit_code:>3} {te:>8} {r.t0_conf[:5]:<5} {codes[:110]}")
            for why in r.reasons:
                P(f"       -> {why}")
            if r.measured:
                P(f"       -> measured {r.measured}")
        P("")

    block("A1. CLEAN SWEEP -- bit-perfect renders, no fault, no imperfection",
          clean,
          "An error-severity finding here is the disqualifying condition: the "
          "tool accusing a healthy device.")
    block("A2. CLEAN SWEEP + RECORDER IMPERFECTIONS (not device faults)", imps,
          "crystal error, broadband noise, AC-coupled input, DC offset. Same "
          "bar: zero errors.")
    block("B1. FAULT SWEEP -- device faults, must be DETECTED with $? != 0", dev)
    block("B2. FAULT SWEEP -- recorder-domain injections, must NOT be accused",
          rec,
          "synth.py calls these not-device-bugs itself (:56-59, :97-99); the "
          "assertion is inverted and they are excluded from the headline.")

    # --- headlines ---
    cl_all = clean + imps
    cry = [r for r in cl_all if r.errors]
    miss = [r for r in dev if not r.ok]
    lint_sessions = sorted({r.session for r in clean if r.lint_errors})
    beat = sorted({r.session for r in clean
                   if "LEDC_BEAT_NOT_SWEPT" in r.lint_errors})
    t0e = [abs(r.t0_err_ms) for r in clean if math.isfinite(r.t0_err_ms)]
    hi_wrong = [r for r in clean if r.t0_conf == "high"
                and math.isfinite(r.t0_err_ms) and abs(r.t0_err_ms) > 5.0]

    P("=" * 100)
    P("HEADLINE NUMBERS")
    P("=" * 100)
    P(f"  clean sessions emitting >=1 error:  {len(cry)} of {len(cl_all)}")
    P(f"     ...of which bit-perfect, no imperfection: "
      f"{len([r for r in clean if r.errors])} of {len(clean)}")
    P(f"  injected faults not detected:       {len(miss)} of {len(dev)}")
    P(f"  recorder injections misreported:    "
      f"{len([r for r in rec if not r.ok])} of {len(rec)}")
    P("")
    P(f"  clean sessions with a STATIC LEDC_* error (also reaches $?): "
      f"{len(lint_sessions)} of {len(clean)}")
    P(f"  ...of those, LEDC_BEAT_NOT_SWEPT, which the firmware delta made "
      f"stale: {len(beat)}")
    if t0e:
        P(f"  t0 error on clean renders: median {np.median(t0e):.1f} ms, "
          f"p90 {np.percentile(t0e, 90):.1f} ms, max {max(t0e):.1f} ms")
    P(f"  clean renders claiming HIGH confidence (+/-5 ms) while t0 is wrong "
      f"by more: {len(hi_wrong)} of {len(clean)}")
    demoted = [r for r in cl_all if r.t0_conf in ("low", "none")]
    P(f"  clean cases where sync fell to LOW/NONE, so every timing finding was "
      f"demoted to a warning and gate A is VACUOUS there: "
      f"{len(demoted)} of {len(cl_all)}")
    if demoted:
        bad = sorted({r.case.split('__')[0] for r in demoted})
        P(f"     profiles affected: {', '.join(bad)}")
    P("")
    code_hist: dict[str, int] = {}
    for r in cl_all:
        for c in r.errors:
            code_hist[c] = code_hist.get(c, 0) + 1
    if code_hist:
        P("  false-positive error codes, most frequent first:")
        for c, n in sorted(code_hist.items(), key=lambda kv: -kv[1]):
            P(f"     {n:>4}  {c}")
    P("")
    if skipped:
        P(f"  NOT RUN ({len(skipped)} cases, so not in any number above):")
        for g in ("clean", "imperfect", "fault"):
            names = sorted(r.case for r in skipped if r.gate == g)
            if names:
                P(f"     {g}: {', '.join(names)}")
        P("")
    gate_a = not cry and not skipped
    gate_b = not miss and not [r for r in rec if not r.ok] and not skipped
    P(f"  GATE A (cry-wolf):   {'PASS' if gate_a else 'FAIL'}")
    P(f"  GATE B (silent-miss):{'PASS' if gate_b else 'FAIL'}")
    P("=" * 100)

    summary = {
        "clean_with_error": len(cry), "clean_total": len(cl_all),
        "clean_bitperfect_with_error": len([r for r in clean if r.errors]),
        "clean_bitperfect_total": len(clean),
        "faults_missed": len(miss), "faults_total": len(dev),
        "recorder_misreported": len([r for r in rec if not r.ok]),
        "recorder_total": len(rec),
        "lint_sessions": len(lint_sessions),
        "lint_session_names": lint_sessions,
        "beat_not_swept_sessions": len(beat),
        "beat_not_swept_names": beat,
        "clean_low_confidence": len(demoted),
        "high_conf_but_t0_wrong": [r.case for r in hi_wrong],
        "t0_err_ms_median": float(np.median(t0e)) if t0e else None,
        "t0_err_ms_max": float(max(t0e)) if t0e else None,
        "gate_a": gate_a, "gate_b": gate_b,
        "skipped": [r.case for r in skipped],
        "code_hist": code_hist,
        "cases": [vars(r) | {"errors": list(r.errors),
                             "lint_errors": list(r.lint_errors),
                             "warnings": list(r.warnings),
                             "reasons": list(r.reasons)} for r in results],
    }
    return (0 if (gate_a and gate_b) else 1), summary


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("all", "clean", "faults"), default="all")
    ap.add_argument("--sessions", default="all",
                    help="'all' (the gate), 'rep' (the documented subset), or "
                         "a regex matched against the basename")
    ap.add_argument("--imp-sessions", default="same",
                    help="which sessions get the recorder-imperfection "
                         "variants: 'same' (the documented IMP_SESSIONS "
                         "sub-subset, the default), 'all', 'rep', 'none', or a "
                         "regex. Seven profiles x 27 sessions is the single "
                         "most expensive block in the sweep, so this is the "
                         "knob to turn when the machine is busy.")
    ap.add_argument("--jobs", type=int,
                    default=max(1, min(4, (os.cpu_count() or 4) - 1)),
                    help="MEASURED: render throughput saturates at about four "
                         "concurrent workers (8 MB/s of WAV in total, against "
                         "6.5 MB/s for one alone), and eight workers were no "
                         "better while using twice the memory and thrashing "
                         "the page cache. Raise it only if you have measured "
                         "that it helps on your machine. (default: %(default)s)")
    ap.add_argument("--max-ms", type=int, default=0,
                    help="trim every session to this many ms. A DEVELOPMENT "
                         "SHORTCUT, not the gate -- rounds 1 and 2 failed "
                         "precisely because short renders hid the bugs.")
    ap.add_argument("--cache-dir", default=None)
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--cached-only", action="store_true",
                    help="judge only cases whose analysis is already cached and "
                         "SKIP the rest, instead of rendering them. A full-length "
                         "sweep of the library is hours of render; this reports "
                         "exactly what has run so far, with the unrun cases "
                         "named rather than silently dropped from the gate.")
    ap.add_argument("--keep-wavs", action="store_true", default=True)
    ap.add_argument("--no-keep-wavs", dest="keep_wavs", action="store_false",
                    help="delete each render after analysing it (slow re-runs, "
                         "tiny disk)")
    ap.add_argument("--budget-gb", type=float, default=48.0,
                    help="cap on the cached-render directory (default 48)")
    ap.add_argument("--keep-free-gb", type=float, default=25.0,
                    help="never let free disk fall below this (default 25)")
    ap.add_argument("--timeout", type=float, default=3600.0)
    ap.add_argument("--lead-in", type=float, default=2.0)
    ap.add_argument("--sample-rate", type=int, default=44100)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--report", default=None, help="also write the report here")
    ap.add_argument("--json", default=None, help="write the summary JSON here")
    ap.add_argument("--list", action="store_true",
                    help="print the case list and the shape-coverage matrix, "
                         "then exit")
    args = ap.parse_args(argv)

    if not os.path.isdir(LIB):
        print(f"error: session library not found at {LIB}", file=sys.stderr)
        return 2

    root = args.cache_dir or _scratch_root()
    paths = Paths(root)
    cases = build_cases(args.mode, args.sessions, args.imp_sessions)

    rendered = sorted({c.session for c in cases})
    lib_cov, rep_cov = shape_coverage(rendered)
    missing = [t for t in SHAPE_TAGS if lib_cov.get(t) and not rep_cov.get(t)]

    print(f"# cache        {root}  (free {_free_gb(root):.0f} GB)")
    print(f"# cases        {len(cases)}  "
          f"({len([c for c in cases if c.gate == 'clean'])} clean, "
          f"{len([c for c in cases if c.gate == 'imperfect'])} imperfect, "
          f"{len([c for c in cases if c.gate == 'fault'])} fault)")
    print(f"# imp sessions {args.imp_sessions} -> "
          f"{len({c.session for c in cases if c.gate == 'imperfect'})} session(s)")
    print("# shape coverage of what THIS RUN renders "
          "(tag: in-library / in-this-run)")
    for t in SHAPE_TAGS:
        print(f"#   {t:<14} {lib_cov.get(t, 0):>3} / {rep_cov.get(t, 0):>3}"
              + ("   <-- NOT COVERED" if t in missing else ""))
    if missing:
        print(f"# WARNING: shapes present in the library but absent from THIS "
              f"RUN: {', '.join(missing)} -- the run is not a full gate")
    if args.list:
        for c in sorted(cases, key=lambda x: x.name):
            print(f"{c.gate:<10} {c.name:<60} {c.note}")
        return 0

    cfg = {"root": root, "render_fp": _module_fingerprint(RENDER_MODULES),
           "analyze_fp": _module_fingerprint(ANALYZE_MODULES),
           "use_cache": not args.no_cache, "max_ms": args.max_ms,
           "keep_wavs": args.keep_wavs, "budget_gb": args.budget_gb,
           "keep_free_gb": args.keep_free_gb, "timeout": args.timeout,
           "lead_in": args.lead_in, "sample_rate": args.sample_rate,
           "seed": args.seed, "cached_only": args.cached_only}

    # Group by render, then schedule the heaviest group first: with N workers
    # the tail of the run is set by the single longest group, so leaving
    # 07_genus_40hz (60 min, 8 cases) until last would add many idle minutes at
    # the end of every run.
    dur: dict[str, int] = {}
    for c in cases:
        if c.session not in dur:
            dur[c.session] = classify(os.path.join(LIB, c.session + ".ledc"))[1]
    groups: dict[tuple, list[Case]] = {}
    for c in sorted(cases, key=lambda x: x.name):
        groups.setdefault(c.render_key, []).append(c)
    glist = sorted(groups.values(),
                   key=lambda g: (-dur[g[0].session] * len(g), g[0].name))
    print(f"# groups       {len(glist)} distinct renders for {len(cases)} cases")

    t0 = time.time()
    results: list[Result] = []
    done = 0
    if args.jobs <= 1:
        _init_worker(cfg)
        it = (run_group(g) for g in glist)
    else:
        import multiprocessing as mp
        ctx = mp.get_context("spawn")
        pool = ctx.Pool(args.jobs, initializer=_init_worker, initargs=(cfg,))
        it = pool.imap_unordered(run_group, glist)
    for rs in it:
        for r in rs:
            results.append(r)
            done += 1
            print(f"# [{done}/{len(cases)}] {'ok  ' if r.ok else 'FAIL'} "
                  f"{r.case}  {r.secs}s", flush=True)
    if args.jobs > 1:
        pool.close()
        pool.join()

    elapsed = time.time() - t0
    rc, summary = emit(results, cases, args, elapsed, sys.stdout)
    if args.report:
        with open(args.report, "w") as fh:
            emit(results, cases, args, elapsed, fh)
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(summary, fh, indent=2, default=str)
    return rc


if __name__ == "__main__":
    sys.exit(main())
