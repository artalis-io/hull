#!/bin/sh
# E2E sandbox tests - verify pledge/unveil enforcement
#
# Tests:
#   1. Hull with manifest logs "[sandbox] applied" (Linux/cosmo only)
#   2. Hull without manifest - default-deny sandbox still applied
#   3. Manifest without hosts - no dns promise
#   4. JS manifest app - sandbox applied (feature parity)
#   5. Phase 1 pledge - applied before load_app, before phase 2
#   6. Kernel enforcement: pledge/unveil violations blocked (Linux only)
#
# Usage: sh tests/e2e_sandbox.sh
#        make e2e-sandbox
#
# Environment:
#   HULL     - path to hull binary (default: ./build/hull)
#   BUILDDIR - build directory for pledge objects (default: ./build)
#
# Returns 0 if all tests pass, 1 on failure.
# Tests skip gracefully on platforms without kernel sandbox.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$SRCDIR/build/hull}"
BUILDDIR="${BUILDDIR:-$SRCDIR/build}"
PASS=0
FAIL=0
SKIP=0
WORKDIR=""
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
    if [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ]; then
        rm -rf "$WORKDIR"
    fi
}
trap cleanup EXIT

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

skip() {
    echo "  SKIP: $1"
    SKIP=$((SKIP + 1))
}

check_contains() {
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 - expected '$3'" ;;
    esac
}

check_not_contains() {
    case "$2" in
        *"$3"*) fail "$1 - unexpected '$3'" ;;
        *)      pass "$1" ;;
    esac
}

wait_for_server() {
    for _i in 1 2 3 4 5 6 7 8 9 10; do
        if curl -s "http://127.0.0.1:$1/" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    echo "  server did not start on port $1"
    return 1
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

# ── Preconditions ────────────────────────────────────────────────────

if [ ! -x "$HULL" ]; then
    echo "e2e_sandbox: hull binary not found at $HULL - run 'make' first"
    exit 1
fi

WORKDIR=$(mktemp -d)

# Detect platform capabilities
UNAME_S=$(uname -s)
SANDBOX_EXPECTED=0
SANDBOX_TYPE=""
if [ "$UNAME_S" = "Linux" ]; then
    SANDBOX_EXPECTED=1
    SANDBOX_TYPE="pledge"
elif [ "$UNAME_S" = "Darwin" ]; then
    SANDBOX_EXPECTED=1
    SANDBOX_TYPE="seatbelt"
elif [ "$UNAME_S" = "OpenBSD" ]; then
    SANDBOX_EXPECTED=1
    SANDBOX_TYPE="pledge"
fi
# Cosmopolitan binaries report Linux on Linux, so check if hull was built
# with cosmocc by looking for the APE magic or platform_cc marker.
#
# A cosmo build only ENFORCES where Cosmopolitan's pledge/unveil have a
# mechanism to enforce with - Linux and OpenBSD. The same APE on Windows or a
# BSD reports "no kernel sandbox on this host" and runs on the capability layer
# alone (see sb_supported() in src/hull/sandbox.c), so expecting a sandbox
# there would assert a protection that deliberately is not claimed. The
# UNAME_S test above has already set the expectation for the hosts that do
# enforce, so this only needs to avoid overriding it on the ones that do not.
if [ -f "$BUILDDIR/platform_cc" ]; then
    PLATFORM_CC=$(cat "$BUILDDIR/platform_cc" 2>/dev/null)
    case "$PLATFORM_CC" in
        *cosmocc*)
            case "$UNAME_S" in
                Linux|OpenBSD) SANDBOX_EXPECTED=1; SANDBOX_TYPE="pledge" ;;
                *)             SANDBOX_EXPECTED=0; SANDBOX_TYPE="" ;;
            esac
            ;;
    esac
fi

# ── Test 1: Hull with manifest app - sandbox applied ────────────────

echo ""
echo "=== Test 1: Hull with manifest - sandbox log ==="

cat > "$WORKDIR/app.lua" << 'EOF'
app.manifest({
    modules = {"hull/http-server@1"},
    fs = { read = {"data"}, write = {"logs"} },
    env = {"PORT"},
    hosts = {"example.com"},
})

app.get("/", function(req, res)
    res:json({status = "ok"})
end)

app.get("/health", function(req, res)
    res:json({status = "ok"})
end)
EOF

LOGFILE="$WORKDIR/hull_sandbox.log"
"$HULL" -p 19880 -d "$WORKDIR/test.db" "$WORKDIR/app.lua" >"$LOGFILE" 2>&1 &
SERVER_PID=$!

if wait_for_server 19880; then
    # Verify HTTP still works
    RESP=$(curl -s "http://127.0.0.1:19880/health")
    check_contains "HTTP works after sandbox" "$RESP" "ok"

    stop_server

    # Check log output
    LOG=$(cat "$LOGFILE")

    if [ "$SANDBOX_EXPECTED" = "1" ]; then
        check_contains "sandbox applied log" "$LOG" "[sandbox] applied"
        if [ "$SANDBOX_TYPE" = "seatbelt" ]; then
            check_contains "sandbox mentions seatbelt" "$LOG" "seatbelt:"
            check_contains "sandbox network-outbound (hosts declared)" "$LOG" "network-outbound"
        else
            check_contains "sandbox mentions pledge" "$LOG" "pledge:"
            check_contains "sandbox dns promise (hosts declared)" "$LOG" "dns"
        fi
    else
        check_contains "sandbox not available log" "$LOG" "NO kernel sandbox on this host"
    fi
else
    stop_server
    fail "hull did not start with manifest app"
fi

# ── Test 2: Hull without manifest - default-deny sandbox ────────────

echo ""
echo "=== Test 2: Hull without manifest - default-deny sandbox ==="

cat > "$WORKDIR/nomanifest.lua" << 'EOF'
app.manifest({ modules = {"hull/http-server@1"} })

app.get("/", function(req, res)
    res:json({status = "ok"})
end)

app.get("/health", function(req, res)
    res:json({status = "ok"})
end)
EOF

LOGFILE2="$WORKDIR/hull_nosandbox.log"
"$HULL" -p 19881 -d "$WORKDIR/test2.db" "$WORKDIR/nomanifest.lua" >"$LOGFILE2" 2>&1 &
SERVER_PID=$!

if wait_for_server 19881; then
    RESP=$(curl -s "http://127.0.0.1:19881/health")
    check_contains "HTTP works without manifest" "$RESP" "ok"

    stop_server

    LOG2=$(cat "$LOGFILE2")
    if [ "$SANDBOX_EXPECTED" = "1" ]; then
        check_contains "sandbox applied (no manifest)" "$LOG2" "[sandbox] applied"
        if [ "$SANDBOX_TYPE" = "seatbelt" ]; then
            check_not_contains "no network-outbound without manifest" "$LOG2" "network-outbound"
        else
            check_not_contains "no dns without manifest" "$LOG2" " dns"
        fi
    else
        check_contains "sandbox not available log" "$LOG2" "NO kernel sandbox on this host"
    fi
else
    stop_server
    fail "hull did not start without manifest"
fi

# ── Test 3: Manifest without hosts - no dns promise ─────────────────

echo ""
echo "=== Test 3: Manifest without hosts - no dns promise ==="

cat > "$WORKDIR/nohosts.lua" << 'EOF'
app.manifest({
    modules = {"hull/http-server@1"},
    fs = { read = {"data"} },
    env = {"PORT"},
})

app.get("/", function(req, res)
    res:json({status = "ok"})
end)

app.get("/health", function(req, res)
    res:json({status = "ok"})
end)
EOF

LOGFILE3="$WORKDIR/hull_nohosts.log"
"$HULL" -p 19882 -d "$WORKDIR/test3.db" "$WORKDIR/nohosts.lua" >"$LOGFILE3" 2>&1 &
SERVER_PID=$!

if wait_for_server 19882; then
    stop_server

    LOG3=$(cat "$LOGFILE3")
    if [ "$SANDBOX_EXPECTED" = "1" ]; then
        check_contains "sandbox applied (no hosts)" "$LOG3" "[sandbox] applied"
        if [ "$SANDBOX_TYPE" = "seatbelt" ]; then
            check_not_contains "no network-outbound without hosts" "$LOG3" "network-outbound"
        else
            check_not_contains "no dns promise without hosts" "$LOG3" " dns"
        fi
    else
        skip "dns promise check - no kernel sandbox on this platform"
    fi
else
    stop_server
    fail "hull did not start with no-hosts manifest"
fi

# ── Test 4: JS manifest app - sandbox applied ────────────────────────

echo ""
echo "=== Test 4: JS manifest app - sandbox log ==="

cat > "$WORKDIR/jsapp.js" << 'EOF'
import { app } from 'hull:app';

app.manifest({
    modules: ["hull/http-server@1"],
    fs: { read: ["data"], write: ["logs"] },
    env: ["PORT"],
    hosts: ["example.com"],
});

app.get("/", (req, res) => {
    res.json({status: "ok"});
});

app.get("/health", (req, res) => {
    res.json({status: "ok"});
});
EOF

LOGFILE4="$WORKDIR/hull_js_sandbox.log"
"$HULL" -p 19883 -d "$WORKDIR/test4.db" "$WORKDIR/jsapp.js" >"$LOGFILE4" 2>&1 &
SERVER_PID=$!

if wait_for_server 19883; then
    RESP=$(curl -s "http://127.0.0.1:19883/health")
    check_contains "JS HTTP works after sandbox" "$RESP" "ok"

    stop_server

    LOG4=$(cat "$LOGFILE4")

    if [ "$SANDBOX_EXPECTED" = "1" ]; then
        check_contains "JS sandbox applied log" "$LOG4" "[sandbox] applied"
        if [ "$SANDBOX_TYPE" = "seatbelt" ]; then
            check_contains "JS sandbox mentions seatbelt" "$LOG4" "seatbelt:"
            check_contains "JS sandbox network-outbound (hosts declared)" "$LOG4" "network-outbound"
        else
            check_contains "JS sandbox mentions pledge" "$LOG4" "pledge:"
            check_contains "JS sandbox dns promise (hosts declared)" "$LOG4" "dns"
        fi
    else
        check_contains "JS sandbox not available log" "$LOG4" "NO kernel sandbox on this host"
    fi
else
    stop_server
    fail "hull did not start with JS manifest app"
fi

# ── Test 5: Phase 1 pledge - log appears before load_app ──────────────

echo ""
echo "=== Test 5: Phase 1 pledge - applied before load_app ==="

LOGFILE5="$WORKDIR/hull_phase1.log"
"$HULL" -p 19884 -d "$WORKDIR/test5.db" "$WORKDIR/app.lua" >"$LOGFILE5" 2>&1 &
SERVER_PID=$!

if wait_for_server 19884; then
    stop_server

    LOG5=$(cat "$LOGFILE5")

    if [ "$SANDBOX_EXPECTED" = "1" ]; then
        if [ "$SANDBOX_TYPE" = "seatbelt" ]; then
            check_contains "phase 1 skipped log" "$LOG5" "phase 1 skipped"
        else
            check_contains "phase 1 pledge log" "$LOG5" "phase 1 pledge applied"
        fi
        check_contains "phase 2 sandbox log" "$LOG5" "[sandbox] applied"

        # Verify phase 1 log appears before phase 2 (line-number ordering)
        if [ "$SANDBOX_TYPE" = "seatbelt" ]; then
            P1_LINE=$(grep -n "phase 1 skipped" "$LOGFILE5" | head -1 | cut -d: -f1)
        else
            P1_LINE=$(grep -n "phase 1 pledge applied" "$LOGFILE5" | head -1 | cut -d: -f1)
        fi
        P2_LINE=$(grep -n "\[sandbox\] applied" "$LOGFILE5" | head -1 | cut -d: -f1)
        if [ -n "$P1_LINE" ] && [ -n "$P2_LINE" ] && [ "$P1_LINE" -lt "$P2_LINE" ]; then
            pass "phase 1 before phase 2 (line $P1_LINE < $P2_LINE)"
        else
            fail "phase 1 should appear before phase 2 (got $P1_LINE vs $P2_LINE)"
        fi
    else
        check_contains "phase 1 not available log" "$LOG5" "NO kernel sandbox on this host"
    fi
else
    stop_server
    fail "hull did not start for phase 1 test"
fi

# ── Test 6: Kernel enforcement (Linux only) ──────────────────────────

echo ""
echo "=== Test 6: Kernel enforcement (sandbox violations) ==="

if [ "$UNAME_S" != "Linux" ] && [ "$UNAME_S" != "Darwin" ] && [ "$UNAME_S" != "OpenBSD" ]; then
    skip "kernel enforcement test - Linux/macOS/OpenBSD only"
elif [ ! -f "$SRCDIR/tests/sandbox_violation.c" ]; then
    fail "sandbox_violation.c not found"
else
    CC="${CC:-cc}"
    SANDBOX_TEST="$BUILDDIR/sandbox_test"
    COMPILE_OK=0

    # Detect cosmocc: pledge/unveil are built-in, no polyfill needed
    IS_COSMO=0
    case "$CC" in
        *cosmocc*) IS_COSMO=1 ;;
    esac
    if [ "$IS_COSMO" = "0" ] && [ -f "$BUILDDIR/platform_cc" ]; then
        case "$(cat "$BUILDDIR/platform_cc" 2>/dev/null)" in
            *cosmocc*) CC=cosmocc; IS_COSMO=1 ;;
        esac
    fi

    if [ "$IS_COSMO" = "1" ]; then
        # Cosmopolitan: pledge/unveil built-in, no polyfill objects needed
        if $CC -std=c11 -O2 -o "$SANDBOX_TEST" \
                "$SRCDIR/tests/sandbox_violation.c" 2>"$WORKDIR/compile.log"; then
            pass "sandbox_violation.c compiled (cosmocc)"
            COMPILE_OK=1
        else
            COMPILE_ERR=$(cat "$WORKDIR/compile.log")
            fail "sandbox_violation.c compilation failed (cosmocc): $COMPILE_ERR"
        fi
    elif [ "$UNAME_S" = "Darwin" ]; then
        # macOS: link against system sandbox framework
        if $CC -std=c11 -O2 -o "$SANDBOX_TEST" \
                "$SRCDIR/tests/sandbox_violation.c" 2>"$WORKDIR/compile.log"; then
            pass "sandbox_violation.c compiled (macOS)"
            COMPILE_OK=1
        else
            COMPILE_ERR=$(cat "$WORKDIR/compile.log")
            fail "sandbox_violation.c compilation failed (macOS): $COMPILE_ERR"
        fi
    elif [ "$UNAME_S" = "OpenBSD" ]; then
        # OpenBSD: pledge/unveil are native - no extra libs needed
        if $CC -std=c11 -O2 -o "$SANDBOX_TEST" \
                "$SRCDIR/tests/sandbox_violation.c" 2>"$WORKDIR/compile.log"; then
            pass "sandbox_violation.c compiled (OpenBSD)"
            COMPILE_OK=1
        else
            COMPILE_ERR=$(cat "$WORKDIR/compile.log")
            fail "sandbox_violation.c compilation failed (OpenBSD): $COMPILE_ERR"
        fi
    else
        # Native Linux: link against pledge polyfill objects. Anchor the glob to
        # the TOP-LEVEL pledge dir ("$BUILDDIR/pledge*") so it never picks up a
        # sub-build's copy (build/tlsless, build/sqliteless, build/flavor-*, ...),
        # which would link two sets and fail with "multiple definition of ...".
        PLEDGE_OBJS=$(find "$BUILDDIR" -path "$BUILDDIR/pledge*" -name '*.o' 2>/dev/null | tr '\n' ' ')

        if [ -z "$PLEDGE_OBJS" ]; then
            fail "pledge polyfill objects not found in $BUILDDIR"
        elif $CC -std=c11 -O2 -o "$SANDBOX_TEST" \
                "$SRCDIR/tests/sandbox_violation.c" \
                $PLEDGE_OBJS -lpthread 2>"$WORKDIR/compile.log"; then
            pass "sandbox_violation.c compiled"
            COMPILE_OK=1
        else
            COMPILE_ERR=$(cat "$WORKDIR/compile.log")
            fail "sandbox_violation.c compilation failed: $COMPILE_ERR"
        fi
    fi

    if [ "$COMPILE_OK" = "1" ]; then
        ENFORCE_OUT=$("$SANDBOX_TEST" 2>&1); RC=$?
        echo "$ENFORCE_OUT" | while IFS= read -r line; do
            echo "    $line"
        done

        if [ "$RC" = "0" ]; then
            pass "kernel enforcement tests passed"
        else
            fail "kernel enforcement tests failed (exit $RC)"
        fi
    fi

    # audit 42: the same program with unveil.c rebuilt to skip Landlock TSYNC,
    # so the per-thread self-restriction path (vendor/pledge patch 0004) is
    # exercised whatever the kernel - a CI kernel without TSYNC takes it
    # anyway, a newer one would otherwise only test TSYNC.
    if [ "$COMPILE_OK" = "1" ] && [ "$UNAME_S" = "Linux" ] && [ "$IS_COSMO" = "0" ]; then
        SANDBOX_TEST2="$BUILDDIR/sandbox_test_no_tsync"
        PLEDGE_OBJS2=$(printf '%s\n' $PLEDGE_OBJS | grep -v '/unveil\.o$' | tr '\n' ' ')
        if $CC -std=c11 -O2 -D_GNU_SOURCE -DHULL_UNVEIL_TEST_NO_TSYNC=1 -w \
                -I"$SRCDIR/vendor/pledge" -o "$SANDBOX_TEST2" \
                "$SRCDIR/tests/sandbox_violation.c" \
                "$SRCDIR/vendor/pledge/libc/calls/unveil.c" \
                $PLEDGE_OBJS2 -lpthread 2>"$WORKDIR/compile2.log"; then
            OUT2=$("$SANDBOX_TEST2" 2>&1); RC2=$?
            printf '%s\n' "$OUT2" | grep -E "audit42|audit43|sync mode|sigpending|every round" | sed 's/^/    /'
            if [ "$RC2" = "0" ] && printf '%s' "$OUT2" | grep -q "PASS: sandbox_audit42.preexisting_thread_landlock"; then
                pass "sandbox_audit42.per_thread_landlock_path (no TSYNC)"
            else
                fail "sandbox_audit42.per_thread_landlock_path (exit $RC2)"
            fi
            # audit 43: on the signal path a send that fails for any reason
            # but ESRCH (here EAGAIN, RLIMIT_SIGPENDING at 0) leaves a missed
            # thread, and the round cap ending the loop is a failure.
            for t43 in sigpending_exhausted round_cap_fails; do
                if printf '%s\n' "$OUT2" | grep -q "PASS: sandbox_audit43.$t43\$"; then
                    pass "sandbox_audit43.$t43 (no TSYNC)"
                else
                    fail "sandbox_audit43.$t43 (no TSYNC)"
                fi
            done
        else
            fail "sandbox_audit42 no-TSYNC build failed: $(cat "$WORKDIR/compile2.log")"
        fi
    fi
fi

# ── Test 7: CLI app.main writing to a real TTY under the sandbox ──────
#
# glibc stdio runs isatty() -> ioctl(TCGETS) on first write to a
# character device (a real tty). The sandbox grants the `tty` pledge
# promise (which covers that ioctl) only when a terminal is present or
# the app is a TUI; a plain app.main printing to a terminal must still
# work. The bug is invisible under a pipe (glibc skips the isatty probe
# for non-char-device fds), which is why it escaped CI - so this test
# drives hull under a PTY. Native-Linux-only: that is the platform whose
# seccomp filter SIGKILLs the ioctl (macOS seatbelt returns EPERM from
# isatty - graceful "not a tty", no crash; cosmo/OpenBSD pledge differ).

echo ""
echo "=== Test 7: CLI app.main writes to a TTY under the sandbox ==="

if [ "$UNAME_S" != "Linux" ] || [ "${IS_COSMO:-0}" = "1" ]; then
    skip "TTY-write regression - native Linux only"
elif ! command -v python3 >/dev/null 2>&1; then
    skip "TTY-write regression - python3 (pty) unavailable"
else
    cat > "$WORKDIR/cli.lua" << 'EOF'
app.manifest({ modules = {} })
app.main(function(ctx)
    ctx.stdout:write("cli-tty-ok\n")
    return 0
end)
EOF
    # Allocate a PTY so isatty(stdout) is true, exercising the char-device
    # path. pty.spawn copies the child's tty output onto our stdout and
    # returns its wait status; a seccomp SIGKILL surfaces as !WIFEXITED.
    TTY_OUT=$(python3 - "$HULL" "$WORKDIR/cli.lua" <<'PY' 2>&1
import os, pty, sys
status = pty.spawn([sys.argv[1], sys.argv[2]])
sys.exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 1)
PY
)
    RC=$?
    if [ "$RC" = "0" ] && printf '%s' "$TTY_OUT" | grep -q "cli-tty-ok"; then
        pass "app.main stdout under a PTY not killed by the sandbox"
    else
        fail "app.main under a PTY failed (exit $RC): $TTY_OUT"
    fi
fi

# ── Test 8: every thread carries the seccomp filter (audit 42) ────────
#
# The kernel attaches a seccomp filter and a Landlock domain per THREAD. Hull
# used to start its worker pool before either sandbox phase, so the threads
# that run worker.dispatch VMs, parse db.async replies and run compute.async
# WASM had no filter and no domain, and since threads share one address space
# that voided the sandbox for the whole process. Now the vendored pledge
# installs every filter with SECCOMP_FILTER_FLAG_TSYNC (vendor/pledge patch
# 0003), the Landlock domain reaches existing threads by
# LANDLOCK_RESTRICT_SELF_TSYNC or per-thread self-restriction (patch 0004),
# and the pool's workers start on first use, inside the sandbox.
#
# Checked from OUTSIDE the process: after a few worker.dispatch jobs (pool
# threads exist) every task in /proc/<pid>/task must read "Seccomp: 2"
# (filter mode) with the same Seccomp_filters count as the main thread - a
# thread that missed phase 2 would show fewer filters, one with none 0. (The
# run watchdog thread used to start while the app loaded; since audit 43 it
# starts after phase 2 - Test 9.) The functional half (a pre-existing
# thread's forbidden open refused by seccomp and by Landlock) is in
# tests/sandbox_violation.c, run by Test 6. Native Linux only.

echo ""
echo "=== Test 8: every thread carries the seccomp filter (audit 42) ==="

T8_COSMO=0
case "${PLATFORM_CC:-}" in *cosmocc*) T8_COSMO=1 ;; esac
if [ "$UNAME_S" != "Linux" ] || [ "$T8_COSMO" = "1" ]; then
    skip "sandbox_audit42 thread checks - native Linux only"
elif [ ! -r /proc/self/status ] || ! grep -q '^Seccomp_filters:' /proc/self/status; then
    skip "sandbox_audit42 thread checks - /proc Seccomp_filters unavailable (Linux < 5.9)"
else
    cat > "$WORKDIR/threads.lua" << 'EOF'
local worker = require("hull.worker")
app.manifest({ modules = { "hull/http-server@1", "hull/worker@1", "hull/db@1" } })
app.get("/work", function(_req, res)
    local r = worker.dispatch(function(ctx) return { n = ctx.v * 2 } end,
                              { v = 21 })
    res:json(r)
end)
-- A pool thread opens its own connection to the -d database AFTER the
-- sandbox: the WAL / SHM files must be granted to it (sandbox.c), which
-- nothing tested while the pool ran outside the sandbox.
app.get("/dbwork", function(_req, res)
    local r = worker.dispatch(function(ctx)
        local db = _G.db
        db.exec("CREATE TABLE IF NOT EXISTS t (v TEXT)")
        db.exec("INSERT INTO t (v) VALUES (?)", { ctx.v })
        return { n = db.query("SELECT COUNT(*) AS n FROM t")[1].n }
    end, { v = "x" })
    res:json(r)
end)
app.get("/", function(_req, res) res:json({ ok = true }) end)
EOF
    LOGFILE8="$WORKDIR/hull_threads.log"
    "$HULL" -p 19887 -d "$WORKDIR/test8.db" "$WORKDIR/threads.lua" >"$LOGFILE8" 2>&1 &
    SERVER_PID=$!
    if wait_for_server 19887; then
        W1=$(curl -s "http://127.0.0.1:19887/work")
        check_contains "sandbox_audit42.worker_dispatch_runs" "$W1" "42"
        D1=$(curl -s "http://127.0.0.1:19887/dbwork")
        check_contains "sandbox_audit42.worker_db_under_sandbox" "$D1" '"n":'
        for _j in 1 2 3 4 5 6; do
            curl -s "http://127.0.0.1:19887/work" >/dev/null 2>&1 &
        done
        sleep 2
        NTASK=0; BAD=""
        MAIN_FILTERS=$(awk '/^Seccomp_filters:/ {print $2}' "/proc/$SERVER_PID/status" 2>/dev/null)
        for st in /proc/"$SERVER_PID"/task/*/status; do
            [ -r "$st" ] || continue
            NTASK=$((NTASK + 1))
            MODE=$(awk '/^Seccomp:/ {print $2}' "$st")
            NF=$(awk '/^Seccomp_filters:/ {print $2}' "$st")
            NAME=$(awk '/^Name:/ {print $2}' "$st")
            TID=$(basename "$(dirname "$st")")
            if [ "$MODE" != "2" ] || [ "$NF" != "$MAIN_FILTERS" ]; then
                BAD="$BAD tid=$TID($NAME):Seccomp=$MODE,filters=$NF"
            fi
        done
        if [ "$NTASK" -ge 2 ]; then
            pass "sandbox_audit42.threads_exist ($NTASK tasks)"
        else
            fail "sandbox_audit42.threads_exist - expected the pool's workers, saw $NTASK task(s)"
        fi
        if [ -n "$MAIN_FILTERS" ] && [ "$MAIN_FILTERS" -ge 2 ] && [ -z "$BAD" ]; then
            pass "sandbox_audit42.every_thread_seccomp (all $NTASK tasks Seccomp: 2, $MAIN_FILTERS filters)"
        else
            fail "sandbox_audit42.every_thread_seccomp - main filters=$MAIN_FILTERS;$BAD"
        fi
        stop_server
        LOG8=$(cat "$LOGFILE8")
        check_contains "sandbox_audit42.landlock_thread_mode_logged" "$LOG8" "threads: "
        check_not_contains "sandbox_audit42.no_unsynced_thread" "$LOG8" "could not be applied"
    else
        stop_server
        fail "sandbox_audit42 - hull did not start: $(tail -5 "$LOGFILE8")"
    fi
fi

# ── Test 9: no thread outlives into phase 2; top-level deadline (audit 43)
#
# A cosmo APE's libc pledge / unveil confine only the calling thread, so a
# thread started before phase 2 keeps phase-1 permissions there. The run
# watchdog used to start when the app's top level was loaded, and the WASM
# watchdog when the top level ran guest code. Now the run watchdog's thread
# starts only after phase 2 (the top level's deadline is polled meanwhile)
# and the WASM watchdog is stopped before it, so on Linux the sandbox finds
# no other thread ("threads: single"); a cosmo build refuses to start if it
# does. The top level stays bounded by time, in both runtimes.

echo ""
echo "=== Test 9: only the main thread at the phase-2 boundary (audit 43) ==="

# Runs "$@" for at most $1 seconds, output in $WORKDIR/bounded.log; prints
# the exit code (124 = killed at the limit).
run_bounded() {
    _lim=$1; shift
    "$@" >"$WORKDIR/bounded.log" 2>&1 &
    _pid=$!
    _i=0
    while kill -0 "$_pid" 2>/dev/null; do
        if [ "$_i" -ge $((_lim * 10)) ]; then
            kill -9 "$_pid" 2>/dev/null; wait "$_pid" 2>/dev/null
            echo 124; return
        fi
        sleep 0.1; _i=$((_i + 1))
    done
    wait "$_pid"; echo $?
}

mkdir -p "$WORKDIR/t9lua" "$WORKDIR/t9js"
cat > "$WORKDIR/t9lua/app.lua" << 'EOF'
app.manifest({})
local x = 0
while true do x = x + 1 end
EOF
cat > "$WORKDIR/t9js/app.js" << 'EOF'
import { app } from "hull:app";
app.manifest({});
let x = 0;
while (true) { x++; }
EOF
for rt9 in lua js; do
    RC9=$(run_bounded 20 "$HULL" --max-instructions 100000000000 --max-run-ms 300 "$WORKDIR/t9$rt9/app.$rt9")
    if [ "$RC9" != "124" ]; then
        pass "sandbox_audit43.toplevel_deadline_$rt9 (stopped, exit $RC9)"
    else
        fail "sandbox_audit43.toplevel_deadline_$rt9 - a top-level loop ran past --max-run-ms 300 for 20 s"
    fi
    if [ "$rt9" = "lua" ]; then
        check_contains "sandbox_audit43.toplevel_deadline_lua_message" \
            "$(cat "$WORKDIR/bounded.log")" "run exceeded its time limit"
    fi
done

T9_COSMO=0
case "${PLATFORM_CC:-}" in *cosmocc*) T9_COSMO=1 ;; esac
if [ "$UNAME_S" != "Linux" ]; then
    skip "sandbox_audit43 phase-2 thread count - Linux only"
elif [ ! -f "$SRCDIR/examples/compute/compute/echo.wasm" ]; then
    skip "sandbox_audit43 phase-2 thread count - examples/compute/compute/echo.wasm missing"
else
    mkdir -p "$WORKDIR/t9srv/compute" "$WORKDIR/t9cli/compute"
    cp "$SRCDIR/examples/compute/compute/echo.wasm" "$WORKDIR/t9srv/compute/"
    cp "$SRCDIR/examples/compute/compute/echo.wasm" "$WORKDIR/t9cli/compute/"
    # The top level runs guest code (compute.instance instantiates, a call
    # runs it): both watchdogs used to be running before phase 2 here.
    cat > "$WORKDIR/t9srv/app.lua" << 'EOF'
local compute = require("hull.compute")
app.manifest({ compute = true, modules = { "hull/http-server@1", "hull/compute@1" } })
local inst = compute.instance("echo")
local first = compute.call("echo", "top")
app.get("/echo", function(_req, res)
    res:json({ top = first, now = compute.call("echo", "req") })
end)
EOF
    cat > "$WORKDIR/t9cli/app.js" << 'EOF'
import { app } from "hull:app";
import { compute } from "hull:compute";
app.manifest({ compute: true, modules: ["hull/compute@1"] });
const inst = compute.instance("echo");
const top = compute.call("echo", "top");
app.main(() => {
    const now = compute.call("echo", "main");
    const txt = (b) => String.fromCharCode(...new Uint8Array(b));
    console.log("echo " + txt(top) + " " + txt(now));
    return 0;
});
EOF
    LOGFILE9="$WORKDIR/hull_t9.log"
    "$HULL" -p 19889 "$WORKDIR/t9srv/app.lua" >"$LOGFILE9" 2>&1 &
    SERVER_PID=$!
    if wait_for_server 19889; then
        R9=$(curl -s "http://127.0.0.1:19889/echo")
        check_contains "sandbox_audit43.compute_after_phase2" "$R9" '"now":"req"'
        stop_server
        LOG9=$(cat "$LOGFILE9")
        if [ "$T9_COSMO" = "1" ]; then
            check_not_contains "sandbox_audit43.cosmo_no_extra_thread" "$LOG9" "threads when the sandbox"
        else
            check_contains "sandbox_audit43.server_single_thread_at_phase2" "$LOG9" "threads: single"
        fi
    else
        stop_server
        fail "sandbox_audit43 - hull did not start: $(tail -5 "$LOGFILE9")"
    fi
    OUT9=$("$HULL" "$WORKDIR/t9cli/app.js" 2>&1)
    check_contains "sandbox_audit43.cli_compute_after_phase2" "$OUT9" "echo top main"
    if [ "$T9_COSMO" = "1" ]; then
        check_not_contains "sandbox_audit43.cosmo_cli_no_extra_thread" "$OUT9" "threads when the sandbox"
    else
        check_contains "sandbox_audit43.cli_single_thread_at_phase2" "$OUT9" "threads: single"
    fi
fi

# ── Summary ──────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
if [ "$SKIP" -gt 0 ]; then
    echo "$PASS/$TOTAL sandbox tests passed ($SKIP skipped)"
else
    echo "$PASS/$TOTAL sandbox tests passed"
fi
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
