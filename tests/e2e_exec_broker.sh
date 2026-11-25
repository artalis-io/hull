#!/bin/sh
# e2e_exec_broker.sh: the command broker on Linux (docs/design/exec_broker.md
# §5 - §8), under the shipping sandbox.
#
# Needs a hull built with `make HL_EXEC_TEST_HOOKS=1`: a binary this user
# owns then passes the root-ownership check (no sudo here), and the server
# logs the broker's pid and a probe's readiness. Skips elsewhere, or fails
# with HULL_REQUIRE_EXEC_BROKER=1 (CI).
#
#   - the child sandbox, seen from the exec'd program (tests/fixtures/
#     exec_probe.c): no fork / clone3 / AF_INET, no PDEATHSIG change or
#     credential change, threads work, PDEATHSIG is SIGKILL after exec;
#   - unix_sockets: AF_UNIX refused without it, allowed with it;
#   - a timeout kills the child (SIGKILL, timed_out); output is cut at the
#     caps while the child is drained to its end;
#   - a real `systemctl show` against the running system manager;
#   - a replaced binary answers ESTALE; a #! script and a set-user-ID
#     binary are refused at start;
#   - abrupt broker death, orderly shutdown and server death, each AFTER the
#     probe confirmed PDEATHSIG and said it was ready: the probe is gone;
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
    echo "SKIP: e2e_exec_broker: $1"; exit 0
}

[ "$(uname -s)" = Linux ] || skip "the broker runs on Linux only"
grep -aq 'exec probe ready pid' "$HULL" || skip "needs make HL_EXEC_TEST_HOOKS=1"
command -v cc >/dev/null 2>&1 || skip "needs a C compiler for the fixture"

# Landlock ABI: below 6 the broker refuses without --allow-degraded-sandbox.
ABI=$(python3 -c 'import ctypes; print(ctypes.CDLL(None, use_errno=True).syscall(444, None, 0, 1))' 2>/dev/null || echo 0)
DEGRADED=""
FRAMES_MODE=""
if [ "$ABI" -lt 6 ] 2>/dev/null; then
    echo "  (Landlock ABI $ABI: running with --allow-degraded-sandbox)"
    DEGRADED="--allow-degraded-sandbox"
    FRAMES_MODE="degraded"
fi

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

cc -O2 -pthread -o "$FIX/exec_probe" "$ROOT/tests/fixtures/exec_probe.c"
chmod 755 "$FIX/exec_probe"
PROBE="$FIX/exec_probe"

# run_main <dir> <app.lua body>: run an app.main app, print its output.
run_main() {
    mkdir -p "$1"
    cat > "$1/app.lua"
    (cd "$1" && "$HULL" $DEGRADED app.lua 2>&1) || true
}

# ── The child sandbox, seen from inside ──────────────────────────────

echo "== child sandbox"
out=$(run_main "$FIX/a1" <<EOF
app.manifest({
  modules = { "hull/exec@1" },
  exec = {
    probe = { path = "$PROBE", argv = { "probe" } },
    ["no-unix"] = { path = "$PROBE", argv = { "unix" } },
    unix = { path = "$PROBE", argv = { "unix" }, unix_sockets = true },
  },
})
local exec = require("hull.exec")
app.main(function()
  for _, n in ipairs({ "probe", "no-unix", "unix" }) do
    local r = exec.run(n)
    print(n .. ": " .. r.stdout:gsub("\n", " "))
  end
  return 0
end)
EOF
)
check_line() {  # check_line <label> <text expected in out>
    if printf '%s\n' "$out" | grep -qF -- "$2"; then pass "$1"; else fail "$1" "$out"; fi
}
check_line "fork refused" "fork=EPERM"
check_line "clone3 answered ENOSYS (glibc falls back to clone)" "clone3=ENOSYS"
check_line "threads work" "thread=ok"
check_line "no network socket" "inet=EACCES"
check_line "PDEATHSIG cannot be cleared" "set_pdeathsig=EPERM"
check_line "no credential change" "setresuid=EPERM"
check_line "no setgroups" "setgroups=EPERM"
check_line "PDEATHSIG is SIGKILL after exec" "pdeathsig=9"
check_line "AF_UNIX refused without unix_sockets" "no-unix: unix=EACCES"
check_line "AF_UNIX allowed with unix_sockets" "unix: unix=ok"

# ── Timeout, truncation, a real systemctl ────────────────────────────

echo "== timeout, caps, systemctl"
HAVE_SYSTEMD=0
[ -d /run/systemd/system ] && [ -x /usr/bin/systemctl ] && HAVE_SYSTEMD=1
out=$(run_main "$FIX/a2" <<EOF
app.manifest({
  modules = { "hull/exec@1" },
  exec = {
    slow = { path = "$PROBE", argv = { "sleep" }, timeout_ms = 300 },
    spew = { path = "$PROBE", argv = { "spew" }, stdout_max = 1000, stderr_max = 100 },
    unit = { path = "/usr/bin/systemctl",
             argv = { "show", "--no-pager", "-p", "ActiveState", "--",
                      { suffix = ".service", chars = "a-z0-9-", max = 64 } },
             unix_sockets = true },
  },
})
local exec = require("hull.exec")
app.main(function()
  local r = exec.run("slow")
  print(("slow: timed_out=%s signal=%s ms=%d"):format(tostring(r.timed_out), tostring(r.signal), r.duration_ms))
  r = exec.run("spew")
  print(("spew: out=%d cut=%s err=%d cut=%s status=%d"):format(#r.stdout, tostring(r.stdout_truncated),
        #r.stderr, tostring(r.stderr_truncated), r.status))
  if $HAVE_SYSTEMD == 1 then
    r = exec.run("unit", { "systemd-journald.service" })
    print("unit: " .. r.stdout:gsub("\n", " "))
  end
  return 0
end)
EOF
)
check_line "a timeout kills the child" "slow: timed_out=true signal=SIGKILL"
check_line "output cut at the caps, the child drained to its end" "spew: out=1000 cut=true err=100 cut=true status=0"
if [ "$HAVE_SYSTEMD" = 1 ]; then
    check_line "a real systemctl show against the system manager" "unit: ActiveState=active"
else
    echo "  (no running systemd here: systemctl check skipped)"
fi

# ── Binary checks ────────────────────────────────────────────────────

echo "== binary checks"
printf '#!/bin/sh\necho hi\n' > "$FIX/script.sh"
chmod 755 "$FIX/script.sh"
out=$(run_main "$FIX/a3" <<EOF
app.manifest({ modules = { "hull/exec@1" }, exec = { s = { path = "$FIX/script.sh" } } })
app.main(function() return 0 end)
EOF
)
check_line "a #! script is refused at start" "is not an ELF executable"

cp "$PROBE" "$FIX/suid_probe"
chmod 4755 "$FIX/suid_probe"
out=$(run_main "$FIX/a4" <<EOF
app.manifest({ modules = { "hull/exec@1" }, exec = { s = { path = "$FIX/suid_probe" } } })
app.main(function() return 0 end)
EOF
)
check_line "a set-user-ID binary is refused at start" "set-user-ID"

# ── Server-mode cases: ESTALE and the three deaths ───────────────────

PORT=${PORT:-19861}
start_server() {  # start_server <dir> <app.lua body>  -> $SPID, $SLOG
    mkdir -p "$1"
    cat > "$1/app.lua"
    SLOG="$1/server.log"
    (cd "$1" && exec "$HULL" $DEGRADED -p "$PORT" app.lua > "$SLOG" 2>&1) &
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

echo "== replaced binary"
cp "$PROBE" "$FIX/swap_probe"
start_server "$FIX/s1" <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/http-server@1" },
  exec = { p = { path = "$FIX/swap_probe", argv = { "unix" } } } })
local exec = require("hull.exec")
app.get("/p", function(req, res)
  local ok, r = pcall(exec.run, "p")
  res:json({ ok = ok, out = ok and r.stdout or tostring(r) })
end)
EOF
first=$(curl -s "http://127.0.0.1:$PORT/p")
cp "$PROBE" "$FIX/swap_probe.new" && mv -f "$FIX/swap_probe.new" "$FIX/swap_probe"
second=$(curl -s "http://127.0.0.1:$PORT/p")
case "$first" in *'"ok":true'*) pass "runs before the swap" ;; *) fail "runs before the swap" "$first" ;; esac
case "$second" in *"was replaced since the app started"*) pass "a replaced binary answers ESTALE" ;;
                  *) fail "a replaced binary answers ESTALE" "$second" ;; esac
kill -TERM "$SPID"; wait "$SPID" 2>/dev/null || true

# The probe confirms PR_GET_PDEATHSIG == SIGKILL as the exec'd program,
# then prints its readiness, which the broker reports (PROBE frame) while
# the call is still running.
READY_APP=$(cat <<EOF
app.manifest({ modules = { "hull/exec@1", "hull/http-server@1" },
  exec = { ready = { path = "$PROBE", argv = { "ready" }, timeout_ms = 60000 } } })
local exec = require("hull.exec")
app.get("/ready", function(req, res)
  local ok, r = pcall(exec.run, "ready")
  res:json({ ok = ok, msg = ok and r.stdout or tostring(r) })
end)
EOF
)

printf '%s\n' "$READY_APP" > "$FIX/ready_app.lua"
death_case() {  # death_case <label> <what to kill: broker|term|server>
    start_server "$FIX/d_$2" < "$FIX/ready_app.lua" || return
    (curl -s -m 20 "http://127.0.0.1:$PORT/ready" > "$FIX/d_$2/resp" 2>&1 &)
    probe=$(log_pid 'exec probe ready pid=\([0-9]*\)') || { fail "$1: the probe never got ready" "$(cat "$SLOG")"; kill -9 "$SPID"; return; }
    broker=$(log_pid 'exec broker pid \([0-9]*\)')
    case "$2" in
        broker) kill -9 "$broker" ;;
        # An orderly stop drains the in-flight request first (the probe sleeps
        # 30 s): wait for the server to exit, then the probe must be gone.
        term)   kill -TERM "$SPID"; wait "$SPID" 2>/dev/null || true ;;
        server) kill -9 "$SPID" ;;
    esac
    if gone_within "$probe"; then pass "$1"; else fail "$1" "probe $probe still alive"; kill -9 "$probe" 2>/dev/null || true; fi
    if [ "$2" = broker ]; then
        sleep 0.3
        case "$(cat "$FIX/d_$2/resp")" in
            *"command broker is gone"*) pass "the waiting call fails cleanly when the broker dies" ;;
            *) fail "the waiting call fails cleanly when the broker dies" "$(cat "$FIX/d_$2/resp")" ;;
        esac
        kill -TERM "$SPID" 2>/dev/null || true
    fi
    wait "$SPID" 2>/dev/null || true
    gone_within "$broker" || fail "$1: the broker outlived the server"
}

echo "== deaths, after exec"
death_case "abrupt broker death (kill -9) takes the probe" broker
death_case "orderly shutdown (SIGTERM) takes the probe" term
death_case "server death (kill -9) takes the broker and the probe" server

# ── Admission by raw frames ──────────────────────────────────────────

echo "== admission (raw frames)"
cc -std=c11 -O1 -I"$ROOT/include" -I"$ROOT/vendor/keel/include" -o "$FIX/frames" \
    "$ROOT/tests/e2e_exec_frames.c" "$ROOT/src/hull/cap/exec.c" "$ROOT/fuzz/fuzz_exec_stubs.c"
if out=$("$FIX/frames" "$HULL" $FRAMES_MODE 2>&1); then
    PASS=$((PASS + $(printf '%s\n' "$out" | grep -c 'PASS:')))
    printf '%s\n' "$out" | grep 'PASS:'
else
    FAIL=$((FAIL + 1))
    printf '%s\n' "$out"
fi

echo
echo "e2e_exec_broker: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
