#!/bin/sh
# Rebuild spandiff.wasm from spandiff.c using the CANONICAL hull_compute.h +
# hull_span.h (the embedded SDK headers from stdlib/cli/lua/hull/compute.lua and
# templates/hull_span.h) and the exact flags `hull compute build` uses. Keeps the
# committed differential fixture honest to the shipped SDK.
# Usage: CLANG=/path/to/clang sh tests/fixtures/compute/build_spandiff.sh
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
FX="$ROOT/tests/fixtures/compute"
CLANG="${CLANG:-clang}"
# Canonical hull_compute.h is embedded in compute.lua; hull_span.h is templates/.
awk '/^local HULL_COMPUTE_H = \[\[/{f=1; sub(/^local HULL_COMPUTE_H = \[\[/,""); } f{print} /^\]\]$/{if(f)exit}' \
  "$ROOT/stdlib/cli/lua/hull/compute.lua" | sed '$d' > "$FX/hull_compute.h"
cp "$ROOT/templates/hull_span.h" "$FX/hull_span.h"
"$CLANG" --target=wasm32-unknown-unknown -nostdlib -O2 -flto -I "$FX" \
  -Wl,--no-entry -Wl,--export=hull_process -Wl,--export=hull_version -Wl,--export=memory \
  -Wl,--allow-undefined -Wl,--initial-memory=131072 -Wl,--max-memory=67108864 \
  -o "$FX/spandiff.wasm" "$FX/spandiff.c"
rm -f "$FX/hull_compute.h" "$FX/hull_span.h"
echo "built $FX/spandiff.wasm ($(wc -c < "$FX/spandiff.wasm") bytes)"
