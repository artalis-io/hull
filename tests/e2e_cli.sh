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
# Two db.udf regressions in one app.main run:
#  (a) teardown ordering: the udf holds a runtime-side closure (JS JS_DupValue /
#      Lua registry ref) freed by sqlite3_close's xDestroy. If the DB is closed
#      AFTER the runtime is freed, QuickJS's JS_FreeRuntime aborts (SIGABRT->134)
#      on the leaked closure. See app_context.c hl_app_context_free (DB closed
#      before the runtime).
#  (b) failed-register double-free: sqlite3_create_function_v2 invokes the udf's
#      xDestroy ITSELF on failure (e.g. a >255-byte name), so the register paths
#      must NOT free the ctx again. A regression double-frees + double-unrefs
#      (heap corruption / QuickJS refcount corruption -> crash, esp. under ASan).
# Either regression flips the clean exit (0) to a non-zero/abort.
run_udf_teardown() {
    runtime="$1"; ext="$2"
    echo "--- db.udf teardown + failed-register (${runtime}) ---"
    d=$(mktemp -d)
    if [ "$ext" = "lua" ]; then
        cat > "${d}/app.lua" <<'LUA'
local db = require("hull.db").default()
app.manifest({ modules = { "hull/db@1" } })
app.main(function()
  db.udf.register("hull_double", function(x) return x * 2 end, { deterministic = true })
  -- (b) a >255-byte name makes sqlite3_create_function_v2 fail; the error must be
  -- raised cleanly (SQLite already ran xDestroy) with no double-free.
  local ok = pcall(function()
    db.udf.register("hull_" .. string.rep("x", 400), function(x) return x end)
  end)
  if ok then return 5 end
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
  let threw = false;
  try { db.udf.register("hull_" + "x".repeat(400), (x) => x); } catch (e) { threw = true; }
  if (!threw) return 5;
  const r = db.query("SELECT hull_double(21) AS v");
  return (r[0] && r[0].v === 42) ? 0 : 3;
});
JS
    fi
    "${HULL_BIN}" "${d}/app.${ext}" -d ":memory:" >/dev/null 2>&1
    rc=$?
    # 0 = udf ran + failed-register handled + clean teardown; non-zero/134/139 = a
    # teardown-leak abort or a failed-register double-free (regression).
    expect_eq "${runtime} db.udf clean exit (teardown + failed-register safe)" "0" "${rc}"
    rm -rf "${d}"
}

run_udf_teardown "lua" "lua"
run_udf_teardown "js"  "js"

# ── fs round-trip through app.main (checkpoint 3, Slice B) ────────────────────
# The path-authorization policy is compiled from the manifest's fs grants and
# wired onto the CLI runtime. A grant of "." authorizes the whole app dir, so a
# write + read-back must round-trip (before Slice B's wiring fix the CLI denied
# every fs op). Exit 0 = round-trip OK.
run_fs_roundtrip() {
    runtime="$1"; ext="$2"
    echo "--- fs round-trip via app.main (${runtime}) ---"
    d=$(mktemp -d)
    if [ "$ext" = "lua" ]; then
        cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = { "hull/fs@1" }, fs = { read = { "." }, write = { "." } } })
app.main(function()
  local fs = require("hull.fs")
  local wok = fs.write("cli.txt", "cli-roundtrip")
  local rok, data = pcall(fs.read, "cli.txt")
  return (wok and rok and data == "cli-roundtrip") and 0 or 3
end)
LUA
    else
        cat > "${d}/app.js" <<'JS'
import { app } from "hull:app";
import { fs } from "hull:fs";
app.manifest({ modules: ["hull/fs@1"], fs: { read: ["."], write: ["."] } });
app.main(() => {
  fs.write("cli.txt", "cli-roundtrip");
  const buf = fs.read("cli.txt");
  const len = (buf && buf.byteLength !== undefined) ? buf.byteLength : (buf ? buf.length : 0);
  return (len === 13) ? 0 : 3;   // "cli-roundtrip" is 13 bytes
});
JS
    fi
    "${HULL_BIN}" "${d}/app.${ext}" >/dev/null 2>&1
    rc=$?
    expect_eq "${runtime} fs round-trip via app.main" "0" "${rc}"
    rm -rf "${d}"
}

run_fs_roundtrip "lua" "lua"
run_fs_roundtrip "js"  "js"

echo
echo "${PASS}/$((PASS + FAIL)) CLI e2e tests passed"
[ "${FAIL}" -eq 0 ]
