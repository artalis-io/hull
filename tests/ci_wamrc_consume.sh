#!/bin/sh
# ci_wamrc_consume.sh - the SINGLE shared reliance-flip consumer helper.
#
# Every x86_64 AOT consumer calls this INSTEAD of building wamrc from source. It
# performs an INDEPENDENT COLD verification of the run-scoped wamrc artifact and
# installs the verified binary where the UNCHANGED downstream AOT commands (and
# tests/ci_ensure_wamrc.sh) expect it - so no consumer rebuilds wamrc:
#
#   1. require the downloaded artifact (wamrc + manifest) - a missing artifact is
#      a HARD failure, never a rebuild;
#   2. configure the wamrc toolchain (cmake configure ONLY, NO compile) to
#      establish this consumer's OWN compiler identity for cold verification;
#   3. cold-verify identity / provenance / checksum / arch against the manifest -
#      any mismatch FAILS the job (scripts/ci/wamrc_artifact.py verify; it never
#      rebuilds);
#   4. install the VERIFIED wamrc at build/wamrc AND build/wamrc-build/wamrc so the
#      downstream `make build/gen_* ...` + ci_ensure_wamrc.sh find an executable
#      wamrc (the "already present -> do nothing" path) and can never fall through
#      to a from-source build.
#
# There is NO fallback rebuild here by design: a bad/absent artifact must redden
# the consumer job (and, via the gate, the whole run), not silently disappear the
# optimization. Local from-source builds still use `make wamrc` directly; the
# arm64 AOT job stays self-building (single consumer, nothing to share).
#
# Usage: sh tests/ci_wamrc_consume.sh [artifact_dir]   (default: artifact)
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ARTIFACT_DIR="${1:-artifact}"
PRODUCER="wamrc-x86_64"

if [ ! -f "$ARTIFACT_DIR/wamrc" ]; then
    echo "::error::wamrc artifact missing ($ARTIFACT_DIR/wamrc) - NOT rebuilding, failing"; exit 1
fi
if [ ! -f "$ARTIFACT_DIR/wamrc.manifest.json" ]; then
    echo "::error::wamrc manifest missing ($ARTIFACT_DIR/wamrc.manifest.json) - NOT rebuilding, failing"; exit 1
fi
chmod +x "$ARTIFACT_DIR/wamrc"

if command -v llvm-config-18 >/dev/null 2>&1; then
    WAMRC_CMAKE_FLAGS="-DLLVM_DIR=$(llvm-config-18 --cmakedir)"
    export WAMRC_CMAKE_FLAGS
fi

# Configure ONLY (generate CMakeCache.txt with the resolved compilers) - this
# establishes the local identity WITHOUT compiling wamrc, so verification is cold.
make wamrc-configure
if [ -x build/wamrc ]; then
    echo "::error::wamrc was built during configure - the consumer must verify COLD"; exit 1
fi

# Independent cold verification; fails the job on any identity/checksum/arch/
# provenance mismatch. NO rebuild.
python3 scripts/ci/wamrc_artifact.py verify \
    --manifest "$ARTIFACT_DIR/wamrc.manifest.json" \
    --wamrc "$ARTIFACT_DIR/wamrc" \
    --producer "$PRODUCER"

# Install the VERIFIED wamrc where downstream expects it. Populate BOTH slots so
# ci_ensure_wamrc.sh's re-copy path (if the mutable build/wamrc ever vanishes
# mid-job) restores the VERIFIED binary and never builds from scratch.
mkdir -p build build/wamrc-build
cp "$ARTIFACT_DIR/wamrc" build/wamrc
cp "$ARTIFACT_DIR/wamrc" build/wamrc-build/wamrc
chmod +x build/wamrc build/wamrc-build/wamrc
echo "verified wamrc installed at build/wamrc (from artifact; no from-source build)"
