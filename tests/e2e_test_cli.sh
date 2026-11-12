#!/bin/sh
# e2e_test_cli.sh - `hull test` for CLI apps (app.main, no routes):
# test.run_main (Lua) / test.runMain (JS) run the app's main in-process with
# ctx.args / stdin / env given by the test and ctx.stdout / stderr captured.
#
#   1. `hull new --type cli` and `hull new --cli` scaffolds pass `hull test`
#      as generated (both runtimes).
#   2. A failing case is reported FAIL and `hull test` exits non-zero;
#      `hull agent test` reports it as "status":"fail" in its JSON.
#   3. main's waits (hull.sleep), stdin, env (manifest-gated), a raised
#      error, a timeout and repeated invocations - both runtimes.
#   4. An app with routes AND app.main gets test.get and run_main; an app
#      with neither is a clear error.
#   5. An app named by a RELATIVE directory (`hull test app`, `hull test ..`)
#      keeps its relative imports working - both runtimes.
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

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# `hull test` is run from inside the app directory.
run_test() { (cd "$1" && "$HULL" test 2>&1); }

echo "=== scaffolds pass hull test ==="
cd "$TMP"
"$HULL" new --type cli mlua >/dev/null 2>&1
"$HULL" new --type cli --runtime js mjs >/dev/null 2>&1
"$HULL" new --cli flua >/dev/null 2>&1
"$HULL" new --cli --runtime js fjs >/dev/null 2>&1
for d in mlua mjs flua fjs; do
    if out=$(run_test "$TMP/$d"); then
        if echo "$out" | grep -q "tests passed" && ! echo "$out" | grep -q "FAIL"; then
            pass "$d scaffold: hull test passes"
        else
            fail "$d scaffold: unexpected output" "$out"
        fi
    else
        fail "$d scaffold: hull test exited non-zero" "$out"
    fi
done

echo "=== a failing case is FAIL ==="
cat > "$TMP/mlua/tests/commands/test_broken.lua" <<'LUA'
test("deliberately wrong exit code", function()
    test.eq(test.run_main({ "greet", "x" }).code, 7)
end)
LUA
cat > "$TMP/mjs/tests/commands/test_broken.js" <<'JS'
test("deliberately wrong exit code", () => {
    test.eq(test.runMain(["greet", "x"]).code, 7);
});
JS
for d in mlua mjs; do
    if out=$(run_test "$TMP/$d"); then
        fail "$d: hull test passed with a failing case" "$out"
    else
        if echo "$out" | grep -q "FAIL  deliberately wrong exit code"; then
            pass "$d: failing case reported FAIL, exit non-zero"
        else
            fail "$d: no FAIL line" "$out"
        fi
    fi
    json=$(cd "$TMP/$d" && "$HULL" agent test . 2>/dev/null || true)
    if echo "$json" | grep -q '"name":"deliberately wrong exit code","status":"fail"' &&
       echo "$json" | grep -q '"failed":1'; then
        pass "$d: hull agent test reports the failure"
    else
        fail "$d: hull agent test JSON" "$json"
    fi
done

echo "=== waits, stdin, env, errors, timeouts ==="
mkdir -p "$TMP/alua/tests" "$TMP/ajs/tests"
cat > "$TMP/alua/app.lua" <<'LUA'
app.manifest({ env = { "GREETING" } })
app.main(function(ctx)
    local cmd = ctx.args[1]
    if cmd == "sleep" then
        hull.sleep(20)
        ctx.stdout:write("slept\n")
        return 0
    elseif cmd == "cat" then
        local first = ctx.stdin:read("*l")
        local rest = ctx.stdin:read("*a")
        ctx.stdout:write("[" .. tostring(first) .. "][" .. rest .. "]")
        return 0
    elseif cmd == "env" then
        ctx.stdout:write(tostring(ctx.env.GREETING))
        return 0
    elseif cmd == "boom" then
        ctx.stdout:write("partial")
        error("kaboom")
    elseif cmd == "hang" then
        hull.sleep(60000)
    end
    return 0
end)
LUA
cat > "$TMP/alua/tests/test_main.lua" <<'LUA'
test("waits on hull.sleep", function()
    local r = test.run_main({ "sleep" })
    test.eq(r.code, 0)
    test.eq(r.stdout, "slept\n")
end)
test("runs main again", function()
    test.eq(test.run_main({ "sleep" }).stdout, "slept\n")
    test.eq(test.run_main({ "sleep" }).stdout, "slept\n")
end)
test("reads stdin", function()
    test.eq(test.run_main({ "cat" }, { stdin = "one\ntwo\n" }).stdout, "[one][two\n]")
end)
test("env is opts.env only", function()
    test.eq(test.run_main({ "env" }, { env = { GREETING = "hi" } }).stdout, "hi")
    test.eq(test.run_main({ "env" }).stdout, "nil")
end)
test("env outside the manifest is refused", function()
    test.err(function() test.run_main({ "env" }, { env = { PATH = "/" } }) end,
             "not in the manifest")
end)
test("a raised error is code 1 with the message on stderr", function()
    local r = test.run_main({ "boom" })
    test.eq(r.code, 1)
    test.eq(r.stdout, "partial")
    test.ok(r.stderr:find("kaboom", 1, true), r.stderr)
end)
test("a main that does not finish times out", function()
    test.err(function() test.run_main({ "hang" }, { timeout_ms = 100 }) end,
             "did not finish")
end)
test("the next run is unaffected", function()
    test.eq(test.run_main({ "sleep" }).code, 0)
end)
LUA
cat > "$TMP/ajs/app.js" <<'JS'
import { app } from "hull:app";
app.manifest({ env: ["GREETING"] });
app.main(async (ctx) => {
    const cmd = ctx.args[0];
    if (cmd === "sleep") {
        await hull.sleep(20);
        ctx.stdout.write("slept\n");
        return 0;
    } else if (cmd === "cat") {
        const first = ctx.stdin.read("l");
        const rest = ctx.stdin.read("a");
        ctx.stdout.write(`[${first}][${rest}]`);
        return 0;
    } else if (cmd === "env") {
        ctx.stdout.write(String(ctx.env.GREETING));
        return 0;
    } else if (cmd === "boom") {
        ctx.stdout.write("partial");
        throw new Error("kaboom");
    } else if (cmd === "hang") {
        await hull.sleep(60000);
    }
    return 0;
});
JS
cat > "$TMP/ajs/tests/test_main.js" <<'JS'
test("waits on hull.sleep", () => {
    const r = test.runMain(["sleep"]);
    test.eq(r.code, 0);
    test.eq(r.stdout, "slept\n");
});
test("runs main again", () => {
    test.eq(test.runMain(["sleep"]).stdout, "slept\n");
    test.eq(test.runMain(["sleep"]).stdout, "slept\n");
});
test("reads stdin", () => {
    test.eq(test.runMain(["cat"], { stdin: "one\ntwo\n" }).stdout, "[one][two\n]");
});
test("env is opts.env only", () => {
    test.eq(test.runMain(["env"], { env: { GREETING: "hi" } }).stdout, "hi");
    test.eq(test.runMain(["env"]).stdout, "undefined");
});
test("env outside the manifest is refused", () => {
    test.err(() => test.runMain(["env"], { env: { PATH: "/" } }), "not in the manifest");
});
test("a raised error is code 1 with the message on stderr", () => {
    const r = test.runMain(["boom"]);
    test.eq(r.code, 1);
    test.eq(r.stdout, "partial");
    test.ok(r.stderr.includes("kaboom"), r.stderr);
});
test("a main that does not finish times out", () => {
    test.err(() => test.runMain(["hang"], { timeoutMs: 100 }), "did not finish");
});
test("the next run is unaffected", () => {
    test.eq(test.runMain(["sleep"]).code, 0);
});
test("an async test body can call it", async () => {
    const r = test.runMain(["sleep"]);
    await hull.sleep(5);
    test.eq(r.stdout, "slept\n");
});
JS
for d in alua ajs; do
    if out=$(run_test "$TMP/$d"); then
        pass "$d: waits / stdin / env / errors / timeouts"
    else
        fail "$d" "$out"
    fi
done

echo "=== routes and app.main together; neither ==="
mkdir -p "$TMP/both/tests" "$TMP/none/tests"
cat > "$TMP/both/app.lua" <<'LUA'
app.manifest({ modules = { "hull/http-server@1" } })
app.get("/ping", function(req, res) res:text("pong") end)
app.main(function(ctx)
    ctx.stdout:write("warm\n")
    return 0
end)
LUA
cat > "$TMP/both/tests/test_both.lua" <<'LUA'
test("test.get works", function()
    test.eq(test.get("/ping").body, "pong")
end)
test("test.run_main works", function()
    test.eq(test.run_main({}).stdout, "warm\n")
end)
LUA
if out=$(run_test "$TMP/both"); then
    pass "routes + app.main: both helpers work"
else
    fail "routes + app.main" "$out"
fi
printf 'app.manifest({})\n' > "$TMP/none/app.lua"
printf 'test("x", function() end)\n' > "$TMP/none/tests/test_x.lua"
if out=$(run_test "$TMP/none"); then
    fail "no routes and no app.main: hull test passed" "$out"
elif echo "$out" | grep -q "no routes or app.main registered"; then
    pass "no routes and no app.main: clear error"
else
    fail "no routes and no app.main: message" "$out"
fi

echo "=== relative app directory keeps relative imports ==="
mkdir -p "$TMP/rel/js/lib" "$TMP/rel/js/tests" "$TMP/rel/lua/lib" "$TMP/rel/lua/tests"
cat > "$TMP/rel/js/app.js" <<'JS'
import { app } from "hull:app";
import { greeting } from "./lib/a.js";
app.manifest({});
app.main((ctx) => { ctx.stdout.write(greeting + "\n"); return 0; });
JS
printf 'export const greeting = "hi";\n' > "$TMP/rel/js/lib/a.js"
cat > "$TMP/rel/js/tests/test_rel.js" <<'JS'
test("relative import", async () => {
    test.eq((await test.runMain([])).stdout, "hi\n");
});
JS
cat > "$TMP/rel/lua/app.lua" <<'LUA'
local a = require("./lib/a")
app.manifest({})
app.main(function(ctx) ctx.stdout:write(a.greeting .. "\n"); return 0 end)
LUA
printf 'return { greeting = "hi" }\n' > "$TMP/rel/lua/lib/a.lua"
cat > "$TMP/rel/lua/tests/test_rel.lua" <<'LUA'
test("relative require", function()
    test.eq(test.run_main({}).stdout, "hi\n")
end)
LUA
# Each case: the directory hull test runs from, then the app argument.
rel_case() {
    if out=$(cd "$TMP/$1" && "$HULL" test "$2" 2>&1); then
        pass "hull test $2 (from $1)"
    else
        fail "hull test $2 (from $1)" "$out"
    fi
}
for rt in js lua; do
    rel_case rel "$rt"
    rel_case "rel/$rt/tests" ..
    rel_case "rel/$rt/lib" "../../$rt"
done

echo ""
echo "e2e_test_cli: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
