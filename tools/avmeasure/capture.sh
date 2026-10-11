#!/bin/zsh
# capture.sh <session.ledc> <seconds> <out.wav> [device_ip]
#
# Records the rig while the device plays a session, unattended.
#
# Two non-obvious things are load-bearing:
#
#  1. `open` on a .command, not a direct exec. macOS gates audio input per
#     APPLICATION, and anything spawned under a long-lived tmux server inherits
#     that server's TCC identity -- which predates any grant you made since.
#     Routing through `open` makes Terminal the responsible process, and
#     Terminal has the grant. A direct call returns a stream of exact zeros and
#     reports success, which is the worst possible failure mode.
#
#  2. `sox rec`, not ffmpeg. ffmpeg's avfoundation input silently dropped 5.56%
#     of samples here (asked 330 s, wrote 311.64 s). Dropped samples compress
#     time and INFLATE every measured frequency -- a 5.89% rate error that looks
#     exactly like a firmware regression. sox captured the same thing with
#     0.0000% missing. The frame count is asserted below; do not remove it.
set -e
LEDC=$1; SECS=$2; OUT=$3; IP=${4:-10.0.0.146}
HERE=$(cd "$(dirname "$0")" && pwd)
DONE=/tmp/.cap_$$.done
CMD=/tmp/.cap_$$.command
cat > "$CMD" <<EOF
#!/bin/zsh
export AUDIODEV="MiniFuse 4"
/opt/homebrew/bin/rec -q -c 6 -r 44100 -b 16 "$OUT" trim 0 $SECS
echo done > "$DONE"
EOF
chmod +x "$CMD"; rm -f "$DONE" "$OUT"
curl -s -X POST --max-time 10 "http://$IP/api/stop" >/dev/null || true
sleep 1
open "$CMD"
sleep 3
curl -s -X POST --data-binary @"$LEDC" --max-time 20 "http://$IP/api/play-config" >/dev/null
for i in {1..600}; do sleep 1; [ -f "$DONE" ] && break; done
curl -s -X POST --max-time 10 "http://$IP/api/stop" >/dev/null || true
rm -f "$CMD" "$DONE"
python3 - "$OUT" "$SECS" <<'PY'
import wave,sys
w=wave.open(sys.argv[1]); N=w.getnframes(); fr=w.getframerate(); exp=int(sys.argv[2])*fr
miss=100*(1-N/exp)
print(f"  capture: {N} frames, {miss:+.4f}% missing")
if abs(miss)>0.01: sys.exit("  REFUSING: lossy capture would corrupt every measurement")
PY
