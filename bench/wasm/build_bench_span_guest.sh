#!/bin/sh
# Rebuild bench_span_guest.wasm from bench_span_guest.c using the CANONICAL
# hull_compute.h + hull_span.h (the embedded SDK headers from
# stdlib/cli/lua/hull/compute.lua and templates/hull_span.h) and the exact flags
# `hull compute build` uses -- plus a larger max-memory so the copy-once (LINEAR)
# baseline can hold the whole dataset in linear memory (up to the wasm32 256 MB
# I/O ceiling). Keeps the committed benchmark guest honest to the shipped SDK.
#
# The committed bench/wasm/bench_span_guest.wasm is the reproducibility anchor;
# tests/bench_mapped_span.sh re-runs this and diffs, so a drifted SDK header or a
# stale binary fails CI. Usage: CLANG=/path/to/clang sh bench/wasm/build_bench_span_guest.sh
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BW="$ROOT/bench/wasm"
CLANG="${CLANG:-clang}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Canonical hull_compute.h is embedded in compute.lua; hull_span.h is templates/.
awk '/^local HULL_COMPUTE_H = \[\[/{f=1; sub(/^local HULL_COMPUTE_H = \[\[/,""); } f{print} /^\]\]$/{if(f)exit}' \
  "$ROOT/stdlib/cli/lua/hull/compute.lua" | sed '$d' > "$TMP/hull_compute.h"
cp "$ROOT/templates/hull_span.h" "$TMP/hull_span.h"
cp "$BW/bench_span_ops.h" "$TMP/bench_span_ops.h"

# --max-memory 300 MiB: copy-once copies the whole dataset into linear memory.
# The harness caps copy-once at the wasm32 256 MB I/O ceiling, so 300 MiB of
# addressable linear memory covers every representable copy-once case.
"$CLANG" --target=wasm32-unknown-unknown -nostdlib -O2 -flto -I "$TMP" \
  -Wl,--no-entry -Wl,--export=hull_process -Wl,--export=hull_version -Wl,--export=memory \
  -Wl,--allow-undefined -Wl,--initial-memory=131072 -Wl,--max-memory=314572800 \
  -o "$BW/bench_span_guest.wasm" "$BW/bench_span_guest.c"
echo "built $BW/bench_span_guest.wasm ($(wc -c < "$BW/bench_span_guest.wasm") bytes)"
