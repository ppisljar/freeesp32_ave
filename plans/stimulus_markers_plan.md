# Stimulus markers & experimental logging

Turn the device from a programmable AV stimulator into something you can run a
reproducible experiment on: every meaningful stimulus event is announced, once,
to an arbitrary set of configured sinks.

## The one hard idea: three timestamps, never conflated

A marker carries three times, and the difference between them is the whole
point. Collapsing them is how marker data goes quietly wrong.

| field | meaning | source |
|---|---|---|
| `t_sched_us` | when the session *intended* the event | the `.ledc` time against `transport_origin_us` |
| `t_disp_us` | when the firmware *processed* it | `esp_timer_get_time()`, stamped ONCE, at **batch start** |
| `t_onset_pred_us` | when the signal is *expected* to reach the output | `t_disp_us` + the measured pipeline constant for that modality |

`t_onset_pred_us` is **predicted, not measured** — named that way in every sink,
every doc, every log record. The constants come from the A/V sync work:

- audio: `AUDIO_DMA_PIPELINE_LAG_US` = 46.439 ms
- LED: the same 46.439 ms anchor **plus** the strip write, which is
  `pixel_index x 30.0 us` (1.25 us/bit x 24 bits), i.e. 0 .. 2.220 ms over the
  74-pixel strip. A single flat constant is wrong here — see task #24.

`t_disp_us` is taken at **batch start**, not inside the marker's own pass, so a
marker shares the exact dispatch reference with the audio and LED entries it
sits beside. (The executor runs audio in pass 1, LED in pass 2, everything else
in pass 3; stamping in pass 3 would add a measurable but pointless offset.)

## `M` lines

A new line type, additive — no change to any existing syntax:

```
M <time_ms> <LABEL> [key=value ...]
```

```
M 0      SESSION_START
M 5000   BASELINE_START
M 35000  STIM_START      condition=alpha10 block=3
M 95000  GLIDE_START     from=10 to=6
M 185000 SESSION_END
```

`LABEL` is `[A-Z0-9_]{1,31}`. Up to 4 `key=value` pairs, 15/31 chars each.
Parsed by the existing tokeniser and scheduled by the existing timeline — an `M`
row is a `config_entry_t` like any other.

`SESSION_START` / `SESSION_END` are emitted by the firmware whether or not the
file writes them, so a recording always has its boundaries.

## Sinks: a configurable list, dispatch to all

Not three hardcoded outputs — a registry. `SETTINGS_MAX_MARKER_SINKS = 4`
slots in `device_settings_t`, each `{enabled, type, ...}`, editable on the
Settings page, and every marker goes to **every enabled sink**. Multiple sinks
of the same type are allowed and useful: two UDP destinations for two recording
machines, or two GPIO pins to mark different modalities.

### A. `gpio` — a TTL pulse

Pin (validated by the existing reserved-GPIO guard), pulse width, active level,
and a **timing mode**:

- `onset` (default) — the pulse is scheduled for `t_onset_pred_us`, so it lines
  up with photons/sound rather than with the parser
- `dispatch` — fires immediately at `t_disp_us`, for measuring the difference

Hard caveat for the docs: this marks the *predicted* stimulus edge, not a
verified one. **The pin must never touch an electrode or an incompatible
input** — opto-isolation guidance goes in the hardware section.

### B. `udp` — network, for research software

Host + port, one JSON object per datagram. Plus `tools/lslbridge/` (Python +
pylsl) turning the stream into an LSL marker stream alongside an EEG stream for
LabRecorder/XDF.

The bridge does **periodic round-trip clock-offset estimation** and maps device
monotonic time to LSL time. Timestamping on arrival is not good enough — jitter
in network delay would land directly in the marker times.

### C. `sdjsonl` — reproducibility with nothing attached

`/sd/logs/<session>-<n>.jsonl`. Opens with a provenance header record:

```json
{"rec":"header","fw_elf_sha256":"...","session_sha256":"...","session":"alpha_01",
 "device":{...},"jitter_seed":...,"pipeline":{"audio_lag_us":46439,"led_lag_us":...}}
{"rec":"marker","label":"STIM_START","t_sched_us":35000000,"t_disp_us":35004121,
 "t_onset_pred_us":35050560,"kv":{"condition":"alpha10","block":"3"}}
{"rec":"footer","markers":42,"dropped":0,"max_sched_disp_delta_us":4121}
```

`fw_elf_sha256` comes free — `/api/version` already reports it.

## Real-time safety

Markers are **never** written to SD or the network from a dispatch path. A
preallocated lock-free ring (64 slots) is drained by a low-priority task.
Overflow increments a counter that lands in the footer and `/api/state` — a
silently dropped marker is worse than a logged gap.

The GPIO sink is the exception: it fires from a timer, not the flusher, because
its whole value is timing precision.

## Steps

1. `main/marker.h` / `marker.c` — three-timestamp record, ring, flusher task,
   sink registry, the three sink implementations.
2. `settings`: the sink array + JSON apply/serialise + GPIO validation.
3. `config_parser`: `M` parse, `CONFIG_ENTRY_MARKER`, pass-3 dispatch at batch
   start, implicit SESSION_START/END.
4. `web_server`: `/api/markers` (live tail, for the UI) — sink config rides the
   existing `/api/settings`.
5. Settings page UI: add/remove/edit sinks.
6. `tools/lslbridge/` + README.
7. `avmeasure`: model `M` lines, lint them, and **verify the marker path from a
   capture** — the rig records the GPIO pulse on a spare input next to the
   optical and audio channels. This is the independent hardware validation of
   the marker path; the existing A/V sync work does not cover it.
8. Docs: `ledc_format.md`, a new `docs/sessions/markers.html`, hardware notes.
9. Build, flash, verify on hardware.

## Out of scope for v1

**Closed-loop / neurofeedback.** It inverts the data flow (an external
controller driving stimulus frequency) and needs its own safety design, plus the
artifact problem: a loop fed by EEG recorded *during* stimulation can end up
tracking its own stimulus rather than the brain. Separate feature.
