#!/bin/sh
# tests/e2e_spans_example.sh - the examples/mapped_spans reference plugin, driven
# from BOTH runtimes over a NON-page-aligned file window, on the interpreter and
# (when wamrc is available) AOT. The plugin uses only the public hull_span.h SDK
# (hull_span_setup / hull_span_find) - this proves that public path end to end:
# a resolved bounded read, an unknown name, and an out-of-range offset.
#
# AOT goes through the PRODUCTION path (`hull build`), which post-#326 passes
# wamrc --enable-shared-heap so the AOT can read the mapped span's backing bytes.
# (A hand-rolled `wamrc` without that flag reads zeros - the #326 bug - so the
# example deliberately does not do that.) Each standalone binary is run from its
# app dir so fs.mmap finds the runtime data file (data.bin).
# (mapped-spans).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
export HULL_QUIET_AOT=1
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }
skip() { printf "  \033[33mSKIP\033[0m: %s\n" "$1"; }

WAMRC=""
for w in "$ROOT/build/wamrc" "$ROOT/build/wamrc-build/wamrc" "$(command -v wamrc 2>/dev/null || true)"; do
    [ -n "$w" ] && [ -x "$w" ] && { WAMRC="$w"; break; }
done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cp -r "$ROOT/examples/mapped_spans" "$TMP/app"

# Guard: the example's committed SDK header is byte-exact to the canonical
# template (no example-local edits / workarounds).
if cmp -s "$TMP/app/compute/spanreader/hull_span.h" "$ROOT/templates/hull_span.h"; then
    pass "example hull_span.h is byte-exact to templates/hull_span.h"
else
    fail "example hull_span.h drifted from templates/hull_span.h"
fi

# Compile the plugin to interpreter WASM via the public header.
if ( cd "$TMP/app" && "$HULL" compute build spanreader ) >"$TMP/build.log" 2>&1 && [ -f "$TMP/app/compute/spanreader.wasm" ]; then
    pass "spanreader compiles to interpreter WASM (hull compute build)"
else
    fail "spanreader compile"; sed -n '1,20p' "$TMP/build.log"; echo "spans-example: ${PASS}p ${FAIL}f"; exit 1
fi

# Assert the four documented outcomes for a running server.
#   $1 label   $2 workdir   $3.. launch command (argv)
check() {
    label="$1"; workdir="$2"; shift 2
    PORT=$((19860 + $$ % 300))
    ( cd "$workdir" && "$@" -p "$PORT" --no-sandbox -l debug ) >"$TMP/srv.log" 2>&1 &
    PID=$!; sleep 2
    if ! kill -0 $PID 2>/dev/null; then fail "$label: server start"; cat "$TMP/srv.log"; return; fi
    ok=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/read?name=source")
    v100=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/read?name=source&off=100")
    unk=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/read?name=nope")
    rng=$(curl -s --max-time 6 "http://127.0.0.1:$PORT/read?name=source&off=5000")
    is_aot=0
    grep -qE "cached module 'spanreader' \(abi=[0-9]+, aot=1" "$TMP/srv.log" && is_aot=1
    kill $PID 2>/dev/null; wait $PID 2>/dev/null
    [ "$ok" = "ok name=source len=4096 foff=8195 off=0 val=3" ] \
        && pass "$label: bounded read (name+len+foffset+value)" \
        || fail "$label: bounded read (got: $ok)"
    [ "$v100" = "ok name=source len=4096 foff=8195 off=100 val=103" ] \
        && pass "$label: bounded read at offset 100" \
        || fail "$label: offset-100 read (got: $v100)"
    [ "$unk" = "unknown name=nope count=1" ] \
        && pass "$label: unknown name -> explicit report" \
        || fail "$label: unknown name (got: $unk)"
    [ "$rng" = "range name=source len=4096 off=5000" ] \
        && pass "$label: out-of-range offset -> reported, not read" \
        || fail "$label: out-of-range (got: $rng)"
    case "$label" in
        *aot) [ "$is_aot" = "1" ] \
            && pass "$label: ran against a real AOT module (aot=1), not the interpreter" \
            || fail "$label: AOT not loaded (interpreter fallback?)";;
    esac
}

echo "--- interpreter (both runtimes) ---"
check "lua/interp" "$TMP/app" "$HULL" app.lua
check "js/interp"  "$TMP/app" "$HULL" app.js

if [ -n "$WAMRC" ]; then
    echo "--- AOT via hull build (production --enable-shared-heap) ---"
    # hull build picks lua when both entries are present, so give js its own
    # single-entry dir. Both binaries embed the AOT wamrc built with
    # --enable-shared-heap; run each from its dir so fs.mmap finds data.bin.
    jsdir="$TMP/app_js"; cp -r "$ROOT/examples/mapped_spans" "$jsdir"; rm -f "$jsdir/app.lua"
    aot_build() {  # rt, dir
        rt="$1"; dir="$2"; bin="$TMP/bin_$rt"
        bo="$("$HULL" build "$dir" -o "$bin" --no-verify-platform 2>&1)"
        if printf '%s' "$bo" | grep -q "platform library not embedded"; then
            skip "$rt/aot: hull is not an embedded build (make EMBED_PLATFORM=1) - CI builds embedded"
            return 2
        fi
        [ -x "$bin" ] || { fail "$rt/aot: hull build produced no binary"; printf '%s\n' "$bo" | tail -8; return 1; }
        printf '%s' "$bo" | grep -q "AOT compute/spanreader.wasm" \
            && pass "$rt/aot: hull build AOT-compiled spanreader (--enable-shared-heap)" \
            || fail "$rt/aot: no AOT compiled"
        check "$rt/aot" "$dir" "$bin"
    }
    aot_build lua "$TMP/app"
    aot_build js  "$jsdir"
else
    skip "AOT: no wamrc - the CI AOT job builds wamrc so this path is exercised there"
fi

echo ""
echo "spans-example: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
