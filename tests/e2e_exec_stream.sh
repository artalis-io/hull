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
    (cd "$1" && exec "$HULL" $DEGRADED -p "$PORT" "$2" > "$SLOG" 2>&1) &
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

EXEC_LUA="
    pipes = { path = \"$P\", argv = { \"pipes\", { min = 1, max = 5, chars = \"0-9\" } },
              stdin_max = 1048576, stderr_max = 524288, timeout_ms = 20000 },
    early = { path = \"$P\", argv = { \"early\" }, stdin_max = 1048576 },
    tick = { path = \"$P\", argv = { \"tick\" }, stream = true, timeout_ms = 10000 },
    echo = { path = \"$P\", argv = { \"echo\" }, stream = true, timeout_ms = 0,
             stdin_max = 1073741824 },
    flood = { path = \"$P\", argv = { \"flood\" }, stream = true, timeout_ms = 10000,
              stdout_max = 100000 },
    silent = { path = \"$P\", argv = { \"silent\" }, stream = true, timeout_ms = 0 },
    deaf = { path = \"$P\", argv = { \"silent\" }, stream = true, timeout_ms = 0,
             stdin_max = 1073741824 },
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
    echo: { path: "$P", argv: ["echo"], stream: true, timeoutMs: 0, stdinMax: 1073741824 },
    flood: { path: "$P", argv: ["flood"], stream: true, timeoutMs: 10000, stdoutMax: 100000 },
    silent: { path: "$P", argv: ["silent"], stream: true, timeoutMs: 0 },
    deaf: { path: "$P", argv: ["silent"], stream: true, timeoutMs: 0, stdinMax: 1073741824 },
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
EOF
}

run_suite() {  # run_suite <lua|js>
    rt=$1
    D="$FIX/$rt"
    mkdir -p "$D"
    if [ "$rt" = lua ]; then lua_app "$D"; ENTRY=app.lua; else js_app "$D"; ENTRY=app.js; fi
    echo "== $rt"
    start_server "$D" "$ENTRY" || return

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

run_suite lua
run_suite js

echo
echo "e2e_exec_stream: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
