#!/bin/bash
# Swap sdkconfig between board variants.
#
# Usage:
#   ./switch_board.sh glasses   # WS2812 strip + raw I2S (PCM5102A etc.)
#   ./switch_board.sh ac101     # A1S board (AC101 variant) + single LED on GPIO 22
#   ./switch_board.sh es8388    # A1S board (ES8388 variant) + two LEDs on GPIO 22/23
#
# After switching, run `idf.py build` to compile for the new board.

set -e

cd "$(dirname "$0")"

case "$1" in
  glasses|ac101|es8388)
    SRC="sdkconfig.$1"
    ;;
  ""|"-h"|"--help"|"help")
    echo "Usage: $0 {glasses|ac101|es8388}"
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
  # Figure out which board the current sdkconfig matches (by AUDIO_DRIVER choice).
  if grep -q "^CONFIG_AUDIO_DRIVER_ES8388=y" sdkconfig 2>/dev/null; then
    CURRENT="es8388"
  elif grep -q "^CONFIG_AUDIO_DRIVER_AC101=y" sdkconfig 2>/dev/null; then
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
