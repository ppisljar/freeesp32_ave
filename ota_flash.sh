#!/bin/bash
# ota_flash.sh — command-line OTA of the main app, over the air (no USB).
#
# This is the CLI equivalent of the browser "Firmware Update" page
# (web/src/js/firmware.js). It drives the dual-boot OTA handoff:
#
#   1. SHA-256 the .bin locally (the updater requires it for the app slot and
#      verifies every byte before committing).
#   2. POST /api/ota  → the main app hands WiFi creds to the updater (NVS) and
#      reboots into ota_1. Response JSON: {ok, target, ssid_handoff}.
#   3. Poll GET /     → until the body is the updater's status line
#      ("OTA updater ready …") instead of the main app's HTML.
#   4. POST /update?sha256=<hex>  with the .bin as the body. The updater
#      verifies the hash, writes ota_0, and auto-reboots into the new app.
#      (For the app slot the reboot can drop the connection the instant the
#      image verifies — we treat a post-upload drop as "probably flashing".)
#   5. Poll /api/state → until the new main app answers, then show /api/version.
#
# This ONLY updates the app (ota_0). Data partitions (storage/cfgfs) are handled
# by the browser page / push_library.sh, not here.
#
# If the updater can't rejoin WiFi it falls back to SoftAP
# (ESP32-AVE-Setup / entrain123 @ 192.168.4.1) on a different network your
# machine isn't on — the script detects this and prints manual recovery steps.
#
# Usage:
#   ./ota_flash.sh <device-host-or-ip> [path-to.bin]
#
#   <device-host-or-ip>  e.g. 10.0.0.162  (may also be set via $DEVICE_HOST)
#   [path-to.bin]        default: build/esp32_audioplayer.bin
#
# Exit status: 0 on confirmed new-app boot, non-zero otherwise.

set -euo pipefail

cd "$(dirname "$0")"

# ---- args -------------------------------------------------------------------
# Usage: ./ota_flash.sh <ip> [--part app|storage|cfgfs] [path-to.bin]
#   app     (default) → the main firmware (ota_0); updater auto-reboots.
#   storage           → web UI SPIFFS image; updater needs an explicit reboot.
#   cfgfs             → config SPIFFS image;    updater needs an explicit reboot.
HOST="${DEVICE_HOST:-}"
BIN=""
PART="app"
while [ $# -gt 0 ]; do
  case "$1" in
    --part) PART="${2:-}"; shift 2 ;;
    -*)     echo "Unknown option: $1" >&2; exit 2 ;;
    *)      if [ -z "$HOST" ]; then HOST="$1"; else BIN="$1"; fi; shift ;;
  esac
done

if [ -z "$HOST" ]; then
  echo "Usage: ./ota_flash.sh <device-host-or-ip> [--part app|storage|cfgfs] [path-to.bin]" >&2
  echo "  (or set \$DEVICE_HOST). Example: ./ota_flash.sh 10.0.0.162" >&2
  exit 2
fi

# Default image per target if none given explicitly.
if [ -z "$BIN" ]; then
  case "$PART" in
    app)     BIN="build/esp32_audioplayer.bin" ;;
    storage) BIN="build/storage.bin" ;;
    cfgfs)   BIN="build/cfgfs.bin" ;;
    *) echo "Unknown --part '$PART' (use app|storage|cfgfs)" >&2; exit 2 ;;
  esac
fi
IS_APP=0; [ "$PART" = "app" ] && IS_APP=1
# strip any scheme / trailing slash the user may have pasted
HOST="${HOST#http://}"; HOST="${HOST#https://}"; HOST="${HOST%/}"
BASE="http://$HOST"

if [ ! -f "$BIN" ]; then
  echo "Firmware image not found: $BIN" >&2
  echo "Build it first (source ./activate.sh && idf.py build)." >&2
  exit 1
fi

UPDATER_TIMEOUT=90   # s — reboot into updater + WiFi (re)join
APP_TIMEOUT=90       # s — flash + reboot into new app
POLL=2               # s — poll interval

# ---- 0. hash + reachability -------------------------------------------------
SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
HEX=$(shasum -a 256 "$BIN" | awk '{print $1}')
echo "=== OTA update ($PART) ==="
echo "Device : $BASE"
echo "Target : $PART"
echo "Image  : $BIN ($SIZE bytes)"
echo "SHA-256: $HEX"
echo ""

echo "--- [0/4] Checking the device is reachable (main app) ---"
code=$(curl -s -o /dev/null -w '%{http_code}' --connect-timeout 5 --max-time 10 "$BASE/api/version" 2>/dev/null || true)
if [ "$code" != "200" ]; then
  # /api/version may not exist on older firmware; fall back to GET /.
  code=$(curl -s -o /dev/null -w '%{http_code}' --connect-timeout 5 --max-time 10 "$BASE/" 2>/dev/null || true)
fi
code=${code:-000}
if [ "$code" = "000" ]; then
  echo "  ✗ No response from $BASE. Is the device powered on and on this network?" >&2
  exit 1
fi
echo "  ✓ Device responded (HTTP $code)."
echo ""

# ---- 1. trigger reboot into the updater -------------------------------------
echo "--- [1/4] Requesting reboot into the updater (POST /api/ota) ---"
ACK=$(curl -s --connect-timeout 5 --max-time 15 -X POST "$BASE/api/ota" || echo '')
echo "  response: ${ACK:-<none>}"
case "$ACK" in
  *'"ok":true'*) : ;;
  *) echo "  ✗ Device did not acknowledge the OTA request." >&2; exit 1 ;;
esac
if printf '%s' "$ACK" | grep -q '"ssid_handoff":false'; then
  echo "  ! No WiFi creds handed off — the updater will come up on SoftAP"
  echo "    (ESP32-AVE-Setup @ 192.168.4.1), NOT on $HOST. See recovery note below."
fi
echo ""

# ---- 2. wait for the updater ------------------------------------------------
echo "--- [2/4] Waiting for the updater to come online (up to ${UPDATER_TIMEOUT}s) ---"
deadline=$(( $(date +%s) + UPDATER_TIMEOUT ))
updater_up=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  body=$(curl -s --connect-timeout 3 --max-time 5 "$BASE/" 2>/dev/null || true)
  if printf '%s' "$body" | grep -qi "OTA updater ready"; then
    updater_up=1
    echo "  ✓ Updater online: $(printf '%s' "$body" | head -1)"
    break
  fi
  printf '  … still waiting (device rebooting)\n'
  sleep "$POLL"
done
if [ "$updater_up" -ne 1 ]; then
  echo "  ✗ Updater did not appear on $BASE within ${UPDATER_TIMEOUT}s." >&2
  echo "" >&2
  echo "  It likely fell back to SoftAP. Recover manually:" >&2
  echo "    1. Join WiFi 'ESP32-AVE-Setup' (password 'entrain123')." >&2
  echo "    2. curl -X POST --data-binary @$BIN \"http://192.168.4.1/update?sha256=$HEX\"" >&2
  exit 1
fi
echo ""

# ---- 3. upload + flash ------------------------------------------------------
# App target resolves to the next app slot (no `part`); data targets pass it.
Q="sha256=$HEX"
[ "$IS_APP" -eq 1 ] || Q="part=$PART&$Q"
echo "--- [3/4] Uploading image (POST /update?$Q) ---"
# For an APP target the updater may reboot the instant the hash verifies,
# dropping the connection before it can respond — so a transport failure AFTER
# the body was sent most likely means "verified & flashing". A DATA target never
# reboots on its own, so a dropped connection there is a genuine failure.
set +e
code=$(curl -s -o /tmp/ota_resp -w '%{http_code}' \
            --connect-timeout 5 --max-time 120 \
            -X POST --data-binary @"$BIN" \
            "$BASE/update?$Q")
curl_rc=$?
set -e
resp=$(cat /tmp/ota_resp 2>/dev/null || true); rm -f /tmp/ota_resp
echo "  curl_rc=$curl_rc  http_code=$code  body=${resp:-<none>}"

case "$code" in
  200) echo "  ✓ Updater accepted and verified the image." ;;
  400) echo "  ✗ 400: missing/malformed SHA-256 (app slot requires a valid hash)." >&2; exit 1 ;;
  413) echo "  ✗ 413: image larger than the target partition." >&2; exit 1 ;;
  422) echo "  ✗ 422: SHA-256 mismatch — image NOT flashed. Re-run to retry." >&2; exit 1 ;;
  500) echo "  ✗ 500: updater could not write the partition. Nothing committed." >&2; exit 1 ;;
  000)
    if [ "$curl_rc" -eq 7 ]; then
      echo "  ✗ Could not connect to the updater to upload." >&2; exit 1
    fi
    if [ "$IS_APP" -eq 1 ]; then
      echo "  ! Connection dropped after upload — updater is likely flashing & rebooting."
    else
      echo "  ✗ Connection dropped on a data upload (updater does not reboot itself here)." >&2
      exit 1
    fi
    ;;
  *) echo "  ✗ Unexpected HTTP $code." >&2; exit 1 ;;
esac
echo ""

# A data target does NOT auto-reboot — the updater stays put, so tell it to boot
# back into the main app before we poll for it.
if [ "$IS_APP" -ne 1 ]; then
  echo "  Data partition written — asking the updater to reboot into the app…"
  curl -s --max-time 5 "$BASE/reboot" >/dev/null 2>&1 || true
fi

# ---- 4. wait for the new main app -------------------------------------------
echo "--- [4/4] Waiting for the new firmware to boot (up to ${APP_TIMEOUT}s) ---"
deadline=$(( $(date +%s) + APP_TIMEOUT ))
app_up=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  code=$(curl -s -o /dev/null -w '%{http_code}' --connect-timeout 3 --max-time 5 "$BASE/api/state" 2>/dev/null || true)
  code=${code:-000}
  if [ "$code" = "200" ]; then
    app_up=1
    break
  fi
  printf '  … booting\n'
  sleep "$POLL"
done
if [ "$app_up" -ne 1 ]; then
  echo "  ✗ Main app did not return within ${APP_TIMEOUT}s. The image may be bad;" >&2
  echo "    recover over SoftAP (192.168.4.1) or a wired flash_all.sh." >&2
  exit 1
fi

echo "  ✓ Main app is back."
VER=$(curl -s --max-time 5 "$BASE/api/version" 2>/dev/null || true)
[ -n "$VER" ] && echo "  version: $VER"
echo ""
if [ "$IS_APP" -eq 1 ]; then
  echo "=== OTA complete — device is running the new firmware. ==="
else
  echo "=== OTA complete — '$PART' partition updated; device booted back into the app. ==="
fi
