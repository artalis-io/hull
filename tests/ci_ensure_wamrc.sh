#!/bin/sh
# ci_ensure_wamrc.sh - precondition for a non-skippable AOT CI step.
#
# Each AOT test step must have its OWN executable build/wamrc rather than
# relying on mutable build/ state left by an earlier step. On one CI run
# (#329) build/wamrc went missing between the readonly and guarded steps of a
# single job, sending the guarded step down the wamrc-unavailable skip path and
# failing the "must NOT skip" gate. The exact removal was never reproduced from
# a clean tree (the config-fingerprint sentinel in the Makefile removes Hull
# objects + build/hull + build/test_* but NOT build/wamrc), so we stop relying
# on a prior step preserving it and make every AOT step self-sufficient:
#
#   * build/wamrc already executable        -> do nothing (the common case);
#   * else re-copy from the persisted        -> cheap, instant;
#     wamrc-build/ tree
#   * else build wamrc from scratch;
#   * then HARD-ASSERT it is executable      -> a missing wamrc is a FATAL
#     error, never a silent skip.
#
# Called as `sh tests/ci_ensure_wamrc.sh` at the top of each non-skippable AOT
# CI step, before the fixture-regenerating `make build/test_wasm_*` (that make
# is where the fixture generator probes for wamrc and would otherwise skip).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -x build/wamrc ]; then
    if [ -x build/wamrc-build/wamrc ]; then
        echo "ci_ensure_wamrc: build/wamrc missing; re-copying from wamrc-build/"
        cp build/wamrc-build/wamrc build/wamrc
    else
        echo "ci_ensure_wamrc: build/wamrc + wamrc-build/ missing; building wamrc"
        if command -v llvm-config-18 >/dev/null 2>&1; then
            WAMRC_CMAKE_FLAGS="-DLLVM_DIR=$(llvm-config-18 --cmakedir)"
            export WAMRC_CMAKE_FLAGS
        fi
        make wamrc
    fi
fi

if [ ! -x build/wamrc ]; then
    echo "::error::build/wamrc is not executable; a non-skippable AOT test cannot run"
    exit 1
fi
echo "ci_ensure_wamrc: build/wamrc present and executable"
