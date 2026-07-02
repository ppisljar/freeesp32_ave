#!/bin/bash
# push_library.sh — deliver the built-in session library to a running device
# OVER THE AIR, without a wired flash.
#
# cfgfs (the device's config partition) is exposed as a file store over HTTP:
#   PUT /api/configs/<name>   writes the body into cfgfs/<name>   (store_put)
#   GET /api/configs          lists *.ledc                        (store_list)
# So we just PUT each library/*.ledc to the device. This is ADDITIVE — it does
# not erase user-saved configs (unlike a full flash_all.sh, which reseeds the
# whole cfgfs partition image). Same-named files are overwritten in place.
#
# This is the routine way to ship/refresh the library on a deployed unit:
# nothing here touches firmware or requires USB. (The wired flash_all.sh seed
# remains the factory/first-provision path.)
#
# Usage:
#   ./push_library.sh <device-host-or-ip> [--no-validate] [--dry-run]
#
#   <device-host-or-ip>  e.g. 192.168.1.42  or  192.168.4.1 (SoftAP fallback)
#                        may also be set via $DEVICE_HOST instead of arg 1
#   --no-validate        skip the local parse/safety-lint gate before pushing
#   --dry-run            print what would be pushed; do not PUT
#
# Exit status: 0 if every file pushed (HTTP 200), non-zero otherwise.

set -euo pipefail

cd "$(dirname "$0")"
SCRIPT_DIR="$(pwd)"
LIB_DIR="$SCRIPT_DIR/library"
VALIDATOR="$SCRIPT_DIR/validate_session.mjs"

# ---- args -------------------------------------------------------------------
HOST="${DEVICE_HOST:-}"
VALIDATE=1
DRY_RUN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-validate) VALIDATE=0; shift ;;
    --dry-run|-n)  DRY_RUN=1; shift ;;
    -*) echo "Unknown option: $1" >&2; exit 2 ;;
    *)  HOST="$1"; shift ;;
  esac
done

if [ -z "$HOST" ]; then
  echo "Usage: ./push_library.sh <device-host-or-ip> [--no-validate] [--dry-run]" >&2
  echo "  (or set \$DEVICE_HOST). Example: ./push_library.sh 192.168.1.42" >&2
  exit 2
fi

# strip any http:// the user may have pasted, and a trailing slash
HOST="${HOST#http://}"; HOST="${HOST#https://}"; HOST="${HOST%/}"
BASE="http://$HOST"

shopt -s nullglob
FILES=("$LIB_DIR"/*.ledc)
shopt -u nullglob
if [ "${#FILES[@]}" -eq 0 ]; then
  echo "No .ledc files in $LIB_DIR — nothing to push." >&2
  exit 1
fi

echo "=== Session library push ==="
echo "Device : $BASE"
echo "Source : $LIB_DIR (${#FILES[@]} files)"
echo ""

# ---- local validation gate --------------------------------------------------
# Refuse to push anything that doesn't parse / fails the strobe-safety lint,
# so a bad edit never lands on a device that may be strapped to someone's head.
if [ "$VALIDATE" -eq 1 ]; then
  if command -v node >/dev/null 2>&1 && [ -f "$VALIDATOR" ]; then
    echo "--- Validating (parse + safety lint) ---"
    vfail=0
    for f in "${FILES[@]}"; do
      if ! out=$(node "$VALIDATOR" "$f" 2>&1); then vfail=1; fi
      echo "  $out" | tail -1
      echo "$out" | grep -q ' 0 error(s)' || vfail=1
    done
    if [ "$vfail" -ne 0 ]; then
      echo "Validation failed — aborting push. (override with --no-validate)" >&2
      exit 1
    fi
    echo ""
  else
    echo "(node or validator not found — skipping local validation)"
    echo ""
  fi
fi

# ---- push -------------------------------------------------------------------
fail=0
pushed=0
for f in "${FILES[@]}"; do
  name="$(basename "$f")"
  bytes=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '  [dry-run] PUT %-26s (%s B) -> %s/api/configs/%s\n' "$name" "$bytes" "$BASE" "$name"
    continue
  fi
  # -X PUT with the raw file as body; capture the HTTP status separately from
  # the JSON body ({"saved":...,"bytes":...}) the handler returns on success.
  code=$(curl -s -o /tmp/push_lib_resp -w '%{http_code}' \
              -X PUT --data-binary @"$f" \
              -H 'Content-Type: text/plain' \
              --connect-timeout 5 --max-time 30 \
              "$BASE/api/configs/$name" || echo 000)
  if [ "$code" = "200" ]; then
    printf '  ✓ %-26s (%s B)  %s\n' "$name" "$bytes" "$(cat /tmp/push_lib_resp)"
    pushed=$((pushed + 1))
  else
    printf '  ✗ %-26s HTTP %s  %s\n' "$name" "$code" "$(cat /tmp/push_lib_resp 2>/dev/null)"
    fail=1
  fi
done
rm -f /tmp/push_lib_resp

echo ""
if [ "$DRY_RUN" -eq 1 ]; then
  echo "--dry-run: nothing pushed."
  exit 0
fi
echo "Pushed $pushed/${#FILES[@]} files."
if [ "$fail" -ne 0 ]; then
  echo "Some files failed — check the device is reachable at $BASE and on the same network." >&2
  exit 1
fi
echo "Done. Open $BASE and check the config list — the sessions should appear."
