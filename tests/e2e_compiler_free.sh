#!/bin/sh
# E2E test: `hull build --no-compiler` — the compiler-free build path.
#
# Emits app_registry.o directly (obj_emit), extracts the bundled app_main.o +
# app_feature_registry-<rt>.o, and links with no C compiler. Builds a real app
# both ways (default + --no-compiler), runs each, and asserts identical serving
# behavior (the emitted registry + relocations must resolve at runtime: the
# embedded migration applies and embedded routes serve). Also checks the
# --with scope guard. See docs/compiler_free_build.md.
#
# Usage: sh tests/e2e_compiler_free.sh   /   make e2e-compiler-free
# Requires: an EMBED_PLATFORM=1 hull, cc (linker), curl.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
WORKDIR=""
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
    [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ] && rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

assert() {
    msg="$1"; shift
    if "$@"; then echo "  ok  $msg"; PASS=$((PASS + 1));
    else echo "  FAIL $msg"; FAIL=$((FAIL + 1)); fi
}

# --no-compiler is native-only (cosmo is dual-arch, unsupported in v1).
case "$($HULL version 2>/dev/null || true)" in
    *cosmo*|*Cosmo*) echo "SKIP: --no-compiler unsupported on cosmo/APE"; exit 0 ;;
esac

WORKDIR="$(mktemp -d)"
APP="$WORKDIR/hello"
cp -R "$SRCDIR/examples/hello" "$APP"
rm -f "$APP"/data.db* 2>/dev/null || true

PORT=8793

# Probe: a --no-compiler build needs an embedded platform lib. If this hull
# wasn't built EMBED_PLATFORM=1, skip cleanly (CI builds embedded).
probe="$($HULL build "$APP" --no-compiler --no-verify-platform -o "$WORKDIR/probe" 2>&1 || true)"
case "$probe" in
    *"platform library not embedded"*|*"cannot find libhull_platform.a"*)
        echo "SKIP: hull not built with EMBED_PLATFORM=1 (no platform lib to compose)"
        exit 0 ;;
esac

serve_and_check() {
    bin="$1"; label="$2"
    # Run in a fresh cwd so the app's data.db is new and the embedded migration
    # actually applies (proving a migrations/ registry entry resolves).
    run="$WORKDIR/run_$label"; mkdir -p "$run"
    ( cd "$run" && "$bin" -p "$PORT" --no-sandbox ) >"$WORKDIR/$label.log" 2>&1 &
    SERVER_PID=$!
    sleep 2
    code="$(curl -s -m 5 -o "$WORKDIR/$label.body" -w '%{http_code}' "http://127.0.0.1:$PORT/" 2>/dev/null || true)"
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
    assert "$label: serves HTTP 200" [ "$code" = "200" ]
    assert "$label: body has the hello message" grep -q "Hello from Hull" "$WORKDIR/$label.body"
    assert "$label: embedded migration applied" grep -q "applied: 001_init.sql" "$WORKDIR/$label.log"
}

echo "== compiler-free build (Lua) =="
$HULL build "$APP" --no-compiler --no-verify-platform -o "$WORKDIR/hello_nc" >"$WORKDIR/build_nc.log" 2>&1
assert "build --no-compiler succeeds" [ -x "$WORKDIR/hello_nc" ]
assert "build log shows the emit step" grep -q "emitting app_registry.o" "$WORKDIR/build_nc.log"
assert "binary exports hl_app_entries" sh -c "nm '$WORKDIR/hello_nc' 2>/dev/null | grep -q hl_app_entries"
serve_and_check "$WORKDIR/hello_nc" "nc_lua"

echo "== default (compiler) build, same app =="
$HULL build "$APP" --no-verify-platform -o "$WORKDIR/hello_cc" >"$WORKDIR/build_cc.log" 2>&1
assert "default build succeeds" [ -x "$WORKDIR/hello_cc" ]
serve_and_check "$WORKDIR/hello_cc" "cc_lua"

echo "== both paths produced equivalent responses =="
# The body carries a live timestamp, so compare the stable message field.
msg_nc="$(grep -o 'Hello from Hull[^"]*' "$WORKDIR/nc_lua.body" 2>/dev/null || true)"
msg_cc="$(grep -o 'Hello from Hull[^"]*' "$WORKDIR/cc_lua.body" 2>/dev/null || true)"
assert "no-compiler message == compiler message" sh -c "[ -n '$msg_nc' ] && [ '$msg_nc' = '$msg_cc' ]"

echo "== compiler-free build (JS) =="
$HULL build "$APP" --runtime js --no-compiler --no-verify-platform -o "$WORKDIR/hello_nc_js" >"$WORKDIR/build_nc_js.log" 2>&1
if [ -x "$WORKDIR/hello_nc_js" ]; then
    serve_and_check "$WORKDIR/hello_nc_js" "nc_js"
else
    echo "  (skip JS: entry not built)"
fi

echo "== scope guard: --no-compiler rejects --with features =="
out="$($HULL build "$APP" --no-compiler --with=gpu --no-verify-platform -o "$WORKDIR/nope" 2>&1 || true)"
assert "--with=gpu is rejected under --no-compiler" sh -c "printf '%s' \"$out\" | grep -qi 'does not support --with'"

echo ""
echo "compiler-free e2e: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
