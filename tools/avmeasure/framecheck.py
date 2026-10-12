#!/usr/bin/env python3
"""Assert a capture has the frame count it was asked for. Exit 1 if not.

Split out of capture.sh so the retry loop does not need a nested heredoc.
This catches a recorder that ran SHORT; it is blind to a timebase break in the
middle, because a dropout removes time rather than padding it -- see
captureintegrity.py, which is the gate for that.
"""
import sys, wave

w = wave.open(sys.argv[1])
n = w.getnframes()
expected = int(sys.argv[2]) * w.getframerate()
missing = 100.0 * (1.0 - n / expected)
print(f"  capture: {n} frames, {missing:+.4f}% missing")
sys.exit(0 if abs(missing) <= 0.01 else 1)
