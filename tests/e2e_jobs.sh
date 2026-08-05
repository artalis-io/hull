#!/bin/sh
# e2e_jobs.sh - hull/jobs@1 durable job queue (Lua + JS).
#
# Phase 1: schema + jobs.init. Asserts jobs.init() is idempotent and creates
# _hull_jobs with the full column set on the default (SQLite) connection, in
# both runtimes. Later phases extend this with enqueue / atomic-claim /
# concurrency (the correctness gate) / worker-mode coverage.
#
# Design: docs/jobs_design.md.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
case "$HULL" in
    /*) ;;
    *)  HULL="$(pwd)/$HULL" ;;
esac

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ - $2}"; }

EXPECT="id,queue,type,payload,status,priority,attempts,max_attempts,run_at,claim_token,claimed_at,dedup_key,last_error,created_at,updated_at"

check_runtime() {
    label="$1"; ext="$2"; body="$3"
    T="$(mktemp -d)"; DB="$T/app.db"
    printf '%s\n' "$body" > "$T/app.$ext"

    out="$("$HULL" "$T/app.$ext" -d "$DB" 2>&1)" || true
    case "$out" in
        *JOBS-INIT-OK*) pass "$label: jobs.init() idempotent, exits ok" ;;
        *) fail "$label: jobs.init() did not report ok" "$out" ;;
    esac

    # Introspect via `hull agent db schema` (bypasses the _hull_ namespace guard).
    cols="$("$HULL" agent db schema "$T" -d "$DB" 2>/dev/null \
        | python3 -c "import sys,json
d=json.load(sys.stdin)
t=[x for x in d.get('tables',[]) if x.get('name')=='_hull_jobs']
print(','.join(c.get('name') for c in t[0].get('columns',[])) if t else 'MISSING')" 2>/dev/null)"
    if [ "$cols" = "$EXPECT" ]; then
        pass "$label: _hull_jobs has the full column set"
    else
        fail "$label: _hull_jobs columns mismatch" "got: $cols"
    fi
    rm -rf "$T"
}

LUA='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init(); jobs.init()   -- idempotent
  ctx.stdout:write("JOBS-INIT-OK\n"); return 0
end)'

JS='import { app } from "hull:app";
import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
app.main((ctx) => { jobs.init(); jobs.init(); ctx.stdout.write("JOBS-INIT-OK\n"); return 0; });'

echo "== Lua =="
check_runtime "lua" "lua" "$LUA"
echo "== JS =="
check_runtime "js" "js" "$JS"

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"
[ "$FAIL" -eq 0 ]
