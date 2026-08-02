#!/bin/sh
# E2E test: `hull build --linker=lld` - the toolchain-free linker axis (Tier A).
#
# Links an app through a side-loaded lld (driving cc with `-fuse-ld=lld`)
# instead of the system linker, and proves the produced binary runs. lld is
# resolved from ~/.hull/tools (as `hull tools install lld` would place it), so
# the test stages a `lld` + its ld.lld/ld64.lld personalities there from a
# system lld (brew/apt/PATH) and skips cleanly if none is available.
# See docs/toolchain_free_build.md.
#
# Usage: sh tests/e2e_linker.sh   /   make e2e-linker
# Requires: an EMBED_PLATFORM=1 hull, a system cc, curl, and an lld somewhere.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0; FAIL=0; WORKDIR=""; SERVER_PID=""; STAGED=""

cleanup() {
    [ -n "$SERVER_PID" ] && { kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; }
    [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ] && rm -rf "$WORKDIR"
    for f in $STAGED; do rm -f "$f"; done
}
trap cleanup EXIT INT TERM

assert() {
    msg="$1"; shift
    if "$@"; then echo "  ok  $msg"; PASS=$((PASS + 1));
    else echo "  FAIL $msg"; FAIL=$((FAIL + 1)); fi
}

[ -x "$HULL" ] || { echo "FAIL: $HULL not found - run 'make' first"; exit 1; }

# Find a system lld. `ld.lld` is the ELF personality; on macOS `ld64.lld`.
find_lld() {
    for c in ld.lld ld64.lld lld; do
        p=$(command -v "$c" 2>/dev/null) && { echo "$p"; return 0; }
    done
    for d in /opt/homebrew/opt/lld/bin /usr/lib/llvm-*/bin /usr/local/opt/lld/bin; do
        for c in ld.lld ld64.lld lld; do
            [ -x "$d/$c" ] && { echo "$d/$c"; return 0; }
        done
    done
    return 1
}

LLD_BIN=$(find_lld) || { echo "SKIP: no system lld found (brew install lld / apt install lld)"; exit 0; }
LLD_DIR=$(dirname "$LLD_BIN")
echo "── using lld from: $LLD_DIR ──"

# Stage lld + personalities into ~/.hull/tools (where hull resolves tools).
TOOLS="$HOME/.hull/tools"; mkdir -p "$TOOLS"
for n in lld ld.lld ld64.lld; do
    if [ -x "$LLD_DIR/$n" ] && [ ! -e "$TOOLS/$n" ]; then
        ln -sf "$LLD_DIR/$n" "$TOOLS/$n"; STAGED="$STAGED $TOOLS/$n"
    fi
done

WORKDIR="$(mktemp -d)"
APP="$WORKDIR/hello"; cp -R "$SRCDIR/examples/hello" "$APP"; rm -f "$APP"/data.db* 2>/dev/null || true
PORT=8798

# Skip cleanly if this hull can't compose (no embedded platform lib).
probe="$($HULL build "$APP" --linker=lld --no-verify-platform -o "$WORKDIR/probe" 2>&1 || true)"
case "$probe" in
    *"platform library not embedded"*|*"cannot find libhull_platform.a"*)
        echo "SKIP: hull not built with EMBED_PLATFORM=1"; exit 0 ;;
esac

echo "== hull build --linker=lld =="
BUILD_OUT="$($HULL build "$APP" --linker=lld --no-verify-platform -o "$WORKDIR/hello_lld" 2>&1)"
assert "build --linker=lld succeeds" [ -x "$WORKDIR/hello_lld" ]
assert "build used the emit path" sh -c "printf '%s' \"$BUILD_OUT\" | grep -qi 'emitting app_registry'"
# It must NOT have fallen back to the compiler (that would mean lld didn't resolve).
assert "build did NOT fall back to the C compiler" sh -c "! printf '%s' \"$BUILD_OUT\" | grep -qi 'using the C compiler'"
assert "binary exports hl_app_entries" sh -c "nm '$WORKDIR/hello_lld' 2>/dev/null | grep -q hl_app_entries"

echo "== run the lld-linked binary =="
run="$WORKDIR/run"; mkdir -p "$run"
( cd "$run" && "$WORKDIR/hello_lld" -p "$PORT" --no-sandbox ) >"$WORKDIR/srv.log" 2>&1 &
SERVER_PID=$!
sleep 2
code="$(curl -s -m5 -o "$WORKDIR/body" -w '%{http_code}' "http://127.0.0.1:$PORT/" 2>/dev/null || true)"
kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
assert "lld-linked binary serves HTTP 200" [ "$code" = "200" ]
assert "response body is correct" grep -q "Hello from Hull" "$WORKDIR/body"

echo ""
echo "linker (lld) e2e: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
