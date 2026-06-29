#!/bin/bash
# Rebuild the web UI and flash ONLY the SPIFFS "storage" partition — fast UI
# iteration without reflashing the ~1 MB app. For a full flash (app + SPIFFS)
# use ./build_web.sh flash instead.
#
# Usage: ./flash_web.sh [--port /dev/tty...]   (port auto-detected if omitted)
set -e
cd "$(dirname "$0")"

echo "=== Building web UI (esbuild + gzip) ==="
cd web
[ -d node_modules ] || npm install
npm run build
cd ..

# shellcheck disable=SC1091
source ./activate.sh

echo ""
echo "=== Regenerating SPIFFS image (build/storage.bin) ==="
# ninja regenerates storage.bin from web/data when its contents change.
idf.py build >/dev/null

echo ""
echo "=== Flashing 'storage' partition only ==="
# parttool reads the partition table to locate 'storage', so no hard-coded
# offset. Pass --port to target a specific device; otherwise it auto-detects.
parttool.py "$@" write_partition --partition-name=storage --input build/storage.bin

echo ""
echo "Done. The app was NOT reflashed; reset the device to load the new UI."
