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
# The interface must be PRESENT. sox falls back to the default device when
# AUDIODEV is missing, so an unplugged MiniFuse silently records the built-in
# microphone instead of failing -- a full-length, correctly-sized WAV of the
# wrong thing. The MiniFuse has dropped off the USB bus mid-session more than
# once, so this is a real failure mode, not a hypothetical one.
if ! system_profiler SPAudioDataType 2>/dev/null | grep -q "MiniFuse"; then
  echo "  REFUSING: MiniFuse 4 is not connected (replug it; check the USB port)" >&2
  exit 1
fi
# A REFUSED capture leaves its recorder running. The recorder lives in a Terminal
# window (see the `open` below, which exists for TCC reasons), so it keeps holding
# the interface for its full requested duration after this script has already
# exited with an error. A prompt retry then collides with it and gets a TRUNCATED
# file -- which the frame-count check refuses, leaving yet another recorder
# running. Three captures in a row were lost to that loop (190 s requested,
# 46.5 s and 98 s delivered) before closing the stale windows by hand fixed it.
# So clear any previous recorder before starting, not after failing.
pkill -9 -f '/opt/homebrew/bin/rec' 2>/dev/null || true
osascript -e 'tell application "Terminal" to close (every window whose name contains ".cap_")' \
          >/dev/null 2>&1 || true

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

# Signal gate. The frame count above proves the RECORDER worked; it says nothing
# about whether the sensor saw anything. A disconnected sensor yields a valid,
# full-length, perfectly-sized WAV of noise -- and analysing one produced four
# consecutive confident-but-fictional results before anyone noticed. Pass
# AVM_FLICKER_HZ to have the capture refuse to return such a file.
if [ -n "$AVM_FLICKER_HZ" ]; then
  python3 "$HERE/signalcheck.py" "$OUT" "${AVM_CH:-2}" "$AVM_FLICKER_HZ" 5 || {
    echo "  REFUSING: capture contains no usable flicker signal"; exit 1; }
fi
