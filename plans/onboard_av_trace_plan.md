# On-device A/V trace — record what the device actually emitted

## Why

Sessions sometimes don't match the `.ledc`: drift, audio/LED desync, events that
seem not to fire. The symptom is too vague to chase by guessing at fixes, so we
need to measure. This is the device-side half of that instrument; the host-side
comparison engine lives in `tools/avmeasure/`.

## The constraint that shapes everything

**The tracer must not perturb what it measures.** We are measuring sub-millisecond
timing, so anything that can block an audio or LED task for even a few hundred
microseconds makes the instrument lie — and a lying instrument is worse than none,
because it sends you chasing bugs that don't exist.

Bandwidth is *not* the problem (see budget below; SD does MB/s and we need tens of
KB/s). The hazard is **SD write-latency spikes**: a wear-levelling or block-erase
stall can block a write for 100 ms+, unpredictably. So:

- Real-time paths (audio generation, LED flicker) **never touch the filesystem**.
  They do one lock-free push into a ring buffer: a bounds check, a `memcpy` of a
  small fixed-size record, and an atomic index store. Tens of nanoseconds, no
  mutex, no allocation, no `snprintf`.
- The ring lives in **PSRAM** (8 MB available; internal DRAM is the scarce pool and
  is already tight — see the `bg_player` streamer-stack fragmentation note in
  `main/bg_player.c`).
- A **low-priority drain task pinned to core 0** moves bytes from the ring to the
  card. Core 1 keeps the flicker and audio tasks it already has. The ring absorbs
  any SD stall shorter than its depth.

A 2 MB PSRAM ring at the budgeted ~35 KB/s is ~57 seconds of slack. No realistic
SD stall comes close.

## Record edges, not frames

The naive design logs every LED frame. At a ~1 kHz update rate that is ~20 KB/s of
mostly-redundant data, and it still quantises every edge to the frame rate.

Instead log **transitions**: when a channel changes on/off state, record the exact
timestamp. For flicker this is 2 records per cycle per channel — at 40 Hz across 4
channels that is 320 records/s instead of 1000, and the timestamps are *exact*
rather than frame-quantised. Edge timing is precisely what we want to compare
against the `.ledc`.

Add a **keyframe** every second carrying full state, so the host can resync if it
joins mid-file or a gap occurs.

## Express audio time in SAMPLES, not microseconds

This is the most important decision in the design.

Record, per audio block, the **cumulative sample index** written to I2S alongside
the wall-clock timestamp. Samples are the device's true audio clock. Converting
sample index → time via the nominal sample rate and comparing that against the
LED path's `esp_timer` microseconds **directly measures relative clock-domain
drift** — one of the two prime suspects for the reported desync. If I2S runs off a
PLL and the flicker off XTAL-derived `esp_timer`, they walk apart over a 20–60
minute session, and this falls straight out of the trace.

## Budget

| Stream | Record | Rate | Bytes/s |
|---|---|---|---|
| LED edges | `u32 t_us`, `u8 ch`, `u8 state`, `u8 r,g,b` (8 B padded) | ~320/s | ~2.5 K |
| LED keyframes | full 4-channel state (~24 B) | 1/s | negligible |
| Audio blocks | `u32 t_us`, `u64 sample_idx`, per-channel `f32 freq, amp` | ~172/s | ~13 K |
| Markers | underrun, gap, timeline event | sparse | negligible |

**~16 KB/s, ~57 MB/hour.** Comfortable for both the card and a 60-minute session.

Records are **fixed-size little-endian binary**. No text formatting in the hot path;
the host converts.

## Gaps must be explicit

If the ring ever fills, the tracer **must write a gap marker** recording how many
records were lost and over what interval. A silently dropped record is
indistinguishable from "the device never emitted that event" — i.e. the tracer
would manufacture a fake bug. This is non-negotiable.

Likewise record **underruns** (`short_writes`) as markers: an underrun can shift
effective I2S latency by up to a whole buffer and *keep* it shifted, which is the
other prime suspect for progressive desync.

## What this cannot see

A trace records when firmware **handed a sample to the DMA**, not when it left the
DAC. The I2S output latency (`dma_desc_num × dma_frame_num / sample_rate`) is
invisible to the device. It is *computable*, so we can state it — but if the real
bug is that this offset ratchets on underruns or drifts against the LED clock, only
the external rig can prove it. The trace localises; the external capture
adjudicates.

## Gating

Kconfig flag, **default off**, consistent with the other debug gates
(`CONFIG_ISR_PROFILING`, `CONFIG_TIMELINE_DEBUG`,
`CONFIG_DEBUG_CODEC_REGISTER_ACCESS`). When off, the hot-path hooks compile out
entirely — zero cost, nothing to argue about in a shipped build. Runtime
start/stop on top, so a developer with it compiled in still only pays for it
during a capture.

Add it to the `DANGEROUS_DEBUG` list in `switch_board.sh` so a bench opt-in can't
ratchet into a tracked board snapshot.

## Host side

Emits the same observed-timeline shape that `tools/avmeasure/observe_wav.py`
produces, so `compare.py` diffs trace-vs-`.ledc` and recording-vs-`.ledc` through
one code path.

## Status

PLANNED — written while the pin-validation workflow held the build directory.
