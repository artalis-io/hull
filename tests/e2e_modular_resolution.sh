#!/bin/sh
# e2e_modular_resolution.sh - 0.13.1 PR#2 (manifest extraction + local-module
# resolution). Asserts ONE module-resolution rule across dev-run and
# manifest-extraction/build for a modular app:
#   - requiring-module-relative resolution (routes/users -> ./../lib/auth),
#   - canonical ./.. collapse + app-root containment,
#   - identical Lua and JS behavior (JS uses explicit .js per ES convention),
#   - a manifest-extraction failure that controls composition is FATAL (build
#     aborts, no binary), not a silent nil manifest,
#   - escape-past-root fails closed identically in dev and build.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
HULL="${HULL_BIN:-build/hull}"
[ -x "$HULL" ] || HULL="./build/hull"
HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
# Canonicalize (pwd -P) so the app root has no symlink component: on macOS
# mktemp lives under /tmp -> /private/tmp, and the seatbelt sandbox allows the
# given path while fs access uses the real one - a location quirk unrelated to
# module resolution. The canonical path makes them agree.
WORK=$(cd "$(mktemp -d)" && pwd -P)
trap 'rm -rf "$WORK"; [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null || true' EXIT
fail() { echo "FAIL: $1"; exit 1; }
pass() { echo "PASS: $1"; }
PORT=39600

# curl that reports the body only on a 2xx; empty otherwise.
get() { curl -fsS "http://127.0.0.1:$1$2" 2>/dev/null || true; }

# ── Build a modular app (deep chain: app -> routes -> ../lib -> ../models) ──
mk_lua() {
    d="$1"; mkdir -p "$d/routes" "$d/lib" "$d/models"
    cat > "$d/app.lua" <<'LUA'
local users = require("./routes/users")
app.manifest({ modules = { "hull/http-server@1" } })
users.register(app)
LUA
    cat > "$d/routes/users.lua" <<'LUA'
local auth = require("./../lib/auth")
local M = {}
function M.register(app) app.get("/who", function(req, res) res:text(auth.who()) end) end
return M
LUA
    cat > "$d/lib/auth.lua" <<'LUA'
local user = require("./../models/user")
return { who = function() return user.name() end }
LUA
    cat > "$d/models/user.lua" <<'LUA'
return { name = function() return "alice" end }
LUA
}
mk_js() {
    d="$1"; mkdir -p "$d/routes" "$d/lib" "$d/models"
    cat > "$d/app.js" <<'JS'
import { app } from "hull:app";
import { register } from "./routes/users.js";
app.manifest({ modules: ["hull/http-server@1"] });
register(app);
JS
    cat > "$d/routes/users.js" <<'JS'
import { who } from "./../lib/auth.js";
export function register(app) { app.get("/who", (req, res) => res.text(who())); }
JS
    cat > "$d/lib/auth.js" <<'JS'
import { name } from "./../models/user.js";
export function who() { return name(); }
JS
    cat > "$d/models/user.js" <<'JS'
export function name() { return "alice"; }
JS
}

run_lang() {
    lang="$1"; entry="$2"; mk="$3"
    app="$WORK/mod_$lang"; "$mk" "$app"

    # 1. dev-run resolves the deep relative chain and serves.
    PORT=$((PORT + 1))
    HULL_DIAG_TRACE_FILE=/dev/null "$HULL" "$app/$entry" -p "$PORT" >/dev/null 2>&1 &
    SRV=$!
    i=0; body=""
    while [ $i -lt 25 ]; do
        sleep 0.3
        body=$(get "$PORT" /who); [ "$body" = "alice" ] && break
        kill -0 "$SRV" 2>/dev/null || break
        i=$((i + 1))
    done
    kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; SRV=""
    [ "$body" = "alice" ] || fail "$lang dev did not resolve the modular chain (/who='$body')"
    pass "$lang dev resolves routes -> ./../lib -> ./../models"

    # 2. build (manifest extraction) resolves the same chain: no warning, and the
    #    declared manifest is honored (http composed, binary produced).
    out=$("$HULL" build "$app" -o "$app/out" --no-verify-platform 2>&1) || \
        fail "$lang build failed: $out"
    echo "$out" | grep -qi 'manifest extraction failed' && \
        fail "$lang build: extraction failed on a valid modular app"
    [ -f "$app/out" ] || fail "$lang build produced no binary"
    pass "$lang manifest extraction resolves the modular chain"

    # 3. the BUILT binary RUNS (not merely builds): the embedded-VFS resolver
    #    must resolve the same nested relative chain (routes -> ./../lib ->
    #    ./../models) the dev filesystem path does.
    PORT=$((PORT + 1))
    "$app/out" -p "$PORT" >/dev/null 2>&1 &
    SRV=$!
    i=0; body=""
    while [ $i -lt 25 ]; do
        sleep 0.3
        body=$(get "$PORT" /who); [ "$body" = "alice" ] && break
        kill -0 "$SRV" 2>/dev/null || break
        i=$((i + 1))
    done
    kill "$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; SRV=""
    [ "$body" = "alice" ] || fail "$lang BUILT binary did not resolve the modular chain at runtime (/who='$body')"
    pass "$lang built binary RUNS the nested modular app"
}

run_lang lua app.lua mk_lua
run_lang js  app.js  mk_js

# ── Escape-past-root fails closed in BOTH dev and build ──
esc="$WORK/escape"; mkdir -p "$esc"
echo 'return { leak = function() return "SECRET" end }' > "$WORK/outside.lua"
cat > "$esc/app.lua" <<'LUA'
local x = require("./../outside")
app.manifest({ modules = {} })
LUA
if "$HULL" build "$esc" -o "$esc/out" --no-verify-platform >/dev/null 2>&1; then
    fail "escape-past-root build should fail closed (it did not)"
fi
[ -f "$esc/out" ] && fail "escape-past-root produced a binary"
pass "escape-past-root fails closed in build (no binary)"

# ── Extraction failure that controls composition is FATAL (not a silent nil) ──
brk="$WORK/broken"; mkdir -p "$brk"
# Error BEFORE app.manifest -> the manifest cannot be determined -> fatal.
cat > "$brk/app.lua" <<'LUA'
error("cannot reach app.manifest")
app.manifest({ modules = {} })
LUA
out=$("$HULL" build "$brk" -o "$brk/out" --no-verify-platform 2>&1 || true)
echo "$out" | grep -qi 'manifest extraction failed' || \
    fail "pre-manifest error was not reported as a fatal extraction failure"
[ -f "$brk/out" ] && fail "fatal extraction still produced a binary"
pass "pre-manifest extraction failure is fatal (command-correct, no binary)"

# ── A valid app that declares its manifest THEN errors later still builds ──
# (the manifest is authoritative for composition; a later top-level throw during
#  extraction - e.g. a subfile touching a tool-VM-absent capability - must not
#  falsely fail the build). Matches JS capture-then-tolerate behavior.
lateok="$WORK/lateok"; mkdir -p "$lateok"
cat > "$lateok/app.lua" <<'LUA'
app.manifest({ modules = {} })
error("late throw after manifest")
LUA
"$HULL" build "$lateok" -o "$lateok/out" --no-verify-platform >/dev/null 2>&1 || \
    fail "a manifest-declared app with a later throw should still build"
[ -f "$lateok/out" ] || fail "late-throw app produced no binary"
pass "manifest-then-error app still builds (manifest is authoritative)"

echo "e2e_modular_resolution: ALL PASS"
