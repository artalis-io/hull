#!/bin/sh
# tests/e2e_cli.sh - End-to-end coverage for examples/*_cli (app.main apps).
#
# Distinct from tests/e2e_examples.sh which exercises HTTP server apps
# by starting them and curl'ing. CLI examples are one-shot: invoke,
# check stdout + exit code, done.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

HULL_BIN="${HULL_BIN:-build/hull}"

# hull exits with a real code; on Windows an APE reports it shifted and a
# POSIX shell reads 0 for every outcome (jart/cosmopolitan#1521). This suite is
# almost entirely exit-code assertions, so without recovering the real status
# it asserts nothing there.
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"
HULL_RC_TMP="${TMPDIR:-/tmp}/hull_cli_rc.$$"
trap 'rm -f "$HULL_RC_TMP" "$HULL_RC_TMP.in"' EXIT
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
    rc=$(hull_run "$HULL_RC_TMP" "${HULL_BIN}" run "${app}" -- world)
    out=$(cat "$HULL_RC_TMP")
    expect_eq "${runtime} hello_cli exit code on valid argv" "0" "${rc}"
    line1=$(echo "${out}" | head -1)
    expect_eq "${runtime} hello_cli greeting line" "hello world" "${line1}"

    # 2. No args → exit 1, usage on stderr
    rc=$(hull_run "$HULL_RC_TMP" "${HULL_BIN}" run "${app}")
    err=$(cat "$HULL_RC_TMP")
    expect_eq "${runtime} hello_cli exit code on no args" "1" "${rc}"
    case "${err}" in
        *usage:*) pass "${runtime} hello_cli usage on stderr" ;;
        *)        fail "${runtime} hello_cli missing usage line (got '${err}')" ;;
    esac

    # 3. --stdin reads from stdin
    out=$(echo "alice" | "${HULL_BIN}" run "${app}" -- --stdin 2>/dev/null | head -1)
    expect_eq "${runtime} hello_cli reads stdin" "hello alice" "${out}"

    # 4. --stdin with empty stdin → exit 2
    : > "$HULL_RC_TMP.in"
    rc=$(HULL_RC_STDIN="$HULL_RC_TMP.in" hull_rc "${HULL_BIN}" run "${app}" -- --stdin)
    expect_eq "${runtime} hello_cli exit code on empty stdin" "2" "${rc}"

    # 5. An app argument without `--` is refused with the fix spelled out. It
    #    used to replace the entry point ("world" was loaded as the app) and
    #    fail with only "app context init failed".
    rc=$(hull_run "$HULL_RC_TMP" "${HULL_BIN}" run "${app}" world)
    err=$(cat "$HULL_RC_TMP")
    expect_eq "${runtime} hello_cli exit code on a stray argument" "1" "${rc}"
    case "${err}" in
        *"go after --"*) pass "${runtime} hello_cli stray argument names the fix" ;;
        *)               fail "${runtime} hello_cli stray argument message (got '${err}')" ;;
    esac

    # 6. An option Hull does not take is refused too: it used to be skipped
    #    silently, so a misspelt one did nothing and said nothing.
    rc=$(hull_run "$HULL_RC_TMP" "${HULL_BIN}" run "${app}" --no-sandbx -- world)
    err=$(cat "$HULL_RC_TMP")
    expect_eq "${runtime} hello_cli exit code on an unknown option" "1" "${rc}"
    case "${err}" in
        *"unknown option '--no-sandbx'"*) pass "${runtime} hello_cli unknown option is named" ;;
        *) fail "${runtime} hello_cli unknown option message (got '${err}')" ;;
    esac

    # 7. ...but the global flags the dispatcher reads still pass through.
    rc=$(hull_run "$HULL_RC_TMP" "${HULL_BIN}" --verbose run "${app}" -- world)
    expect_eq "${runtime} hello_cli accepts a global flag" "0" "${rc}"
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
    rc=$(hull_rc "${HULL_BIN}" "${d}/app.${ext}" -d ":memory:")
    # 0 = udf ran + failed-register handled + clean teardown; non-zero/134/139 = a
    # teardown-leak abort or a failed-register double-free (regression).
    expect_eq "${runtime} db.udf clean exit (teardown + failed-register safe)" "0" "${rc}"
    rm -rf "${d}"
}

run_udf_teardown "lua" "lua"
run_udf_teardown "js"  "js"

# ── fs round-trip through app.main ────────────────────
# The path-authorization policy is compiled from the manifest's fs grants and
# wired onto the CLI runtime. A grant of "." authorizes the whole app dir, so a
# write + read-back must round-trip (before the wiring fix the CLI denied
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
    rc=$(hull_rc "${HULL_BIN}" "${d}/app.${ext}")
    expect_eq "${runtime} fs round-trip via app.main" "0" "${rc}"
    rm -rf "${d}"
}

run_fs_roundtrip "lua" "lua"
run_fs_roundtrip "js"  "js"

# ── fs.stat + fs.list parity through app.main ─────────
# Proves the Lua and JS bindings return EQUIVALENT values + error tokens for the
# metadata (stat) and enumeration (list) surface: present -> metadata, absent ->
# nil/null, deterministic byte-order list, and a "not_found" token on a missing
# dir (Lua returns (nil, err); JS throws with the token in the message). The app
# asserts every case internally and returns 0 only when all match.
run_fs_statlist() {
    runtime="$1"; ext="$2"
    echo "--- fs.stat + fs.list parity via app.main (${runtime}) ---"
    d=$(mktemp -d)
    if [ "$ext" = "lua" ]; then
        cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = { "hull/fs@1" }, fs = { read = { "." }, write = { "." } } })
app.main(function()
  local fs = require("hull.fs")
  fs.write("data/a.csv", "1"); fs.write("data/b.txt", "1"); fs.write("data/c.csv", "1")
  local st = fs.stat("data/a.csv")
  if not (st and st.type == "file" and st.size == 1) then return 2 end
  if fs.stat("data/nope") ~= nil then return 3 end            -- absent -> nil
  local es = fs.list("data")
  if #es ~= 3 then return 4 end
  if not (es[1].name == "a.csv" and es[2].name == "b.txt" and es[3].name == "c.csv") then return 5 end
  if es[1].type ~= "file" then return 6 end                    -- deterministic order
  local l, err = fs.list("data/missingdir")
  if l ~= nil or err ~= "not_found" then return 7 end          -- error token
  return 0
end)
LUA
    else
        cat > "${d}/app.js" <<'JS'
import { app } from "hull:app";
import { fs } from "hull:fs";
app.manifest({ modules: ["hull/fs@1"], fs: { read: ["."], write: ["."] } });
app.main(() => {
  fs.write("data/a.csv", "1"); fs.write("data/b.txt", "1"); fs.write("data/c.csv", "1");
  const st = fs.stat("data/a.csv");
  if (!(st && st.type === "file" && st.size === 1)) return 2;
  if (fs.stat("data/nope") !== null) return 3;                 // absent -> null
  const es = fs.list("data");
  if (es.length !== 3) return 4;
  if (!(es[0].name === "a.csv" && es[1].name === "b.txt" && es[2].name === "c.csv")) return 5;
  if (es[0].type !== "file") return 6;                          // deterministic order
  let tok = null;
  try { fs.list("data/missingdir"); } catch (e) { tok = String(e.message || e); }
  if (tok === null || tok.indexOf("not_found") < 0) return 7;   // error token
  return 0;
});
JS
    fi
    rc=$(hull_rc "${HULL_BIN}" "${d}/app.${ext}")
    expect_eq "${runtime} fs.stat + fs.list parity via app.main" "0" "${rc}"
    rm -rf "${d}"
}

run_fs_statlist "lua" "lua"
run_fs_statlist "js"  "js"

# ── app.main as a startup hook that waits, then serve (audit 5 C1) ──
# The main coroutine's registry ref was released twice when main yielded:
# Lua's free list then handed one slot to two requests, and the first one's
# parked coroutine was collected and resumed from freed memory.
run_main_hook_then_serve() {
    echo "--- app.main waits, then two overlapping parked requests (lua) ---"
    d=$(mktemp -d)
    port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()' 2>/dev/null || echo 19877)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = { "hull/http-server@1" } })
app.main(function() hull.sleep(5) return 0 end)
app.get("/slow", function(req, res)
    hull.sleep(400)
    res:text("slow-ok")
end)
app.get("/gc", function(req, res)
    collectgarbage(); collectgarbage()
    res:text("gc-ok")
end)
LUA
    "${HULL_BIN}" -p "${port}" -d "${d}/data.db" "${d}/app.lua" > "${d}/log" 2>&1 &
    pid=$!
    i=0
    while [ $i -lt 50 ] && ! curl -s -o /dev/null "http://127.0.0.1:${port}/gc"; do
        sleep 0.2; i=$((i + 1))
    done
    curl -s -m 10 "http://127.0.0.1:${port}/slow" > "${d}/a" &
    ca=$!
    sleep 0.05
    curl -s -m 10 "http://127.0.0.1:${port}/slow" > "${d}/b" &
    cb=$!
    sleep 0.05
    curl -s -m 10 "http://127.0.0.1:${port}/gc" > /dev/null
    wait $ca; wait $cb
    expect_eq "lua parked request A survives (main hook waited)" "slow-ok" "$(cat "${d}/a")"
    expect_eq "lua parked request B survives (main hook waited)" "slow-ok" "$(cat "${d}/b")"
    if kill -0 "$pid" 2>/dev/null; then pass "lua server alive after overlapping parked requests"
    else fail "lua server died: $(tail -5 "${d}/log")"; fi
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    rm -rf "${d}"
}

run_main_hook_then_serve

# ── A BUILT CLI tool (the app.main runner, serve_cli.c) ──────────────
# `hull <app>` runs on the server runner; only a `hull build` of an app with no
# HTTP module reaches serve_cli.c, so these build one. Skipped when hull has no
# compiler it can drive (hull doctor's verdict, as in e2e_build.sh) or no
# platform library: none embedded and no libhull_platform*.a beside it.
can_build_cli() {
    _doc=$("${HULL_BIN}" doctor --json 2>/dev/null || true)
    case "$_doc" in
        *'"build_compiler":null'*) return 1 ;;
        *'"platform_embedded":"none"'*)
            ls "$(dirname "${HULL_BIN}")"/libhull_platform*.a >/dev/null 2>&1 || return 1 ;;
    esac
    return 0
}

# build_cli <dir> [extra hull build args...] -> the built binary's path, or "".
build_cli() {
    _d="$1"; shift
    _cc=""
    hull_is_ape "${HULL_BIN}" && _cc="--compiler cosmocc"
    # shellcheck disable=SC2086
    "${HULL_BIN}" build --no-verify-platform $_cc "$@" -o "${_d}/tool" "${_d}" \
        > "${_d}/build.log" 2>&1
    for _b in "${_d}/tool" "${_d}/tool.com"; do
        [ -f "$_b" ] && { echo "$_b"; return 0; }
    done
    echo ""
}

run_built_cli() {
    echo "--- built app.main tool: error paths + its own options ---"
    if ! can_build_cli; then
        echo "  SKIP: no build compiler or platform library for hull build"
        return 0
    fi
    hull_abs=$(cd "$(dirname "${HULL_BIN}")" && pwd)/$(basename "${HULL_BIN}")

    # (audit 6 M2) Options the app.main runner does not implement are the
    # tool's own arguments: they used to be refused as "spelled --hull-s",
    # and --hull-s was then unknown, so no spelling reached the app.
    d=$(mktemp -d)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({})
app.main(function(ctx)
    ctx.stdout:write(table.concat(ctx.args, " ") .. "\n")
    return 0
end)
LUA
    bin=$(build_cli "$d")
    if [ -z "$bin" ]; then
        fail "built tool: hull build failed ($(tail -3 "${d}/build.log"))"
    else
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" -s pattern -m msg --tls-cert c --wasm-gas 9)
        expect_eq "built tool: -s/-m/--tls-cert/--wasm-gas reach the app (exit)" "0" "$rc"
        case "$(cat "$HULL_RC_TMP")" in
            *"-s pattern -m msg --tls-cert c --wasm-gas 9"*) pass "built tool: the app saw its options" ;;
            *) fail "built tool: app options (got '$(cat "$HULL_RC_TMP")')" ;;
        esac
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" --no-sandbox x)
        expect_eq "built tool: a bare --no-sandbox is still refused" "1" "$rc"
        case "$(cat "$HULL_RC_TMP")" in
            *"--hull-no-sandbox"*) pass "built tool: refusal names --hull-no-sandbox" ;;
            *) fail "built tool: --no-sandbox refusal (got '$(cat "$HULL_RC_TMP")')" ;;
        esac
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" --hull-max-instructions 50000000 -- ok)
        expect_eq "built tool: a --hull- option it implements is taken" "0" "$rc"
    fi
    rm -rf "$d"

    # (audit 6 M1) An undeclared $VAR reference fails after the app context
    # exists. The error path freed the context, then wrote the runtime it
    # lived in (unmapped): SIGSEGV instead of exit 1.
    d=$(mktemp -d)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ hosts = { "$HULL_E2E_UNDECLARED_VAR" } })
app.main(function() return 0 end)
LUA
    bin=$(build_cli "$d")
    if [ -z "$bin" ]; then
        fail "undeclared-ref tool: hull build failed ($(tail -3 "${d}/build.log"))"
    else
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin")
        expect_eq "built tool: undeclared \$VAR reference exits 1 (no crash)" "1" "$rc"
        case "$(cat "$HULL_RC_TMP")" in
            *HULL_E2E_UNDECLARED_VAR*) pass "built tool: the undeclared variable is named" ;;
            *) fail "built tool: undeclared ref message (got '$(cat "$HULL_RC_TMP")')" ;;
        esac
    fi
    rm -rf "$d"

    # (audit 6 M1) --verify-sig with a manifest that differs at run time from
    # the signed one (math.random is seeded per VM): the policy-check failure
    # path destroyed the seal arena under the live module set, then wrote the
    # freed runtime.
    d=$(mktemp -d)
    (cd "$d" && "$hull_abs" keygen dev >/dev/null 2>&1)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ env = { "HULL_E2E_" .. math.random(1, 1000000000) } })
app.main(function() return 0 end)
LUA
    if [ ! -f "${d}/dev.key" ]; then
        fail "verify-sig tool: hull keygen failed"
    else
        bin=$(build_cli "$d" --sign "${d}/dev.key")
        if [ -z "$bin" ]; then
            fail "verify-sig tool: hull build failed ($(tail -3 "${d}/build.log"))"
        else
            rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" --verify-sig "${d}/dev.pub" --hull-no-verify-platform)
            expect_eq "built tool: --verify-sig policy mismatch exits 1 (no crash)" "1" "$rc"
            case "$(cat "$HULL_RC_TMP")" in
                *"refusing to start"*) pass "built tool: the policy mismatch is reported" ;;
                *) fail "built tool: verify-sig mismatch message (got '$(cat "$HULL_RC_TMP")')" ;;
            esac
        fi
    fi
    rm -rf "$d"
}

run_built_cli

echo
echo "${PASS}/$((PASS + FAIL)) CLI e2e tests passed"
[ "${FAIL}" -eq 0 ]
