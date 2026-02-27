#!/bin/sh
# E2E sandbox tests — verify pledge/unveil enforcement
#
# Tests:
#   1. Hull with manifest logs "[sandbox] applied" (Linux/cosmo only)
#   2. Hull without manifest logs "no manifest" (permissive)
#   3. Manifest without hosts — no dns promise
#   4. JS manifest app — sandbox applied (feature parity)
#   5. Kernel enforcement: pledge/unveil violations blocked (Linux only)
#
# Usage: sh tests/e2e_sandbox.sh
#        make e2e-sandbox
#
# Environment:
#   HULL     — path to hull binary (default: ./build/hull)
#   BUILDDIR — build directory for pledge objects (default: ./build)
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
        *)      fail "$1 — expected '$3'" ;;
    esac
}

check_not_contains() {
    case "$2" in
        *"$3"*) fail "$1 — unexpected '$3'" ;;
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
    echo "e2e_sandbox: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

WORKDIR=$(mktemp -d)

# Detect platform capabilities
UNAME_S=$(uname -s)
SANDBOX_EXPECTED=0
if [ "$UNAME_S" = "Linux" ]; then
    SANDBOX_EXPECTED=1
fi
# Cosmopolitan binaries report Linux on Linux, so check if hull was built
# with cosmocc by looking for the APE magic or platform_cc marker
if [ -f "$BUILDDIR/platform_cc" ]; then
    PLATFORM_CC=$(cat "$BUILDDIR/platform_cc" 2>/dev/null)
    case "$PLATFORM_CC" in
        *cosmocc*) SANDBOX_EXPECTED=1 ;;
    esac
fi

# ── Test 1: Hull with manifest app — sandbox applied ────────────────

echo ""
echo "=== Test 1: Hull with manifest — sandbox log ==="

cat > "$WORKDIR/app.lua" << 'EOF'
app.manifest({
    fs = { read = {"/tmp"}, write = {"/tmp"} },
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
        check_contains "sandbox mentions pledge" "$LOG" "pledge:"
        check_contains "sandbox dns promise (hosts declared)" "$LOG" "dns"
    else
        check_contains "sandbox not available log" "$LOG" "kernel sandbox not available"
    fi
else
    stop_server
    fail "hull did not start with manifest app"
fi

# ── Test 2: Hull without manifest — permissive ──────────────────────

echo ""
echo "=== Test 2: Hull without manifest — permissive ==="

cat > "$WORKDIR/nomanifest.lua" << 'EOF'
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
    check_contains "no manifest log" "$LOG2" "no manifest"
    check_not_contains "sandbox should NOT be applied" "$LOG2" "[sandbox] applied"
else
    stop_server
    fail "hull did not start without manifest"
fi

# ── Test 3: Manifest without hosts — no dns promise ─────────────────

echo ""
echo "=== Test 3: Manifest without hosts — no dns promise ==="

cat > "$WORKDIR/nohosts.lua" << 'EOF'
app.manifest({
    fs = { read = {"/tmp"} },
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
        check_not_contains "no dns promise without hosts" "$LOG3" " dns"
    else
        skip "dns promise check — no kernel sandbox on this platform"
    fi
else
    stop_server
    fail "hull did not start with no-hosts manifest"
fi

# ── Test 4: JS manifest app — sandbox applied ────────────────────────

echo ""
echo "=== Test 4: JS manifest app — sandbox log ==="

cat > "$WORKDIR/jsapp.js" << 'EOF'
import { app } from 'hull:app';

app.manifest({
    fs: { read: ["/tmp"], write: ["/tmp"] },
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
        check_contains "JS sandbox mentions pledge" "$LOG4" "pledge:"
        check_contains "JS sandbox dns promise (hosts declared)" "$LOG4" "dns"
    else
        check_contains "JS sandbox not available log" "$LOG4" "kernel sandbox not available"
    fi
else
    stop_server
    fail "hull did not start with JS manifest app"
fi

# ── Test 5: Kernel enforcement (Linux only) ──────────────────────────

echo ""
echo "=== Test 5: Kernel enforcement (pledge/unveil violations) ==="

if [ "$UNAME_S" != "Linux" ]; then
    skip "kernel enforcement test — Linux only"
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
    else
        # Native Linux: link against pledge polyfill objects
        PLEDGE_OBJS=$(find "$BUILDDIR" -name 'pledge_*.o' 2>/dev/null | tr '\n' ' ')

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
