#!/bin/bash
# Swap sdkconfig between board variants.
#
# Usage:
#   ./switch_board.sh glasses     # ESP32: WS2812 strip + raw I2S (PCM5102A etc.)
#   ./switch_board.sh ac101       # ESP32: A1S board (AC101) + single LED on GPIO 22
#   ./switch_board.sh es8388      # ESP32: A1S board (ES8388) + two LEDs on GPIO 22/23
#   ./switch_board.sh yb_s3_dac   # ESP32-S3: YB-ESP32-S3-DAC (TLV320DAC3101, 16 MB)
#
# After switching, run `idf.py build` to compile for the new board.
#
# NOTE: yb_s3_dac is a different CHIP TARGET (esp32s3), not just a different
# pin map. Switching to or from it changes the whole toolchain, so the build
# directory is fully reconfigured — expect a long first build. See
# legacyesp32.md for what differs between the targets.

set -e

cd "$(dirname "$0")"

case "$1" in
  glasses|ac101|es8388|yb_s3_dac)
    SRC="sdkconfig.$1"
    ;;
  ""|"-h"|"--help"|"help")
    echo "Usage: $0 {glasses|ac101|es8388|yb_s3_dac}"
    echo ""
    echo "Available board configs:"
    ls -1 sdkconfig.* 2>/dev/null | sed 's/^sdkconfig\./  /'
    exit 0
    ;;
  *)
    echo "Unknown board: $1" >&2
    echo "Available board configs:"
    ls -1 sdkconfig.* 2>/dev/null | sed 's/^sdkconfig\./  /' >&2
    exit 1
    ;;
esac

if [ ! -f "$SRC" ]; then
  echo "Missing $SRC — cannot switch" >&2
  exit 1
fi

# Preserve current sdkconfig back to its named snapshot if it has uncommitted edits.
if [ -f "sdkconfig" ]; then
  # Figure out which board the current sdkconfig matches. Check the chip target
  # first — an esp32s3 config can only be the S3 board, and its codec symbols
  # would otherwise fall through to the "glasses" default and overwrite it.
  if grep -q '^CONFIG_IDF_TARGET="esp32s3"' sdkconfig 2>/dev/null; then
    CURRENT="yb_s3_dac"
  elif grep -q "^CONFIG_AUDIO_DEFAULT_CODEC_ES8388=y" sdkconfig 2>/dev/null; then
    CURRENT="es8388"
  elif grep -q "^CONFIG_AUDIO_DEFAULT_CODEC_AC101=y" sdkconfig 2>/dev/null; then
    CURRENT="ac101"
  else
    CURRENT="glasses"
  fi
  if ! diff -q "sdkconfig" "sdkconfig.$CURRENT" >/dev/null 2>&1; then
    echo "Current sdkconfig differs from sdkconfig.$CURRENT — saving back to snapshot"
    cp "sdkconfig" "sdkconfig.$CURRENT"
  fi
fi

cp "$SRC" sdkconfig
echo "Switched to $1 board config (copied $SRC -> sdkconfig)"
echo "Run: idf.py build"
