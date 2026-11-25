#!/bin/sh
# e2e_exec_broker_macos.sh: the command broker on macOS
# (docs/design/exec_broker.md §6.4), under the shipping Seatbelt profile.
#
# Needs a hull built with `make HL_EXEC_TEST_HOOKS=1`: a binary this user
# owns then passes the root-ownership check (no sudo here), and the server
# logs the broker's pid and a probe's readiness. Skips elsewhere, or fails
# with HULL_REQUIRE_EXEC_BROKER=1 (CI).
#
#   - the profile, seen from the exec'd program (tests/fixtures/
#     exec_probe_macos.c): no fork / posix_spawn, no AF_INET connect, no
#     read of the home directory, /etc readable, threads work;
#   - exec of an undeclared binary refused, a declared one that forks
#     (time(1)) refused its fork;
#   - unix_sockets: an AF_UNIX connect refused without it, allowed with it;
#   - the start report names what macOS cannot narrow ("exec: limit:");
#   - a timeout kills the child (SIGKILL, timed_out); output cut at the caps;
#   - stdin: 900 KiB round-trips through cat, a command that never reads it
#     ends, and one that never reads it and sleeps times out cleanly;
#   - a #! script, a set-user-ID binary and this hull binary itself are
#     refused at start;
#   - a sha256 pin: a match runs, a mismatch refuses the start, a binary
#     rewritten in place fails the next call; a replaced binary is ESTALE;
#   - orderly shutdown and server death take the running command; broker
#     death (SIGKILL) leaves it (no parent-death signal on macOS: the
#     documented limit) and the waiting call fails cleanly;
#   - admission by raw frames (tests/e2e_exec_frames.c).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; [ -n "$2" ] && printf '%s\n' "$2" | sed 's/^/        /'; }
skip() {
    if [ "${HULL_REQUIRE_EXEC_BROKER:-0}" = 1 ]; then echo "FAIL: $1"; exit 1; fi
    echo "SKIP: e2e_exec_broker_macos: $1"; exit 0
}

[ "$(uname -s)" = Darwin ] || skip "this suite is for macOS (Linux: e2e_exec_broker.sh)"
grep -aq 'exec probe ready pid' "$HULL" || skip "needs make HL_EXEC_TEST_HOOKS=1"
command -v cc >/dev/null 2>&1 || skip "needs a C compiler for the fixture"

# Every directory above a declared binary must be writable by its owner only
# (the safe-path rule), which rules out /tmp: the fixtures go in a private
# directory directly under $HOME.
case "$HOME" in /*) ;; *) skip "HOME is not an absolute path" ;; esac
FIX=$(mktemp -d "$HOME/.hull-e2e-exec.XXXXXX")
chmod 700 "$FIX"
PIDS=""
cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null || true; done
    case "$FIX" in "$HOME"/.hull-e2e-exec.*) rm -rf "$FIX" ;; esac
}
trap cleanup EXIT

cc -O2 -o "$FIX/exec_probe" "$ROOT/tests/fixtures/exec_probe_macos.c"
chmod 755 "$FIX/exec_probe"
PROBE="$FIX/exec_probe"
# Seatbelt matches the real path (/Users/... has no links, but say it once).
HOME_REAL=$(cd "$HOME" && pwd -P)

# run_main <dir> <app.lua body on stdin>: run an app.main app; $out gets its
# output. (No heredoc inside a command substitution: macOS /bin/sh is
# bash 3.2, which misparses one.)
run_main() {
    mkdir -p "$1"
    cat > "$1/app.lua"
    out=$( (cd "$1" && "$HULL" app.lua 2>&1) || true)
}
check_line() {  # check_line <label> <text expected in out>
    if printf '%s\n' "$out" | grep -qF -- "$2"; then pass "$1"; else fail "$1" "$out"; fi
}

# ── The profile, seen from inside ────────────────────────────────────

echo "== child sandbox"
run_main "$FIX/a1" <<EOF
app.manifest({
  modules = { "hull/exec@1" },
  exec = {
    probe = { path = "$PROBE", argv = { "probe", "$HOME_REAL" } },
    ["no-unix"] = { path = "$PROBE", argv = { "unix" } },
    env = { path = "/usr/bin/env", argv = { "/bin/ls", "/" } },
    time = { path = "/usr/bin/time", argv = { "/bin/echo", "x" } },
  },
})
local exec = require("hull.exec")
app.main(function()
  for _, n in ipairs({ "probe", "no-unix" }) do
    local r = exec.run(n)
    print(n .. ": " .. r.stdout:gsub("\n", " "))
  end
  for _, n in ipairs({ "env", "time" }) do
    local r = exec.run(n)
    print(n .. ": status=" .. r.status .. " " .. r.stderr:gsub("\n", " "))
  end
  return 0
end)
EOF
check_line "fork refused" "fork=EPERM"
check_line "posix_spawn refused" "posix_spawn=EPERM"
check_line "threads work" "thread=ok"
check_line "no network connect" "inet=EPERM"
check_line "the home directory is not readable" "home=EPERM"
check_line "/etc is readable" "etc=ok"
check_line "AF_UNIX connect refused without unix_sockets" "no-unix: unix=EPERM"
check_line "exec of an undeclared binary refused" "Operation not permitted"
check_line "a declared command cannot fork (time)" "time: status=1"
check_line "the start report names the shared sandbox" "exec: limit: macOS runs every command in ONE shared sandbox"
check_line "the start report names the missing parent-death signal" "exec: limit: no parent-death signal on macOS"

# unix_sockets on one command opens AF_UNIX to every command of the app
# (the union: macOS has no per-child narrowing).
run_main "$FIX/a1u" <<EOF
app.manifest({
  modules = { "hull/exec@1" },
  exec = {
    unix = { path = "$PROBE", argv = { "unix" }, unix_sockets = true },
    other = { path = "$PROBE", argv = { "unix" } },
  },
})
local exec = require("hull.exec")
app.main(function()
  for _, n in ipairs({ "unix", "other" }) do print(n .. ": " .. exec.run(n).stdout:gsub("\n", " ")) end
  return 0
end)
EOF
check_line "AF_UNIX connect allowed with unix_sockets" "unix: unix=ok"
check_line "...and to the app's other commands (the union)" "other: unix=ok"

# ── Timeout, truncation ──────────────────────────────────────────────

echo "== timeout, caps"
run_main "$FIX/a2" <<EOF
app.manifest({
  modules = { "hull/exec@1" },
  exec = {
    slow = { path = "$PROBE", argv = { "sleep" }, timeout_ms = 300 },
    spew = { path = "$PROBE", argv = { "spew" }, stdout_max = 1000, stderr_max = 100 },
  },
})
local exec = require("hull.exec")
app.main(function()
  local r = exec.run("slow")
  print(("slow: timed_out=%s signal=%s ms=%d"):format(tostring(r.timed_out), tostring(r.signal), r.duration_ms))
  r = exec.run("spew")
  print(("spew: out=%d cut=%s err=%d cut=%s status=%d"):format(#r.stdout, tostring(r.stdout_truncated),
        #r.stderr, tostring(r.stderr_truncated), r.status))
  return 0
end)
EOF
check_line "a timeout kills the child" "slow: timed_out=true signal=SIGKILL"
check_line "output cut at the caps, the child drained to its end" "spew: out=1000 cut=true err=100 cut=true status=0"

# ── stdin ─────────────────────────────────────────────────────────────

echo "== stdin"
run_main "$FIX/in1" <<EOF
app.manifest({
  modules = { "hull/exec@1" },
  exec = {
    cat = { path = "/bin/cat", stdin_max = 1048576, stdout_max = 1048576 },
    ["true"] = { path = "/usr/bin/true", stdin_max = 1048576 },
    slowin = { path = "$PROBE", argv = { "sleep" }, stdin_max = 1048576, timeout_ms = 300 },
  },
})
local exec = require("hull.exec")
app.main(function()
  local big = string.rep("0123456789abcdef", 57600)   -- 900 KiB: many pipe buffers
  local r = exec.run("cat", {}, { stdin = big })
  print(("cat: status=%d len=%d same=%s"):format(r.status, #r.stdout, tostring(r.stdout == big)))
  r = exec.run("true", {}, { stdin = big })
  print(("unread: status=%d timed_out=%s"):format(r.status, tostring(r.timed_out)))
  r = exec.run("slowin", {}, { stdin = big })
  print(("slowin: timed_out=%s signal=%s"):format(tostring(r.timed_out), tostring(r.signal)))
  return 0
end)
EOF
check_line "900 KiB of stdin reaches the command intact" "cat: status=0 len=921600 same=true"
check_line "a command that never reads its stdin still ends" "unread: status=0 timed_out=false"
check_line "a command that never reads its stdin times out cleanly" "slowin: timed_out=true signal=SIGKILL"

# ── The child manifest (exec.<name>.manifest, §3) ─────────────────────

# The command's grant is written exactly as the app's own fs grant (app-root
# relative), and the broker grants exactly the path it names.
echo "== child manifest"
CM="$FIX/cm"
mkdir -p "$CM/data" "$CM/secret" "$CM/out"
echo "granted-bytes" > "$CM/data/msg.txt"
echo "not-granted" > "$CM/secret/x.txt"
CMR=$(cd "$CM" && pwd -P)
run_main "$CM" <<EOF
app.manifest({
  modules = { "hull/exec@1", "hull/fs@1" },
  fs = { read = { "data/msg.txt" } },
  exec = {
    show = { path = "/bin/cat", argv = { "$CMR/data/msg.txt" },
             manifest = { fs = { read = { "data/msg.txt" } } } },
    peek = { path = "/bin/cat", argv = { "$CMR/secret/x.txt" },
             manifest = { fs = { read = { "data/msg.txt" } } } },
    save = { path = "/usr/bin/tee", argv = { "$CMR/out/w.txt" }, stdin_max = 64,
             manifest = { fs = { write = { "out/" } } } },
  },
})
local exec = require("hull.exec")
local fs = require("hull.fs")
app.main(function()
  print("app: " .. fs.read("data/msg.txt"))
  local r = exec.run("show")
  print(("show: status=%d out=%s"):format(r.status, r.stdout))
  r = exec.run("peek")
  print(("peek: status=%d"):format(r.status))
  r = exec.run("save", {}, { stdin = "written-by-child" })
  print(("save: status=%d"):format(r.status))
  return 0
end)
EOF
check_line "the app reads its own grant" "app: granted-bytes"
check_line "the command reads the same grant, written the same way" "show: status=0 out=granted-bytes"
check_line "a file beside the grant is not reachable" "peek: status=1"
check_line "a directory write grant lets the command create a file in it" "save: status=0"
if [ "$(cat "$CM/out/w.txt" 2>/dev/null)" = "written-by-child" ]; then
    pass "the file holds what the command wrote"
else
    fail "the file holds what the command wrote" "$out"
fi
run_main "$CM" <<EOF
app.manifest({ modules = { "hull/exec@1" }, exec = {
  del = { path = "/bin/rm", argv = { "$CMR/out/w.txt" },
          manifest = { fs = { write = { "out/" } } } } } })
local exec = require("hull.exec")
app.main(function()
  local r = exec.run("del")
  print(("del: status=%d"):format(r.status))
  return 0
end)
EOF
check_line "a write grant does not delete" "del: status=1"
run_main "$FIX/cm3" <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/db@1" }, exec = {
  a = { path = "/bin/cat", manifest = { fs = { write = { "data.db" } } } } } })
app.main(function() return 0 end)
EOF
check_line "a write grant to the app's database is refused at start" "is the app's database"
mkdir -p "$FIX/cm4/data"
run_main "$FIX/cm4" <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/db@1" },
  databases = { named = { other = "data/other.db" } },
  exec = { a = { path = "/bin/cat", manifest = { fs = { write = { "data/" } } } } } })
app.main(function() return 0 end)
EOF
check_line "a write grant holding a named database is refused at start" "contains the app's database"
# A first start: the database's directory does not exist yet.
run_main "$FIX/cm5" <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/db@1" },
  fs = { write = { "data/" } },
  databases = { named = { other = "data/sub/other.db" } },
  exec = { a = { path = "/bin/cat", manifest = { fs = { write = { "data/" } } } } } })
app.main(function() return 0 end)
EOF
check_line "a first start (the database directory not made yet) is refused" "contains the app's database"
run_main "$FIX/cm6" <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/db@1" },
  databases = { named = { d1 = "dbs/d1.db", d2 = "dbs/d2.db", d3 = "dbs/d3.db", d4 = "dbs/d4.db", d5 = "dbs/d5.db", d6 = "dbs/d6.db", d7 = "dbs/d7.db", d8 = "dbs/d8.db", d9 = "dbs/d9.db", d10 = "dbs/d10.db", d11 = "dbs/d11.db", d12 = "dbs/d12.db", d13 = "dbs/d13.db", d14 = "dbs/d14.db", d15 = "dbs/d15.db", d16 = "dbs/d16.db", d17 = "dbs/d17.db" } },
  exec = { a = { path = "/bin/cat", manifest = { fs = { write = { "out/" } } } } } })
app.main(function() return 0 end)
EOF
check_line "17 databases with a command write grant: refused, naming the limit" "protects at most 16"
run_main "$FIX/cm2" <<EOF
app.manifest({ modules = { "hull/exec@1" }, exec = {
  a = { path = "/bin/cat", manifest = { fs = { read = { "data/gone.txt" } } } } } })
app.main(function() return 0 end)
EOF
check_line "a missing grant refuses the start, naming the field" \
    "exec.a.manifest.fs.read[1] 'data/gone.txt' does not exist"

# ── Binary checks ────────────────────────────────────────────────────

echo "== binary checks"
printf '#!/bin/sh\necho hi\n' > "$FIX/script.sh"
chmod 755 "$FIX/script.sh"
run_main "$FIX/a3" <<EOF
app.manifest({ modules = { "hull/exec@1" }, exec = { s = { path = "$FIX/script.sh" } } })
app.main(function() return 0 end)
EOF
check_line "a #! script is refused at start" "is not a Mach-O executable"

cp "$PROBE" "$FIX/suid_probe"
chmod 4755 "$FIX/suid_probe"
run_main "$FIX/a4" <<EOF
app.manifest({ modules = { "hull/exec@1" }, exec = { s = { path = "$FIX/suid_probe" } } })
app.main(function() return 0 end)
EOF
check_line "a set-user-ID binary is refused at start" "set-user-ID"

cp "$HULL" "$FIX/hull_copy"
mkdir -p "$FIX/a5"
cat > "$FIX/a5/app.lua" <<EOF
app.manifest({ modules = { "hull/exec@1" }, exec = { s = { path = "$FIX/hull_copy" } } })
app.main(function() return 0 end)
EOF
out=$( (cd "$FIX/a5" && "$FIX/hull_copy" app.lua 2>&1) || true)
check_line "this hull binary itself is refused (it may fork)" "is this hull binary itself"

# ── The sha256 pin ──────────────────────────────────────────────────

echo "== sha256 pin"
PIN=$(shasum -a 256 "$PROBE" | cut -d' ' -f1)
PIN_UPPER=$(printf '%s' "$PIN" | tr a-f A-F)
run_main "$FIX/p1" <<EOF
app.manifest({ modules = { "hull/exec@1" },
  exec = { p = { path = "$PROBE", argv = { "unix" }, sha256 = "$PIN_UPPER" } } })
local exec = require("hull.exec")
app.main(function() print("pinned: " .. exec.run("p").stdout:gsub("\\n", "")) return 0 end)
EOF
check_line "a matching pin runs (either hex case)" "pinned: unix=EPERM"
WRONG=$(printf '%064d' 0)
run_main "$FIX/p2" <<EOF
app.manifest({ modules = { "hull/exec@1" },
  exec = { p = { path = "$PROBE", argv = { "unix" }, sha256 = "$WRONG" } } })
app.main(function() return 0 end)
EOF
check_line "a wrong pin refuses the app at start" "does not match its sha256 pin (its sha256 is $PIN)"

# ── Server-mode cases: ESTALE and the deaths ─────────────────────────

PORT=${PORT:-19862}
start_server() {  # start_server <dir> <app.lua body>  -> $SPID, $SLOG, $PORT
    PORT=$((PORT + 1))   # a fresh port each time: a stopping server may hold the last
    mkdir -p "$1"
    cat > "$1/app.lua"
    SLOG="$1/server.log"
    (cd "$1" && exec "$HULL" -p "$PORT" app.lua > "$SLOG" 2>&1) &
    SPID=$!
    PIDS="$PIDS $SPID"
    for i in $(seq 1 100); do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/" && return 0
        sleep 0.05
    done
    fail "server did not start" "$(cat "$SLOG")"
    return 1
}
log_pid() {  # log_pid <regex with one group> -> waits up to 5 s
    for i in $(seq 1 100); do
        p=$(sed -n "s/.*$1.*/\\1/p" "$SLOG" | tail -1)
        [ -n "$p" ] && { echo "$p"; return 0; }
        sleep 0.05
    done
    return 1
}
gone_within() {  # gone_within <pid> -> 0 when it is gone within 2 s
    for i in $(seq 1 40); do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.05
    done
    return 1
}
probe_app() {  # probe_app <path> [sha256]
    pin=""
    [ -n "${2:-}" ] && pin=", sha256 = \"$2\""
    cat <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/http-server@1" },
  exec = { p = { path = "$1", argv = { "unix" }$pin } } })
local exec = require("hull.exec")
app.get("/p", function(req, res)
  local ok, r = pcall(exec.run, "p")
  res:json({ ok = ok, out = ok and r.stdout or tostring(r) })
end)
EOF
}

echo "== replaced binary"
cp "$PROBE" "$FIX/swap_probe"
probe_app "$FIX/swap_probe" > "$FIX/s1.lua"
start_server "$FIX/s1" < "$FIX/s1.lua"
first=$(curl -s "http://127.0.0.1:$PORT/p")
cp "$PROBE" "$FIX/swap_probe.new" && mv -f "$FIX/swap_probe.new" "$FIX/swap_probe"
second=$(curl -s "http://127.0.0.1:$PORT/p")
case "$first" in *'"ok":true'*) pass "runs before the swap" ;; *) fail "runs before the swap" "$first" ;; esac
case "$second" in *"was replaced since the app started"*) pass "a replaced binary answers ESTALE" ;;
                  *) fail "a replaced binary answers ESTALE" "$second" ;; esac
kill -TERM "$SPID"; wait "$SPID" 2>/dev/null || true

echo "== pinned binary rewritten in place"
cp "$PROBE" "$FIX/pin_probe"
probe_app "$FIX/pin_probe" "$PIN" > "$FIX/s2.lua"
start_server "$FIX/s2" < "$FIX/s2.lua"
first=$(curl -s "http://127.0.0.1:$PORT/p")
printf 'x' >> "$FIX/pin_probe"   # same inode, new content
second=$(curl -s "http://127.0.0.1:$PORT/p")
case "$first" in *'"ok":true'*) pass "a pinned binary runs before the rewrite" ;;
                 *) fail "a pinned binary runs before the rewrite" "$first" ;; esac
case "$second" in *"does not match its sha256 pin"*) pass "a binary rewritten in place fails its pin" ;;
                  *) fail "a binary rewritten in place fails its pin" "$second" ;; esac
kill -TERM "$SPID"; wait "$SPID" 2>/dev/null || true

cat > "$FIX/ready_app.lua" <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/http-server@1" },
  exec = { ready = { path = "$PROBE", argv = { "ready" }, timeout_ms = 60000 } } })
local exec = require("hull.exec")
app.get("/ready", function(req, res)
  local ok, r = pcall(exec.run, "ready")
  res:json({ ok = ok, msg = ok and r.stdout or tostring(r) })
end)
EOF

death_case() {  # death_case <label> <what to kill: broker|term|server>
    start_server "$FIX/d_$2" < "$FIX/ready_app.lua" || return
    (curl -s -m 20 "http://127.0.0.1:$PORT/ready" > "$FIX/d_$2/resp" 2>&1 &)
    probe=$(log_pid 'exec probe ready pid=\([0-9]*\)') || { sleep 1; fail "$1: the probe never got ready" "$(cat "$SLOG"; echo "resp: $(cat "$FIX/d_$2/resp" 2>&1)")"; kill -9 "$SPID"; return; }
    PIDS="$PIDS $probe"
    broker=$(log_pid 'exec broker pid \([0-9]*\)')
    case "$2" in
        broker) kill -9 "$broker" ;;
        term)   kill -TERM "$SPID"; wait "$SPID" 2>/dev/null || true ;;
        server) kill -9 "$SPID" ;;
    esac
    if [ "$2" = broker ]; then
        # The documented limit: no parent-death signal on macOS, so the
        # command runs on; it cannot fork, and its CPU limit holds.
        sleep 0.3
        if kill -0 "$probe" 2>/dev/null; then pass "$1"; else fail "$1" "probe $probe is gone (the limit text is stale)"; fi
        kill -9 "$probe" 2>/dev/null || true
        case "$(cat "$FIX/d_$2/resp")" in
            *"command broker is gone"*) pass "the waiting call fails cleanly when the broker dies" ;;
            *) fail "the waiting call fails cleanly when the broker dies" "$(cat "$FIX/d_$2/resp")" ;;
        esac
        kill -TERM "$SPID" 2>/dev/null || true
    else
        if gone_within "$probe"; then pass "$1"; else fail "$1" "probe $probe still alive"; kill -9 "$probe" 2>/dev/null || true; fi
    fi
    wait "$SPID" 2>/dev/null || true
    gone_within "$broker" || fail "$1: the broker outlived the server"
}

echo "== deaths, after exec"
death_case "orderly shutdown (SIGTERM) takes the command" term
death_case "server death (kill -9) takes the broker and the command" server
death_case "broker death (kill -9) leaves the command (documented limit)" broker

# ── Admission by raw frames ──────────────────────────────────────────

echo "== admission (raw frames)"
cc -std=c11 -O1 -I"$ROOT/include" -I"$ROOT/vendor/keel/include" -o "$FIX/frames" \
    "$ROOT/tests/e2e_exec_frames.c" "$ROOT/src/hull/cap/exec.c" "$ROOT/fuzz/fuzz_exec_stubs.c"
if out=$("$FIX/frames" "$HULL" 2>&1); then
    PASS=$((PASS + $(printf '%s\n' "$out" | grep -c 'PASS:')))
    printf '%s\n' "$out" | grep 'PASS:'
else
    FAIL=$((FAIL + 1))
    printf '%s\n' "$out"
fi

echo
echo "e2e_exec_broker_macos: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
