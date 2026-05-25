#!/bin/sh
# E2E test: hull build --compiler=tcc end-to-end
#
# Verifies that the embedded TinyCC actually works to build a working
# hull app binary on platforms where TCC is supported (Linux native).
# On platforms where TCC is unsupported (macOS, cosmo), confirms hull
# rejects --compiler=tcc with a clear error and a non-zero exit code.
#
# Usage: sh tests/e2e_tcc.sh
#        make e2e-tcc
# Requires: cc (for system-compiler fallback test), curl
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
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
trap cleanup EXIT INT TERM

assert() {
    msg="$1"; shift
    if "$@"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        FAIL=$((FAIL + 1))
    fi
}

# Detect platform — TCC is only viable on Linux native (non-cosmo).
PLATFORM=$(uname -s)
case "$PLATFORM" in
    Linux)  TCC_VIABLE=1 ;;
    Darwin) TCC_VIABLE=0 ;;
    *)      TCC_VIABLE=0 ;;
esac

# Confirm hull binary exists
if [ ! -x "$HULL" ]; then
    echo "FAIL: $HULL not found — run 'make' first"
    exit 1
fi

echo "── hull build --compiler=tcc ($PLATFORM) ──"

# Confirm hull doctor reports tcc-embedded
echo ""
echo "Test: hull doctor reports tcc embedding"
DOCTOR_JSON=$("$HULL" doctor --json 2>&1 || true)
echo "$DOCTOR_JSON" | grep -q '"tcc_embedded"'
assert "doctor --json includes tcc_embedded field" [ $? -eq 0 ]

TCC_EMBEDDED=$(echo "$DOCTOR_JSON" | grep -o '"tcc_embedded": *true' || echo "")
if [ -z "$TCC_EMBEDDED" ]; then
    echo "  SKIP: tcc is not embedded in this build"
    echo ""
    echo "── Summary ──"
    echo "  Passed: $PASS"
    echo "  Failed: $FAIL"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
fi

# Set up a minimal app
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/hull_e2e_tcc.XXXXXX")
mkdir -p "$WORKDIR/myapp"
cat > "$WORKDIR/myapp/app.lua" << 'EOF'
-- app.get / .post / .use are installed by hull/http-server; the
-- intrinsic `app` only carries .manifest / .main / .router. Declare
-- it explicitly so the fixture loads under the module resolver.
app.manifest({ modules = { "hull/http-server@1" } })
app.get("/", function(req, res) res:text("hello from tcc-built hull") end)
app.get("/echo/:msg", function(req, res) res:text(req.params.msg) end)
EOF

cd "$WORKDIR/myapp"

if [ "$TCC_VIABLE" = "1" ]; then
    # ── Linux: TCC must produce a working binary ─────────────────
    echo ""
    echo "Test: hull build --compiler=tcc produces working binary"

    "$HULL" build --compiler=tcc -o "$WORKDIR/hello.tcc" . > "$WORKDIR/build.log" 2>&1
    BUILD_RC=$?
    if [ "$BUILD_RC" -ne 0 ]; then
        echo "  --- build.log (exit $BUILD_RC) ---"
        sed 's/^/  | /' "$WORKDIR/build.log"
        echo "  --- end build.log ---"
    fi
    assert "hull build --compiler=tcc exits 0" [ "$BUILD_RC" -eq 0 ]
    assert "output binary exists" [ -x "$WORKDIR/hello.tcc" ]

    # Confirm the build actually used tcc
    grep -qi "compiling with tcc" "$WORKDIR/build.log"
    assert "build log shows 'compiling with tcc'" [ $? -eq 0 ]

    # Confirm the binary runs and serves HTTP
    PORT=$((10000 + (RANDOM % 50000)))
    "$WORKDIR/hello.tcc" -p "$PORT" -d "$WORKDIR/data.db" &
    SERVER_PID=$!
    sleep 1

    BODY=$(curl -s "http://127.0.0.1:$PORT/" || echo FAIL)
    assert "tcc-built binary serves /" [ "$BODY" = "hello from tcc-built hull" ]

    BODY=$(curl -s "http://127.0.0.1:$PORT/echo/world" || echo FAIL)
    assert "tcc-built binary serves /echo/world" [ "$BODY" = "world" ]

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""

    # Confirm auto-select also picks tcc on Linux
    echo ""
    echo "Test: hull build (no --compiler) selects tcc by default on Linux"
    "$HULL" build -o "$WORKDIR/hello.auto" . > "$WORKDIR/auto.log" 2>&1
    AUTO_RC=$?
    if [ "$AUTO_RC" -ne 0 ]; then
        echo "  --- auto.log (exit $AUTO_RC) ---"
        sed 's/^/  | /' "$WORKDIR/auto.log"
        echo "  --- end auto.log ---"
    fi
    assert "hull build exits 0" [ "$AUTO_RC" -eq 0 ]
    grep -qi "compiling with tcc" "$WORKDIR/auto.log"
    assert "default backend on Linux is tcc" [ $? -eq 0 ]

else
    # ── macOS/cosmo: --compiler=tcc must fail with a clear error ─
    echo ""
    echo "Test: hull build --compiler=tcc rejected on $PLATFORM"
    "$HULL" build --compiler=tcc -o "$WORKDIR/hello.tcc" . > "$WORKDIR/build.log" 2>&1
    BUILD_RC=$?
    assert "hull build --compiler=tcc exits non-zero on $PLATFORM" \
           [ "$BUILD_RC" -ne 0 ]
    grep -qi "tcc.*not available\|tcc.*not" "$WORKDIR/build.log"
    assert "error message mentions tcc unavailability" [ $? -eq 0 ]

    # System cc default must still work
    echo ""
    echo "Test: hull build (default) still works on $PLATFORM"
    "$HULL" build -o "$WORKDIR/hello.sys" . >/dev/null 2>&1
    assert "default build exits 0" [ $? -eq 0 ]
    assert "default-built binary exists" [ -x "$WORKDIR/hello.sys" ]
fi

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
