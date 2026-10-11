#!/usr/bin/env python3
"""Prepend a sync marker to a .ledc and shift every timestamp to make room.

WHY THIS EXISTS

The analyzer has to locate device t=0 inside the recording. Without a sharp
feature to correlate against it falls back to cross-correlation, and on a
session that fades in slowly -- which is most therapeutic sessions -- that
localises t=0 to within seconds, not milliseconds. The tool is honest about it
(it reports LOW confidence and demotes findings smaller than its own
uncertainty) but the measurement is then worthless.

A 600 ms burst of full-brightness light plus a pure tone fixes that: both edges
are sharp in both domains, so t=0 lands to +/-5 ms.

Doing this by hand means editing every timestamp in the file. On a 30-minute
session that is dozens of edits and one typo produces a reference file that
disagrees with reality in a way no test will catch -- you would be measuring
the device against a corrupted idea of what it was asked to do. Hence a script.

The marker is deliberately NOT therapeutic: full-brightness white at 40 Hz for
600 ms. Keep your eyes shut or look away until the session proper begins.
"""
import argparse, re, sys

# A timestamp is the first bare integer on the line, after an optional command
# letter. The firmware also accepts a glued form ("A1500 ..."), so match that.
# Nothing else on the line is touched: modulation specs like ~8:22:12000 and
# interpolation prefixes like >200 contain digits and MUST survive untouched.
LINE_RE = re.compile(r'^(\s*)([ASG]{1,2})?(\s*)(\d+)(.*)$', re.S)

def shift_line(line, delta):
    if not line.strip() or line.lstrip().startswith('#'):
        return line
    if re.match(r'^\s*BG\b', line, re.I):
        return line                      # BG is a global property, not scheduled
    m = LINE_RE.match(line.rstrip('\n'))
    if not m:
        return line
    lead, cmd, gap, t, rest = m.groups()
    return f"{lead}{cmd or ''}{gap}{int(t) + delta}{rest}\n"

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--ledc', required=True, help='session to transform')
    p.add_argument('--out', required=True, help='capture-ready output path')
    p.add_argument('--tone', type=float, default=3000.0,
                   help='marker tone in Hz; pass the same value to '
                        'analyze.py --sync-tone (default: 3000)')
    p.add_argument('--audio-ch', type=int, default=1,
                   help='generator channel for the marker tone (default: 1)')
    p.add_argument('--marker-ms', type=int, default=600)
    p.add_argument('--gap-ms', type=int, default=400,
                   help='silence between marker and session (default: 400)')
    a = p.parse_args()

    delta = a.marker_ms + a.gap_ms
    src = open(a.ledc).read().splitlines(keepends=True)
    tone = int(a.tone) if float(a.tone).is_integer() else a.tone

    out = [
        f"# SYNC MARKER prepended by marker.py -- every original timestamp\n",
        f"# shifted +{delta} ms. Analyse with: --sync-tone {tone}\n",
        f"#\n",
        f"# The marker is NOT part of the session: {a.marker_ms} ms of\n",
        f"# full-brightness white at 40 Hz. Look away until it has passed.\n",
        f"0 40 50 100 255 255 255 15\n",
        f"A 0 {tone} 0 70 0 {a.audio_ch}\n",
        f"{a.marker_ms} 0 50 0 0 0 0 15\n",
        f"A {a.marker_ms} 0 0 0 0 {a.audio_ch}\n",
        f"\n# ---- original session, timestamps +{delta} ms ----\n",
    ]
    out += [shift_line(l, delta) for l in src]
    open(a.out, 'w').writelines(out)

    print(f"wrote {a.out}")
    print(f"  marker {a.marker_ms} ms @ {tone} Hz, session shifted +{delta} ms")
    print(f"  analyse with:  --ledc {a.out} --sync-tone {tone}")

if __name__ == '__main__':
    sys.exit(main())
