#!/bin/sh
# e2e_build_excludes.sh - hull build leaves the app's tests/ directory and
# the tooling configs at its root out of the binary (hsctl platform gap 11),
# in a Lua app and a TypeScript app:
#   - tests/** (test files, fixtures, tests/tsconfig.json) and the root
#     package.json / package-lock.json / tsconfig.json / tsconfig.*.json are
#     not embedded; data/package.json (deeper in the tree) is
#   - the built app still runs
#   - the build prints one line naming what it left out
#   - an app that requires / imports a left-out file fails the build,
#     naming the importing file and the target
#   - a signed build of an app with tests/ verifies (hull verify), and
#     package.sig lists exactly the kept files
# Needs the platform library (hull build); skipped without one and on a
# cosmo APE hull.
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
HULL=${HULL:-build/hull}
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
. "$(dirname "$0")/lib/hull_rc.sh"
if hull_is_ape "$HULL"; then
    echo "e2e_build_excludes: SKIP (hull build of a native app is not available on a cosmo/APE hull)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
HOME="$WORK/home"
mkdir -p "$HOME" "$WORK/out"
export HOME
unset HULL_CACHE_DIR HULL_NO_CACHE HULL_NO_BUILD_CACHE 2>/dev/null || true
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }

# The tooling and test files both apps carry.
add_tooling() {
    mkdir -p "$1/tests/fixtures" "$1/data"
    printf '{ "name": "excluded-pkg-marker" }\n' > "$1/package.json"
    printf '{ "name": "excluded-lock-marker" }\n' > "$1/package-lock.json"
    printf '{ "x": "excluded-tsconfig-marker" }\n' > "$1/tsconfig.json"
    printf '{ "x": "excluded-tsbuild-marker" }\n' > "$1/tsconfig.build.json"
    printf '{ "x": "excluded-teststs-marker" }\n' > "$1/tests/tsconfig.json"
    printf '{ "vector": "excluded-fixture-marker" }\n' > "$1/tests/fixtures/vectors.json"
    printf '{ "name": "shipped-data-marker" }\n' > "$1/data/package.json"
}

lua_app() {
    mkdir -p "$1/lib"
    cat > "$1/app.lua" <<'LUA'
local util = require("./lib/util")
app.manifest({ modules = {} })
app.main(function(ctx) print(util.greet("lua")) return 0 end)
LUA
    cat > "$1/lib/util.lua" <<'LUA'
return { greet = function(n) return "hi " .. n end }
LUA
    add_tooling "$1"
    cat > "$1/tests/test_app.lua" <<'LUA'
test("greets", function() test.eq(1, 1) end)
LUA
    cat > "$1/tests/helper.lua" <<'LUA'
return { excluded_helper_marker = true }
LUA
}

ts_app() {
    mkdir -p "$1/lib"
    cat > "$1/app.ts" <<'TS'
import { app } from "hull:app";
import { greet } from "./lib/a.ts";
app.manifest({ modules: [] });
app.main((ctx): number => { console.log(greet("ts")); return 0; });
TS
    cat > "$1/lib/a.ts" <<'TS'
export function greet(n: string): string { return "hi " + n; }
TS
    add_tooling "$1"
    cat > "$1/tests/test_app.ts" <<'TS'
const n: number = 1;
test("greets", () => { test.eq(n, 1); });
TS
    cat > "$1/tests/helper.ts" <<'TS'
export const excluded_helper_marker: boolean = true;
TS
}

# One probe build: without a platform library there is nothing to test.
mkdir -p "$WORK/probe"
printf 'app.manifest({ modules = {} })\napp.main(function() return 0 end)\n' > "$WORK/probe/app.lua"
probe=$("$HULL" build --no-verify-platform "$WORK/probe" -o "$WORK/out/probe" 2>&1) || {
    case "$probe" in
        *"platform library not embedded"*|*"cannot find libhull_platform.a"*|*"libhull_platform.a"*)
            echo "e2e_build_excludes: SKIP (no platform library; make platform)"; exit 0 ;;
    esac
    echo "e2e_build_excludes: probe build failed: $probe"; exit 1
}

for rt in lua ts; do
    echo "== build_excludes ($rt)"
    d="$WORK/app-$rt"
    mkdir -p "$d"
    "${rt}_app" "$d"
    if [ "$rt" = ts ] && ! "$HULL" agent inspect "$d" 2>/dev/null |
            grep -q '"analyzable":true,"capabilities":\[[^]]*"type_erasure"'; then
        echo "  SKIP: no JavaScript frontend in this build"
        continue
    fi
    bin="$WORK/out/bin-$rt"
    rc=0; "$HULL" build --no-verify-platform "$d" -o "$bin" > "$WORK/b-$rt.txt" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then bad "$rt: hull build (rc $rc)"; cat "$WORK/b-$rt.txt"; continue; fi

    # (a) Nothing under tests/ and no root tooling config is embedded;
    # data/package.json is.
    strings -n 6 "$bin" > "$WORK/s-$rt.txt"
    leaked=""
    # (Content markers, and entry names the hull runtime's own strings never
    # spell: the binary also carries Hull's code, which names tsconfig.json.)
    for m in excluded-fixture-marker excluded_helper_marker excluded-pkg-marker \
             excluded-lock-marker excluded-tsconfig-marker excluded-tsbuild-marker \
             excluded-teststs-marker './tests/' './package.json' './package-lock.json' \
             './tsconfig.build.json'; do
        has "$WORK/s-$rt.txt" "$m" && leaked="$leaked $m"
    done
    [ -z "$leaked" ] && ok "$rt: tests/ and the root tooling configs are not embedded" ||
        bad "$rt: embedded:$leaked"
    has "$WORK/s-$rt.txt" "shipped-data-marker" && has "$WORK/s-$rt.txt" "./data/package.json" &&
        ok "$rt: data/package.json (below the root) is still embedded" ||
        bad "$rt: data/package.json is missing from the binary"

    # (b) The built app runs.
    mkdir -p "$WORK/run-$rt"
    cp "$bin" "$WORK/run-$rt/app"
    out=$(cd "$WORK/run-$rt" && ./app 2>&1); rc=$?
    [ "$rc" -eq 0 ] && case "$out" in *"hi $rt"*) true ;; *) false ;; esac &&
        ok "$rt: the built app runs" || bad "$rt: built app (rc $rc): $out"

    # (c) One line names what was left out.
    has "$WORK/b-$rt.txt" "hull build: excluded 8 tooling/test file(s): tests/ (4), package-lock.json, package.json, tsconfig.build.json, tsconfig.json" &&
        ok "$rt: the build lists what it left out" ||
        { bad "$rt: excluded-files line"; cat "$WORK/b-$rt.txt"; }

    # (d) An app that loads a left-out file fails the build.
    if [ "$rt" = lua ]; then
        cp "$d/lib/util.lua" "$WORK/util.good"
        printf 'local h = require("./../tests/helper")\n' | cat - "$WORK/util.good" > "$d/lib/util.lua"
        want='lib/util.lua requires ./../tests/helper (tests/helper.lua)'
    else
        cp "$d/lib/a.ts" "$WORK/a.good"
        printf 'import cfg from "../package.json";\n' | cat - "$WORK/a.good" > "$d/lib/a.ts"
        want='lib/a.ts imports ../package.json (package.json)'
    fi
    rc=0; "$HULL" build --no-verify-platform "$d" -o "$WORK/out/bad-$rt" > "$WORK/bad-$rt.txt" 2>&1 || rc=$?
    [ "$rc" -ne 0 ] && has "$WORK/bad-$rt.txt" "$want" && [ ! -e "$WORK/out/bad-$rt" ] &&
        ok "$rt: an app that loads a left-out file fails the build" ||
        { bad "$rt: import of a left-out file (rc $rc)"; cat "$WORK/bad-$rt.txt"; }
    if [ "$rt" = lua ]; then cp "$WORK/util.good" "$d/lib/util.lua"; else cp "$WORK/a.good" "$d/lib/a.ts"; fi
    if [ "$rt" = ts ]; then
        cp "$d/app.ts" "$WORK/app.good"
        printf 'import { excluded_helper_marker } from "./tests/helper.ts";\n' | cat - "$WORK/app.good" > "$d/app.ts"
        rc=0; "$HULL" build --no-verify-platform "$d" -o "$WORK/out/bad2-$rt" > "$WORK/bad2-$rt.txt" 2>&1 || rc=$?
        [ "$rc" -ne 0 ] && has "$WORK/bad2-$rt.txt" 'app.ts imports ./tests/helper.ts (tests/helper.ts)' &&
            ok "$rt: an import of tests/ fails the build" ||
            { bad "$rt: import of tests/ (rc $rc)"; cat "$WORK/bad2-$rt.txt"; }
        cp "$WORK/app.good" "$d/app.ts"
    fi

    # (e) A signed build of the app with tests/ verifies, and package.sig
    # lists the kept files only.
    k="$WORK/keys-$rt"
    mkdir -p "$k"
    cp build/libhull_platform.a "$k/" 2>/dev/null || true
    cp build/platform_canary_hash "$k/" 2>/dev/null || true
    (cd "$k" && "$HULL" keygen > /dev/null 2>&1 && "$HULL" sign-platform --dir "$k/" "$k/developer" > /dev/null 2>&1) || true
    rc=0; "$HULL" build --no-verify-platform --sign "$k/developer.key" --platform-sig "$k/platform.sig" \
        "$d" -o "$d/app" > "$WORK/sb-$rt.txt" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        bad "$rt: signed build (rc $rc)"; cat "$WORK/sb-$rt.txt"
    else
        if has "$d/package.sig" '"data/package.json"' && ! has "$d/package.sig" '"tests/' &&
           ! has "$d/package.sig" '"package.json"' && ! has "$d/package.sig" '"tsconfig.json"'; then
            ok "$rt: package.sig covers exactly the kept files"
        else
            bad "$rt: package.sig file list"; cat "$d/package.sig"
        fi
        "$HULL" verify --no-verify-platform --platform-key "$k/developer.pub" "$d" > "$WORK/v-$rt.txt" 2>&1 &&
            has "$WORK/v-$rt.txt" "all checks passed" &&
            ok "$rt: hull verify passes on a signed app with tests/ and tooling configs" ||
            { bad "$rt: hull verify"; cat "$WORK/v-$rt.txt"; }
        out=$(cd "$d" && ./app --verify-sig "$k/developer.pub" --hull-no-verify-platform 2>&1); rc=$?
        [ "$rc" -eq 0 ] && case "$out" in *"hi $rt"*) true ;; *) false ;; esac &&
            ok "$rt: the signed binary starts under --verify-sig beside tests/" ||
            bad "$rt: --verify-sig (rc $rc): $out"
    fi
done

[ "$fail" -eq 0 ] && echo "e2e_build_excludes: OK" || { echo "e2e_build_excludes: FAILED"; exit 1; }
