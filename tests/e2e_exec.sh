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
#   - at load: an unusable section refuses the app (a malformed sha256 pin, an unknown key, a
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
  fs = { read = { "data/" } },
  exec = {
    ["unit-state"] = {
      path = "/usr/bin/systemctl",
      argv = { "show", "--no-pager", "-p", "ActiveState,NRestarts", "--",
               { prefix = "hs-", suffix = ".service", chars = "a-z0-9-", max = 64,
                 ["repeat"] = { min = 1, max = 3 } } },
      host = { unix_sockets = true },
      timeout_ms = 5000, stdout_max = 16,
    },
    ["gpu-query"] = {
      path = "/usr/bin/nvidia-smi",
      argv = { "--query-gpu=index,name", "--format=csv,noheader,nounits" },
      host = { read = { "/dev/nvidiactl" }, write = { "/dev/nvidiactl" } },
    },
    ["key-import"] = { path = "/bin/cat", stdin_max = 8 },
    -- the app's own grant, in identical syntax, and a pin on the entry
    report = {
      path = "/bin/cat",
      sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      manifest = { fs = { read = { "data/" } } },
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
test.exec_stub("report", { stdout = "rows" })
test("a command with a child manifest and a pin loads and runs", function()
  test.eq(exec.run("report").stdout, "rows")
end)

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
  refused(function() exec.run("unit-state", { string.rep("a", 70000) }) end,
          "longer than 65536 bytes")
end)
test("a call inside a transaction is refused coded wait_refused", function()
  local conn = require("hull.db").default()
  conn.exec("BEGIN")
  local ok, err = pcall(exec.run, "gpu-query")
  conn.exec("ROLLBACK")
  test.eq(ok, false)
  test.ok(tostring(err):find("[hull:wait_refused]", 1, true) == 1, tostring(err))
end)
test("stdin reaches the stub, within the command's stdin_max", function()
  local got
  test.exec_stub("key-import", function(args, name, stdin)
    got = stdin
    return { stdout = stdin }
  end)
  test.eq(exec.run("key-import", {}, { stdin = "a\0b" }).stdout, "a\0b")
  test.eq(got, "a\0b")
  exec.run("key-import")
  test.eq(got, nil)
  refused(function() exec.run("key-import", {}, { stdin = "123456789" }) end,
          "stdin is 9 bytes; the command's stdin_max is 8")
  refused(function() exec.run("gpu-query", {}, { stdin = "x" }) end, "takes no stdin")
  refused(function() exec.run("key-import", {}, { stdn = "x" }) end, "unknown option")
  refused(function() exec.run("key-import", {}, { stdin = 5 }) end, "stdin must be a string")
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
  fs: { read: ["data/"] },
  exec: {
    "unit-state": {
      path: "/usr/bin/systemctl",
      argv: ["show", "--no-pager", "-p", "ActiveState,NRestarts", "--",
             { prefix: "hs-", suffix: ".service", chars: "a-z0-9-", max: 64,
               repeat: { min: 1, max: 3 } }],
      host: { unixSockets: true }, timeoutMs: 5000, stdoutMax: 16,
    },
    "gpu-query": {
      path: "/usr/bin/nvidia-smi",
      argv: ["--query-gpu=index,name", "--format=csv,noheader,nounits"],
      host: { read: ["/dev/nvidiactl"], write: ["/dev/nvidiactl"] },
    },
    "key-import": { path: "/bin/cat", stdinMax: 8 },
    // the app's own grant, in identical syntax, and a pin on the entry
    report: {
      path: "/bin/cat",
      sha256: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      manifest: { fs: { read: ["data/"] } },
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
test.execStub("report", { stdout: "rows" });
test("a command with a child manifest and a pin loads and runs", async () => {
  test.eq((await exec.run("report")).stdout, "rows");
});

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
test("stdin reaches the stub, within the command's stdinMax", async () => {
  let got;
  test.execStub("key-import", (args, name, stdin) => {
    got = stdin;
    return { stdout: stdin };
  });
  const r = await exec.run("key-import", [], { stdin: new Uint8Array([97, 0, 255]) });
  test.eq(Array.from(new Uint8Array(r.stdoutBytes)).join(","), "97,0,255");
  test.eq(got instanceof ArrayBuffer, true);
  await exec.run("key-import", [], { stdin: "é" });       /* UTF-8: 2 bytes */
  test.eq(got.byteLength, 2);
  await exec.run("key-import");
  test.eq(got, undefined);
  refused(() => exec.run("key-import", [], { stdin: "123456789" }),
          "stdin is 9 bytes; the command's stdinMax is 8");
  refused(() => exec.run("gpu-query", [], { stdin: "x" }), "takes no stdin");
  refused(() => exec.run("key-import", [], { stdn: "x" }), "unknown option");
  refused(() => exec.run("key-import", [], { stdin: 5 }), "stdin must be a string");
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
    if [ "$rc" = 0 ] && printf '%s\n' "$out" | grep -q "8/8 tests passed"; then
        pass "$rt: stubs, matching, stdin, refusals, child manifest (8 cases)"
    else
        fail "$rt: hull test" "$out"
    fi
done

# ── hull test with stream stubs ───────────────────────────────────────
# test.exec_stub's stream form (docs/design/exec_stream.md §8): chunks in
# order then the result, a stub function given the args and stdin, on_write
# answering writes, the stream's totals, and a stub inside a test.get
# request (which may not wait). Needs a host with the command broker.

case "$(uname -s)" in Linux|Darwin) STREAM_STUBS=1 ;; *) STREAM_STUBS=0 ;; esac
if [ "$STREAM_STUBS" = 1 ]; then
mkdir -p "$TMPDIR/lua_s/tests" "$TMPDIR/js_s/tests"
cat > "$TMPDIR/lua_s/app.lua" <<'EOF'
app.manifest({
  modules = { "hull/exec@1", "hull/http-server@1", "hull/db@1" },
  exec = {
    follow = { path = "/bin/cat", stream = true, timeout_ms = 0 },
    repl = { path = "/bin/cat", argv = { { chars = "a-z", max = 8 } }, stream = true,
             timeout_ms = 0, stdin_max = 8 },
    capped = { path = "/bin/cat", stream = true, timeout_ms = 1000, stdout_max = 5 },
    plain = { path = "/bin/cat" },
  },
})
local exec = require("hull.exec")
app.get("/follow", function(req, res)
  local s, out = exec.stream("follow"), {}
  while true do local c = s:read(); if not c then break end; out[#out + 1] = c.data end
  res:text(table.concat(out) .. " status=" .. s:result().status)
end)
EOF
cat > "$TMPDIR/lua_s/tests/test_stream.lua" <<'EOF'
local exec = require("hull.exec")
local function refused(fn, want)
  local ok, err = pcall(fn)
  test.eq(ok, false)
  test.ok(tostring(err):find(want, 1, true) ~= nil, tostring(err))
end
test("chunks in order, then the result", function()
  test.exec_stub("follow", { chunks = {
    { fd = "stdout", data = "one\n" }, { fd = "stderr", data = "warn" },
    { fd = "stdout", data = "two\n" } }, status = 3 })
  local s, got = exec.stream("follow"), {}
  while true do local c = s:read(); if not c then break end; got[#got + 1] = c.fd .. ":" .. c.data end
  test.eq(table.concat(got, "|"), "stdout:one\n|stderr:warn|stdout:two\n")
  local r = s:result()
  test.eq(r.status, 3)
  test.eq(r.signal, nil)
  test.eq(r.stdout_length, 8)
  test.eq(r.stderr_length, 4)
end)
test("a stub function gets the args and stdin; on_write answers each write", function()
  local seen_args, seen_in
  test.exec_stub("repl", function(args, name, stdin)
    seen_args, seen_in = args[1] .. "/" .. name, stdin
    return { on_write = function(b) return { { fd = "stdout", data = "got:" .. b } } end }
  end)
  local s = exec.stream("repl", { "abc" }, { stdin = "hi", stdin_open = true })
  test.eq(seen_args, "abc/repl")
  test.eq(seen_in, "hi")
  test.eq(s:write("ping"), true)
  test.eq(s:read().data, "got:ping")
  refused(function() s:write("12345") end, "stdin_too_large")   -- 2 + 4 + 5 > 8
  s:close_write()
  refused(function() s:write("x") end, "stdin_closed")
  test.eq(s:read(), nil)
end)
test("the stream's total cuts the output and kills it", function()
  test.exec_stub("capped", { chunks = { { fd = "stdout", data = "123456789" },
                                        { fd = "stdout", data = "more" } } })
  local s = exec.stream("capped")
  test.eq(s:read().data, "12345")
  test.eq(s:read(), nil)
  local r = s:result()
  test.eq(r.stdout_truncated, true)
  test.eq(r.signal, "SIGKILL")
  test.eq(r.stdout_length, 5)
end)
test("a stream stub inside a test.get request answers with no wait", function()
  test.exec_stub("follow", { chunks = { { fd = "stdout", data = "a" }, { fd = "stdout", data = "b" } } })
  local r = test.get("/follow")
  test.eq(r.status, 200)
  test.eq(r.body, "ab status=0")
end)
test("no stub, an ordinary command, or a bad chunk is refused", function()
  test.exec_stub("capped", nil)            -- stubs last for the whole file
  refused(function() exec.stream("capped") end, "no stub for 'capped'")
  refused(function() exec.stream("plain") end, "not a stream command")
  test.exec_stub("follow", { chunks = { { fd = "stdin", data = "x" } } })
  refused(function() exec.stream("follow") end, 'fd must be "stdout" or "stderr"')
end)
EOF
cat > "$TMPDIR/js_s/app.js" <<'EOF'
import { app } from "hull:app";
import { exec } from "hull:exec";
app.manifest({
  modules: ["hull/exec@1", "hull/http-server@1", "hull/db@1"],
  exec: {
    follow: { path: "/bin/cat", stream: true, timeoutMs: 0 },
    repl: { path: "/bin/cat", argv: [{ chars: "a-z", max: 8 }], stream: true, timeoutMs: 0,
            stdinMax: 8 },
    capped: { path: "/bin/cat", stream: true, timeoutMs: 1000, stdoutMax: 5 },
    plain: { path: "/bin/cat" },
  },
});
const text = (u8) => { let s = ""; for (const b of u8) s += String.fromCharCode(b); return s; };
app.get("/follow", async (req, res) => {
  const s = exec.stream("follow");
  let out = "";
  for await (const c of s) out += text(c.data);
  res.text(out + " status=" + (await s.result()).status);
});
EOF
cat > "$TMPDIR/js_s/tests/test_stream.js" <<'EOF'
import { exec } from "hull:exec";
const text = (u8) => { let s = ""; for (const b of u8) s += String.fromCharCode(b); return s; };
async function refused(fn, want) {
  let err = null;
  try { await fn(); } catch (e) { err = e; }
  test.ok(err !== null, "expected an error with " + want);
  test.ok(String(err && err.message).includes(want), String(err && err.message));
}
test("chunks in order, then the result", async () => {
  test.execStub("follow", { chunks: [
    { fd: "stdout", data: "one\n" }, { fd: "stderr", data: "warn" },
    { fd: "stdout", data: "two\n" }], status: 3 });
  const s = exec.stream("follow");
  const got = [];
  for await (const c of s) got.push(c.fd + ":" + text(c.data));
  test.eq(got.join("|"), "stdout:one\n|stderr:warn|stdout:two\n");
  const r = await s.result();
  test.eq(r.status, 3);
  test.eq(r.signal, null);
  test.eq(r.stdoutLength, 8);
  test.eq(r.stderrLength, 4);
});
test("a stub function gets the args and stdin; onWrite answers each write", async () => {
  let seenArgs, seenIn;
  test.execStub("repl", (args, name, stdin) => {
    seenArgs = args[0] + "/" + name;
    seenIn = text(new Uint8Array(stdin));
    return { onWrite: (b) => [{ fd: "stdout", data: "got:" + text(b) }] };
  });
  const s = exec.stream("repl", ["abc"], { stdin: "hi", stdinOpen: true });
  test.eq(seenArgs, "abc/repl");
  test.eq(seenIn, "hi");
  await s.write("ping");
  test.eq(text((await s.read()).data), "got:ping");
  await refused(() => s.write("12345"), "stdin_too_large");
  s.closeWrite();
  await refused(() => s.write("x"), "stdin_closed");
  test.eq(await s.read(), null);
});
test("the stream's total cuts the output and kills it", async () => {
  test.execStub("capped", { chunks: [{ fd: "stdout", data: "123456789" },
                                     { fd: "stdout", data: "more" }] });
  const s = exec.stream("capped");
  test.eq(text((await s.read()).data), "12345");
  test.eq(await s.read(), null);
  const r = await s.result();
  test.eq(r.stdoutTruncated, true);
  test.eq(r.signal, "SIGKILL");
  test.eq(r.stdoutLength, 5);
});
test("a stream stub inside a test.get request answers with no wait", async () => {
  test.execStub("follow", { chunks: [{ fd: "stdout", data: "a" }, { fd: "stdout", data: "b" }] });
  const r = await test.get("/follow");
  test.eq(r.status, 200);
  test.eq(r.body, "ab status=0");
});
test("no stub, an ordinary command, or a bad chunk is refused", async () => {
  test.execStub("capped", null);            // stubs last for the whole file
  await refused(() => exec.stream("capped"), "no stub for 'capped'");
  await refused(() => exec.stream("plain"), "not a stream command");
  test.execStub("follow", { chunks: [{ fd: "stdin", data: "x" }] });
  await refused(() => exec.stream("follow"), 'fd must be "stdout" or "stderr"');
});
EOF
for rt in lua js; do
    echo "== hull test, stream stubs ($rt) =="
    out=$(cd "$TMPDIR/${rt}_s" && "$HULL" test . 2>&1) && rc=0 || rc=$?
    if [ "$rc" = 0 ] && printf '%s\n' "$out" | grep -q "5/5 tests passed"; then
        pass "$rt: stream stubs: chunks, args and stdin, on_write, totals, test.get, refusals (5 cases)"
    else
        fail "$rt: hull test stream stubs" "$out"
    fi
done
fi

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
refuse "a stdin_max over the ceiling" "stdin_max must be 1..1048576" \
    'exec = { a = { path = "/bin/true", stdin_max = 1048577 } }' \
    'exec: { a: { path: "/bin/true", stdinMax: 1048577 } }'
refuse "a short sha256 pin" "sha256 must be 64 hex characters" \
    'exec = { a = { path = "/bin/true", sha256 = "00" } }' \
    'exec: { a: { path: "/bin/true", sha256: "00" } }'
refuse "a sha256 pin that is not hex" "sha256 must be 64 hex characters" \
    'exec = { a = { path = "/bin/true", sha256 = string.rep("g", 64) } }' \
    'exec: { a: { path: "/bin/true", sha256: "g".repeat(64) } }'
refuse "a sha256 pin that is not a string" "sha256 must be a string" \
    'exec = { a = { path = "/bin/true", sha256 = 5 } }' \
    'exec: { a: { path: "/bin/true", sha256: 5 } }'
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
refuse "host.write outside /dev, /run, /tmp (fs.write)" "must be under /dev, /run or /tmp" \
    'exec = { a = { path = "/bin/true", host = { write = { "/etc/x" } } } }' \
    'exec: { a: { path: "/bin/true", host: { write: ["/etc/x"] } } }'
refuse "a refused env name" "env may not set LD_PRELOAD" \
    'exec = { a = { path = "/bin/true", env = { LD_PRELOAD = "/x.so" } } }' \
    'exec: { a: { path: "/bin/true", env: { LD_PRELOAD: "/x.so" } } }'
refuse "hull/exec without an exec section" "requires an \`exec\` section" "" ""

echo "== the child manifest (exec.<name>.manifest) =="
refuse "an app-only capability inside a child manifest" \
    "exec.a.manifest.hosts: an app capability a command cannot be given" \
    'exec = { a = { path = "/bin/true", manifest = { hosts = { "api.example.com" } } } }' \
    'exec: { a: { path: "/bin/true", manifest: { hosts: ["api.example.com"] } } }'
refuse "a pin inside a child manifest" \
    "exec.a.manifest.sha256: not a manifest field; it belongs on the command entry (exec.a.sha256)" \
    'exec = { a = { path = "/bin/true", manifest = { sha256 = string.rep("0", 64) } } }' \
    'exec: { a: { path: "/bin/true", manifest: { sha256: "0".repeat(64) } } }'
refuse "the app's env allowlist inside a child manifest" "exec.a.manifest.env:" \
    'exec = { a = { path = "/bin/true", manifest = { env = { "HOME" } } } }' \
    'exec: { a: { path: "/bin/true", manifest: { env: ["HOME"] } } }'
refuse "an unknown key inside a child manifest" "exec.a.manifest: unknown key 'fss'" \
    'exec = { a = { path = "/bin/true", manifest = { fss = {} } } }' \
    'exec: { a: { path: "/bin/true", manifest: { fss: {} } } }'
refuse "the whole app directory for a command" "grants the whole app directory" \
    'exec = { a = { path = "/bin/true", manifest = { fs = { read = { "." } } } } }' \
    'exec: { a: { path: "/bin/true", manifest: { fs: { read: ["."] } } } }'
refuse "a pattern for a command" "is a pattern, which a command cannot be given" \
    'exec = { a = { path = "/bin/true", manifest = { fs = { read = { "logs/*.log" } } } } }' \
    'exec: { a: { path: "/bin/true", manifest: { fs: { read: ["logs/*.log"] } } } }'
refuse "an absolute path in a child manifest" "is not an fs grant" \
    'exec = { a = { path = "/bin/true", manifest = { fs = { read = { "/etc" } } } } }' \
    'exec: { a: { path: "/bin/true", manifest: { fs: { read: ["/etc"] } } } }'
# exec_v028.legacy_keys_refused: the v0.26.0 spellings on the command entry
# are refused, naming the replacement (with or without host beside them).
refuse "exec_v028.legacy_keys_refused: fs on the entry" \
    "exec.a.fs: removed in 0.28.0, use exec.a.host" \
    'exec = { a = { path = "/bin/true", fs = { read = { "/etc" } } } }' \
    'exec: { a: { path: "/bin/true", fs: { read: ["/etc"] } } }'
refuse "exec_v028.legacy_keys_refused: fs beside host" \
    "exec.a.fs: removed in 0.28.0, use exec.a.host" \
    'exec = { a = { path = "/bin/true", fs = { read = { "/etc" } }, host = { read = { "/usr" } } } }' \
    'exec: { a: { path: "/bin/true", fs: { read: ["/etc"] }, host: { read: ["/usr"] } } }'
refuse "exec_v028.legacy_keys_refused: unix_sockets on the entry" \
    "exec.a.unix_sockets: removed in 0.28.0, use exec.a.host.unix_sockets" \
    'exec = { a = { path = "/bin/true", unix_sockets = true } }' \
    'exec: { a: { path: "/bin/true", unix_sockets: true } }'
# exec_audit53.*: a control byte in a command's grant is refused at load,
# and the error names the entry, not its bytes.
refuse "exec_audit53.control_byte: a control byte in a child grant" "holds a control character" \
    'exec = { a = { path = "/bin/true", manifest = { fs = { read = { "data/a\nX" } } } } }' \
    'exec: { a: { path: "/bin/true", manifest: { fs: { read: ["data/a\nX"] } } } }'
# JS reads two spellings; both at once was one read and the other signed and
# shown by hull inspect.
js_refuse() {   # label want section
    d="$TMPDIR/refuse_js2"
    rm -rf "$d"; mkdir -p "$d"
    printf 'import { app } from "hull:app";\napp.manifest({ modules: ["hull/exec@1"], %s });\napp.main(() => 0);\n' \
        "$3" > "$d/app.js"
    out=$(cd "$d" && "$HULL" app.js 2>&1) && rc=0 || rc=$?
    if [ "$rc" != 0 ] && printf '%s\n' "$out" | grep -qF -- "$2"; then pass "js: $1"
    else fail "js: $1 (rc=$rc)" "$out"; fi
}
js_refuse "exec_audit53.both_spellings: host.unixSockets and host.unix_sockets" \
    "unixSockets and unix_sockets both given" \
    'exec: { a: { path: "/bin/true", host: { unixSockets: false, unix_sockets: true } } }'
js_refuse "exec_audit53.both_spellings: timeoutMs and timeout_ms" \
    "timeoutMs and timeout_ms both given" \
    'exec: { a: { path: "/bin/true", timeoutMs: 1000, timeout_ms: 2000 } }'
js_refuse "exec_v028.legacy_keys_refused: unixSockets on the entry" \
    "exec.a.unixSockets: removed in 0.28.0, use exec.a.host.unixSockets" \
    'exec: { a: { path: "/bin/true", unixSockets: true } }'
js_refuse "exec_v028.legacy_keys_refused: a null unixSockets on the entry" \
    "exec.a.unixSockets: removed in 0.28.0, use exec.a.host.unixSockets" \
    'exec: { a: { path: "/bin/true", unixSockets: null, host: { unixSockets: true } } }'
# A null first spelling inside host is absent: unix_sockets is read, not dropped.
d="$TMPDIR/js_null_spelling"
rm -rf "$d"; mkdir -p "$d/tests"
cat > "$d/app.js" <<'APP'
import { app } from "hull:app";
app.manifest({ modules: ["hull/exec@1"],
  exec: { a: { path: "/bin/cat", host: { unixSockets: null, unix_sockets: true } } } });
app.main(() => 0);
APP
printf 'import { exec } from "hull:exec";\ntest.execStub("a", { stdout: "x" });\ntest("runs", async () => { test.eq((await exec.run("a")).stdout, "x"); });\n' \
    > "$d/tests/test_null.js"
out=$(cd "$d" && "$HULL" test . 2>&1) && rc=0 || rc=$?
if [ "$rc" = 0 ]; then
    pass "js: exec_audit53.null_spelling: a null host.unixSockets falls back to unix_sockets"
else
    fail "js: exec_audit53.null_spelling (rc=$rc)" "$out"
fi
refuse "host.write outside /dev, /run, /tmp" "must be under /dev, /run or /tmp" \
    'exec = { a = { path = "/bin/true", host = { write = { "/etc/x" } } } }' \
    'exec: { a: { path: "/bin/true", host: { write: ["/etc/x"] } } }'

# ── Outside hull test ─────────────────────────────────────────────────

# On Linux the command broker runs the declared command (tests/
# e2e_exec_broker.sh covers it in depth); on a host without one the app is
# refused at start, and with --allow-degraded-sandbox exec.run fails.
echo "== hull inspect shows a command's whole authority =="
d="$TMPDIR/inspect"
rm -rf "$d"; mkdir -p "$d"
cat > "$d/package.sig" <<'SIG'
{"manifest":{"modules":["hull/exec@1"],"exec":{"gpu-query":{"path":"/usr/bin/nvidia-smi","host":{"read":["/dev/nvidiactl"],"write":["/dev/nvidiactl"]},"manifest":{"fs":{"read":["data/gpu-ids"],"write":["out/"]}}},"old":{"path":"/bin/cat","host":{"read":["/etc"],"unix_sockets":true}}}}}
SIG
out=$("$HULL" inspect "$d" 2>&1) || true
for want in "host.read:  /dev/nvidiactl" "host.write: /dev/nvidiactl" \
            "manifest.fs.read:  data/gpu-ids" "manifest.fs.write: out/" \
            "host.read:  /etc" "local IPC: may connect to any pathname unix socket" \
            "Effective authority (each command its own sandbox)" \
            "macOS: each command exactly as listed above"; do
    if printf '%s\n' "$out" | grep -qF -- "$want"; then pass "inspect: $want"
    else fail "inspect: $want" "$out"; fi
done
# macOS gives each command a broker of its own (#1218): no union is shown.
if printf '%s\n' "$out" | grep -qF "ONE sandbox"; then
    fail "inspect: no macOS union (each command its own broker)" "$out"
else
    pass "inspect: no macOS union (each command its own broker)"
fi

# exec_inspect_audit53.*: inspect shows the grants the extractor enforces,
# one line per manifest string, and does not crash on a planted non-string
# entry.
d="$TMPDIR/inspect53"
rm -rf "$d"; mkdir -p "$d"
cat > "$d/package.sig" <<'SIG'
{"manifest":{"modules":["hull/exec@1"],"exec":{"mixed":{"path":"/bin/ls","host":{"read":["/etc/hosts"],"write":["/tmp/out"],"unix_sockets":true}},"forge":{"path":"/bin/cat","host":{"read":["/x\n      unix sockets:      no"]}}}}}
SIG
out=$("$HULL" inspect "$d" 2>&1) || true
for want in "host.read:  /etc/hosts" "host.write: /tmp/out" \
            '/x\n      unix sockets:      no'; do
    if printf '%s\n' "$out" | grep -qF -- "$want"; then pass "exec_inspect_audit53: $want"
    else fail "exec_inspect_audit53: $want" "$out"; fi
done
if printf '%s\n' "$out" | grep -qx "      unix sockets:      no"; then
    fail "exec_inspect_audit53.forge: a grant forged an output line" "$out"
else
    pass "exec_inspect_audit53.forge: a newline in a grant stays on its line"
fi
cat > "$d/package.sig" <<'SIG'
{"manifest":{"modules":["hull/exec@1"],"exec":{"bad":{"path":"/bin/cat","manifest":{"fs":{"read":[{}]}},"host":{"read":[1,"/etc"]},"argv":[{"enum":[{}]},{"repeat":{"min":{},"max":[]}}],"timeout_ms":1.5}}}}
SIG
out=$("$HULL" inspect "$d" 2>&1) && rc=0 || rc=$?
if printf '%s\n' "$out" | grep -q "invalid value\|attempt to"; then
    fail "exec_inspect_audit53.non_string: inspect crashed (rc=$rc)" "$out"
elif printf '%s\n' "$out" | grep -qF "host.read:  /etc"; then
    pass "exec_inspect_audit53.non_string: non-string entries are skipped"
else
    fail "exec_inspect_audit53.non_string (rc=$rc)" "$out"
fi

# exec_stream_inspect: a stream command shows its own limits, flagged where
# unbounded, and the exec.run limits derived from them.
d="$TMPDIR/inspect_stream"
rm -rf "$d"; mkdir -p "$d"
cat > "$d/package.sig" <<'SIG'
{"manifest":{"modules":["hull/exec@1"],"exec":{"follow":{"path":"/usr/bin/journalctl","stream":true,"timeout_ms":0,"stdin_max":4096},"tail":{"path":"/usr/bin/tail","stream":true,"timeoutMs":120000,"stdoutMax":33554432,"stderrMax":4096,"concurrency":1}}}}
SIG
out=$("$HULL" inspect "$d" 2>&1) || true
for want in "stream: [!] no time limit, stdout [!] no cap, stderr [!] no cap, 2 at once" \
            "exec.run on it: 60000 ms, stdout 16777216, stderr 1048576" \
            "stdin: up to 4096 bytes from the app over the stream's life (write())" \
            "stream: 120000 ms, stdout 33554432, stderr 4096, 1 at once"; do
    if printf '%s\n' "$out" | grep -qF -- "$want"; then pass "exec_stream_inspect: $want"
    else fail "exec_stream_inspect: $want" "$out"; fi
done

echo "== the hull check lint =="
# hull check (and hull build) warn about risky declared commands; a warning
# never fails the step. The import step must also resolve an app that
# declares commands (the tool-side resolve once dropped the section).
lint_app() {   # dir runtime
    d=$1
    rm -rf "$d"; mkdir -p "$d/tests"
    if [ "$2" = lua ]; then
        cat > "$d/app.lua" <<'APP'
app.manifest({ modules = { "hull/exec@1" }, exec = {
  ["sh-run"] = { path = "/bin/sh", argv = { "-c", { chars = "a-z" } } },
  loose = { path = "/home/x/tool", argv = { "show", { allow_dash = true } } },
  ok = { path = "/usr/bin/systemctl", argv = { "show", "--", { chars = "a-z" } },
         sha256 = string.rep("ab", 32) },
  follow = { path = "/usr/bin/journalctl", argv = { "-f" }, stream = true, timeout_ms = 0 },
  slow = { path = "/usr/bin/journalctl", stream = true, timeout_ms = 120000,
           stdout_max = 1048576 },
  okstream = { path = "/usr/bin/journalctl", stream = true, timeout_ms = 30000,
               stdout_max = 1048576, stderr_max = 65536 },
} })
local exec = require("hull.exec")
app.main(function() return 0 end)
APP
        printf 'test("ok", function() test.eq(1, 1) end)\n' > "$d/tests/test_app.lua"
    else
        cat > "$d/app.js" <<'APP'
import { app } from "hull:app";
import { exec } from "hull:exec";
app.manifest({ modules: ["hull/exec@1"], exec: {
  "sh-run": { path: "/bin/sh", argv: ["-c", { chars: "a-z" }] },
  loose: { path: "/home/x/tool", argv: ["show", { allowDash: true }] },
  ok: { path: "/usr/bin/systemctl", argv: ["show", "--", { chars: "a-z" }],
        sha256: "ab".repeat(32) },
  follow: { path: "/usr/bin/journalctl", argv: ["-f"], stream: true, timeoutMs: 0 },
  slow: { path: "/usr/bin/journalctl", stream: true, timeoutMs: 120000, stdoutMax: 1048576 },
  okstream: { path: "/usr/bin/journalctl", stream: true, timeoutMs: 30000,
              stdoutMax: 1048576, stderrMax: 65536 },
} });
app.main(() => 0);
APP
        printf 'test("ok", () => { test.eq(1, 1); });\n' > "$d/tests/test_app.js"
    fi
}
for rt in lua js; do
    d="$TMPDIR/lint_$rt"
    lint_app "$d" $rt
    out=$("$HULL" check "$d" 2>&1) && rc=0 || rc=$?
    want_ok=1
    for w in "WARN exec.sh-run: sh is a general-purpose program" \
             "WARN exec.loose: /home/x/tool is outside" \
             "WARN exec.loose: argv 2 may start with '-'" \
             "WARN exec.loose: argv 2 has no chars or enum" \
             "WARN exec.sh-run: argv 2 follows fixed arguments with no \"--\"" \
             "WARN exec.follow: stream command with no time limit" \
             "WARN exec.follow: stream command with no total on stdout or stderr" \
             "WARN exec.slow: stream command with a time limit of 120 s" \
             "WARN exec.slow: stream command with no total on stderr: it may write without bound; declare stderr_max"; do
        printf '%s\n' "$out" | grep -qF -- "$w" || { want_ok=0; break; }
    done
    if [ $want_ok = 1 ]; then pass "$rt: hull check warns about risky commands"
    else fail "$rt: hull check warns about risky commands (missing: $w)" "$out"; fi
    if printf '%s\n' "$out" | grep -q "WARN exec.ok"; then
        fail "$rt: a narrow pinned command (and a bounded stream) gets no warning" "$out"
    else
        pass "$rt: a narrow pinned command (and a bounded stream) gets no warning"
    fi
    if printf '%s\n' "$out" | grep -q "import analysis failed\|requires an \`exec\` section"; then
        fail "$rt: the import step resolves an app with commands" "$out"
    else
        pass "$rt: the import step resolves an app with commands"
    fi
done

echo "== outside hull test"
mkdir -p "$TMPDIR/run_lua" "$TMPDIR/run_js"
cat > "$TMPDIR/run_lua/app.lua" <<'APP'
app.manifest({ modules = { "hull/exec@1" }, exec = { a = { path = "/usr/bin/true" } } })
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
app.manifest({ modules: ["hull/exec@1"], exec: { a: { path: "/usr/bin/true" } } });
app.main(async () => {
  try { const r = await exec.run("a"); return r.status === 0 ? 0 : 5; }
  catch (e) { return String(e).includes("broker is not running") ? 7 : 4; }
});
APP
for rt in lua js; do
    entry="app.$rt"
    os=$(uname -s)
    if [ "$os" = Linux ] || [ "$os" = Darwin ]; then
        flags=""
        if [ "$os" = Linux ]; then
            abi=$(python3 -c 'import ctypes; print(ctypes.CDLL(None, use_errno=True).syscall(444, None, 0, 1))' 2>/dev/null || echo 0)
            [ "$abi" -lt 6 ] 2>/dev/null && flags="--allow-degraded-sandbox"
        fi
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
