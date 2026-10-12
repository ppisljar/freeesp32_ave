#!/usr/bin/env python3
"""Refuse a capture whose TIMEBASE broke, which the frame count cannot see.

WHY THIS EXISTS
---------------
capture.sh already asserts the frame count, and that catches a recorder which
dropped samples at the END or ran short. It is completely blind to a timebase
DISCONTINUITY in the middle: sox/USB can lose a block of frames and still write
a file of exactly the right length, because the missing time is simply absent
rather than padded.

That is not a hypothetical. An 11-minute capture that passed the frame-count
check with "+0.0000% missing" contained FIVE such discontinuities, the largest
268 frames (6.08 ms). One of them was reported as a firmware defect -- a cycle
6.24 ms short, "confirmed" by two independent optical detectors -- and the
diagnosis survived a day of scrutiny because both sensors really did see it.
They saw it because the whole recording stepped, not because the LED did.

HOW IT WORKS
------------
The device's audio carrier is clocked by I2S and the codec. Nothing in the LED
path, in esp_timer, or in the optical chain can move it. So its phase is a clock
we can check the recording against:

  * demodulate the carrier in 5 ms blocks,
  * subtract the nominal per-block phase advance,
  * what remains is the recording timebase's own wander.

Measured block-to-block noise on a good capture is ~1 us, so a dropped block
stands out by three orders of magnitude.

WHAT IT CANNOT DO
-----------------
The phase step is only known MODULO one carrier period (2.5 ms at 400 Hz), so
the reported frame count is the smallest consistent value, not necessarily the
true one. It is enough to REJECT a capture, which is the job. To resolve the
true size, combine it with a second observable at a different scale -- e.g. the
light lattice step, which is what pinned the 268-frame event.

A reference-free variant was tried and rejected: a dropout sprays broadband
energy onto inputs carrying no signal, and an unused input really did burst to
126 LSB against a baseline of 1 at one of the events -- but only at ONE of the
three largest. Not a usable gate.

REQUIREMENT: the session must emit a CONTINUOUS tone on the measured audio
channel. A gated tone works (blocks with the gate shut are skipped), but a
session with no audio at all cannot be checked this way, and this tool says so
rather than passing it silently.

usage: captureintegrity.py FILE.wav [--carrier 400] [--audio-ch 3]
                           [--block-ms 5] [--fail-frames 10]
exit 0 = clean, 1 = timebase broken, 2 = could not check (no carrier found)
"""
import sys
import wave
import numpy as np

BLOCK_MS_DEFAULT = 5.0
FAIL_FRAMES_DEFAULT = 10      # a 10-frame (227 us) step dwarfs the ~0.06 ms
                              # per-cycle timing noise this rig achieves


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    path = argv[0]
    o = {}
    i = 1
    while i < len(argv):
        o[argv[i].lstrip('-')] = argv[i + 1]
        i += 2
    f0 = float(o.get('carrier', 400.0))
    ch = int(o.get('audio-ch', 3)) - 1
    block_ms = float(o.get('block-ms', BLOCK_MS_DEFAULT))
    fail_frames = float(o.get('fail-frames', FAIL_FRAMES_DEFAULT))

    w = wave.open(path)
    fr, nch, N = w.getframerate(), w.getnchannels(), w.getnframes()
    if ch >= nch:
        print(f"  capture has only {nch} channels; cannot read ch{ch+1}")
        return 2
    x = (np.frombuffer(w.readframes(N), dtype='<i2')
         .reshape(-1, nch)[:, ch].astype(np.float64)) / 32768.0

    B = int(block_ms * 1e-3 * fr)
    nb = len(x) // B
    if nb < 20:
        print("  capture too short to check")
        return 2
    ref = np.exp(-2j * np.pi * f0 * (np.arange(B) / fr))
    z = np.array([np.sum(x[k * B:(k + 1) * B] * ref) for k in range(nb)])
    amp = np.abs(z)
    lively = amp[amp > 0]
    if lively.size == 0 or np.median(lively) <= 0:
        print(f"  no {f0:g} Hz carrier present — integrity NOT verified")
        return 2
    # skip blocks where a gate is shut; a gated carrier is still usable
    good = amp > 0.25 * np.median(lively)
    if good.sum() < 20:
        print(f"  {f0:g} Hz carrier too weak to use — integrity NOT verified")
        return 2

    advance = 2 * np.pi * f0 * B / fr
    d = np.diff(np.angle(z)) - advance
    d = (d + np.pi) % (2 * np.pi) - np.pi          # wrap to +/- half a cycle
    step_ms = d / (2 * np.pi * f0) * 1e3           # phase error -> milliseconds
    usable = good[:-1] & good[1:]
    v = step_ms[usable]
    centre = float(np.median(v))
    sd = float(np.median(np.abs(v - centre)) * 1.4826)
    thr_ms = max(0.02, 20.0 * sd)

    # A self-calibrating threshold silently turns into a rubber stamp when the
    # reference is bad. Measured: a session cycling through square/sawtooth/noise
    # "carriers" gave 369 us of phase noise (a clean sine gives ~1 us), which
    # inflated the threshold to 7.4 ms and reported "clean" while being blind to
    # anything smaller than three dropped BLOCKS. So bound the noise explicitly
    # and refuse to render a verdict rather than issue a worthless pass.
    # Use a pure SINE carrier for the reference tone; its harmonics are what
    # contaminate the single-bin demodulation.
    if sd > 10e-3:
        print(f"  carrier {f0:g} Hz on ch{ch+1} is too impure to use as a clock "
              f"(phase noise {sd*1e3:.1f} us; a clean sine gives ~0.001). "
              f"Integrity NOT verified — give the session a continuous SINE "
              f"reference tone.")
        return 2

    events, cur = [], []
    for k in np.where(usable)[0]:
        if abs(step_ms[k] - centre) > thr_ms:
            t = k * B / fr
            if cur and t - cur[-1][0] < 0.05:
                cur.append((t, step_ms[k]))
            else:
                if cur:
                    events.append(cur)
                cur = [(t, step_ms[k])]
    if cur:
        events.append(cur)

    print(f"  carrier {f0:g} Hz on ch{ch+1}: {int(usable.sum())} usable "
          f"{block_ms:g} ms blocks, phase noise {sd*1e6:.1f} ns, "
          f"threshold {thr_ms*1e3:.0f} us")
    worst = 0.0
    for g in events:
        tot = sum(s for _, s in g)
        frames = tot * 1e-3 * fr
        worst = max(worst, abs(frames))
        print(f"    t = {g[0][0]:8.3f} s   step {tot*1e3:+9.1f} us "
              f"= {frames:+8.1f} frames (modulo one carrier period)")
    if worst >= fail_frames:
        print(f"  VERDICT: TIMEBASE BROKEN — worst step {worst:.1f} frames. "
              f"Do not interpret this capture; re-record.")
        return 1
    if events:
        print(f"  VERDICT: minor wander only (worst {worst:.1f} frames), "
              f"below the {fail_frames:g}-frame bar. Usable.")
    else:
        print("  VERDICT: timebase clean.")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
