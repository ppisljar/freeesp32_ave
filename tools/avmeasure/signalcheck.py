#!/usr/bin/env python3
"""Is there actually a flicker signal in this capture?

Run this BEFORE interpreting anything. It exists because four consecutive
analyses of a disconnected sensor produced confident, detailed, entirely
fictional results: "117 missing pulses", "345 dropouts", and 36 dropouts in a
block of CONSTANT brightness where a dropout is impossible.

The giveaway was available in every one of them: the 10 Hz line sat BELOW the
noise either side of it. A real flicker stands above its own neighbourhood.
Comparing the signal band against 50 Hz hum instead -- which is what flattered
an earlier capture to a bogus "47 dB SNR" -- measures the wrong thing entirely,
because the hum is narrowband and miles away from the signal.

usage: signalcheck.py FILE.wav CHANNEL FLICKER_HZ [start_s] [dur_s]
"""
import sys, wave, numpy as np

def check(path, ch, f0, t0=0.0, dur=None):
    w = wave.open(path); fr = w.getframerate(); n = w.getnchannels()
    w.setpos(int(t0 * fr))
    N = w.getnframes() - int(t0 * fr)
    if dur: N = min(N, int(dur * fr))
    x = np.frombuffer(w.readframes(N), dtype='<i2').reshape(-1, n).astype(np.float64)[:, ch-1] / 32768
    x = x - x.mean()
    X = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    f = np.fft.rfftfreq(len(x), 1 / fr)
    def e(lo, hi):
        b = (f >= lo) & (f < hi)
        return float(np.sqrt((X[b] ** 2).sum()))
    line = e(f0 * 0.99, f0 * 1.01)
    # the neighbourhood EXCLUDING the line: what noise looks like right there
    near = np.hypot(e(f0 * 0.6, f0 * 0.95), e(f0 * 1.05, f0 * 1.4))
    snr = 20 * np.log10(line / max(near, 1e-12))
    rms = float(np.sqrt((x ** 2).mean()))
    print(f"  {path}  ch{ch}  {f0} Hz")
    print(f"    rms            {20*np.log10(rms+1e-12):7.1f} dBFS")
    print(f"    {f0} Hz line vs its own neighbourhood: {snr:+6.1f} dB")
    if snr < 6:
        print("    VERDICT: NO SIGNAL. Do not interpret this capture.")
        print("             Check the sensor is connected, aimed and gained up.")
        return 1
    if snr < 20:
        print("    VERDICT: marginal. Single-cycle events are NOT reliable here.")
        return 2
    print("    VERDICT: good. Single-cycle analysis is meaningful.")
    return 0

if __name__ == '__main__':
    a = sys.argv[1:]
    sys.exit(check(a[0], int(a[1]), float(a[2]),
                   float(a[3]) if len(a) > 3 else 0.0,
                   float(a[4]) if len(a) > 4 else None))
