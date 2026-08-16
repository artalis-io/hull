#!/bin/sh
# Rebuild + verify tests/fixtures/compute/spanread64.wasm from spanread64.wat (#334).
#
# The committed .wasm is BYTE-REPRODUCIBLE from the .wat with wat2wasm
# --enable-memory64 (the same tool that produces echo64.wasm). This script
# regenerates it and verifies its SHA-256 against the recorded pin, so an edit to
# the .wat without regenerating -- or a wat2wasm version that changes the encoding
# -- fails loudly instead of silently drifting.
#
#   Run as a drift check:   sh tests/fixtures/compute/build_spanread64.sh
#   Accept a new binary:    UPDATE=1 sh tests/fixtures/compute/build_spanread64.sh
#                           (then update SHA256_PIN below to the printed value)
#
# Tool: wabt wat2wasm. Pinned encoding produced with wat2wasm 1.0.34.
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
FX="$ROOT/tests/fixtures/compute"
WAT2WASM="${WAT2WASM:-wat2wasm}"
SHA256_PIN=556edcc909e4d4998fc01fceffe306a52771ba312effbd13b6b59c59f7e73b40

command -v "$WAT2WASM" >/dev/null 2>&1 || { echo "FATAL: wat2wasm not found (set WAT2WASM=...)" >&2; exit 1; }
echo "wat2wasm: $("$WAT2WASM" --version)"

"$WAT2WASM" --enable-memory64 "$FX/spanread64.wat" -o "$FX/spanread64.wasm"

# sha256 (Linux busybox: sha256sum; macOS: shasum -a 256)
if command -v sha256sum >/dev/null 2>&1; then
    got=$(sha256sum "$FX/spanread64.wasm" | awk '{print $1}')
else
    got=$(shasum -a 256 "$FX/spanread64.wasm" | awk '{print $1}')
fi

echo "built $FX/spanread64.wasm ($(wc -c < "$FX/spanread64.wasm") bytes), sha256 $got"

if [ "${UPDATE:-0}" = "1" ]; then
    echo "UPDATE=1: not verifying; set SHA256_PIN=$got in this script."
    exit 0
fi
[ "$got" = "$SHA256_PIN" ] || {
    echo "FATAL: spanread64.wasm sha256 drift: got $got, pinned $SHA256_PIN." >&2
    echo "  If the .wat change is intentional, re-run with UPDATE=1 and update SHA256_PIN." >&2
    exit 1
}
echo "OK: spanread64.wasm matches the recorded SHA-256 pin."
