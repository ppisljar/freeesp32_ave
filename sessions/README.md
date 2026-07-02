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
