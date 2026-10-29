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
    # hull_run captures stderr too: skip Hull's own log records ("HH:MM:SS
    # LEVEL ...", e.g. the macOS sandbox notice) to reach the app's first line.
    line1=$(echo "${out}" | grep -v '^[0-9][0-9]:[0-9][0-9]:[0-9][0-9] ' | head -1)
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

if HULL_BIN="$HULL_BIN" sh tests/e2e_db_open_failure.sh; then
    pass "Lua/JS database-open failures exit cleanly"
else
    fail "Lua/JS database-open failures exit cleanly"
fi

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
        # Only the app.main runner (serve_cli.c) leaves these to the app. A
        # tool built against the full `make platform` library (not the slim
        # base a release composes from) links the Keel server runner, whose
        # parser takes them - so the check needs the slim composition.
        if command -v nm >/dev/null 2>&1 && nm "$bin" 2>/dev/null | grep -q kl_http_server_init; then
            echo "  SKIP: built tool options (this platform library links the server runner, not the slim base)"
        else
            rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" -s pattern -m msg --tls-cert c --read-timeout 9)
            expect_eq "built tool: -s/-m/--tls-cert/--read-timeout reach the app (exit)" "0" "$rc"
            case "$(cat "$HULL_RC_TMP")" in
                *"-s pattern -m msg --tls-cert c --read-timeout 9"*) pass "built tool: the app saw its options" ;;
                *) fail "built tool: app options (got '$(cat "$HULL_RC_TMP")')" ;;
            esac
        fi
        # (audit 7 M2) The runner parses the --wasm-* ceilings (round 6), so
        # they are reserved like every downgrade it implements: a bare
        # --wasm-timeout-ms used to lift the manifest's compute ceilings to
        # the compile-time maxima from the tool's argv.
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" --wasm-timeout-ms 3600000 x)
        expect_eq "built tool: a bare --wasm-timeout-ms is refused" "1" "$rc"
        case "$(cat "$HULL_RC_TMP")" in
            *"--hull-wasm-timeout-ms"*) pass "built tool: refusal names --hull-wasm-timeout-ms" ;;
            *) fail "built tool: --wasm-timeout-ms refusal (got '$(cat "$HULL_RC_TMP")')" ;;
        esac
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" --hull-wasm-gas 1000000 -- ok)
        expect_eq "built tool: --hull-wasm-gas is taken" "0" "$rc"
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
        # --sign needs a platform.sig (the developer platform layer); make one
        # over this build's platform library, as e2e_composed_sig.sh does.
        sigarg=""
        plat="$(dirname "${HULL_BIN}")/libhull_platform.a"
        if [ -f "$plat" ]; then
            cp "$plat" "${d}/"
            cp "$(dirname "${HULL_BIN}")/platform_canary_hash" "${d}/" 2>/dev/null || true
            "$hull_abs" sign-platform --dir "${d}/" "${d}/dev" >/dev/null 2>&1 \
                && sigarg="--platform-sig ${d}/platform.sig"
        fi
        # shellcheck disable=SC2086
        bin=$(build_cli "$d" --sign "${d}/dev.key" $sigarg)
        if [ -z "$bin" ] && grep -q "cannot read platform.sig" "${d}/build.log"; then
            echo "  SKIP: verify-sig tool (no platform.sig for this hull)"
        elif [ -z "$bin" ]; then
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

    # (audit 7 L3) A built SERVER binary reserves every resource option it
    # parses: a bare --read-timeout / --workers / --queue-capacity /
    # --drain-timeout used to be honoured (a forwarded --read-timeout 600000
    # holds each idle connection for ten minutes). The refusal happens while
    # the arguments are parsed, before anything listens.
    d=$(mktemp -d)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = { "hull/http-server@1" } })
app.get("/", function(_req, res) res:text("ok") end)
LUA
    bin=$(build_cli "$d")
    if [ -z "$bin" ]; then
        fail "built server: hull build failed ($(tail -3 "${d}/build.log"))"
    else
        for opt in --read-timeout --workers --queue-capacity --drain-timeout; do
            rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" "$opt" 2)
            expect_eq "built server: a bare $opt is refused" "1" "$rc"
            case "$(cat "$HULL_RC_TMP")" in
                *"--hull-${opt#--}"*) pass "built server: refusal names --hull-${opt#--}" ;;
                *) fail "built server: $opt refusal (got '$(cat "$HULL_RC_TMP")')" ;;
            esac
        done
    fi
    rm -rf "$d"

    # (audit 7 caps L2) WAMR loads AOT code from any file that starts with
    # its magic, whatever the name, so hull build checks compute/*.wasm by
    # its bytes: unstamped AOT under a .wasm name (an unpatched wamrc's
    # `-o x.wasm`) has no bytecode to fall back to, and fails the build.
    d=$(mktemp -d)
    mkdir -p "${d}/compute"
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = { "hull/compute@1" } })
app.main(function() return 0 end)
LUA
    # "\0aot", version 7, target-info section (type 0, size 48), no stamp.
    printf '\000aot\007\000\000\000\000\000\000\000\060\000\000\000' > "${d}/compute/x.wasm"
    head -c 112 /dev/zero >> "${d}/compute/x.wasm"
    _cc=""
    hull_is_ape "${HULL_BIN}" && _cc="--compiler cosmocc"
    # shellcheck disable=SC2086
    hull_run "$HULL_RC_TMP" "${HULL_BIN}" build --no-verify-platform --no-aot $_cc \
        -o "${d}/tool" "$d" >/dev/null
    case "$(cat "$HULL_RC_TMP")" in
        *"holds AOT code"*) pass "hull build: unstamped AOT named .wasm is refused" ;;
        *) fail "hull build: AOT-as-.wasm (got '$(tail -3 "$HULL_RC_TMP")')" ;;
    esac
    if [ -f "${d}/tool" ] || [ -f "${d}/tool.com" ]; then
        fail "hull build: produced a binary embedding unstamped AOT"
    else
        pass "hull build: no binary produced"
    fi
    rm -rf "$d"

    # (audit 19 H3) AOT committed in the app tree is native code from
    # whoever wrote the tree; its stamp only says it claims to come from
    # Hull's wamrc. A build embeds only the AOT it compiles itself unless
    # --trust-prebuilt-aot: STAMPED AOT under a .wasm name fails the build,
    # and a committed compute/*.aot.* is left out.
    stamped_aot() {
        # "\0aot", version 7, target-info section (type 0, size 48), then the
        # stamp at offset 40: version 1, magic "HULL" (0x48554C4C, LE).
        printf '\000aot\007\000\000\000\000\000\000\000\060\000\000\000' > "$1"
        head -c 24 /dev/zero >> "$1"
        printf '\001\000\000\000LLUH' >> "$1"
        head -c 80 /dev/zero >> "$1"
    }
    d=$(mktemp -d)
    mkdir -p "${d}/compute"
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = { "hull/compute@1" } })
app.main(function() return 0 end)
LUA
    stamped_aot "${d}/compute/x.wasm"
    # shellcheck disable=SC2086
    hull_run "$HULL_RC_TMP" "${HULL_BIN}" build --no-verify-platform --no-aot $_cc \
        -o "${d}/tool" "$d" >/dev/null
    case "$(cat "$HULL_RC_TMP")" in
        *"--trust-prebuilt-aot"*) pass "hull build: stamped AOT named .wasm is refused" ;;
        *) fail "hull build: stamped AOT-as-.wasm (got '$(tail -3 "$HULL_RC_TMP")')" ;;
    esac
    if [ -f "${d}/tool" ] || [ -f "${d}/tool.com" ]; then
        fail "hull build: embedded committed AOT without --trust-prebuilt-aot"
    else
        pass "hull build: no binary from committed AOT"
    fi
    rm -rf "$d"

    d=$(mktemp -d)
    mkdir -p "${d}/compute"
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = { "hull/compute@1" } })
app.main(function() return 0 end)
LUA
    cp tests/fixtures/compute/echo.wasm "${d}/compute/echo.wasm"
    stamped_aot "${d}/compute/echo.aot.x86_64"
    bin=$(build_cli "$d" --no-aot)
    if grep -q "not embedding 1 committed AOT" "${d}/build.log"; then
        pass "hull build: committed compute/*.aot.* left out"
    else
        fail "hull build: committed AOT (got '$(tail -3 "${d}/build.log")')"
    fi
    [ -n "$bin" ] && pass "hull build: app with committed AOT still builds" \
        || fail "hull build: app with committed AOT failed to build"
    rm -rf "$d"

    # (audit 19) Two files that embed under one name ("x.json.lua" and
    # "x.json" are both "./x.json") fail the build on the default,
    # compiler-free path too: only the C-emitting path checked, and the
    # binary the other built aborted at startup on the VFS order assert.
    d=$(mktemp -d)
    mkdir -p "${d}/lib"
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA
    echo 'return {}' > "${d}/lib/x.json.lua"
    echo '{}' > "${d}/lib/x.json"
    bin=$(build_cli "$d")
    if grep -q "two files embed under the same name" "${d}/build.log"; then
        pass "hull build: duplicate entry names refused"
    else
        fail "hull build: duplicate entry names (got '$(tail -3 "${d}/build.log")')"
    fi
    [ -z "$bin" ] && pass "hull build: no binary with duplicate entry names" \
        || fail "hull build: produced a binary with duplicate entry names"
    rm -rf "$d"

    # (audit 19) A --sign that cannot read platform.sig removes the binary it
    # linked, as every other --sign failure does: it left it unsigned.
    d=$(mktemp -d)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA
    ( cd "$d" && "${hull_abs}" keygen >/dev/null 2>&1 ) || true
    echo 'null' > "${d}/null.sig"
    if [ -f "${d}/developer.key" ]; then
        bin=$(build_cli "$d" --sign "${d}/developer.key" --platform-sig "${d}/null.sig")
        if grep -q "cannot read platform.sig" "${d}/build.log"; then
            pass "hull build --sign: unreadable platform.sig refused"
        else
            fail "hull build --sign: platform.sig (got '$(tail -3 "${d}/build.log")')"
        fi
        [ -z "$bin" ] && pass "hull build --sign: no unsigned binary left behind" \
            || fail "hull build --sign: left an unsigned binary at the output path"
    else
        fail "hull keygen produced no developer.key"
    fi
    rm -rf "$d"

    # (audit 20) An error RAISED inside the signing step - json.decode of a
    # malformed platform.sig - skipped that cleanup and left the unsigned
    # binary behind too; the whole step now runs under pcall.
    d=$(mktemp -d)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA
    ( cd "$d" && "${hull_abs}" keygen >/dev/null 2>&1 ) || true
    printf '{"x":' > "${d}/bad.sig"
    if [ -f "${d}/developer.key" ]; then
        bin=$(build_cli "$d" --sign "${d}/developer.key" --platform-sig "${d}/bad.sig")
        if grep -q "hull build --sign:" "${d}/build.log"; then
            pass "hull build --sign: malformed platform.sig refused"
        else
            fail "hull build --sign: malformed platform.sig (got '$(tail -3 "${d}/build.log")')"
        fi
        [ -z "$bin" ] && pass "hull build --sign: no unsigned binary after a raised error" \
            || fail "hull build --sign: left an unsigned binary after a raised error"
    else
        fail "hull keygen produced no developer.key"
    fi
    rm -rf "$d"

    # (audit 20) find_files skips links below its root but followed the root:
    # an app whose static/ was a symlink had the tree it pointed at embedded
    # and served. Refused now, as the dev server refuses it.
    d=$(mktemp -d)
    mkdir -p "${d}/app" "${d}/elsewhere"
    echo 'not the app' > "${d}/elsewhere/secret.txt"
    cat > "${d}/app/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA
    if ln -s ../elsewhere "${d}/app/static" 2>/dev/null && [ -L "${d}/app/static" ]; then
        bin=$(build_cli "${d}/app")
        if grep -q "is a symbolic link" "${d}/app/build.log"; then
            pass "hull build: symlinked static/ refused"
        else
            fail "hull build: symlinked static/ (got '$(tail -3 "${d}/app/build.log")')"
        fi
        [ -z "$bin" ] && pass "hull build: no binary from a symlinked static/" \
            || fail "hull build: embedded a symlinked static/"
    else
        echo "  SKIP: hull build symlinked static/ (no symlinks here)"
    fi
    rm -rf "$d"

    # (audit 21) A symlinked compute/ was compiled - its sources read and its
    # .wasm written through the link, outside the app - before the build
    # refused the root; `hull compute build` never checked at all.
    d=$(mktemp -d)
    mkdir -p "${d}/app" "${d}/elsewhere/m"
    printf 'int hull_process(void) { return 0; }\n' > "${d}/elsewhere/m/m.c"
    cat > "${d}/app/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA
    if ln -s ../elsewhere "${d}/app/compute" 2>/dev/null && [ -L "${d}/app/compute" ]; then
        bin=$(build_cli "${d}/app")
        if grep -q "is a symbolic link" "${d}/app/build.log"; then
            pass "hull build: symlinked compute/ refused"
        else
            fail "hull build: symlinked compute/ (got '$(tail -3 "${d}/app/build.log")')"
        fi
        [ ! -e "${d}/elsewhere/m.wasm" ] && pass "hull build: nothing compiled through compute/" \
            || fail "hull build: wrote a .wasm through a symlinked compute/"
        ( cd "${d}/app" && "${hull_abs}" compute build > "${d}/cb.log" 2>&1 ) || true
        if grep -q "is a symbolic link" "${d}/cb.log"; then
            pass "hull compute build: symlinked compute/ refused"
        else
            fail "hull compute build: symlinked compute/ (got '$(tail -3 "${d}/cb.log")')"
        fi
    else
        echo "  SKIP: hull build symlinked compute/ (no symlinks here)"
    fi
    rm -rf "$d"

    # (audit 21) --sign ignored a package.sig it could not write (a symlink -
    # tool.write_file does not follow one): exit 0, "wrote package.sig".
    d=$(mktemp -d)
    cat > "${d}/app.lua" <<'LUA'
app.manifest({ modules = {} })
app.main(function() return 0 end)
LUA
    ( cd "$d" && "${hull_abs}" keygen >/dev/null 2>&1 ) || true
    if [ -f "${d}/developer.key" ] &&
       ln -s "${d}/nowhere/package.sig" "${d}/package.sig" 2>/dev/null &&
       [ -L "${d}/package.sig" ]; then
        bin=$(build_cli "$d" --sign "${d}/developer.key")
        if grep -q "cannot read platform.sig" "${d}/build.log"; then
            echo "  SKIP: hull build --sign unwritable package.sig (no platform.sig for this hull)"
        elif grep -q "cannot write" "${d}/build.log"; then
            pass "hull build --sign: an unwritable package.sig fails the build"
        else
            fail "hull build --sign: unwritable package.sig (got '$(tail -3 "${d}/build.log")')"
        fi
        [ -z "$bin" ] && pass "hull build --sign: no unsigned binary when package.sig is not written" \
            || fail "hull build --sign: left a binary without its package.sig"
    else
        echo "  SKIP: hull build --sign unwritable package.sig (no keygen or symlinks here)"
    fi
    rm -rf "$d"
}

run_built_cli

# (audit 31) A --verify-sig run loads no bytecode / compiled template from the
# shared code cache: its entries are sealed under ~/.hull/cache.key, which
# every app process of the user holds, while the signature and the disk gate
# hash SOURCE - a cache hit ran bytecode a sibling app could have forged. Each
# signed binary (an app.main tool on the serve_cli runner, and one declaring
# hull/http-server on serve.c) runs once without --verify-sig (it fills a
# fresh HULL_CACHE_DIR) and once with it (it leaves another empty).
run_verify_sig_no_code_cache() {
    echo "--- --verify-sig reads and writes no code cache (audit 31) ---"
    if ! can_build_cli; then
        echo "  SKIP: no build compiler or platform library for hull build"
        return 0
    fi
    hull_abs=$(cd "$(dirname "${HULL_BIN}")" && pwd)/$(basename "${HULL_BIN}")
    for kind in cli server; do
        d=$(mktemp -d)
        mods='"hull/json@1"'
        [ "$kind" = server ] && mods='"hull/json@1", "hull/http-server@1"'
        cat > "${d}/app.lua" <<LUA
app.manifest({ modules = { ${mods} } })
local json = require("hull.json")
app.main(function() return json.encode({ a = 1 }) == '{"a":1}' and 3 or 4 end)
LUA
        (cd "$d" && "$hull_abs" keygen dev >/dev/null 2>&1)
        if [ ! -f "${d}/dev.key" ]; then
            fail "verify-sig cache ($kind): hull keygen failed"
            rm -rf "$d"; continue
        fi
        sigarg=""
        plat="$(dirname "${HULL_BIN}")/libhull_platform.a"
        if [ -f "$plat" ]; then
            cp "$plat" "${d}/"
            cp "$(dirname "${HULL_BIN}")/platform_canary_hash" "${d}/" 2>/dev/null || true
            "$hull_abs" sign-platform --dir "${d}/" "${d}/dev" >/dev/null 2>&1 \
                && sigarg="--platform-sig ${d}/platform.sig"
        fi
        # shellcheck disable=SC2086
        bin=$(build_cli "$d" --sign "${d}/dev.key" $sigarg)
        if [ -z "$bin" ] && grep -q "cannot read platform.sig" "${d}/build.log"; then
            echo "  SKIP: verify-sig cache ($kind) (no platform.sig for this hull)"
            rm -rf "$d"; continue
        elif [ -z "$bin" ]; then
            fail "verify-sig cache ($kind): hull build failed ($(tail -3 "${d}/build.log"))"
            rm -rf "$d"; continue
        fi
        # HOME outside the app directory: an app directory above $HOME is
        # refused by the sandbox.
        h=$(mktemp -d)
        mkdir -p "${d}/cache_plain" "${d}/cache_signed"
        _saved_home=$HOME
        HOME="$h"; export HOME
        HULL_CACHE_DIR="${d}/cache_plain"; export HULL_CACHE_DIR
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin")
        expect_eq "verify-sig cache ($kind): unsigned-check run exits 3" "3" "$rc"
        HULL_CACHE_DIR="${d}/cache_signed"; export HULL_CACHE_DIR
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" --verify-sig "${d}/dev.pub" --hull-no-verify-platform)
        expect_eq "verify-sig cache ($kind): --verify-sig run exits 3" "3" "$rc"
        unset HULL_CACHE_DIR
        HOME=$_saved_home; export HOME
        n_plain=$(find "${d}/cache_plain" -type f | wc -l | tr -d ' ')
        n_signed=$(find "${d}/cache_signed" -type f | wc -l | tr -d ' ')
        if [ "$n_plain" -gt 0 ]; then
            pass "verify-sig cache ($kind): a plain run fills the code cache ($n_plain entries)"
        else
            fail "verify-sig cache ($kind): a plain run wrote no cache entry (test cannot tell)"
        fi
        expect_eq "verify-sig cache ($kind): a --verify-sig run writes no cache entry" "0" "$n_signed"
        # Seed the signed run's cache with the plain run's entries, each one
        # spoiled: a run that READS an entry finds its seal broken, deletes it
        # ("not ours: drop it") and writes a fresh one, so a tree whose bytes
        # are unchanged afterwards was never read.
        rm -rf "${d}/cache_signed"
        cp -R "${d}/cache_plain" "${d}/cache_signed"
        find "${d}/cache_signed" -type f -exec sh -c 'printf x >> "$1"' _ {} \;
        sum_before=$(cd "${d}/cache_signed" && find . -type f -exec cksum {} + | sort)
        HOME="$h"; export HOME
        HULL_CACHE_DIR="${d}/cache_signed"; export HULL_CACHE_DIR
        rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" --verify-sig "${d}/dev.pub" --hull-no-verify-platform)
        unset HULL_CACHE_DIR
        HOME=$_saved_home; export HOME
        expect_eq "verify-sig cache ($kind): --verify-sig run over a filled cache exits 3" "3" "$rc"
        sum_after=$(cd "${d}/cache_signed" && find . -type f -exec cksum {} + | sort)
        if [ -n "$sum_before" ] && [ "$sum_before" = "$sum_after" ]; then
            pass "verify-sig cache ($kind): a --verify-sig run reads no cache entry"
        else
            fail "verify-sig cache ($kind): a --verify-sig run read (and replaced) cache entries"
        fi
        rm -rf "$d" "$h"
    done
}

run_verify_sig_no_code_cache

# Audit 33: a require with no Lua caller frame - require as a coroutine's
# body - was taken for the runtime's own preload: neither tracked by the
# pre-manifest import tracker nor refused, so an app with an empty modules list
# loaded hull.db (and queried the database). It must fail app load like any
# other undeclared top-level import.
run_require_no_frame() {
    echo "--- require with no caller frame (lua) ---"
    d=$(mktemp -d "${TMPDIR:-/tmp}/hull_a33req.XXXXXX")
    case "$HULL_BIN" in
        /*) bin="$HULL_BIN" ;;
        *)  bin="$(pwd)/$HULL_BIN" ;;
    esac
    for shape in wrap create pcall; do
        case "$shape" in
            wrap)   line='local m = coroutine.wrap(require)("hull.db")' ;;
            create) line='local _, m = coroutine.resume(coroutine.create(require), "hull.db")' ;;
            pcall)  line='local _, m = pcall(coroutine.wrap(require), "hull.db")' ;;
        esac
        for decl in none db; do
            if [ "$decl" = db ]; then mods='"hull/db@1"'; else mods=''; fi
            cat > "${d}/app.lua" <<EOF
local app = require("hull.app")
${line}
app.manifest({ modules = { ${mods} } })
app.main(function(ctx)
    ctx.stdout:write("A33_RAN " .. type(m) .. "\n")
    return 0
end)
EOF
            rc=$(cd "$d" && hull_run "$HULL_RC_TMP" "$bin" run app.lua)
            out=$(cat "$HULL_RC_TMP")
            if [ "$decl" = db ]; then
                expect_eq "require no-frame ($shape): declared hull/db runs" "0" "$rc"
                case "$out" in
                    *"A33_RAN table"*) pass "require no-frame ($shape): declared module loads" ;;
                    *) fail "require no-frame ($shape): declared module did not load (got '$out')" ;;
                esac
            else
                if [ "$rc" = "1" ]; then
                    pass "require no-frame ($shape): undeclared hull/db fails app load (exit $rc)"
                else
                    fail "require no-frame ($shape): undeclared hull/db (expected exit 1, got $rc)"
                fi
                case "$out" in
                    *A33_RAN*) fail "require no-frame ($shape): app.main ran" ;;
                    *"hull/db"*"not declared"*) pass "require no-frame ($shape): names hull/db" ;;
                    *) fail "require no-frame ($shape): message (got '$out')" ;;
                esac
            fi
        done
    done
    rm -rf "$d"
}

run_require_no_frame

echo
echo "${PASS}/$((PASS + FAIL)) CLI e2e tests passed"
[ "${FAIL}" -eq 0 ]
