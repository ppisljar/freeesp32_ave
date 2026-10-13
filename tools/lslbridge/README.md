# LSL bridge

Forwards freeesp32_ave stimulus markers onto [Lab Streaming
Layer](https://labstreaminglayer.readthedocs.io/), so LabRecorder can record
them into an XDF alongside an EEG.

```
pip install pylsl
./lslbridge.py --device 10.0.0.146
```

Then point the device at this machine — **Settings → Stimulus markers → add a
UDP sink**, host = this machine's IP, port = 9009 — and start LabRecorder. The
stream appears as `AVE_Markers` (type `Markers`).

## What it is actually for

Not the plumbing. The **clock mapping**.

The naive bridge stamps each marker with the time the datagram *arrives*. That
is wrong for anything where timing matters: WiFi latency varies by milliseconds
to tens of milliseconds, and every bit of that variation lands directly in your
event times. It is also one-sided — an arrival time cannot distinguish a late
send from a late receive.

So this estimates the offset between the device's monotonic clock and the local
LSL clock, continuously, and maps each marker's **device** timestamp
(`t_disp_mono_us`) onto the LSL timebase:

```
lsl_time = device_mono_us / 1e6 + offset
```

`offset` comes from round-tripping `GET /api/clock` and taking the midpoint
against the device's reported time — the classic NTP-style estimate. Probes
slower than 50 ms are **discarded**, not averaged in, because a slow round trip
gives a wide bound on where the device's clock actually was. The running
estimate is the **median** of the last 15 survivors, so one stalled probe cannot
drag it.

Measured on the YB-ESP32-S3-DAC over WiFi:

| | via `/api/state` | via `/api/clock` |
|---|---|---|
| median round trip | 44 ms | **24 ms** |
| offset spread over 15 samples | 10.7 ms | **7.5 ms** |
| mapping error vs round-trip midpoint | ~10.6 ms | **0.84 ms** median |

`/api/clock` exists for exactly this: a ~20-byte reply carrying
`esp_timer_get_time()` — the same clock `t_disp_mono_us` is stamped from, which
is what makes the two comparable. Probing a 5.6 KB endpoint instead bounds the
offset at roughly ±½ RTT, which is useless for aligning an EEG.

## What it does not do

**It does not substitute the predicted onset for the event time.** Each marker
carries three timestamps and `t_onset_pred_us` is *predicted* — dispatch plus a
modelled pipeline latency — not measured. The bridge maps the dispatch stamp,
and `--raw` pushes the whole record so the analyst can see all three and choose.

If the clock is not yet mapped when the first markers arrive, it falls back to
arrival time **and says so**, on the console and per marker. A silent fallback
would be indistinguishable from a correct mapping.

## Output

Session boundaries and the footer are printed rather than forwarded as markers:

```
  SESSION  sha=f522ea9ae2e6… fw=5149ff4a98911e2a lags audio=46439us led=47549us
           offset +396875.922643 s from 15 samples, spread 7.5 ms, median rtt 23.9 ms
  LIGHT_ON         seq=2 lsl=396877.148321
  AUDIO_ON         seq=3 lsl=396879.148320
  END      markers=7 dropped=0 sink_errors=0
```

A non-zero `dropped` is called out explicitly — it means the recording has gaps.

## Options

| flag | meaning |
|---|---|
| `--device` | host or IP for the clock probe. **Prefer the IP**: mDNS resolution can add seconds of its own and will wreck the offset estimate. |
| `--port` | UDP port to listen on (default 9009) |
| `--bind` | address to bind (default all) |
| `--name` | LSL stream name (default `AVE_Markers`) |
| `--source-id` | LSL source id; keep it stable so recordings can be linked |
| `--raw` | push the entire JSON record as the marker string instead of just the label |
