#!/bin/sh
# e2e_feature_runtime.sh — runtimes as composable features: the slim invariant.
#
# Proves that a produced single-runtime app does not carry the OTHER interpreter:
#   - hull build app.lua -> a binary with ZERO QuickJS symbols
#   - hull build app.js  -> a binary with ZERO Lua-VM symbols
# and that each slim binary actually serves. Also checks the hull toolchain
# itself still runs both runtimes. Locks in the runtime-feature slim so it can't
# silently regress. See docs/runtime_feature_phase3.md.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

HULL=./build/hull

echo "=== build hull + the runtime-less platform lib + both runtime archives ==="
make >/dev/null
make platform >/dev/null
# The base is runtime-less: a produced app composes exactly one runtime archive,
# so hull build needs them present (in build/, found by build.lua). The
# distributed hull embeds them instead.
make feature-lua feature-js >/dev/null

# Count DEFINED symbols matching $2. nm is portable but names differ by object
# format: Mach-O prefixes an underscore (_JS_NewRuntime), ELF does not. The type
# column is one letter; exclude 'U'/'u' (undefined references) - a runtime-less
# archive legitimately has undefined refs to a runtime it does not carry, which
# resolve only when a runtime is composed. We assert the base does not DEFINE a
# VM, not that it never mentions one.
count_syms() { nm "$1" 2>/dev/null | grep -cE " [A-TV-Za-tv-z] _?$2" || true; }

echo "=== the base platform lib itself is RUNTIME-LESS (0 VM symbols) ==="
base_qjs=$(count_syms build/libhull_platform.a "JS_NewRuntime")
base_lua=$(count_syms build/libhull_platform.a "lua_newstate")
echo "libhull_platform.a: QuickJS=$base_qjs Lua-VM=$base_lua"
[ "$base_qjs" -eq 0 ] && [ "$base_lua" -eq 0 ] || {
    echo "FAIL: base platform lib is not runtime-less"; exit 1; }
echo "ok  base platform lib carries no interpreter"

LUA_APP=$(mktemp -d); JS_APP=$(mktemp -d)
trap 'rm -rf "$LUA_APP" "$JS_APP"' EXIT

cat > "$LUA_APP/app.lua" <<'LUA'
app.manifest({ modules = { "hull/http-server@1", "hull/json@1" } })
app.get("/ping", function(req, res) res:json({ rt = "lua" }) end)
LUA

cat > "$JS_APP/app.js" <<'JS'
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1"] });
app.get("/ping", (req, res) => { res.json({ rt: "js" }); });
JS

serve_check() { # $1 = binary, $2 = port
    "$1" -p "$2" >/tmp/rt_serve.log 2>&1 & pid=$!
    i=0; code=000
    while [ $i -lt 20 ]; do
        code=$(curl -s -o /dev/null -w "%{http_code}" "localhost:$2/ping" 2>/dev/null || echo 000)
        [ "$code" = "200" ] && break
        i=$((i + 1)); sleep 0.2
    done
    kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
    [ "$code" = "200" ]
}

echo "=== hull build app.lua -> slim (no QuickJS) ==="
"$HULL" build --no-verify-platform -o "$LUA_APP/bin" "$LUA_APP" >/dev/null
qjs=$(count_syms "$LUA_APP/bin" "JS_New")
echo "QuickJS symbols in the lua app: $qjs"
[ "$qjs" -eq 0 ] || { echo "FAIL: lua app carries QuickJS ($qjs symbols)"; exit 1; }
serve_check "$LUA_APP/bin" 8391 || { echo "FAIL: slim lua app did not serve"; cat /tmp/rt_serve.log; exit 1; }
echo "ok  lua app is QuickJS-free and serves"

echo "=== hull build app.js -> slim (no Lua VM) ==="
"$HULL" build --no-verify-platform -o "$JS_APP/bin" "$JS_APP" >/dev/null
luavm=$(count_syms "$JS_APP/bin" "lua_newstate")
echo "Lua-VM symbols in the js app: $luavm"
[ "$luavm" -eq 0 ] || { echo "FAIL: js app carries the Lua VM ($luavm symbols)"; exit 1; }
serve_check "$JS_APP/bin" 8392 || { echo "FAIL: slim js app did not serve"; cat /tmp/rt_serve.log; exit 1; }
echo "ok  js app is Lua-VM-free and serves"

echo "=== the hull toolchain still runs BOTH runtimes ==="
serve_check_dir() { # $1 = entry file, $2 = port
    "$HULL" "$1" -p "$2" >/tmp/rt_tc.log 2>&1 & pid=$!
    i=0; code=000
    while [ $i -lt 20 ]; do
        code=$(curl -s -o /dev/null -w "%{http_code}" "localhost:$2/ping" 2>/dev/null || echo 000)
        [ "$code" = "200" ] && break
        i=$((i + 1)); sleep 0.2
    done
    kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
    [ "$code" = "200" ]
}
serve_check_dir "$LUA_APP/app.lua" 8393 || { echo "FAIL: toolchain did not run the lua app"; exit 1; }
serve_check_dir "$JS_APP/app.js"  8394 || { echo "FAIL: toolchain did not run the js app"; exit 1; }
echo "ok  toolchain runs lua AND js"

echo "=== rejection: an app.js with no resolvable js runtime archive fails clearly ==="
# The runtime is auto-inferred from the entry extension and composed from the
# embedded copy (distributed hull) -> build/ -> ~/.hull/feature. Isolate so ALL
# of those miss: stage this (non-embedded) hull where no runtime archive sits
# beside it, run from that empty dir so the relative build/ + ../build/ search
# paths miss, and point HOME at an empty dir (no ~/.hull/feature). Composing the
# js runtime must then fail with a clear "js runtime feature lib was not found"
# + `make feature-js` hint and produce NO broken binary.
RJ_STAGE=$(mktemp -d); RJ_HOME=$(mktemp -d); RJ_APP=$(mktemp -d)
trap 'rm -rf "$LUA_APP" "$JS_APP" "$RJ_STAGE" "$RJ_HOME" "$RJ_APP"' EXIT
cp "$HULL" "$RJ_STAGE/hull"
# Stage the platform lib beside it (so platform resolution succeeds) but NOT the
# js runtime archive: the build must get past linking-the-base and fail
# specifically on composing the js runtime.
cp ./build/libhull_platform.a "$RJ_STAGE/libhull_platform.a"
cp "$JS_APP/app.js" "$RJ_APP/app.js"
RJ_OUT=$(cd "$RJ_STAGE" && HOME="$RJ_HOME" ./hull build --no-verify-platform \
    -o "$RJ_APP/bin" "$RJ_APP" 2>&1) || true
echo "$RJ_OUT" | grep -q "'js' runtime feature lib was not found" \
    || { echo "$RJ_OUT"; echo "FAIL: expected the js-runtime-not-found error"; exit 1; }
echo "$RJ_OUT" | grep -q "make feature-js" \
    || { echo "$RJ_OUT"; echo "FAIL: rejection lacks the build-from-source hint"; exit 1; }
[ ! -x "$RJ_APP/bin" ] || { echo "FAIL: produced a binary despite the missing js runtime"; exit 1; }
echo "ok  app.js with no resolvable js runtime fails clearly, no binary"

echo "PASS: runtime slim holds - a single-runtime app drops the other interpreter"
