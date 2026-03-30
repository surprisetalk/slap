#!/bin/bash
set -e
cd "$(dirname "$0")"

emcc -std=c99 -O2 -DSLAP_SDL \
  -s USE_SDL=2 \
  -s ASYNCIFY \
  -s ASYNCIFY_STACK_SIZE=65536 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_RUNTIME_METHODS='["callMain","ENV"]' \
  -s INVOKE_RUN=0 \
  --preload-file examples/life.slap@/life.slap \
  --shell-file template.html \
  slap.c -o web/index.html

echo "Built: web/index.html, web/index.js, web/index.wasm, web/index.data"
echo "Run: cd web && python3 -m http.server 8080  then open http://localhost:8080"
