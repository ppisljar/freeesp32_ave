# Session Library

Built-in entrainment sessions shipped on the device.

## Layout
```
sessions/
  SESSION_DESIGN_GUIDELINES.md   # the authoring rulebook (lessons learned)
  README.md                      # this file
  plans/                         # one authoring plan per session (markdown)
  library/                       # the .ledc session files — THIS folder gets flashed
```

## What gets flashed
Only **`sessions/library/*.ledc`** is packed into the device's `cfgfs` SPIFFS
partition (where the web UI lists/loads configs). The guidelines and plans are
repo docs and are **not** flashed. See `plans/000_library_flashing.md` for the
build/flash wiring.

## Authoring
Every `.ledc` in `library/` must follow `SESSION_DESIGN_GUIDELINES.md` (arc,
parameter ranges, and the mandatory **safety** rules) and validate against the
firmware grammar (`main/config_parser.c`). Each session has a plan in `plans/`.

## Current planned set
| # | Session | Goal | ~Duration |
|---|---------|------|-----------|
| 001 | Sleep Onset | sleep | ~45 min |
| 002 | Power Nap | restorative nap | ~22 min |
| 003 | Deep Relaxation | stress relief | ~30 min |
| 004 | Meditation (Theta) | meditation | ~35 min |
| 005 | Focus (SMR) | focus/study | ~30 min |
| 006 | Energize | wake-up | ~18 min |
| 007 | 40 Hz GENUS | cognition | 60 min |
| 008 | Calm / Anxiety (opt) | calm | ~25 min |
| 009 | Lucid / Hypnagogic (opt) | lucid dreaming | ~60 min |
| … | (10–25 lucid / gateway / astral set) | various | various |
| 026 | Alpha + Breath | calm coherence (0.1 Hz breath LFO) | ~15 min |
| 027 | 40 Hz GENUS (dim / invisible-flicker) | cognition, comfort variant | ~30 min |
| 028 | AM-Noise Sleep Bed (smooth) | sleep masking + slow drive | ~30 min |
| 029 | AM-Noise Sleep Bed (pulsed) | sleep, trapezoid burst variant | ~30 min |
| 030 | Split-Field Lucid | per-eye 16 Hz \| 6 Hz | ~15 min |
| 031 | Eyes-Closed Ganzfeld (amber) | homogeneous-field imagery | ~22 min |
| 032 | Hypnagogic Visions | closed-eye visionary threshold (LED sine gate + flicker jitter) | ~27 min |
| 033 | Ego-Dissolution Gamma | non-dual unity (40 Hz invisible antiphase pair + gamma binaural/harmonic + beat jitter) | ~30 min |
| 034 | Gateway / OBE (Monroe) | out-of-body threshold (septon stack + EEG-contour wave 7 + trapezoid iso + phased pink + beat jitter) | ~32 min |
| 035 | WBTB Lucid Gamma | lucid dreaming (split-field + phase-locked antiphase 40 Hz + bilateral duty click) | ~25 min |
| 036 | Shamanic Trance Drum | deep theta drum-trance (monaural drum + trapezoid LED/audio + brown AM + rotating pan + jitter) | ~30 min |
