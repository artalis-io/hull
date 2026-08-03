#!/bin/sh
# e2e_linker_zig.sh - `hull build --linker=zig` (the toolchain-free zig backend).
#
# zig links via `zig cc [--target=<triple>]` - it bundles clang + lld + crt +
# libc, so it is the turnkey cross-compiler. This is the ONLY e2e that exercises
# the zig linker backend (the `embed-zig` CI job tests the libhull ABI embedder,
# a different thing). It stages a system zig into ~/.hull/tools/zig/ (where
# `hull tools install zig` places it), builds a real app through zig, and - when
# the target is the host - runs it.
#
# zig targets LINUX cleanly (its Mach-O linker rejects the macOS SDK .tbd stubs),
# and cross-linking needs a platform lib matching the TARGET (the embedded one is
# the host's - the tracked cross-compile gap, docs/build_arc_audit.md #2/#4). So
# this e2e only runs on a Linux x86_64 host, where the embedded platform lib
# matches the zig native target: it builds + RUNS a real app through zig. On any
# other host it SKIPS (a foreign-target link can't succeed until a target
# platform lib is publishable). Also skips if no zig / no embedded platform lib.
#
# Requires: an EMBED_PLATFORM=1 hull, a system zig (0.13.x), curl.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$SRCDIR/build/hull}"
PASS=0; FAIL=0
assert() { msg="$1"; shift; if "$@"; then echo "  ok  $msg"; PASS=$((PASS+1)); \
    else echo "  FAIL $msg"; FAIL=$((FAIL+1)); fi; }

[ -x "$HULL" ] || { echo "FAIL: $HULL not found - run 'make' first"; exit 1; }

# Find a system zig.
find_zig() {
    p=$(command -v zig 2>/dev/null) && { echo "$p"; return 0; }
    for d in /opt/zig /opt/homebrew/bin /usr/local/bin; do
        [ -x "$d/zig" ] && { echo "$d/zig"; return 0; }
    done
    return 1
}
# Cross-linking needs a target-matching platform lib (not yet publishable), so
# restrict to a Linux x86_64 host where zig's native target matches the embedded
# (host) platform lib.
HOST_OS=$(uname -s); HOST_ARCH=$(uname -m)
if [ "$HOST_OS" != "Linux" ] || { [ "$HOST_ARCH" != "x86_64" ] && [ "$HOST_ARCH" != "amd64" ]; }; then
    echo "SKIP: --linker=zig e2e runs on Linux x86_64 only (cross-target needs a"
    echo "      target platform lib; see docs/build_arc_audit.md #2/#4). Host: $HOST_OS/$HOST_ARCH"
    exit 0
fi

ZIG_BIN=$(find_zig) || { echo "SKIP: no system zig found (install from ziglang.org)"; exit 0; }
echo "── using zig from: $ZIG_BIN ($("$ZIG_BIN" version 2>/dev/null)) ──"

# Stage zig into ~/.hull/tools/zig/zig (the bundle layout hull resolves). A
# symlink is enough: zig finds its lib/ tree via the real exe path.
TOOLS="$HOME/.hull/tools/zig"; mkdir -p "$TOOLS"
STAGED=""
if [ ! -e "$TOOLS/zig" ]; then ln -sf "$ZIG_BIN" "$TOOLS/zig"; STAGED="$TOOLS/zig"; fi
cleanup() { [ -n "$STAGED" ] && rm -f "$STAGED" 2>/dev/null || true; rm -rf "${WORKDIR:-}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

WORKDIR="$(mktemp -d)"
# A pure compute app.main (no HTTP) - the smallest thing that composes a runtime
# and links cleanly for a foreign target.
APP="$WORKDIR/app"; mkdir -p "$APP"
cat > "$APP/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA

# Skip cleanly if this hull can't compose (no embedded platform lib).
probe="$($HULL build "$APP" --linker=zig --target=x86_64-linux-gnu --no-verify-platform -o "$WORKDIR/probe" 2>&1 || true)"
case "$probe" in
    *"platform library not embedded"*|*"cannot find libhull_platform.a"*)
        echo "SKIP: hull not built with EMBED_PLATFORM=1"; exit 0 ;;
esac

# Native Linux x86_64: the glibc target matches the host's glibc platform lib,
# so this builds a runnable binary (musl would ABI-mismatch the glibc-built .a).
echo "== hull build --linker=zig (native x86_64-linux-gnu) =="
OUT="$($HULL build "$APP" --linker=zig --target=x86_64-linux-gnu \
       --no-verify-platform -o "$WORKDIR/app_zig" 2>&1)" || { echo "$OUT"; }
assert "build --linker=zig succeeds"     [ -x "$WORKDIR/app_zig" ]
magic=$(od -An -tx1 -N4 "$WORKDIR/app_zig" 2>/dev/null | tr -d ' ')
assert "produced a Linux ELF (magic 7f454c46)" [ "$magic" = "7f454c46" ]
"$WORKDIR/app_zig"; rc=$?
assert "zig-linked binary runs (exit 0)" [ "$rc" -eq 0 ]

echo "== hull build --linker=zig composed a runtime =="
assert "output names the composed runtime" \
    sh -c 'echo "$1" | grep -q "composed runtime"' _ "$OUT"

echo ""
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
echo "PASS: e2e_linker_zig (zig backend builds + runs/cross-builds a real app)"
