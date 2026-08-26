#!/usr/bin/env bash
#
# e2e_cache_cosmo.sh - verify the cache layer (list / prune / clear /
# verify / doctor / inspect / blob_store / sharding / opt-outs)
# works on a Cosmopolitan-built hull binary.
#
# The cache layer touches code paths that differ under cosmocc:
# - the poll backend (kqueue/epoll → poll)
# - cosmo libc's atomic rename / mkdir / stat semantics
# - cosmo's PATH_MAX / NAME_MAX values
# - Linux-vs-macOS sandbox handling (relevant when cosmo runs on Linux)
#
# This script reuses tests/e2e_cache.sh and points it at the cosmo
# binary by setting HULL=. The actual assertion logic is shared with
# the native suite, so any regression in the cache subsystem under
# cosmo manifests as a failure here.
#
# Build prerequisites:
#   - cosmocc on PATH (see vendor/keel comment for the fat-build rules)
#   - `make platform-cosmo` first (creates multi-arch platform archives)
#   - `make CC=cosmocc` (fat APE) for a binary runnable on the build host
#
# The Makefile target `make e2e-cache-cosmo` orchestrates the build +
# run. CI invokes this on a Linux x86_64 runner where the fat APE
# launches the x86_64 slice natively.

set -e

cd "$(dirname "$0")/.."

if [ -z "${HULL:-}" ]; then
    echo "FAIL: HULL is not set. Build with 'make CC=cosmocc' first" >&2
    echo "      and invoke this script via 'make e2e-cache-cosmo'." >&2
    exit 2
fi

if [ ! -x "$HULL" ]; then
    echo "FAIL: HULL='$HULL' is not an executable file." >&2
    exit 2
fi

echo "=== Sanity: hull binary is a cosmo APE ==="
file "$HULL" 2>&1 | head -1
"$HULL" version 2>&1 || true
echo ""

# Quick smoke before delegating to the full suite: make sure
# `cache list` returns a sensible registry on the cosmo build -
# this confirms the cache_registry compiled in and the doctor
# subsystem is wired up, before we bring in the slower writes.
echo "=== Smoke: cache list on cosmo ==="
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! HOME="$TMP" "$HULL" cache list >/dev/null 2>&1; then
    echo "FAIL: cosmo binary cannot enumerate the cache registry." >&2
    exit 1
fi

# Now run the shared cache suite - every assertion in e2e_cache.sh
# runs against the cosmo binary instead of the native one. That
# covers list / prune / clear / verify / inspect / doctor, plus
# every env-var opt-out, plus every --json shape check.
echo ""
echo "=== Delegating to tests/e2e_cache.sh with cosmo binary ==="
exec bash tests/e2e_cache.sh
