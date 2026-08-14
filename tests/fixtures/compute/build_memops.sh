#!/bin/sh
# Rebuild memops.wasm from memops.c using the CANONICAL hull_compute.h (the
# embedded SDK header from stdlib/cli/lua/hull/compute.lua) and the exact flags
# `hull compute build` uses. Keeps the committed fixture honest to the SDK.
# Usage: CLANG=/path/to/clang sh tests/fixtures/compute/build_memops.sh
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
FX="$ROOT/tests/fixtures/compute"
CLANG="${CLANG:-clang}"
awk '/^local HULL_COMPUTE_H = \[\[/{f=1; sub(/^local HULL_COMPUTE_H = \[\[/,""); } f{print} /^\]\]$/{if(f)exit}' \
  "$ROOT/stdlib/cli/lua/hull/compute.lua" | sed '$d' > "$FX/.hull_compute_canon.h"
mv "$FX/.hull_compute_canon.h" "$FX/hull_compute.h"
"$CLANG" --target=wasm32-unknown-unknown -nostdlib -O2 -flto -I "$FX" \
  -Wl,--no-entry -Wl,--export=hull_process -Wl,--export=hull_version -Wl,--export=memory \
  -Wl,--allow-undefined -Wl,--initial-memory=131072 -Wl,--max-memory=67108864 \
  -o "$FX/memops.wasm" "$FX/memops.c"
rm -f "$FX/hull_compute.h"
echo "built $FX/memops.wasm ($(wc -c < "$FX/memops.wasm") bytes)"
