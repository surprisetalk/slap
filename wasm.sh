#!/bin/bash
set -e
cd "$(dirname "$0")"

PROG="${1:-examples/life.slap}"
mkdir -p web

emcc -std=c99 -O2 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS='["_slap_init","_slap_frame","_slap_keydown","_slap_canvas_ptr","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["stringToNewUTF8","HEAPU8"]' \
  wasm.c -o web/slap.js

cp "$PROG" web/program.slap
cp wasm.html web/index.html

echo "Built: web/index.html, web/slap.js, web/slap.wasm"
echo "Run: cd web && python3 -m http.server 8080"
