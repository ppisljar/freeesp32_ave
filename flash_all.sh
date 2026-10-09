#!/bin/bash
# flash_all.sh — ONE-TIME wired full flash for the dual-boot OTA layout.
#
# This is the ONLY way to lay down the new partition table + both app images
# (bootloader, partition table, otadata seed, ota_0 main app, ota_1 OTA updater,
# storage SPIFFS). OTA cannot change the partition table, so the very first
# deploy of this layout MUST be done over USB with this script. After this,
# routine app updates go over the air (browser Firmware Update -> updater -> ota_0)
# and fast web-UI iteration uses ./flash_web.sh.
#
# It:
#   1. builds the web UI (web/ npm run build -> web/data/*.gz -> storage.bin)
#   2. builds the main app   (idf.py build in freeesp32_ave/)
#   3. builds the updater    (../esp32ota/, RE-TARGETED to match the main app)
#   4. derives partition offsets at runtime from the flashed partition table
#   5. reads chip, bootloader/partition offsets and flash mode/freq/size from
#      sdkconfig + build/flash_args (NOT hard-coded)
#   6. assembles ONE esptool write_flash call and (after confirmation) runs it
#
# MULTI-TARGET: works for both the classic ESP32 boards and the ESP32-S3
# (YB-ESP32-S3-DAC). The chip comes from CONFIG_IDF_TARGET in sdkconfig, and the
# bootloader offset differs by chip (0x1000 on classic ESP32, 0x0 on S3) so it is
# read from build/flash_args rather than assumed. The esp32ota updater is a
# separate project and is automatically re-targeted to the same chip — flashing
# a wrong-chip updater to ota_1 would leave the device unable to recover over
# the air. Switch boards with ./switch_board.sh first.
#
# Usage:
#   ./flash_all.sh [--port /dev/tty.usbserial-XXXX] [--yes] [--dry-run]
#     --port    serial device (default: $ESPPORT; required if unset)
#     --yes     skip the interactive confirmation before flashing
#     --dry-run build + verify + print the esptool command, but DO NOT flash
#
# Flash map (every offset below is DERIVED at run time, shown here for the
# classic 4 MB board as an example — the S3 16 MB layout differs):
#   0x1000*   build/bootloader/bootloader.bin            2nd-stage bootloader
#   0x8000    build/partition_table/partition-table.bin  the canonical table
#   0xd000    build/ota_data_initial.bin                 otadata seed -> boot ota_0
#   0x10000   build/esp32_audioplayer.bin                -> ota_0 (main app)
#   0x210000  ../esp32ota/build/esp32_minimal_ota.bin    -> ota_1 (updater)
#   0x2c0000  build/storage.bin                          -> storage (web SPIFFS)
#   0x340000  build/cfgfs.bin                            -> cfgfs (session library)
#   * 0x0 on ESP32-S3 — read from build/flash_args, never assumed.
#
# NOTE: cfgfs is seeded with the built-in session library (sessions/library/
# *.ledc, packed by main/CMakeLists.txt). Because this is a FULL flash, it
# OVERWRITES any user-saved configs in cfgfs with the shipped library. This is
# expected for the one-time wired / factory flash. Routine updates do NOT touch
# cfgfs: ./flash_web.sh writes only storage, and OTA writes only the ota_0 app.

set -euo pipefail

cd "$(dirname "$0")"
SCRIPT_DIR="$(pwd)"
OTA_DIR="$SCRIPT_DIR/../esp32ota"

# ---- args -------------------------------------------------------------------
PORT="${ESPPORT:-}"
ASSUME_YES=0
DRY_RUN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="${2:-}"; shift 2 ;;
    --port=*) PORT="${1#*=}"; shift ;;
    --yes|-y) ASSUME_YES=1; shift ;;
    --dry-run|-n) DRY_RUN=1; shift ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

BAUD=460800

# ---- 1. web UI --------------------------------------------------------------
echo "=== [1/3] Building web UI (esbuild + gzip) ==="
cd "$SCRIPT_DIR/web"
[ -d node_modules ] || npm install
npm run build
cd "$SCRIPT_DIR"

# ---- activate ESP-IDF -------------------------------------------------------
echo ""
echo "=== Activating ESP-IDF environment ==="
# export.sh / activate.sh reference unset vars; relax -u while sourcing.
set +u
# shellcheck disable=SC1091
source ./activate.sh
set -u
cd "$SCRIPT_DIR"

# ---- 2. main app ------------------------------------------------------------
echo ""
echo "=== [2/3] Building main app (freeesp32_ave) ==="
idf.py build
# NOTE: a 'ota_1 too small for esp32_audioplayer.bin' warning here is EXPECTED
# and harmless — IDF checks the main app against ALL app slots, but the main app
# is flashed to ota_0; the updater (which fits) goes to ota_1.

# ---- resolve the chip target from the main app's config ---------------------
# Everything below (updater target, bootloader offset, esptool --chip) follows
# from this, so the S3 and classic boards share one script.
CHIP=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' "$SCRIPT_DIR/sdkconfig")
if [ -z "$CHIP" ]; then
  echo "Could not read CONFIG_IDF_TARGET from sdkconfig — build first." >&2
  exit 1
fi
echo ""
echo "Chip target (from sdkconfig): $CHIP"

# ---- 3. updater -------------------------------------------------------------
# The updater is a SEPARATE project with its own sdkconfig. It must be built for
# the SAME chip as the main app — a mismatched image bricks ota_1 and the device
# cannot recover over the air. Re-target it only when it actually differs, since
# set-target wipes its sdkconfig and forces a full rebuild.
echo ""
echo "=== [3/3] Building OTA updater (esp32ota) for $CHIP ==="
cd "$OTA_DIR"
OTA_CHIP=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' sdkconfig 2>/dev/null || true)
if [ "$OTA_CHIP" != "$CHIP" ]; then
  echo "  updater is currently '${OTA_CHIP:-unset}' — switching to '$CHIP'"
  rm -f sdkconfig
  idf.py set-target "$CHIP"
fi
idf.py build
cd "$SCRIPT_DIR"

# ---- resolve offsets from the partition table -------------------------------
PT="$SCRIPT_DIR/build/partition_table/partition-table.bin"
PARTTOOL="$IDF_PATH/components/partition_table/parttool.py"
part_off() {
  python "$PARTTOOL" -q --partition-table-file "$PT" \
    get_partition_info --partition-name "$1" --info offset
}
part_size() {
  python "$PARTTOOL" -q --partition-table-file "$PT" \
    get_partition_info --partition-name "$1" --info size
}

OTA0_OFF=$(part_off ota_0);     OTA0_SZ=$(part_size ota_0)
OTA1_OFF=$(part_off ota_1);     OTA1_SZ=$(part_size ota_1)
STORAGE_OFF=$(part_off storage); STORAGE_SZ=$(part_size storage)
CFGFS_OFF=$(part_off cfgfs);     CFGFS_SZ=$(part_size cfgfs)
OTADATA_OFF=$(part_off otadata)

# The bootloader offset is CHIP-SPECIFIC: 0x1000 on the classic ESP32 but 0x0 on
# the ESP32-S3 (and other newer targets). Rather than hardcode a table of chips,
# read the offset IDF itself generated in build/flash_args — it is always right
# for whatever target was built. Same for the partition table offset.
read_flash_arg_off() {  # $1 = image path suffix to match
  awk -v want="$1" '$2 ~ want { print $1; exit }' "$SCRIPT_DIR/build/flash_args"
}
BOOTLOADER_OFF=$(read_flash_arg_off 'bootloader\.bin$')
PARTTABLE_OFF=$(read_flash_arg_off 'partition-table\.bin$')
: "${BOOTLOADER_OFF:?could not find bootloader offset in build/flash_args}"
: "${PARTTABLE_OFF:?could not find partition-table offset in build/flash_args}"

# ---- flash params from build/flash_args (first line) ------------------------
FLASH_PARAMS=$(head -1 "$SCRIPT_DIR/build/flash_args")

# ---- artifact paths ---------------------------------------------------------
F_BOOTLOADER="$SCRIPT_DIR/build/bootloader/bootloader.bin"
F_PARTTABLE="$SCRIPT_DIR/build/partition_table/partition-table.bin"
F_OTADATA="$SCRIPT_DIR/build/ota_data_initial.bin"
F_APP="$SCRIPT_DIR/build/esp32_audioplayer.bin"
F_UPDATER="$OTA_DIR/build/esp32_minimal_ota.bin"
F_STORAGE="$SCRIPT_DIR/build/storage.bin"
F_CFGFS="$SCRIPT_DIR/build/cfgfs.bin"

# ---- verification: existence + fit + no overlap -----------------------------
fail=0
check_exists() { [ -f "$1" ] || { echo "MISSING: $1" >&2; fail=1; }; }
check_exists "$F_BOOTLOADER"
check_exists "$F_PARTTABLE"
check_exists "$F_OTADATA"
check_exists "$F_APP"
check_exists "$F_UPDATER"
check_exists "$F_STORAGE"
check_exists "$F_CFGFS"
[ "$fail" -eq 0 ] || { echo "Aborting: missing artifact(s)." >&2; exit 1; }

sz() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; }
hex() { printf '0x%x' "$1"; }

APP_SZ=$(sz "$F_APP")
UPDATER_SZ=$(sz "$F_UPDATER")
STORAGE_FSZ=$(sz "$F_STORAGE")
CFGFS_FSZ=$(sz "$F_CFGFS")

check_fit() { # name image_size slot_offset slot_size
  local name="$1" isz="$2" off="$3" slot="$4"
  local off_d slot_d margin
  off_d=$((off)); slot_d=$((slot)); margin=$((slot_d - isz))
  if [ "$isz" -gt "$slot_d" ]; then
    echo "OVERFLOW: $name image $isz B > slot $slot_d B (offset $(hex off_d))" >&2
    fail=1
  fi
  printf '  %-9s @ %-8s slot=%-9s img=%-9s free=%-9s end=%s\n' \
    "$name" "$(hex "$off_d")" "$slot_d" "$isz" "$margin" "$(hex $((off_d + isz)))"
}

echo ""
echo "=== Flash map verification ==="
check_fit ota_0   "$APP_SZ"     "$OTA0_OFF"    "$OTA0_SZ"
check_fit ota_1   "$UPDATER_SZ" "$OTA1_OFF"    "$OTA1_SZ"
check_fit storage "$STORAGE_FSZ" "$STORAGE_OFF" "$STORAGE_SZ"
check_fit cfgfs   "$CFGFS_FSZ"   "$CFGFS_OFF"   "$CFGFS_SZ"
[ "$fail" -eq 0 ] || { echo "Aborting: image does not fit its slot." >&2; exit 1; }

# ---- assemble esptool command ----------------------------------------------
ESPTOOL=(python -m esptool --chip "$CHIP")
[ -n "$PORT" ] && ESPTOOL+=(-p "$PORT")
ESPTOOL+=(-b "$BAUD" --before default_reset --after hard_reset write_flash)
# shellcheck disable=SC2206
ESPTOOL+=($FLASH_PARAMS)
ESPTOOL+=(
  "$BOOTLOADER_OFF" "$F_BOOTLOADER"
  "$PARTTABLE_OFF"  "$F_PARTTABLE"
  "$OTADATA_OFF"    "$F_OTADATA"
  "$OTA0_OFF"       "$F_APP"
  "$OTA1_OFF"       "$F_UPDATER"
  "$STORAGE_OFF"    "$F_STORAGE"
  "$CFGFS_OFF"      "$F_CFGFS"
)

echo ""
echo "=== Assembled flash command ==="
echo "Flash params (from build/flash_args): $FLASH_PARAMS"
printf '%q ' "${ESPTOOL[@]}"; echo
echo ""

if [ "$DRY_RUN" -eq 1 ]; then
  echo "--dry-run: built + verified only. NOT flashing."
  exit 0
fi

if [ -z "$PORT" ]; then
  echo "No --port/\$ESPPORT given; esptool will auto-detect the serial port."
fi

PORT_DESC="${PORT:-auto-detect}"
if [ "$ASSUME_YES" -ne 1 ]; then
  echo "About to perform a FULL wired flash to $PORT_DESC."
  echo "This erases the existing layout and writes the dual-boot OTA table."
  echo "NOTE: cfgfs is reseeded with the built-in session library — any"
  echo "      user-saved configs on the device will be OVERWRITTEN."
  read -r -p "Proceed? [y/N] " ans
  case "$ans" in y|Y|yes|YES) ;; *) echo "Aborted."; exit 1 ;; esac
fi

echo ""
echo "=== Flashing ==="
"${ESPTOOL[@]}"
echo ""
echo "Done. Device should boot into the main app (ota_0)."
echo "Fast UI iteration afterward: ./flash_web.sh"
