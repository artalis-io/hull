#!/bin/sh
# tests/e2e_cli.sh — End-to-end coverage for examples/*_cli (app.main apps).
#
# Distinct from tests/e2e_examples.sh which exercises HTTP server apps
# by starting them and curl'ing. CLI examples are one-shot: invoke,
# check stdout + exit code, done.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL_BIN="${HULL_BIN:-build/hull}"
PASS=0
FAIL=0

# pass/fail recorder. `$3` is the test label.
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

# expect_eq <label> <expected> <actual>
expect_eq() {
    if [ "$2" = "$3" ]; then
        pass "$1"
    else
        fail "$1 (expected '$2', got '$3')"
    fi
}

# run_hello_cli <runtime> <ext>
run_hello_cli() {
    runtime="$1"; ext="$2"
    app="examples/hello_cli/app.${ext}"
    echo "--- hello_cli (${runtime}) ---"

    # 1. Valid argv → exit 0, stdout starts with "hello"
    out=$("${HULL_BIN}" run "${app}" -- world 2>/dev/null)
    rc=$?
    expect_eq "${runtime} hello_cli exit code on valid argv" "0" "${rc}"
    line1=$(echo "${out}" | head -1)
    expect_eq "${runtime} hello_cli greeting line" "hello world" "${line1}"

    # 2. No args → exit 1, usage on stderr
    err=$("${HULL_BIN}" run "${app}" 2>&1 >/dev/null)
    rc=$?
    expect_eq "${runtime} hello_cli exit code on no args" "1" "${rc}"
    case "${err}" in
        *usage:*) pass "${runtime} hello_cli usage on stderr" ;;
        *)        fail "${runtime} hello_cli missing usage line (got '${err}')" ;;
    esac

    # 3. --stdin reads from stdin
    out=$(echo "alice" | "${HULL_BIN}" run "${app}" -- --stdin 2>/dev/null | head -1)
    expect_eq "${runtime} hello_cli reads stdin" "hello alice" "${out}"

    # 4. --stdin with empty stdin → exit 2
    rc=$(echo "" | "${HULL_BIN}" run "${app}" -- --stdin >/dev/null 2>&1; echo $?)
    expect_eq "${runtime} hello_cli exit code on empty stdin" "2" "${rc}"
}

run_hello_cli "lua" "lua"
run_hello_cli "js"  "js"

# run_udf_teardown <runtime> <ext>
# Regression: a db.udf-registering app.main app must exit CLEANLY (0) after the
# runtime is torn down. The udf holds a runtime-side closure (JS JS_DupValue /
# Lua registry ref) freed by sqlite3_close's xDestroy; if the DB is closed AFTER
# the runtime is freed, QuickJS's JS_FreeRuntime aborts (SIGABRT -> 134) on the
# leaked closure. See app_context.c hl_app_context_free (DB closed before runtime).
run_udf_teardown() {
    runtime="$1"; ext="$2"
    echo "--- db.udf teardown (${runtime}) ---"
    d=$(mktemp -d)
    if [ "$ext" = "lua" ]; then
        cat > "${d}/app.lua" <<'LUA'
local db = require("hull.db").default()
app.manifest({ modules = { "hull/db@1" } })
app.main(function()
  db.udf.register("hull_double", function(x) return x * 2 end, { deterministic = true })
  local r = db.query("SELECT hull_double(21) AS v")
  return (r[1] and r[1].v == 42) and 0 or 3
end)
LUA
    else
        cat > "${d}/app.js" <<'JS'
import { app } from "hull:app";
import { db as dbmod } from "hull:db";
app.manifest({ modules: ["hull/db@1"] });
app.main(() => {
  const db = dbmod.default();
  db.udf.register("hull_double", (x) => x * 2, { deterministic: true });
  const r = db.query("SELECT hull_double(21) AS v");
  return (r[0] && r[0].v === 42) ? 0 : 3;
});
JS
    fi
    "${HULL_BIN}" "${d}/app.${ext}" -d ":memory:" >/dev/null 2>&1
    rc=$?
    # 0 = udf ran + clean teardown; 134 = the JS_FreeRuntime leak abort (regression).
    expect_eq "${runtime} db.udf app.main clean exit (no teardown abort)" "0" "${rc}"
    rm -rf "${d}"
}

run_udf_teardown "lua" "lua"
run_udf_teardown "js"  "js"

echo
echo "${PASS}/$((PASS + FAIL)) CLI e2e tests passed"
[ "${FAIL}" -eq 0 ]
