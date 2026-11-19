#!/bin/sh
# e2e_manifest_readers.sh - the commands that read an app's manifest without
# running it (hull manifest, hull modules list, hull modules analyze, and so
# hull check) on a Lua, a JavaScript and a TypeScript app. `hull modules
# list` looked for app.lua alone, so a JavaScript app (and `hull check`,
# which starts there) failed with "no app.lua found"; `hull modules analyze`
# ran app.js through the Lua extractor and never scanned .ts files.
# Then `hull modules analyze`'s own reading, case by case: the `app` methods a
# module adds, a module that is not intrinsic, a dependency the resolver
# admits, a hyphenated name, an erased `import type`, a comment.
set -eu

HULL=${HULL:-build/hull}
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
HOME="$WORK/home"
mkdir -p "$HOME"
export HOME
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }

mkdir -p "$WORK/lua" "$WORK/js" "$WORK/ts/lib"
cat > "$WORK/lua/app.lua" <<'LUA'
local app = require("hull.app")
local time = require("hull.time")
app.manifest({ modules = { "hull/time@1" } })
app.main(function() return time.now() > 0 and 0 or 1 end)
LUA
cat > "$WORK/js/app.js" <<'JS'
import { app } from "hull:app";
import { time } from "hull:time";
app.manifest({ modules: ["hull/time@1"] });
app.main(() => (time.now() > 0 ? 0 : 1));
JS
cat > "$WORK/ts/app.ts" <<'TS'
import { app } from "hull:app";
import { stamp } from "./lib/stamp.ts";
app.manifest({ modules: ["hull/time@1"] });
app.main((): number => (stamp() > 0 ? 0 : 1));
TS
cat > "$WORK/ts/lib/stamp.ts" <<'TS'
import { time } from "hull:time";
export function stamp(): number { return time.now(); }
TS

for rt in lua js ts; do
    d="$WORK/$rt"
    echo "== $rt"
    "$HULL" manifest "$d" > "$WORK/m.txt" 2>&1 && has "$WORK/m.txt" '"hull/time@1"' &&
        ok "$rt: hull manifest" || { bad "$rt: hull manifest"; cat "$WORK/m.txt"; }
    "$HULL" modules list "$d" > "$WORK/l.txt" 2>&1 && has "$WORK/l.txt" "hull/time@1" &&
        ok "$rt: hull modules list" || { bad "$rt: hull modules list"; cat "$WORK/l.txt"; }
    "$HULL" modules analyze "$d" > "$WORK/a.txt" 2>&1 &&
        has "$WORK/a.txt" "every module used is declared" &&
        ok "$rt: hull modules analyze finds the import declared" ||
        { bad "$rt: hull modules analyze"; cat "$WORK/a.txt"; }
    # hull check gets past both manifest steps (its tests and verify steps
    # are not this test's: the app has no tests and no signature).
    "$HULL" check "$d" > "$WORK/c.txt" 2>&1 || true
    if has "$WORK/c.txt" "[hull:check] running tests..." &&
       ! has "$WORK/c.txt" "manifest validation failed" && ! has "$WORK/c.txt" "import analysis failed"; then
        ok "$rt: hull check gets past the manifest and import steps"
    else
        bad "$rt: hull check"; cat "$WORK/c.txt"
    fi
done

# An import the manifest does not declare is reported in TypeScript too
# (only .js files were scanned).
cat > "$WORK/ts/lib/hash.ts" <<'TS'
import { crypto } from "hull:crypto";
export const h = (s: string): string => crypto.sha256(s);
TS
rc=0; "$HULL" modules analyze "$WORK/ts" > "$WORK/u.txt" 2>&1 || rc=$?
[ "$rc" -ne 0 ] && has "$WORK/u.txt" "hull/crypto" &&
    ok "ts: an undeclared import in a .ts file is reported" || { bad "ts: undeclared import (rc $rc)"; cat "$WORK/u.txt"; }

# analyze_case NAME FILE EXPECT_RC [MUST_HAVE] [MUST_NOT_HAVE]: a one-file
# JavaScript (FILE .js) or Lua (FILE .lua) app, read from stdin.
analyze_case() {
    name=$1; file=$2; want=$3; must=${4:-}; mustnt=${5:-}
    c="$WORK/case_$name"
    mkdir -p "$c"
    cat > "$c/$file"
    rc=0; "$HULL" modules analyze "$c" > "$WORK/case.txt" 2>&1 || rc=$?
    if [ "$rc" -eq "$want" ] && { [ -z "$must" ] || has "$WORK/case.txt" "$must"; } &&
       { [ -z "$mustnt" ] || ! has "$WORK/case.txt" "$mustnt"; }; then
        ok "analyze: $name"
    else
        bad "analyze: $name (rc $rc)"; cat "$WORK/case.txt"
    fi
}
echo "== hull modules analyze"
analyze_case "app.get uses the declared hull/http-server" app.js 0 "OK - every module used" <<'JS'
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1", "hull/timers@1"] });
app.get("/", (_req, res) => res.text("ok"));
app.every(1000, () => {});
JS
analyze_case "app.get without hull/http-server is undeclared" app.lua 1 "hull/http-server" <<'LUA'
local app = require("hull.app")
app.manifest({ modules = {} })
app.get("/", function(_req, res) res:text("ok") end)
LUA
analyze_case "an undeclared hull/log is reported (not intrinsic)" app.js 1 "hull/log" <<'JS'
import { app } from "hull:app";
import { log } from "hull:log";
app.manifest({ modules: [] });
app.main(() => { log.info("x"); return 0; });
JS
analyze_case "hull/db admitted as session's dependency is not undeclared" app.js 0 "" "Undeclared" <<'JS'
import { app } from "hull:app";
import { session } from "hull:web:middleware:session";
import { db } from "hull:db";
app.manifest({ modules: ["hull/web/middleware/session@1"] });
app.main(() => (db && session ? 0 : 1));
JS
analyze_case "a hyphenated module name is read" app.lua 1 "hull/http-client" <<'LUA'
local app = require("hull.app")
local http = require "hull.http-client"
app.manifest({ modules = {} })
app.main(function() return http and 0 or 1 end)
LUA
mkdir -p "$WORK/case_type"
cat > "$WORK/case_type/app.ts" <<'TS'
import { app } from "hull:app";
import type { DbConnection } from "hull:db";
// app.get("/x") in a comment is not a call
const c: DbConnection | null = null;
app.manifest({ modules: [] });
app.main((): number => (c ? 1 : 0));
TS
rc=0; "$HULL" modules analyze "$WORK/case_type" > "$WORK/case.txt" 2>&1 || rc=$?
[ "$rc" -eq 0 ] && has "$WORK/case.txt" "OK - every module used" &&
    ok "analyze: an erased import type and a comment use nothing" || { bad "analyze: import type (rc $rc)"; cat "$WORK/case.txt"; }

[ "$fail" -eq 0 ] && echo "e2e_manifest_readers: OK" || { echo "e2e_manifest_readers: FAILED"; exit 1; }
