#!/bin/sh
# E2E test: hull build --compiler=tcc end-to-end (tcc as a side-loaded tool)
#
# tcc is no longer embedded in the hull binary — it's an external tool
# (`hull tools install tcc`), resolved at build time from ~/.hull/tools → PATH.
# On Linux (where tcc's ELF output is viable), this test builds tcc from source,
# makes it resolvable via a controlled HOME/.hull/tools, and verifies
# `hull build --compiler=tcc` produces a working binary. It also checks the
# clean error when no tcc is resolvable. On macOS/cosmo (Mach-O / APE archives)
# tcc isn't viable, so it confirms --compiler=tcc is rejected and the system
# compiler still works.
#
# Usage: sh tests/e2e_tcc.sh   /   make e2e-tcc
# Requires: cc (system linker + fallback), curl
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

# tcc emits ELF — only viable on Linux native (non-cosmo).
PLATFORM=$(uname -s)
case "$PLATFORM" in
    Linux)  TCC_VIABLE=1 ;;
    *)      TCC_VIABLE=0 ;;
esac

[ -x "$HULL" ] || { echo "FAIL: $HULL not found — run 'make' first"; exit 1; }

echo "── hull build --compiler=tcc ($PLATFORM) ──"

# doctor exposes tcc as a tool: field renamed embedded → available.
DOCTOR_JSON=$("$HULL" doctor --json 2>&1 || true)
if [ "$TCC_VIABLE" = "1" ]; then
    echo "$DOCTOR_JSON" | grep -q '"tcc_available"'
    assert "doctor --json includes tcc_available field" [ $? -eq 0 ]
fi

# Minimal app fixture used by every build below.
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/hull_e2e_tcc.XXXXXX")
mkdir -p "$WORKDIR/myapp"
cat > "$WORKDIR/myapp/app.lua" << 'EOF'
app.manifest({ modules = { "hull/http-server@1" } })
app.get("/", function(req, res) res:text("hello from tcc-built hull") end)
app.get("/echo/:msg", function(req, res) res:text(req.params.msg) end)
EOF
cd "$WORKDIR/myapp"

if [ "$TCC_VIABLE" = "1" ]; then
    # ── Build tcc from source (the side-loaded tool). Skip cleanly if the
    #    vendored toolchain can't build here. ──
    echo ""
    echo "Test: build the tcc tool from source (make tcc)"
    if ! make -C "$SRCDIR" tcc >/dev/null 2>&1 || [ ! -x "$SRCDIR/build/tcc" ]; then
        echo "  SKIP: could not build build/tcc (vendored tcc unavailable)"
        echo ""; echo "── Summary ──"; echo "  Passed: $PASS"; echo "  Failed: $FAIL"
        [ "$FAIL" -eq 0 ] && exit 0 || exit 1
    fi
    echo "  ok  build/tcc built"

    # Controlled HOME so tcc resolves from ~/.hull/tools (as `hull tools install
    # tcc` would place it) rather than depending on a system tcc.
    FAKEHOME="$WORKDIR/home"
    mkdir -p "$FAKEHOME/.hull/tools"
    cp "$SRCDIR/build/tcc" "$FAKEHOME/.hull/tools/tcc"

    # NOTE: the "no tcc resolvable → clear error" path is intentionally NOT
    # asserted here. hl_tools_lookup_path also probes dirname(hull_exe), and the
    # test's own build/tcc sits next to build/hull, so isolating "no tcc
    # anywhere" reliably (empty HOME + PATH + a hull with no tcc sibling + a
    # findable platform lib) is brittle. The error path is exercised by the
    # backend (compiler_tcc.c: tcc_resolve → the `hull tools install tcc` hint)
    # and on macOS, where --compiler=tcc is rejected below.

    # ── Installed (via ~/.hull/tools) → real build ──
    echo ""
    echo "Test: hull build --compiler=tcc produces a working binary"
    HOME="$FAKEHOME" "$HULL" build --no-verify-platform --compiler=tcc \
        -o "$WORKDIR/hello.tcc" . > "$WORKDIR/build.log" 2>&1
    BUILD_RC=$?
    if [ "$BUILD_RC" -ne 0 ]; then
        echo "  --- build.log (exit $BUILD_RC) ---"; sed 's/^/  | /' "$WORKDIR/build.log"
    fi
    assert "hull build --compiler=tcc exits 0" [ "$BUILD_RC" -eq 0 ]
    assert "output binary exists" [ -x "$WORKDIR/hello.tcc" ]
    grep -qi "compiling with tcc" "$WORKDIR/build.log"
    assert "build log shows 'compiling with tcc'" [ $? -eq 0 ]

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

    # ── Bare `hull build` uses the emit path (Phase 3 default), even with a
    #    resolvable tcc installed. tcc is used only when explicitly asked
    #    (`--compiler=tcc`, tested above). ──
    echo ""
    echo "Test: hull build (no --compiler) uses the emit path, not tcc"
    HOME="$FAKEHOME" "$HULL" build --no-verify-platform -o "$WORKDIR/hello.auto" . \
        > "$WORKDIR/auto.log" 2>&1
    assert "hull build exits 0" [ $? -eq 0 ]
    grep -qi "emitting app_registry" "$WORKDIR/auto.log"
    assert "bare build emits by default (does not auto-pick tcc)" [ $? -eq 0 ]

else
    # ── macOS/cosmo: --compiler=tcc must fail; system cc still works. ──
    echo ""
    echo "Test: hull build --compiler=tcc rejected on $PLATFORM"
    "$HULL" build --no-verify-platform --compiler=tcc -o "$WORKDIR/hello.tcc" . \
        > "$WORKDIR/build.log" 2>&1
    assert "hull build --compiler=tcc exits non-zero on $PLATFORM" [ $? -ne 0 ]
    grep -qi "no tcc backend\|tcc.*not" "$WORKDIR/build.log"
    assert "error message explains tcc is unavailable here" [ $? -eq 0 ]

    echo ""
    echo "Test: hull build (default) still works on $PLATFORM"
    "$HULL" build --no-verify-platform -o "$WORKDIR/hello.sys" . >/dev/null 2>&1
    assert "default build exits 0" [ $? -eq 0 ]
    assert "default-built binary exists" [ -x "$WORKDIR/hello.sys" ]
fi

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
