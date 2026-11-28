#!/bin/sh
# e2e_exec_stream.sh - exec.run's pipe handling and exec.stream, both
# runtimes, through the real command broker (docs/design/exec_stream.md).
#
# Needs a hull built with `make HL_EXEC_TEST_HOOKS=1` (a fixture this user
# owns passes the root-ownership check). Linux and macOS; skips elsewhere.
#
#   - exec.run of a command that writes 300000 bytes to stderr (past one
#     pipe window and the kernel pipe buffer) BEFORE it reads a 900000-byte
#     stdin: both pipes byte-exact, its status, no deadlock - and an
#     unrelated request is answered while it runs;
#   - exec.run of a command that closes its stdin early: the rest is not
#     written, the result is normal;
#   - exec.stream: output before the command ends; an interactive stdin
#     round trip with no coalescing delay; the total cap; close(); a
#     client disconnect killing the command; refusal for an ordinary
#     command.
#   - the paths phase, plain and with --vm-thread: 8 streams at once,
#     interleaved; 8 held open and a 9th refused busy, the slots free again
#     once they are closed; SIGTERM with 3 streams held and one parked in a
#     request: exit 0 within the drain time, every command and the broker
#     gone, no sanitizer report;
#   - hull dev: a reload with streams open ends the old server, its
#     commands and its broker, and the new one runs streams; stopping hull
#     dev with streams open leaves no command behind.
#
# E2E_ONLY=paths|dev|audit|offheap runs that phase alone (both runtimes).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HULL=${HULL:-$ROOT/build/hull}
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/        /'; }
skip() {
    if [ "${HULL_REQUIRE_EXEC_BROKER:-0}" = 1 ]; then echo "FAIL: $1"; exit 1; fi
    echo "SKIP: e2e_exec_stream: $1"; exit 0
}

case "$(uname -s)" in Linux|Darwin) ;; *) skip "the broker runs on Linux and macOS" ;; esac
grep -aq 'exec probe ready pid' "$HULL" || skip "needs make HL_EXEC_TEST_HOOKS=1"
command -v cc >/dev/null 2>&1 || skip "needs a C compiler for the fixture"
command -v curl >/dev/null 2>&1 || skip "needs curl"

DEGRADED=""
if [ "$(uname -s)" = Linux ]; then
    ABI=$(python3 -c 'import ctypes; print(ctypes.CDLL(None, use_errno=True).syscall(444, None, 0, 1))' 2>/dev/null || echo 0)
    if [ "$ABI" -lt 6 ] 2>/dev/null; then DEGRADED="--allow-degraded-sandbox"; fi
fi

# The safe-path rule rules out /tmp: a private directory under $HOME.
case "$HOME" in /*) ;; *) skip "HOME is not an absolute path" ;; esac
FIX=$(mktemp -d "$HOME/.hull-e2e-exec.XXXXXX")
chmod 700 "$FIX"
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null || true; done
    case "$FIX" in "$HOME"/.hull-e2e-exec.*) rm -rf "$FIX" ;; esac
}
trap cleanup EXIT

cc -O2 -o "$FIX/exec_pipes" "$ROOT/tests/fixtures/exec_pipes.c"
chmod 755 "$FIX/exec_pipes"
P="$FIX/exec_pipes"
WANT_PIPES=$(awk 'BEGIN { for (i = 0; i < 900000; i++) printf "I" }' | "$P" pipes 0 2>/dev/null)

PORT=${PORT:-19962}
start_server() {  # start_server <dir> <entry file> -> $SPID, $SLOG, $PORT
    PORT=$((PORT + 1))
    SLOG="$1/server.log"
    (cd "$1" && exec "$HULL" $DEGRADED ${EXTRA:-} -p "$PORT" "$2" > "$SLOG" 2>&1) &
    SPID=$!
    PIDS="$PIDS $SPID"
    for i in $(seq 1 100); do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/ping" && return 0
        sleep 0.05
    done
    fail "server did not start" "$(cat "$SLOG")"
    return 1
}
stop_server() { kill -TERM "$SPID" 2>/dev/null || true; wait "$SPID" 2>/dev/null || true; }
get() { curl -s -m "${2:-20}" "http://127.0.0.1:$PORT$1"; }
gone_within() {  # gone_within <pid>: 0 when it is gone within 3 s
    for i in $(seq 1 60); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.05; done
    return 1
}
all_gone() {  # all_gone <comma-separated pids>: 0 when every one is gone within 3 s
    for p in $(printf '%s' "$1" | tr ',' ' '); do gone_within "$p" || return 1; done
}
new_silent() {  # new_silent <lines before>: the pid of the next "silent pid=" line in $SLOG
    for i in $(seq 1 100); do
        [ "$(grep -c 'silent pid=' "$SLOG")" -gt "$1" ] && break; sleep 0.05
    done
    sed -n 's/.*silent pid=\([0-9]*\).*/\1/p' "$SLOG" | tail -1
}
clean_log() {
    ! grep -qiE "AddressSanitizer|LeakSanitizer|runtime error:|Assertion|double free|Segmentation|use-after" "$1"
}
# stop_timed -> STOP_RC, STOP_MS: SIGTERM, then the exit status (bounded at 20 s).
stop_timed() {
    t0=$(python3 -c 'import time; print(int(time.time() * 1000))')
    kill -TERM "$SPID" 2>/dev/null || true
    ( sleep 20; kill -9 "$SPID" 2>/dev/null ) >/dev/null 2>&1 &
    wd=$!
    STOP_RC=0
    wait "$SPID" 2>/dev/null || STOP_RC=$?
    STOP_MS=$(( $(python3 -c 'import time; print(int(time.time() * 1000))') - t0 ))
    kill "$wd" 2>/dev/null || true
    wait "$wd" 2>/dev/null || true
}

EXEC_LUA="
    pipes = { path = \"$P\", argv = { \"pipes\", { min = 1, max = 5, chars = \"0-9\" } },
              stdin_max = 1048576, stderr_max = 524288, timeout_ms = 20000 },
    early = { path = \"$P\", argv = { \"early\" }, stdin_max = 1048576 },
    tick = { path = \"$P\", argv = { \"tick\" }, stream = true, timeout_ms = 10000 },
    echo = { path = \"$P\", argv = { \"echo\" }, stream = true, timeout_ms = 0,
             stdin_max = 1073741824, concurrency = 8 },
    flood = { path = \"$P\", argv = { \"flood\" }, stream = true, timeout_ms = 10000,
              stdout_max = 100000 },
    silent = { path = \"$P\", argv = { \"silent\" }, stream = true, timeout_ms = 0,
               concurrency = 8 },
    deaf = { path = \"$P\", argv = { \"silent\" }, stream = true, timeout_ms = 0,
             stdin_max = 1073741824 },
    both = { path = \"$P\", argv = { \"both\" }, stream = true, timeout_ms = 30000,
             concurrency = 8 },
    one = { path = \"$P\", argv = { \"echo\" }, stream = true, timeout_ms = 0,
            stdin_max = 1048576, concurrency = 1 },
"

lua_app() {
cat > "$1/app.lua" <<EOF
app.manifest({
  modules = { "hull/exec@1", "hull/http-server@1", "hull/time@1", "hull/log@1", "hull/db@1" },
  exec = { $EXEC_LUA },
})
local db = require("hull.db")
local exec = require("hull.exec")
local time = require("hull.time")
local log = require("hull.log")
app.get("/ping", function(req, res) res:text("pong") end)
app.get("/pipes", function(req, res)
  local r = exec.run("pipes", { req.query.ms or "0" }, { stdin = string.rep("I", 900000) })
  local allE = #r.stderr == 300000 and not r.stderr:find("[^E]")
  res:text(string.format("status=%d stdout=%s stderr=%d allE=%s cut=%s/%s",
    r.status, r.stdout:gsub("\n$", ""), #r.stderr, tostring(allE),
    tostring(r.stdout_truncated), tostring(r.stderr_truncated)))
end)
app.get("/early", function(req, res)
  local r = exec.run("early", {}, { stdin = string.rep("z", 500000) })
  res:text(string.format("status=%d stdout=%s", r.status, r.stdout:gsub("\n$", "")))
end)
app.get("/tick", function(req, res)
  local t0 = time.now_ms()
  local s = exec.stream("tick")
  local first = s:read()
  local t1 = time.now_ms() - t0
  local rest = ""
  while true do local c = s:read(); if not c then break end; rest = rest .. c.data end
  local r = s:result()
  res:text(string.format("first=%s first_ms=%d rest=%s total_ms=%d status=%d len=%d",
    first.data:gsub("\n$", ""), t1, rest:gsub("\n$", ""), time.now_ms() - t0, r.status,
    r.stdout_length))
end)
app.get("/echo", function(req, res)
  local s = exec.stream("echo", {}, { stdin_open = true })
  local t0 = time.now_ms()
  local ok = true
  for i = 1, 50 do
    s:write(i .. "\n")
    local got = ""
    while not got:find("\n") do got = got .. s:read().data end
    ok = ok and got == i .. "\n"
  end
  local ms = time.now_ms() - t0
  s:close_write()
  while s:read() do end
  res:text(string.format("ok=%s ms=%d status=%d", tostring(ok), ms, s:result().status))
end)
app.get("/flood", function(req, res)
  local s = exec.stream("flood")
  local n = 0
  while true do local c = s:read(); if not c then break end; n = n + #c.data end
  local r = s:result()
  res:text(string.format("n=%d cut=%s signal=%s len=%d", n, tostring(r.stdout_truncated),
    tostring(r.signal), r.stdout_length))
end)
app.get("/close", function(req, res)
  local s = exec.stream("silent")
  local pid = s:read().data:gsub("\n$", "")
  s:close()
  res:text(string.format("pid=%s after=%s signal=%s", pid, tostring(s:read()),
    tostring(s:result().signal)))
end)
app.get("/hang", function(req, res)
  local s = exec.stream("silent")
  log.info("silent pid=" .. s:read().data:gsub("\n$", ""))
  s:read()                               -- waits; the client goes
  res:text("never")
end)
app.get("/ordinary", function(req, res)
  local ok, err = pcall(exec.stream, "early")
  res:text(tostring(ok) .. " " .. tostring(err))
end)
-- Review of PR 2 (the hooks server: HULL_TEST_EXEC_WAKE_DELAY_MS, _FAIL_WRITE).
local function line(c) return c and (c.data:gsub("\n$", "")) or "nil" end
local S, H
app.get("/share/start", function(req, res) S = exec.stream("echo", {}, { stdin_open = true }); res:text("ok") end)
app.get("/share/a", function(req, res) res:text("a=" .. line(S:read())) end)
app.get("/share/write", function(req, res) S:write("x\n"); res:text("w") end)
app.get("/share/b", function(req, res)
  local ok, c = pcall(S.read, S)
  res:text(ok and ("b=" .. line(c)) or ("b-err=" .. tostring(c)))
end)
app.get("/share/close", function(req, res) S:close(); S = nil; res:text("closed") end)
app.get("/h1/start", function(req, res)
  H = exec.stream("one", {}, { stdin_open = true }); H:write("q\n"); res:text("ok")
end)
app.get("/h1/go", function(req, res)
  local got = line(H:read())          -- its CREDIT write fails (the hook)
  H:close(); H = nil
  collectgarbage(); collectgarbage()
  local again = "busy"
  for i = 1, 30 do
    -- Refused (busy) until the killed call is reaped: the server's own
    -- check, or the broker's BUSY, which ends the new stream.
    local ok, c = pcall(function()
      local s2 = exec.stream("one", {}, { stdin_open = true })
      s2:write("r\n")
      local r = s2:read()
      s2:close()
      return r
    end)
    if ok and c and c.data == "r\n" then again = "ok"; break end
    hull.sleep(100)
  end
  res:text("got=" .. got .. " again=" .. again)
end)
app.get("/l1", function(req, res)
  local s = exec.stream("echo", {}, { stdin_open = true })
  local d = db.default()
  d.exec("BEGIN")
  local ok, e = pcall(s.write, s, string.rep("z", 1048576))
  d.exec("COMMIT")
  local code = ok and "none" or (tostring(e):match("%[hull:([a-z_]+)%]") or tostring(e))
  local ok2, e2 = pcall(s.write, s, "hello\n")
  local first = ok2 and s:read().data:sub(1, 5) or ("err:" .. tostring(e2))
  s:close()
  res:text("refused=" .. code .. " first=" .. first)
end)
app.get("/m1", function(req, res)
  local s = exec.stream("deaf", {}, { stdin_open = true })
  log.info("m1 pid=" .. line(s:read()))
  local ok, e = pcall(s.write, s, string.rep("z", 1048576))
  res:text("ok=" .. tostring(ok) .. " err=" .. tostring(e))
end)
-- The off-heap hold (exec_stream.md §5.1): 8 unread streams of 512 KiB
-- each under a 3 MiB heap limit; those past it end, the rest keep going.
-- A stream the broker took: the last round's killed commands may not be
-- reaped yet (busy from the broker), so retry those.
local function open_live(name)
  for _ = 1, 50 do
    local s = exec.stream(name)
    local c = s:read()
    if c then return s, #c.data end
    local ok, e = pcall(s.result, s)
    s:close()
    if ok or not tostring(e):find("busy", 1, true) then return s, 0 end
    hull.sleep(100)
  end
  error("the broker stayed busy")
end
app.get("/both", function(req, res)
  local k = tonumber(req.query.n or "8")
  local ss, got = {}, {}
  for i = 1, k do ss[i], got[i] = open_live("both") end
  hull.sleep(1000)
  local ended, live, msg = 0, 0, "none"
  for i = 1, k do
    local s, n = ss[i], got[i]
    while n < 1100000 do
      local c = s:read()
      if not c then break end
      n = n + #c.data
    end
    if n >= 1100000 then
      live = live + 1
    else
      ended = ended + 1
      local ok, e = pcall(s.result, s)
      if not ok then msg = tostring(e) end
    end
    s:close()
  end
  res:text(string.format("ended=%d live=%d msg=%s", ended, live, msg))
end)
app.get("/both1", function(req, res)
  local s, n = open_live("both")
  while n < 2500000 do local c = s:read(); if not c then break end; n = n + #c.data end
  s:close()
  res:text("n_ok=" .. tostring(n >= 2500000))
end)
-- The paths phase: many streams, SIGTERM with streams open, --vm-thread, hull dev.
app.get("/ver", function(req, res) res:text("ver-one") end)
local held = {}
app.get("/fill", function(req, res)
  local pids = {}
  for i = 1, 8 do
    local s = exec.stream("silent")
    held[#held + 1] = s
    pids[#pids + 1] = line(s:read())
  end
  local ok, e = pcall(exec.stream, "silent")
  local code = ok and "opened" or (tostring(e):match("%[hull:([a-z_]+)%]") or tostring(e))
  log.info("held pids=" .. table.concat(pids, ","))
  res:text("opened=" .. #held .. " ninth=" .. code)
end)
app.get("/fill/close", function(req, res)
  for _, s in ipairs(held) do s:close() end
  held = {}
  res:text("closed")
end)
app.get("/hold", function(req, res)
  local pids = {}
  for i = 1, 3 do
    local s = exec.stream("silent")
    held[#held + 1] = s
    pids[#pids + 1] = line(s:read())
  end
  log.info("held pids=" .. table.concat(pids, ","))
  res:text("held=" .. #held)
end)
app.get("/par", function(req, res)
  local id = req.query.id or "0"
  local s = exec.stream("echo", {}, { stdin_open = true })
  local ok = true
  for i = 1, 10 do
    local msg = id .. ":" .. i .. "\n"
    s:write(msg)
    local got = ""
    while not got:find("\n") do got = got .. s:read().data end
    ok = ok and got == msg
    hull.sleep(20)
  end
  s:close_write()
  while s:read() do end
  res:text("ok=" .. tostring(ok) .. " status=" .. s:result().status)
end)
EOF
}

js_app() {
EXEC_JS=$(printf '%s' "$EXEC_LUA" | sed -e 's/\([a-z_]*\) = {/\1: {/g' -e 's/ = / : /g' \
    -e 's/stdin_max/stdinMax/g; s/stderr_max/stderrMax/g; s/stdout_max/stdoutMax/g; s/timeout_ms/timeoutMs/g' \
    -e 's/argv : {\(.*\)},/argv: [\1],/' )
cat > "$1/app.js" <<EOF
import { app } from "hull:app";
import { exec } from "hull:exec";
import { time } from "hull:time";
import { log } from "hull:log";
import { db as dbModule } from "hull:db";
app.manifest({
  modules: ["hull/exec@1", "hull/http-server@1", "hull/time@1", "hull/log@1", "hull/db@1"],
  exec: {
    pipes: { path: "$P", argv: ["pipes", { min: 1, max: 5, chars: "0-9" }],
             stdinMax: 1048576, stderrMax: 524288, timeoutMs: 20000 },
    early: { path: "$P", argv: ["early"], stdinMax: 1048576 },
    tick: { path: "$P", argv: ["tick"], stream: true, timeoutMs: 10000 },
    echo: { path: "$P", argv: ["echo"], stream: true, timeoutMs: 0, stdinMax: 1073741824,
            concurrency: 8 },
    flood: { path: "$P", argv: ["flood"], stream: true, timeoutMs: 10000, stdoutMax: 100000 },
    silent: { path: "$P", argv: ["silent"], stream: true, timeoutMs: 0, concurrency: 8 },
    deaf: { path: "$P", argv: ["silent"], stream: true, timeoutMs: 0, stdinMax: 1073741824 },
    both: { path: "$P", argv: ["both"], stream: true, timeoutMs: 30000, concurrency: 8 },
    one: { path: "$P", argv: ["echo"], stream: true, timeoutMs: 0, stdinMax: 1048576,
           concurrency: 1 },
  },
});
const text = (u8) => { let s = ""; for (const b of u8) s += String.fromCharCode(b); return s; };
const chomp = (s) => s.replace(/\n\$/, "");
app.get("/ping", (req, res) => res.text("pong"));
app.get("/pipes", async (req, res) => {
  const r = await exec.run("pipes", [req.query.ms || "0"], { stdin: "I".repeat(900000) });
  const allE = r.stderr.length === 300000 && !/[^E]/.test(r.stderr);
  res.text(\`status=\${r.status} stdout=\${chomp(r.stdout)} stderr=\${r.stderr.length} allE=\${allE} cut=\${r.stdoutTruncated}/\${r.stderrTruncated}\`);
});
app.get("/early", async (req, res) => {
  const r = await exec.run("early", [], { stdin: "z".repeat(500000) });
  res.text(\`status=\${r.status} stdout=\${chomp(r.stdout)}\`);
});
app.get("/tick", async (req, res) => {
  const t0 = time.nowMs();
  const s = exec.stream("tick");
  const first = await s.read();
  const t1 = time.nowMs() - t0;
  let rest = "";
  for await (const c of s) rest += text(c.data);
  const r = await s.result();
  res.text(\`first=\${chomp(text(first.data))} first_ms=\${t1} rest=\${chomp(rest)} total_ms=\${time.nowMs() - t0} status=\${r.status} len=\${r.stdoutLength}\`);
});
app.get("/echo", async (req, res) => {
  const s = exec.stream("echo", [], { stdinOpen: true });
  const t0 = time.nowMs();
  let ok = true;
  for (let i = 1; i <= 50; i++) {
    await s.write(i + "\n");
    let got = "";
    while (!got.includes("\n")) got += text((await s.read()).data);
    ok = ok && got === i + "\n";
  }
  const ms = time.nowMs() - t0;
  s.closeWrite();
  while (await s.read()) {}
  res.text(\`ok=\${ok} ms=\${ms} status=\${(await s.result()).status}\`);
});
app.get("/flood", async (req, res) => {
  const s = exec.stream("flood");
  let n = 0;
  for await (const c of s) n += c.data.length;
  const r = await s.result();
  res.text(\`n=\${n} cut=\${r.stdoutTruncated} signal=\${r.signal} len=\${r.stdoutLength}\`);
});
app.get("/close", async (req, res) => {
  const s = exec.stream("silent");
  const pid = chomp(text((await s.read()).data));
  s.close();
  const after = await s.read();
  res.text(\`pid=\${pid} after=\${after === null ? "nil" : "chunk"} signal=\${(await s.result()).signal}\`);
});
app.get("/hang", async (req, res) => {
  const s = exec.stream("silent");
  log.info("silent pid=" + chomp(text((await s.read()).data)));
  await s.read();
  res.text("never");
});
app.get("/ordinary", (req, res) => {
  try { exec.stream("early"); res.text("true"); } catch (e) { res.text("false " + e.message); }
});
// Review of PR 2 (the hooks server: HULL_TEST_EXEC_WAKE_DELAY_MS, _FAIL_WRITE).
const line = (c) => (c ? chomp(text(c.data)) : "nil");
let S, H;
app.get("/share/start", (req, res) => { S = exec.stream("echo", [], { stdinOpen: true }); res.text("ok"); });
app.get("/share/a", async (req, res) => { res.text("a=" + line(await S.read())); });
app.get("/share/write", async (req, res) => { await S.write("x\n"); res.text("w"); });
app.get("/share/b", async (req, res) => {
  try { res.text("b=" + line(await S.read())); } catch (e) { res.text("b-err=" + e.message + " code=" + e.code); }
});
app.get("/share/close", (req, res) => { S.close(); S = null; res.text("closed"); });
app.get("/h1/start", async (req, res) => {
  H = exec.stream("one", [], { stdinOpen: true }); await H.write("q\n"); res.text("ok");
});
app.get("/h1/go", async (req, res) => {
  const got = line(await H.read());   // its CREDIT write fails (the hook)
  H.close(); H = null;
  let again = "busy";
  for (let i = 0; i < 30; i++) {
    try {
      const s2 = exec.stream("one", [], { stdinOpen: true });
      await s2.write("r\n");
      const c = await s2.read();
      s2.close();
      if (c && text(c.data) === "r\n") { again = "ok"; break; }
    } catch (e) { /* busy: retry */ }
    await hull.sleep(100);
  }
  res.text("got=" + got + " again=" + again);
});
app.get("/l1", async (req, res) => {
  const s = exec.stream("echo", [], { stdinOpen: true });
  const d = dbModule.default();
  d.exec("BEGIN");
  let code = "none";
  try { await s.write("z".repeat(1048576)); } catch (e) { code = e.code || e.message; }
  d.exec("COMMIT");
  let first;
  try { await s.write("hello\n"); first = text((await s.read()).data).slice(0, 5); }
  catch (e) { first = "err:" + e.message; }
  s.close();
  res.text("refused=" + code + " first=" + first);
});
app.get("/m1", async (req, res) => {
  const s = exec.stream("deaf", [], { stdinOpen: true });
  log.info("m1 pid=" + line(await s.read()));
  try { await s.write("z".repeat(1048576)); res.text("ok=true"); }
  catch (e) { res.text("ok=false err=" + e.name + ": " + e.message); }
});
// The off-heap hold (exec_stream.md §5.1): 8 unread streams of 512 KiB
// each under a 3 MiB heap limit; those past it end, the rest keep going.
// A stream the broker took: the last round's killed commands may not be
// reaped yet (busy from the broker), so retry those.
async function openLive(name) {
  for (let i = 0; i < 50; i++) {
    const s = exec.stream(name);
    const c = await s.read();
    if (c) return [s, c.data.length];
    let busy = false;
    try { await s.result(); } catch (e) { busy = e.code === "busy" || /busy/.test(e.message); }
    s.close();
    if (!busy) return [s, 0];
    await hull.sleep(100);
  }
  throw new Error("the broker stayed busy");
}
app.get("/both", async (req, res) => {
  const k = Number(req.query.n || "8");
  const ss = [];
  for (let i = 0; i < k; i++) ss.push(await openLive("both"));
  await hull.sleep(1000);
  let ended = 0, live = 0, msg = "none";
  for (const [s, got] of ss) {
    let n = got;
    while (n < 1100000) {
      const c = await s.read();
      if (!c) break;
      n += c.data.length;
    }
    if (n >= 1100000) live++;
    else {
      ended++;
      try { await s.result(); } catch (e) { msg = e.message; }
    }
    s.close();
  }
  res.text("ended=" + ended + " live=" + live + " msg=" + msg);
});
app.get("/both1", async (req, res) => {
  const [s, got] = await openLive("both");
  let n = got;
  while (n < 2500000) { const c = await s.read(); if (!c) break; n += c.data.length; }
  s.close();
  res.text("n_ok=" + (n >= 2500000));
});
// The paths phase: many streams, SIGTERM with streams open, --vm-thread, hull dev.
app.get("/ver", (req, res) => res.text("ver-one"));
let held = [];
app.get("/fill", async (req, res) => {
  const pids = [];
  for (let i = 0; i < 8; i++) {
    const s = exec.stream("silent");
    held.push(s);
    pids.push(line(await s.read()));
  }
  let code = "opened";
  try { exec.stream("silent"); } catch (e) { code = e.code || e.message; }
  log.info("held pids=" + pids.join(","));
  res.text("opened=" + held.length + " ninth=" + code);
});
app.get("/fill/close", (req, res) => {
  for (const s of held) s.close();
  held = [];
  res.text("closed");
});
app.get("/hold", async (req, res) => {
  const pids = [];
  for (let i = 0; i < 3; i++) {
    const s = exec.stream("silent");
    held.push(s);
    pids.push(line(await s.read()));
  }
  log.info("held pids=" + pids.join(","));
  res.text("held=" + held.length);
});
app.get("/par", async (req, res) => {
  const id = req.query.id || "0";
  const s = exec.stream("echo", [], { stdinOpen: true });
  let ok = true;
  for (let i = 1; i <= 10; i++) {
    const msg = id + ":" + i + "\n";
    await s.write(msg);
    let got = "";
    while (!got.includes("\n")) got += text((await s.read()).data);
    ok = ok && got === msg;
    await hull.sleep(20);
  }
  s.closeWrite();
  while (await s.read()) {}
  res.text("ok=" + ok + " status=" + (await s.result()).status);
});
EOF
}

run_suite() {  # run_suite <lua|js>
    rt=$1
    D="$FIX/$rt"
    mkdir -p "$D"
    if [ "$rt" = lua ]; then lua_app "$D"; ENTRY=app.lua; else js_app "$D"; ENTRY=app.js; fi
    [ -n "${E2E_ONLY:-}" ] && return   # E2E_ONLY=<phase>: that phase alone
    echo "== $rt"
    start_server "$D" "$ENTRY" || return

    # exec_stream.startup_report: stream commands are marked, with their time
    if grep -q 'exec: tick -> .* (stream, 10000 ms)' "$SLOG" &&
       grep -q 'exec: echo -> .* (stream, no time limit)' "$SLOG" &&
       grep 'exec: pipes -> ' "$SLOG" | grep -vq '(stream'; then
        pass "$rt: the startup report marks stream commands and their time limit"
    else
        fail "$rt: startup report" "$(grep 'exec: [a-z]* -> ' "$SLOG")"
    fi

    # exec_run_pipes.stderr_before_stdin
    out=$(get /pipes)
    want="status=7 stdout=$WANT_PIPES stderr=300000 allE=true cut=false/false"
    [ "$out" = "$want" ] && pass "$rt: exec.run drains stderr past one window while its stdin is still being written" \
        || fail "$rt: exec.run pipes" "got:  $out
want: $want"

    # exec_run_pipes.loop_serves_while_pending
    get "/pipes?ms=1500" 30 > "$D/slow.out" &
    slow=$!
    sleep 0.3
    t0=$(date +%s)
    ping=$(get /ping 5)
    alive=0; kill -0 "$slow" 2>/dev/null && alive=1
    wait "$slow"
    [ "$ping" = pong ] && [ "$alive" = 1 ] && [ "$(cat "$D/slow.out")" = "$want" ] \
        && pass "$rt: an unrelated request is answered while exec.run is pending" \
        || fail "$rt: loop responsive" "ping=$ping alive=$alive slow=$(cat "$D/slow.out")"

    # exec_run_pipes.early_stdin_close
    out=$(get /early)
    [ "$out" = "status=0 stdout=ok" ] && pass "$rt: a command that closes its stdin early: the rest is dropped, the result normal" \
        || fail "$rt: early stdin close" "$out"

    # exec_stream.tick: the first line long before the end
    out=$(get /tick)
    case "$out" in
        "first=one first_ms="*" rest=two total_ms="*" status=0 len=8")
            fm=$(printf '%s' "$out" | sed 's/.*first_ms=\([0-9]*\).*/\1/')
            tm=$(printf '%s' "$out" | sed 's/.*total_ms=\([0-9]*\).*/\1/')
            [ "$fm" -lt 500 ] && [ "$tm" -ge 900 ] \
                && pass "$rt: exec.stream output arrives before the command ends (${fm} ms, end ${tm} ms)" \
                || fail "$rt: tick timing" "$out" ;;
        *) fail "$rt: tick" "$out" ;;
    esac

    # exec_stream.echo: 50 request / reply round trips, no coalescing delay
    out=$(get /echo)
    case "$out" in
        "ok=true ms="*" status=0")
            ms=$(printf '%s' "$out" | sed 's/.*ms=\([0-9]*\).*/\1/')
            [ "$ms" -lt 500 ] && pass "$rt: 50 stdin / stdout round trips in ${ms} ms (no 20 ms coalescing per reply)" \
                || fail "$rt: echo too slow" "$out" ;;
        *) fail "$rt: echo" "$out" ;;
    esac

    # exec_stream.total_cap
    out=$(get /flood)
    [ "$out" = "n=100000 cut=true signal=SIGKILL len=100000" ] \
        && pass "$rt: the stream's total cap: 100000 bytes, then killed" || fail "$rt: flood" "$out"

    # exec_stream.close
    out=$(get /close)
    case "$out" in
        "pid="*" after=nil signal=SIGKILL")
            pid=$(printf '%s' "$out" | sed 's/pid=\([0-9]*\).*/\1/')
            gone_within "$pid" && pass "$rt: close() kills the command; later reads end" \
                || fail "$rt: close left the command running" "$out" ;;
        *) fail "$rt: close" "$out" ;;
    esac

    # exec_stream.disconnect
    curl -s -m 1 "http://127.0.0.1:$PORT/hang" > /dev/null 2>&1
    pid=$(sed -n 's/.*silent pid=\([0-9]*\).*/\1/p' "$SLOG" | tail -1)
    if [ -n "$pid" ] && gone_within "$pid"; then
        pass "$rt: a client disconnect kills the command its stream was reading"
    else
        fail "$rt: disconnect" "pid=$pid $(tail -5 "$SLOG")"
    fi

    # exec_stream.ordinary_refused
    out=$(get /ordinary)
    case "$out" in "false"*"not a stream command"*) pass "$rt: exec.stream of an ordinary command is refused" ;;
        *) fail "$rt: ordinary" "$out" ;; esac

    # After all of it the server still runs commands.
    out=$(get /pipes)
    [ "$out" = "$want" ] && pass "$rt: exec.run still works afterwards" || fail "$rt: afterwards" "$out"
    stop_server

    # ── Review of PR 2: a server with the stream test hooks ──
    rm -f "$D/failwrite"
    export HULL_TEST_EXEC_WAKE_DELAY_MS=300 HULL_TEST_EXEC_FAIL_WRITE="$D/failwrite"
    start_server "$D" "$ENTRY"
    rc=$?
    unset HULL_TEST_EXEC_WAKE_DELAY_MS HULL_TEST_EXEC_FAIL_WRITE
    [ "$rc" -eq 0 ] || return

    # exec_stream_review.m2_parked_read_keeps_its_chunk: a second reader
    # while a read is parked is refused, even once the chunk is in.
    get /share/start > /dev/null
    get /share/a 10 > "$D/a.out" &
    apid=$!
    sleep 0.2
    get /share/write > /dev/null
    sleep 0.1                        # the chunk is in; A's wake is 300 ms off
    b=$(get /share/b 5)
    wait "$apid"
    a=$(cat "$D/a.out")
    get /share/close > /dev/null
    case "$a|$b" in
        "a=x|b-err="*busy*) pass "$rt: a parked read keeps its chunk; a second reader is refused busy" ;;
        *) fail "$rt: shared stream, two readers" "a=[$a] b=[$b]" ;;
    esac

    # exec_stream_review.h1_failed_credit_frees_the_call: a failed write to
    # the broker ends the call and frees its slot (concurrency 1 again).
    get /h1/start > /dev/null
    sleep 0.3
    : > "$D/failwrite"
    out=$(get /h1/go 10)
    [ "$out" = "got=q again=ok" ] \
        && pass "$rt: a stream whose write to the broker failed gives its slot back" \
        || fail "$rt: failed CREDIT write" "$out $(grep ERROR "$SLOG" | tail -3)"

    # exec_stream_review.l1_refused_write_sends_nothing: a write that would
    # wait inside a transaction is refused before anything is queued.
    out=$(get /l1 10)
    [ "$out" = "refused=wait_refused first=hello" ] \
        && pass "$rt: a write refused inside a transaction sends nothing" \
        || fail "$rt: write inside a transaction" "$out"

    # exec_stream_review.m1_write_parked_when_broker_lost (last: it kills
    # the broker): the parked write fails as stdin_closed in both runtimes.
    get /m1 10 > "$D/m1.out" &
    mpid=$!
    for i in $(seq 1 60); do grep -q 'm1 pid=' "$SLOG" && break; sleep 0.05; done
    sleep 0.3
    broker=$(sed -n 's/.*exec broker pid \([0-9]*\).*/\1/p' "$SLOG" | tail -1)
    [ -n "$broker" ] && kill -9 "$broker" 2>/dev/null
    wait "$mpid"
    out=$(cat "$D/m1.out")
    case "$out" in
        "ok=false err="*stdin_closed*) pass "$rt: a write parked when the broker is lost fails as stdin_closed" ;;
        *) fail "$rt: write parked, broker lost" "broker=$broker $out" ;;
    esac
    stop_server
}

# ── The paths phase: many streams at once, SIGTERM with streams open; run
# plain and again with --vm-thread (every entry on the VM thread). ──
phase_paths() {  # phase_paths <rt> <entry> <label> [extra flags]
    rt=$1; ENTRY=$2; L="$rt$3"
    EXTRA="--drain-timeout 300 ${4:-}"
    start_server "$D" "$ENTRY" || { EXTRA=; return; }
    EXTRA=
    if [ -n "${4:-}" ]; then
        grep -q 'VM thread on' "$SLOG" && pass "$L: the server runs its entries on the VM thread" \
            || fail "$L: no VM thread" "$(head -20 "$SLOG")"
        out=$(get /tick)
        case "$out" in "first=one first_ms="*" rest=two total_ms="*" status=0 len=8")
            pass "$L: exec.stream output, read on the VM thread" ;; *) fail "$L: tick" "$out" ;; esac
        out=$(get /echo)
        case "$out" in "ok=true ms="*" status=0") pass "$L: stdin / stdout round trips" ;;
            *) fail "$L: echo" "$out" ;; esac
        out=$(get /close)
        case "$out" in "pid="*" after=nil signal=SIGKILL")
            pid=$(printf '%s' "$out" | sed 's/pid=\([0-9]*\).*/\1/')
            gone_within "$pid" && pass "$L: close() kills the command" || fail "$L: close" "$out" ;;
            *) fail "$L: close" "$out" ;; esac
        curl -s -m 1 "http://127.0.0.1:$PORT/hang" > /dev/null 2>&1
        pid=$(sed -n 's/.*silent pid=\([0-9]*\).*/\1/p' "$SLOG" | tail -1)
        [ -n "$pid" ] && gone_within "$pid" && pass "$L: a client disconnect kills the command" \
            || fail "$L: disconnect" "pid=$pid"
    fi

    # exec_stream_paths.parallel: 8 streams at once, each a 10-step echo
    # with sleeps between, so they interleave; each sees only its own bytes.
    cps=""
    for i in 1 2 3 4 5 6 7 8; do get "/par?id=$i" 30 > "$D/par.$i" & cps="$cps $!"; done
    for c in $cps; do wait "$c"; done
    bad=""
    for i in 1 2 3 4 5 6 7 8; do [ "$(cat "$D/par.$i")" = "ok=true status=0" ] || bad="$bad $i=[$(cat "$D/par.$i")]"; done
    [ -z "$bad" ] && pass "$L: 8 streams at once, interleaved, each gets only its own output" \
        || fail "$L: parallel streams" "$bad"

    # exec_stream_paths.ninth_refused: 8 held open, the 9th refused busy;
    # once they are closed and reaped, streams start again.
    out=$(get /fill 20)
    pids=$(sed -n 's/.*held pids=\([0-9,]*\).*/\1/p' "$SLOG" | tail -1)
    [ "$out" = "opened=8 ninth=busy" ] && pass "$L: 8 streams held open, a 9th is refused busy" \
        || fail "$L: fill" "$out"
    get /fill/close > /dev/null
    all_gone "$pids" && pass "$L: closing them kills all 8 commands" || fail "$L: fill close" "pids=$pids"
    out=""
    for i in $(seq 1 20); do
        out=$(get "/par?id=again" 20)
        [ "$out" = "ok=true status=0" ] && break
        sleep 0.1
    done
    [ "$out" = "ok=true status=0" ] && pass "$L: streams start again once the slots are free" \
        || fail "$L: after fill" "$out"

    # exec_stream_paths.sigterm_open_streams: 3 streams held by the app, a
    # 4th parked in a request. SIGTERM: the server exits 0 within the drain
    # time and its teardown; every command and the broker are gone.
    out=$(get /hold 20)
    pids=$(sed -n 's/.*held pids=\([0-9,]*\).*/\1/p' "$SLOG" | tail -1)
    n0=$(grep -c 'silent pid=' "$SLOG")
    curl -s -m 20 "http://127.0.0.1:$PORT/hang" > /dev/null 2>&1 &
    hang=$!
    hp=$(new_silent "$n0")
    broker=$(sed -n 's/.*exec broker pid \([0-9]*\).*/\1/p' "$SLOG" | tail -1)
    sleep 0.2
    stop_timed
    wait "$hang" 2>/dev/null || true
    if [ "$out" = "held=3" ] && [ "$STOP_RC" -eq 0 ] && [ "$STOP_MS" -lt 5000 ] &&
       all_gone "$pids,$hp" && { [ -z "$broker" ] || gone_within "$broker"; } && clean_log "$SLOG"; then
        pass "$L: SIGTERM with 4 streams open exits 0 in ${STOP_MS} ms; every command and the broker gone"
    else
        fail "$L: SIGTERM with streams open" "out=$out rc=$STOP_RC ms=$STOP_MS pids=$pids,$hp broker=$broker
$(tail -8 "$SLOG")"
    fi
}

# ── hull dev: a reload (SIGTERM to the served child, then a new one) with a
# stream open; stopping hull dev with one open. ──
phase_dev() {  # phase_dev <rt> <entry>
    rt=$1; ENTRY=$2
    DD="$FIX/dev-$rt"
    rm -rf "$DD"; mkdir -p "$DD"
    cp "$D/$ENTRY" "$DD/$ENTRY"
    PORT=$((PORT + 1))
    SLOG="$DD/dev.log"
    (cd "$DD" && exec "$HULL" dev --no-types $DEGRADED -p "$PORT") > "$SLOG" 2>&1 &
    SPID=$!
    PIDS="$PIDS $SPID"
    up=0
    for i in $(seq 1 200); do [ "$(get /ver 2)" = ver-one ] && { up=1; break; }; sleep 0.05; done
    [ "$up" = 1 ] || { fail "$rt dev: hull dev did not start" "$(tail -20 "$SLOG")"; kill -9 "$SPID"; return; }
    child=$(pgrep -P "$SPID" | head -1)

    curl -s -m 30 "http://127.0.0.1:$PORT/hang" > /dev/null 2>&1 &
    hang=$!
    hp=$(new_silent 0)
    out=$(get /hold 20)
    pids=$(sed -n 's/.*held pids=\([0-9,]*\).*/\1/p' "$SLOG" | tail -1)
    broker=$(sed -n 's/.*exec broker pid \([0-9]*\).*/\1/p' "$SLOG" | tail -1)

    sleep 1.1   # hull dev compares whole-second mtimes
    sed 's/ver-one/ver-two/' "$DD/$ENTRY" > "$DD/$ENTRY.new" && mv "$DD/$ENTRY.new" "$DD/$ENTRY"
    re=0
    for i in $(seq 1 300); do [ "$(get /ver 2)" = ver-two ] && { re=1; break; }; sleep 0.05; done
    wait "$hang" 2>/dev/null || true
    if [ "$re" = 1 ] && [ "$out" = "held=3" ] && all_gone "$pids,$hp" &&
       { [ -z "$broker" ] || gone_within "$broker"; } && { [ -z "$child" ] || gone_within "$child"; }; then
        pass "$rt dev: a reload with 4 streams open ends the old server, its commands and its broker"
    else
        fail "$rt dev: reload with streams open" "re=$re out=$out pids=$pids,$hp broker=$broker child=$child
$(tail -15 "$SLOG")"
    fi
    out=$(get /echo)
    case "$out" in "ok=true ms="*" status=0") pass "$rt dev: the reloaded server runs streams" ;;
        *) fail "$rt dev: echo after reload" "$out" ;; esac

    out=$(get /hold 20)
    pids=$(sed -n 's/.*held pids=\([0-9,]*\).*/\1/p' "$SLOG" | tail -1)
    child=$(pgrep -P "$SPID" | head -1)
    stop_timed
    if [ "$out" = "held=3" ] && all_gone "$pids" && { [ -z "$child" ] || gone_within "$child"; } &&
       clean_log "$SLOG"; then
        pass "$rt dev: stopping hull dev with 3 streams open leaves no command behind (rc=$STOP_RC)"
    else
        fail "$rt dev: stop with streams open" "out=$out rc=$STOP_RC pids=$pids child=$child"
    fi
}

# ── --audit: a start and an end record per exec.run / exec.stream call,
# never the bytes (docs/design/exec_broker.md §12). ──
phase_audit() {  # phase_audit <rt> <entry>
    rt=$1; ENTRY=$2
    EXTRA="--audit"
    start_server "$D" "$ENTRY" || { EXTRA=; return; }
    EXTRA=
    get /tick > /dev/null
    get /pipes > /dev/null
    get /echo > /dev/null
    get /close > /dev/null
    stop_server
    out=$(python3 -I - "$SLOG" "$WANT_PIPES" <<'PYCHK'
import json, sys
recs = []
for line in open(sys.argv[1], errors="replace"):
    line = line.strip()
    if line.startswith("{") and '"cap":"exec.' in line:
        recs.append(json.loads(line))
bad = []
def one(cap, name, event):
    m = [r for r in recs if r["cap"] == cap and r["name"] == name and r["event"] == event]
    if len(m) != 1: bad.append("%s %s %s: %d records" % (cap, name, event, len(m)))
    return m[0] if m else {}
s, e = one("exec.stream", "tick", "start"), one("exec.stream", "tick", "end")
if s.get("args") != [] or s.get("stdin_bytes") != 0: bad.append("tick start %r" % s)
if (e.get("outcome"), e.get("status"), e.get("signal"), e.get("stdout_bytes"),
        e.get("timed_out"), e.get("id")) != ("done", 0, None, 8, False, s.get("id")) or \
        e.get("duration_ms", 0) < 900:
    bad.append("tick end %r" % e)
s, e = one("exec.run", "pipes", "start"), one("exec.run", "pipes", "end")
if s.get("args") != ["0"] or s.get("stdin_bytes") != 900000: bad.append("pipes start %r" % s)
if (e.get("outcome"), e.get("status"), e.get("stderr_bytes"), e.get("stdout_truncated")) != \
        ("done", 7, 300000, False) or e.get("stdout_bytes") != len(sys.argv[2]) + 1:
    bad.append("pipes end %r" % e)
e = one("exec.stream", "echo", "end")
if (e.get("outcome"), e.get("status"), e.get("stdin_bytes"), e.get("stdout_bytes")) != \
        ("done", 0, 141, 141):
    bad.append("echo end %r" % e)
e = one("exec.stream", "silent", "end")
if e.get("outcome") != "cancelled" or "status" in e: bad.append("close end %r" % e)
text = open(sys.argv[1], errors="replace").read()
for r in recs:
    j = json.dumps(r)
    if "IIIIIIII" in j or "EEEEEEEE" in j or sys.argv[2] in j: bad.append("bytes logged: %s" % j[:120])
print("ok %d" % len(recs) if not bad else "; ".join(bad))
PYCHK
)
    case "$out" in
        "ok 8") pass "$rt: --audit writes a start and an end record per call, with no output or stdin bytes" ;;
        *) fail "$rt: audit records" "$out" ;;
    esac
}

# ── The off-heap hold: a stream's queued chunks count against the VM's
# heap limit with its other off-heap copies (exec_stream.md §5.1). ──
phase_offheap() {  # phase_offheap <rt> <entry>
    rt=$1; ENTRY=$2
    EXTRA="-m 3m"
    start_server "$D" "$ENTRY" || { EXTRA=; return; }
    EXTRA=
    f1=$(get "/both?n=5" 60)
    a=$(get /both 60)
    b=$(get /both 60)
    f2=$(get "/both?n=5" 60)
    c=$(get /both1 60)
    # 8 unread streams queue 4 MiB: past the 3 MiB ceiling at least 2 end,
    # with the reason, and the others keep going. 5 (2.5 MiB) all fit, before
    # the over-limit rounds and after them: bytes left held would end one.
    ok=1
    for r in "$a" "$b"; do
        case "$r" in
            "ended=0 "*|"ended=1 "*|*" live=0 "*) ok=0 ;;
            "ended="*" live="*" msg="*"off-heap copies past its heap limit"*) ;;
            *) ok=0 ;;
        esac
    done
    [ "$ok" = 1 ] && [ "$f1" = "ended=0 live=5 msg=none" ] && [ "$f2" = "$f1" ] &&
        [ "$c" = "n_ok=true" ] \
        && pass "$rt: queued output counts against the heap limit: past it a stream ends, said so; nothing is left held" \
        || fail "$rt: off-heap hold" "five: $f1 / $f2
eight: $a / $b
one: $c"
    stop_server
}

run_paths() {  # run_paths <lua|js>
    rt=$1
    D="$FIX/$rt"
    if [ "$rt" = lua ]; then ENTRY=app.lua; else ENTRY=app.js; fi
    echo "== $rt: paths"
    if [ -n "${E2E_ONLY:-}" ]; then "phase_$E2E_ONLY" "$rt" "$ENTRY"; return; fi
    phase_paths "$rt" "$ENTRY" ""
    phase_paths "$rt" "$ENTRY" " vm-thread" "--vm-thread"
    phase_dev "$rt" "$ENTRY"
    phase_audit "$rt" "$ENTRY"
    phase_offheap "$rt" "$ENTRY"
}

run_suite lua
run_paths lua
run_suite js
run_paths js

echo
echo "e2e_exec_stream: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
