#!/bin/sh
# e2e_exec.sh: hull/exec PR 1 (docs/design/exec_broker.md) - the manifest's
# `exec` section, the argument matcher and the hull test stubs, both runtimes.
#
# Under hull test a declared command is answered only by a test.exec_stub /
# test.execStub stub, after the full match. Checked here:
#   - under hull test: a stub answers, its output cut to stdout_max; a stub
#     function gets a copy of the arguments; option injection, a byte outside
#     the slot's class, too few / too many values and an undeclared name are
#     refused before the stub; a call inside a transaction is refused coded
#     wait_refused; a removed stub, or one for an undeclared name, is refused;
#   - at load: an unusable section refuses the app (sha256, an unknown key, a
#     typo inside a slot, a non-integer, a bad name, `repeat` on a non-last
#     entry, fs.write outside /dev /run /tmp), and hull/exec needs a section;
#   - outside hull test: on Linux the broker runs the command; elsewhere the
#     app is refused at start, or with --allow-degraded-sandbox exec.run
#     fails.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; [ -n "$2" ] && printf '%s\n' "$2" | sed 's/^/        /'; }

TMPDIR=$(mktemp -d)
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

# ── hull test with stubs ──────────────────────────────────────────────

mkdir -p "$TMPDIR/lua/tests" "$TMPDIR/js/tests"
cat > "$TMPDIR/lua/app.lua" <<'EOF'
app.manifest({
  modules = { "hull/exec@1", "hull/http-server@1", "hull/db@1" },
  exec = {
    ["unit-state"] = {
      path = "/usr/bin/systemctl",
      argv = { "show", "--no-pager", "-p", "ActiveState,NRestarts", "--",
               { prefix = "hs-", suffix = ".service", chars = "a-z0-9-", max = 64,
                 ["repeat"] = { min = 1, max = 3 } } },
      unix_sockets = true,
      timeout_ms = 5000, stdout_max = 16,
    },
    ["gpu-query"] = {
      path = "/usr/bin/nvidia-smi",
      argv = { "--query-gpu=index,name", "--format=csv,noheader,nounits" },
      fs = { read = { "/dev/nvidiactl" }, write = { "/dev/nvidiactl" } },
    },
  },
})
local exec = require("hull.exec")
app.get("/state", function(req, res)
  local r = exec.run("unit-state", { req.query.unit })
  res:json({ status = r.status, out = r.stdout, cut = r.stdout_truncated })
end)
EOF
cat > "$TMPDIR/lua/tests/test_exec.lua" <<'EOF'
local exec = require("hull.exec")
local seen
test.exec_stub("unit-state", function(args, name)
  seen = args
  args[1] = "changed"
  return { stdout = "ActiveState=active " .. name, duration_ms = 3 }
end)
test.exec_stub("gpu-query", { stdout = "0, NVIDIA A100\n", status = 0 })

local function refused(fn, pat)
  local ok, err = pcall(fn)
  test.eq(ok, false)
  test.ok(tostring(err):find(pat, 1, true) ~= nil, tostring(err))
end

test("a handler's call is answered by the stub, cut to stdout_max", function()
  local r = test.get("/state?unit=hs-api.service")
  test.eq(r.status, 200)
  test.eq(r.json.cut, true)
  test.eq(#r.json.out, 16)
end)
test("a stub function gets a copy of the arguments", function()
  local mine = { "hs-a.service", "hs-b.service" }
  local r = exec.run("unit-state", mine)
  test.eq(seen[2], "hs-b.service")
  test.eq(mine[1], "hs-a.service")
  test.eq(r.duration_ms, 3)
  test.eq(r.timed_out, false)
  test.eq(r.signal, nil)
end)
test("a fixed command takes no argument", function()
  local r = exec.run("gpu-query")
  test.eq(r.stdout, "0, NVIDIA A100\n")
  test.eq(r.stdout_truncated, false)
  refused(function() exec.run("gpu-query", { "x" }) end, "takes 0 arguments")
end)
test("refused before the stub", function()
  refused(function() exec.run("unit-state", { "--host=evil" }) end, "starts with '-'")
  refused(function() exec.run("unit-state", { "hs-API.service" }) end, "outside the slot's chars")
  refused(function() exec.run("unit-state", { "sshd.service" }) end, "prefix")
  refused(function() exec.run("unit-state", {}) end, "takes 1..3 arguments")
  refused(function() exec.run("unit-state", { "hs-a.service", "hs-b.service",
                                              "hs-c.service", "hs-d.service" }) end, "takes 1..3")
  refused(function() exec.run("unit-state", { 42 }) end, "must be a string")
  refused(function() exec.run("nope") end, "not declared")
end)
test("a call inside a transaction is refused coded wait_refused", function()
  local conn = require("hull.db").default()
  conn.exec("BEGIN")
  local ok, err = pcall(exec.run, "gpu-query")
  conn.exec("ROLLBACK")
  test.eq(ok, false)
  test.ok(tostring(err):find("[hull:wait_refused]", 1, true) == 1, tostring(err))
end)
test("a removed stub, or one for an undeclared name, is refused", function()
  test.exec_stub("gpu-query", nil)
  refused(function() exec.run("gpu-query") end, "no stub for 'gpu-query'")
  refused(function() test.exec_stub("nope", {}) end, "not declared")
end)
EOF

cat > "$TMPDIR/js/app.js" <<'EOF'
import { app } from "hull:app";
import { exec } from "hull:exec";
app.manifest({
  modules: ["hull/exec@1", "hull/http-server@1", "hull/db@1"],
  exec: {
    "unit-state": {
      path: "/usr/bin/systemctl",
      argv: ["show", "--no-pager", "-p", "ActiveState,NRestarts", "--",
             { prefix: "hs-", suffix: ".service", chars: "a-z0-9-", max: 64,
               repeat: { min: 1, max: 3 } }],
      unixSockets: true, timeoutMs: 5000, stdoutMax: 16,
    },
    "gpu-query": {
      path: "/usr/bin/nvidia-smi",
      argv: ["--query-gpu=index,name", "--format=csv,noheader,nounits"],
      fs: { read: ["/dev/nvidiactl"], write: ["/dev/nvidiactl"] },
    },
  },
});
app.get("/state", async (req, res) => {
  const r = await exec.run("unit-state", [req.query.unit]);
  res.json({ status: r.status, out: r.stdout, cut: r.stdoutTruncated });
});
EOF
cat > "$TMPDIR/js/tests/test_exec.js" <<'EOF'
import { exec } from "hull:exec";
import { db } from "hull:db";
let seen;
test.execStub("unit-state", (args, name) => {
  seen = args;
  args[0] = "changed";
  return { stdout: "ActiveState=active " + name, durationMs: 3 };
});
test.execStub("gpu-query", { stdout: "0, NVIDIA A100\n", status: 0 });

function refused(fn, pat) {
  let msg = "";
  try { fn(); } catch (e) { msg = String(e); }
  test.ok(msg.includes(pat), msg || "(no error)");
}

test("a stub answers, cut to stdoutMax", async () => {
  const r = await exec.run("unit-state", ["hs-api.service"]);
  test.eq(r.stdoutTruncated, true);
  test.eq(r.stdout.length, 16);
  test.eq(r.stdoutBytes.byteLength, 16);
});
test("a stub function gets a copy of the arguments", async () => {
  const mine = ["hs-a.service", "hs-b.service"];
  const r = await exec.run("unit-state", mine);
  test.eq(seen[1], "hs-b.service");
  test.eq(mine[0], "hs-a.service");
  test.eq(r.durationMs, 3);
  test.eq(r.timedOut, false);
  test.eq(r.signal, null);
});
test("a fixed command takes no argument", async () => {
  const r = await exec.run("gpu-query");
  test.eq(r.stdout, "0, NVIDIA A100\n");
  test.eq(r.stdoutTruncated, false);
  refused(() => exec.run("gpu-query", ["x"]), "takes 0 arguments");
});
test("refused before the stub", () => {
  refused(() => exec.run("unit-state", ["--host=evil"]), "starts with '-'");
  refused(() => exec.run("unit-state", ["hs-API.service"]), "outside the slot's chars");
  refused(() => exec.run("unit-state", ["sshd.service"]), "prefix");
  refused(() => exec.run("unit-state", []), "takes 1..3 arguments");
  refused(() => exec.run("unit-state", ["hs-a.service", "hs-b.service",
                                        "hs-c.service", "hs-d.service"]), "takes 1..3");
  refused(() => exec.run("unit-state", [42]), "must be a string");
  refused(() => exec.run("nope"), "not declared");
});
test("a call inside a transaction is refused coded wait_refused", () => {
  const conn = db.default();
  conn.exec("BEGIN");
  let code = "";
  try { exec.run("gpu-query"); } catch (e) { code = e.code; }
  conn.exec("ROLLBACK");
  test.eq(code, "wait_refused");
});
test("a removed stub, or one for an undeclared name, is refused", () => {
  test.execStub("gpu-query", null);
  refused(() => exec.run("gpu-query"), "no stub for 'gpu-query'");
  refused(() => test.execStub("nope", {}), "not declared");
});
EOF

for rt in lua js; do
    echo "== hull test ($rt) =="
    out=$(cd "$TMPDIR/$rt" && "$HULL" test . 2>&1) && rc=0 || rc=$?
    if [ "$rc" = 0 ] && printf '%s\n' "$out" | grep -q "6/6 tests passed"; then
        pass "$rt: stubs, matching, refusals (6 cases)"
    else
        fail "$rt: hull test" "$out"
    fi
done

# ── Load-time refusals ────────────────────────────────────────────────

# refuse <label> <expected text> <lua exec section> <js exec section>
refuse() {
    label=$1 want=$2 lua_sec=$3 js_sec=$4
    d="$TMPDIR/refuse_lua"
    rm -rf "$d"; mkdir -p "$d"
    printf 'app.manifest({ modules = { "hull/exec@1" }, %s })\napp.main(function() return 0 end)\n' \
        "$lua_sec" > "$d/app.lua"
    out=$(cd "$d" && "$HULL" app.lua 2>&1) && rc=0 || rc=$?
    if [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -qF -- "$want"; then
        pass "lua: $label"
    else
        fail "lua: $label (rc=$rc)" "$out"
    fi
    d="$TMPDIR/refuse_js"
    rm -rf "$d"; mkdir -p "$d"
    printf 'import { app } from "hull:app";\napp.manifest({ modules: ["hull/exec@1"], %s });\napp.main(() => 0);\n' \
        "$js_sec" > "$d/app.js"
    out=$(cd "$d" && "$HULL" app.js 2>&1) && rc=0 || rc=$?
    if [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -qF -- "$want"; then
        pass "js: $label"
    else
        fail "js: $label (rc=$rc)" "$out"
    fi
}

echo "== load-time refusals =="
refuse "sha256 is refused until it is enforced" "sha256 is not supported yet" \
    'exec = { a = { path = "/bin/true", sha256 = "00" } }' \
    'exec: { a: { path: "/bin/true", sha256: "00" } }'
refuse "an unknown command key" "unknown key 'timeout'" \
    'exec = { a = { path = "/bin/true", timeout = 5 } }' \
    'exec: { a: { path: "/bin/true", timeout: 5 } }'
refuse "an unknown slot key" "unknown key 'max_len'" \
    'exec = { a = { path = "/bin/true", argv = { { max_len = 5 } } } }' \
    'exec: { a: { path: "/bin/true", argv: [{ max_len: 5 }] } }'
refuse "a non-integer limit" "must be an integer" \
    'exec = { a = { path = "/bin/true", timeout_ms = "5000" } }' \
    'exec: { a: { path: "/bin/true", timeoutMs: 5000.5 } }'
refuse "a timeout out of range" "timeout_ms must be 100..60000" \
    'exec = { a = { path = "/bin/true", timeout_ms = 99 } }' \
    'exec: { a: { path: "/bin/true", timeoutMs: 99 } }'
refuse "a bad command name" "is not a command name" \
    'exec = { ["Bad_Name"] = { path = "/bin/true" } }' \
    'exec: { "Bad_Name": { path: "/bin/true" } }'
refuse "a relative path" "absolute path" \
    'exec = { a = { path = "bin/true" } }' \
    'exec: { a: { path: "bin/true" } }'
refuse "repeat on a non-last entry" "only the last argv entry may repeat" \
    'exec = { a = { path = "/bin/true", argv = { { ["repeat"] = { max = 2 } }, "x" } } }' \
    'exec: { a: { path: "/bin/true", argv: [{ repeat: { max: 2 } }, "x"] } }'
refuse "fs.write outside /dev, /run, /tmp" "must be under /dev, /run or /tmp" \
    'exec = { a = { path = "/bin/true", fs = { write = { "/etc/x" } } } }' \
    'exec: { a: { path: "/bin/true", fs: { write: ["/etc/x"] } } }'
refuse "a refused env name" "env may not set LD_PRELOAD" \
    'exec = { a = { path = "/bin/true", env = { LD_PRELOAD = "/x.so" } } }' \
    'exec: { a: { path: "/bin/true", env: { LD_PRELOAD: "/x.so" } } }'
refuse "hull/exec without an exec section" "requires an \`exec\` section" "" ""

# ── Outside hull test ─────────────────────────────────────────────────

# On Linux the command broker runs the declared command (tests/
# e2e_exec_broker.sh covers it in depth); on a host without one the app is
# refused at start, and with --allow-degraded-sandbox exec.run fails.
echo "== outside hull test"
mkdir -p "$TMPDIR/run_lua" "$TMPDIR/run_js"
cat > "$TMPDIR/run_lua/app.lua" <<'APP'
app.manifest({ modules = { "hull/exec@1" }, exec = { a = { path = "/bin/true" } } })
local exec = require("hull.exec")
app.main(function()
  local ok, r = pcall(exec.run, "a")
  if ok then return r.status == 0 and 0 or 5 end
  return tostring(r):find("broker is not running", 1, true) and 7 or 4
end)
APP
cat > "$TMPDIR/run_js/app.js" <<'APP'
import { app } from "hull:app";
import { exec } from "hull:exec";
app.manifest({ modules: ["hull/exec@1"], exec: { a: { path: "/bin/true" } } });
app.main(async () => {
  try { const r = await exec.run("a"); return r.status === 0 ? 0 : 5; }
  catch (e) { return String(e).includes("broker is not running") ? 7 : 4; }
});
APP
for rt in lua js; do
    entry="app.$rt"
    if [ "$(uname -s)" = Linux ]; then
        flags=""
        abi=$(python3 -c 'import ctypes; print(ctypes.CDLL(None, use_errno=True).syscall(444, None, 0, 1))' 2>/dev/null || echo 0)
        [ "$abi" -lt 6 ] 2>/dev/null && flags="--allow-degraded-sandbox"
        rc=0; out=$(cd "$TMPDIR/run_$rt" && "$HULL" $flags "$entry" 2>&1) || rc=$?
        if [ "$rc" = 0 ]; then pass "$rt: the broker runs a declared command"
        else fail "$rt: the broker runs a declared command (rc=$rc)" "$out"; fi
    else
        rc=0; out=$(cd "$TMPDIR/run_$rt" && "$HULL" "$entry" 2>&1) || rc=$?
        if [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -qF "has no command broker"; then
            pass "$rt: no broker on this host: refused at start"
        else fail "$rt: no broker on this host: refused at start (rc=$rc)" "$out"; fi
        rc=0; out=$(cd "$TMPDIR/run_$rt" && "$HULL" --allow-degraded-sandbox "$entry" 2>&1) || rc=$?
        if [ "$rc" = 7 ]; then pass "$rt: --allow-degraded-sandbox starts, exec.run fails"
        else fail "$rt: --allow-degraded-sandbox starts, exec.run fails (rc=$rc)" "$out"; fi
    fi
done

echo
echo "e2e_exec: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
