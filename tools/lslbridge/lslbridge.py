#!/usr/bin/env python3
"""Bridge freeesp32_ave stimulus markers onto Lab Streaming Layer.

The device sends one JSON object per UDP datagram (see the `udp` marker sink).
This turns that into an LSL marker stream that LabRecorder can record into an
XDF alongside an EEG.

THE POINT OF THIS PROGRAM IS THE CLOCK MAPPING.

Stamping each marker with the time it ARRIVES is the obvious implementation and
it is wrong for anything where timing matters: WiFi latency varies by
milliseconds to tens of milliseconds, and that variation lands directly in your
event times. It is also one-sided — you cannot tell a late send from a late
receive.

So the bridge estimates the offset between the device's monotonic clock and the
local LSL clock, continuously, and maps every marker's *device* timestamp onto
the LSL timebase:

    lsl_time = device_mono_us / 1e6 + offset

`offset` is measured by round-tripping a cheap HTTP request to the device and
taking the midpoint of the local send/receive pair against the device's reported
monotonic time (the classic NTP-style estimate). Samples with a large round-trip
are discarded rather than averaged in, because a slow round trip gives a wide
bound on where the device's clock actually was. The running estimate is the
median of the surviving samples, which ignores the occasional outlier that a
mean would absorb.

WHICH DEVICE TIMESTAMP IS USED
------------------------------
Each marker carries three times, and the bridge maps `t_disp_mono_us` (the raw
device monotonic stamp at dispatch) because that is the one in the device's own
clock domain. The LSL sample then carries the full record, so the analyst can
still see t_sched_us, t_disp_us and t_onset_pred_us and decide which one their
question needs.

`t_onset_pred_us` is PREDICTED, not measured. The bridge does not silently
substitute it for the event time.

Usage:
    pip install pylsl
    ./lslbridge.py --device 10.0.0.146
    ./lslbridge.py --device esp32-ave-mobile.local --port 9009 --name AVE_Markers

Then point the device's UDP sink at this machine's IP on the same port
(Settings -> Stimulus markers -> UDP), and start LabRecorder.
"""

import argparse
import json
import socket
import statistics
import sys
import threading
import time
import urllib.request

try:
    from pylsl import StreamInfo, StreamOutlet, local_clock
except ImportError:
    sys.exit("pylsl is required:  pip install pylsl")


# --------------------------------------------------------------------------
# clock mapping
# --------------------------------------------------------------------------

class ClockMap:
    """device monotonic microseconds -> local LSL seconds.

    Thread-safe. `offset` is seconds to ADD to (device_mono_us / 1e6).
    """

    # Round trips slower than this say nothing useful about where the device's
    # clock was at the midpoint, so they are dropped instead of averaged in.
    MAX_RTT_S = 0.050
    KEEP = 15            # samples in the rolling window

    def __init__(self, base_url):
        self._base = base_url.rstrip("/")
        self._lock = threading.Lock()
        self._samples = []        # [(offset_s, rtt_s)]
        self._offset = None
        self._last_err = None

    def sample_once(self):
        """One NTP-style probe. Returns the offset in seconds, or None."""
        # /api/clock, not /api/state: the offset's accuracy is bounded by the
        # ROUND TRIP, and /api/state is a ~5.6 KB response with a ~44 ms median
        # RTT — a +/-22 ms bound, useless for aligning an EEG. /api/clock is
        # ~40 bytes and reports esp_timer_get_time() directly, which is the same
        # clock t_disp_mono_us is stamped from.
        try:
            t0 = local_clock()
            with urllib.request.urlopen(self._base + "/api/clock", timeout=2.0) as r:
                body = r.read()
            t1 = local_clock()
        except Exception as e:                      # noqa: BLE001 - reported, not raised
            self._last_err = str(e)
            return None

        rtt = t1 - t0
        if rtt > self.MAX_RTT_S:
            return None
        try:
            dev_us = float(json.loads(body)["mono_us"])
        except Exception as e:                      # noqa: BLE001
            self._last_err = f"cannot read device clock: {e}"
            return None

        # The device read its clock somewhere inside the round trip; the
        # midpoint is the least-wrong assumption, with +/- rtt/2 of uncertainty.
        local_mid = t0 + rtt / 2.0
        offset = local_mid - dev_us / 1e6

        with self._lock:
            self._samples.append((offset, rtt))
            if len(self._samples) > self.KEEP:
                self._samples.pop(0)
            # Median, not mean: one stalled probe should not drag the estimate.
            self._offset = statistics.median(o for o, _ in self._samples)
        return offset

    def to_lsl(self, device_mono_us):
        with self._lock:
            off = self._offset
        if off is None:
            return None
        return device_mono_us / 1e6 + off

    def status(self):
        with self._lock:
            if not self._samples:
                return f"no clock samples yet ({self._last_err or 'probing'})"
            rtts = [r for _, r in self._samples]
            offs = [o for o, _ in self._samples]
            spread = (max(offs) - min(offs)) * 1000.0
            return (f"offset {self._offset:+.6f} s from {len(self._samples)} samples, "
                    f"spread {spread:.1f} ms, median rtt {statistics.median(rtts)*1000:.1f} ms")


def clock_thread(cmap, stop):
    """Probe often at first (so the first markers are mappable), then settle."""
    n = 0
    while not stop.is_set():
        cmap.sample_once()
        n += 1
        stop.wait(0.5 if n < 10 else 5.0)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", required=True,
                    help="device host or IP (for the clock probe). Prefer the IP: "
                         "mDNS resolution can add seconds of its own.")
    ap.add_argument("--port", type=int, default=9009, help="UDP port to listen on")
    ap.add_argument("--bind", default="0.0.0.0", help="address to bind")
    ap.add_argument("--name", default="AVE_Markers", help="LSL stream name")
    ap.add_argument("--source-id", default="freeesp32_ave_markers",
                    help="LSL source_id (keep stable so recordings can be linked)")
    ap.add_argument("--raw", action="store_true",
                    help="push the whole JSON record as the marker string "
                         "instead of just the label")
    args = ap.parse_args()

    base = args.device
    if not base.startswith("http"):
        base = "http://" + base

    info = StreamInfo(args.name, "Markers", 1, 0, "string", args.source_id)
    desc = info.desc()
    desc.append_child_value("source", "freeesp32_ave")
    desc.append_child_value("timestamps",
                            "mapped from the device monotonic clock; "
                            "t_onset_pred_us in the payload is PREDICTED, not measured")
    outlet = StreamOutlet(info)

    cmap = ClockMap(base)
    stop = threading.Event()
    th = threading.Thread(target=clock_thread, args=(cmap, stop), daemon=True)
    th.start()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)

    print(f"LSL stream '{args.name}' ({args.source_id}) is open.")
    print(f"Listening for marker datagrams on {args.bind}:{args.port}")
    print(f"Clock probe -> {base}/api/clock")
    print("Point the device's UDP sink at this machine, then start LabRecorder.\n")

    n_markers = n_unmapped = 0
    try:
        while True:
            try:
                data, _ = sock.recvfrom(4096)
            except socket.timeout:
                continue
            except KeyboardInterrupt:
                break

            text = data.decode("utf-8", "replace").strip()
            try:
                rec = json.loads(text)
            except json.JSONDecodeError:
                print(f"  ! not JSON, ignored: {text[:80]}")
                continue

            kind = rec.get("rec")
            if kind == "header":
                p = rec.get("pipeline", {})
                print(f"  SESSION  sha={rec.get('session_sha256','')[:12]}… "
                      f"fw={rec.get('fw_elf_sha256','')} "
                      f"lags audio={p.get('audio_lag_us')}us led={p.get('led_lag_us')}us")
                print(f"           {cmap.status()}")
                continue
            if kind == "footer":
                print(f"  END      markers={rec.get('markers')} "
                      f"dropped={rec.get('dropped')} sink_errors={rec.get('sink_errors')}")
                if rec.get("dropped"):
                    print("           *** DROPPED MARKERS: this recording has gaps ***")
                continue

            dev_us = rec.get("t_disp_mono_us")
            ts = cmap.to_lsl(dev_us) if dev_us is not None else None
            if ts is None:
                # Fall back to arrival time, and SAY SO. Silently substituting it
                # would be indistinguishable from a correctly mapped marker.
                ts = local_clock()
                n_unmapped += 1
                suffix = "  [ARRIVAL TIME — clock not yet mapped]"
            else:
                suffix = ""

            payload = text if args.raw else rec.get("label", "?")
            outlet.push_sample([payload], ts)
            n_markers += 1
            print(f"  {rec.get('label','?'):16s} seq={rec.get('seq')} "
                  f"lsl={ts:.6f}{suffix}")
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        sock.close()
        print(f"\n{n_markers} marker(s) forwarded"
              + (f", {n_unmapped} with arrival-time fallback" if n_unmapped else ""))
        print(cmap.status())


if __name__ == "__main__":
    main()
