#!/bin/bash
# Build the web UI (esbuild -> gzipped assets in web/data) then build the
# firmware (which packs web/data into the SPIFFS "storage" partition).
# Usage: ./build_web.sh [extra idf.py args...]   e.g. ./build_web.sh flash
set -e
cd "$(dirname "$0")"

echo "=== Building web UI (esbuild + gzip) ==="
cd web
[ -d node_modules ] || npm install
npm run build
cd ..

echo ""
echo "=== Building firmware ==="
# shellcheck disable=SC1091
source ./activate.sh
idf.py build "$@"
