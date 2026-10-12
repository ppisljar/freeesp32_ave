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
# caffeinate: the Mac going to sleep mid-capture truncates the file while
# leaving it otherwise valid, and the longer the capture the likelier it is --
# a 75 s request failed three times in a row at 50%, 16% and 28% missing, and
# the same sleep killed two long-running agents outright. -i blocks idle sleep,
# -m keeps the disk awake, -s covers the system while on mains. The recorder is
# the thing that must not be interrupted, so caffeinate wraps rec itself rather
# than the orchestrating script.
/usr/bin/caffeinate -ims /opt/homebrew/bin/rec -q -c 6 -r 44100 -b 16 "$OUT" trim 0 $SECS
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

chmod +x "$CMD"

# RETRY. Roughly one capture in three came back truncated -- the recorder lives in
# a Terminal window (an `open` indirection, so Terminal and not the tmux server is
# the TCC-responsible process for microphone access), and a window from a previous
# run can still hold the interface when the next one starts. Each failure used to
# cost a full manual re-run, and a retry issued too soon collided with the very
# recorder it was retrying and failed again. Clearing the recorder and settling
# before EACH attempt breaks that loop.
ATTEMPTS=${AVM_ATTEMPTS:-3}
for attempt in $(seq 1 $ATTEMPTS); do
  pkill -9 -f '/opt/homebrew/bin/rec' 2>/dev/null || true
  osascript -e 'tell application "Terminal" to close (every window whose name contains ".cap_")' \
            >/dev/null 2>&1 || true
  rm -f "$DONE" "$OUT"
  curl -s -X POST --max-time 10 "http://$IP/api/stop" >/dev/null || true
  sleep 4
  open "$CMD"
  sleep 3
  curl -s -X POST --data-binary @"$LEDC" --max-time 20 "http://$IP/api/play-config" >/dev/null
  for i in {1..900}; do sleep 1; [ -f "$DONE" ] && break; done
  curl -s -X POST --max-time 10 "http://$IP/api/stop" >/dev/null || true
  if python3 "$HERE/framecheck.py" "$OUT" "$SECS"; then
    break
  fi
  if [ "$attempt" = "$ATTEMPTS" ]; then
    echo "  REFUSING: lossy capture after $ATTEMPTS attempts" >&2
    rm -f "$CMD" "$DONE"; exit 1
  fi
  echo "  (truncated -- retrying, attempt $((attempt+1))/$ATTEMPTS)" >&2
done
rm -f "$CMD" "$DONE"

# TIMEBASE gate. The frame count above proves the recorder produced the right
# NUMBER of frames; it is blind to a discontinuity in the middle, because the
# missing time is simply absent rather than padded. An 11-minute capture that
# passed the frame-count check with "+0.0000% missing" held five such breaks, the
# largest 268 frames (6.08 ms) -- and one of them was reported as a firmware
# defect, "confirmed" by two independent optical detectors, because the whole
# recording stepped rather than the LED. Set AVM_CARRIER_HZ to the session's
# continuous SINE reference tone to check this.
if [ -n "$AVM_CARRIER_HZ" ]; then
  python3 "$HERE/captureintegrity.py" "$OUT" --carrier "$AVM_CARRIER_HZ" \
          --audio-ch "${AVM_AUDIO_CH:-3}" || {
    rc=$?
    if [ "$rc" = "1" ]; then
      echo "  REFUSING: capture timebase is broken; re-record" >&2; exit 1
    fi
    echo "  WARNING: capture timebase could not be verified" >&2
  }
fi

# Signal gate. The frame count above proves the RECORDER worked; it says nothing
# about whether the sensor saw anything. A disconnected sensor yields a valid,
# full-length, perfectly-sized WAV of noise -- and analysing one produced four
# consecutive confident-but-fictional results before anyone noticed. Pass
# AVM_FLICKER_HZ to have the capture refuse to return such a file.
if [ -n "$AVM_FLICKER_HZ" ]; then
  python3 "$HERE/signalcheck.py" "$OUT" "${AVM_CH:-2}" "$AVM_FLICKER_HZ" 5 || {
    echo "  REFUSING: capture contains no usable flicker signal"; exit 1; }
fi
