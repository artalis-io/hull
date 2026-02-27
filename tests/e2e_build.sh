#!/bin/sh
# E2E build pipeline tests — exercises hull keygen/build/verify/inspect/manifest
#
# Tests the full build tool chain from source:
#   1. Build hull + platform.a from source
#   2. hull keygen — generates Ed25519 keypair
#   3. hull manifest — extracts manifest from app source
#   4. hull build — compiles standalone app binary (unsigned)
#   5. hull build --sign — compiles + signs
#   6. hull verify — checks signature
#   7. hull inspect — displays capabilities + signature
#   8. Built app serves HTTP correctly
#   9. Tampered file detected by verify
#  10. Multi-file app builds and serves correctly
#  11. Built binary has subcommand support (keygen, manifest, etc.)
#  12. Error cases
#  13. Self-build chain (hull → hull2 → hull3)
#
# Usage: sh tests/e2e_build.sh
#        make e2e-build
# Requires: cc, curl
#
# SPDX-License-Identifier: AGPL-3.0-or-later

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
trap cleanup EXIT

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

check_contains() {
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 — expected '$3' in: $2" ;;
    esac
}

check_file_exists() {
    if [ -f "$2" ]; then pass "$1"; else fail "$1 — not found: $2"; fi
}

check_file_executable() {
    if [ -x "$2" ]; then pass "$1"; else fail "$1 — not executable: $2"; fi
}

check_exit() {
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 — expected exit $2, got $3"; fi
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

# ── Step 0: Build hull + platform.a from source ───────────────────────

echo ""
echo "=== Step 0: Build hull + platform.a from source ==="

cd "$SRCDIR"
if make clean >/dev/null 2>&1 && make >/dev/null 2>&1; then
    pass "make (hull binary)"
else
    fail "make (hull binary)"
    echo "FATAL: cannot continue without hull binary"
    exit 1
fi

if make platform >/dev/null 2>&1; then
    pass "make platform (libhull_platform.a)"
else
    fail "make platform (libhull_platform.a)"
    echo "FATAL: cannot continue without platform library"
    exit 1
fi

check_file_exists "hull binary exists" "$HULL"
check_file_executable "hull binary executable" "$HULL"
check_file_exists "platform .a exists" "$SRCDIR/build/libhull_platform.a"

# Verify hull_main is exported
if nm "$SRCDIR/build/libhull_platform.a" 2>/dev/null | grep -q "_hull_main"; then
    pass "hull_main exported from platform .a"
else
    fail "hull_main NOT exported from platform .a"
fi

# ── Set up temp working directory ─────────────────────────────────────

WORKDIR=$(mktemp -d)
cd "$WORKDIR"

# ── Step 1: hull keygen ───────────────────────────────────────────────

echo ""
echo "=== Step 1: hull keygen ==="

"$HULL" keygen >/dev/null 2>&1; RC=$?
check_exit "keygen exits 0" 0 $RC
check_file_exists "developer.pub created" "$WORKDIR/developer.pub"
check_file_exists "developer.key created" "$WORKDIR/developer.key"

# Verify key format (64 hex chars for pubkey, 128 for secret key)
if [ -f "$WORKDIR/developer.pub" ]; then
    PUBKEY_LEN=$(wc -c < "$WORKDIR/developer.pub" | tr -d ' ')
    if [ "$PUBKEY_LEN" = "65" ]; then
        pass "pubkey length (64 hex + newline)"
    else
        fail "pubkey length — expected 65, got $PUBKEY_LEN"
    fi
fi
if [ -f "$WORKDIR/developer.key" ]; then
    SECKEY_LEN=$(wc -c < "$WORKDIR/developer.key" | tr -d ' ')
    if [ "$SECKEY_LEN" = "129" ]; then
        pass "seckey length (128 hex + newline)"
    else
        fail "seckey length — expected 129, got $SECKEY_LEN"
    fi
fi

# keygen with custom prefix
"$HULL" keygen myapp >/dev/null 2>&1
check_file_exists "custom prefix pubkey" "$WORKDIR/myapp.pub"
check_file_exists "custom prefix seckey" "$WORKDIR/myapp.key"

# ── Step 2: Create test app ──────────────────────────────────────────

echo ""
echo "=== Step 2: Create test app ==="

mkdir -p "$WORKDIR/myapp"
cat > "$WORKDIR/myapp/app.lua" << 'APPEOF'
app.manifest({
    fs = { read = {"data/"}, write = {"uploads/"} },
    env = {"PORT", "DATABASE_URL"},
    hosts = {"api.stripe.com", "api.sendgrid.com"},
})

app.get("/", function(req, res)
    res:json({message = "hello from hull build"})
end)

app.get("/health", function(req, res)
    res:json({status = "ok"})
end)
APPEOF

pass "test app created"

# ── Step 3: hull manifest ─────────────────────────────────────────────

echo ""
echo "=== Step 3: hull manifest ==="

MANIFEST_OUT=$("$HULL" manifest "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "manifest exits 0" 0 $RC
check_contains "manifest has fs.read" "$MANIFEST_OUT" "data/"
check_contains "manifest has fs.write" "$MANIFEST_OUT" "uploads/"
check_contains "manifest has env" "$MANIFEST_OUT" "PORT"
check_contains "manifest has hosts" "$MANIFEST_OUT" "api.stripe.com"

# ── Step 4: hull build (unsigned) ─────────────────────────────────────

echo ""
echo "=== Step 4: hull build (unsigned) ==="

BUILD_OUT=$("$HULL" build -o "$WORKDIR/myapp/myapp" "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "build exits 0" 0 $RC
check_contains "build reports compiling" "$BUILD_OUT" "compiling"
check_contains "build reports linking" "$BUILD_OUT" "linking"
check_file_exists "built binary exists" "$WORKDIR/myapp/myapp"
check_file_executable "built binary executable" "$WORKDIR/myapp/myapp"

# No hull.sig should exist (unsigned)
if [ ! -f "$WORKDIR/myapp/hull.sig" ]; then
    pass "no hull.sig for unsigned build"
else
    fail "hull.sig should not exist for unsigned build"
fi

# Check binary size is reasonable (should be 1-10MB)
if [ -f "$WORKDIR/myapp/myapp" ]; then
    SIZE=$(wc -c < "$WORKDIR/myapp/myapp" | tr -d ' ')
    if [ "$SIZE" -gt 500000 ] && [ "$SIZE" -lt 10000000 ]; then
        pass "binary size reasonable (${SIZE} bytes)"
    else
        fail "binary size unexpected: $SIZE bytes"
    fi
fi

# ── Step 5: Built binary serves HTTP ──────────────────────────────────

echo ""
echo "=== Step 5: Built binary serves HTTP ==="

"$WORKDIR/myapp/myapp" -p 19870 "$WORKDIR/myapp/app.lua" >/dev/null 2>&1 &
SERVER_PID=$!

if wait_for_server 19870; then
    RESP=$(curl -s "http://127.0.0.1:19870/")
    check_contains "GET / returns message" "$RESP" "hello from hull build"

    RESP=$(curl -s "http://127.0.0.1:19870/health")
    check_contains "GET /health returns ok" "$RESP" "ok"
else
    fail "built binary did not start"
fi

stop_server

# ── Step 6: hull build --sign ─────────────────────────────────────────

echo ""
echo "=== Step 6: hull build --sign ==="

# Remove previous build artifacts
rm -f "$WORKDIR/myapp/myapp" "$WORKDIR/myapp/hull.sig"

BUILD_OUT=$("$HULL" build --sign "$WORKDIR/developer.key" -o "$WORKDIR/myapp/myapp" "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "signed build exits 0" 0 $RC
check_file_exists "signed binary exists" "$WORKDIR/myapp/myapp"
check_file_exists "hull.sig exists" "$WORKDIR/myapp/hull.sig"

# Verify hull.sig is valid JSON with expected fields
if [ -f "$WORKDIR/myapp/hull.sig" ]; then
    SIG_CONTENT=$(cat "$WORKDIR/myapp/hull.sig")
    check_contains "hull.sig has version" "$SIG_CONTENT" '"version"'
    check_contains "hull.sig has files" "$SIG_CONTENT" '"files"'
    check_contains "hull.sig has signature" "$SIG_CONTENT" '"signature"'
    check_contains "hull.sig has public_key" "$SIG_CONTENT" '"public_key"'
    check_contains "hull.sig has manifest" "$SIG_CONTENT" '"manifest"'
    check_contains "hull.sig has fs capabilities" "$SIG_CONTENT" "data/"
fi

# ── Step 7: hull verify (valid) ───────────────────────────────────────

echo ""
echo "=== Step 7: hull verify (valid signature) ==="

VERIFY_OUT=$("$HULL" verify "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "verify exits 0" 0 $RC
check_contains "verify reports OK" "$VERIFY_OUT" "OK"
check_contains "verify reports valid" "$VERIFY_OUT" "signature valid"

# ── Step 8: hull inspect ──────────────────────────────────────────────

echo ""
echo "=== Step 8: hull inspect ==="

INSPECT_OUT=$("$HULL" inspect "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "inspect exits 0" 0 $RC
check_contains "inspect shows version" "$INSPECT_OUT" "v1"
check_contains "inspect shows fs.read" "$INSPECT_OUT" "data/"
check_contains "inspect shows fs.write" "$INSPECT_OUT" "uploads/"
check_contains "inspect shows env" "$INSPECT_OUT" "PORT"
check_contains "inspect shows hosts" "$INSPECT_OUT" "api.stripe.com"
check_contains "inspect shows signature" "$INSPECT_OUT" "Signature:"
check_contains "inspect shows VALID" "$INSPECT_OUT" "VALID"

# ── Step 9: Tamper detection ──────────────────────────────────────────

echo ""
echo "=== Step 9: Tamper detection ==="

# Save original app.lua
cp "$WORKDIR/myapp/app.lua" "$WORKDIR/myapp/app.lua.bak"

# Tamper with the file
echo '-- tampered' >> "$WORKDIR/myapp/app.lua"

VERIFY_OUT=$("$HULL" verify "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "verify detects tamper (exit 1)" 1 $RC
check_contains "verify reports FAILED" "$VERIFY_OUT" "FAILED"
check_contains "verify mentions modified files" "$VERIFY_OUT" "Modified files"

# Restore original
mv "$WORKDIR/myapp/app.lua.bak" "$WORKDIR/myapp/app.lua"

# Verify it passes again after restore
VERIFY_OUT=$("$HULL" verify "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "verify passes after restore" 0 $RC

# ── Step 10: Multi-file app ──────────────────────────────────────────

echo ""
echo "=== Step 10: Multi-file app ==="

mkdir -p "$WORKDIR/multiapp/lib"
cat > "$WORKDIR/multiapp/app.lua" << 'APPEOF'
local greet = require("./lib/greet")

app.manifest({
    env = {"APP_NAME"},
})

app.get("/", function(req, res)
    res:json({message = greet.hello("world")})
end)

app.get("/health", function(req, res)
    res:json({status = "ok", app = "multiapp"})
end)
APPEOF

cat > "$WORKDIR/multiapp/lib/greet.lua" << 'LIBEOF'
local M = {}
function M.hello(name)
    return "Hello, " .. name .. "!"
end
return M
LIBEOF

BUILD_OUT=$("$HULL" build --sign "$WORKDIR/developer.key" -o "$WORKDIR/multiapp/multiapp" "$WORKDIR/multiapp" 2>&1); RC=$?
check_exit "multi-file build exits 0" 0 $RC
check_contains "multi-file build finds 2 files" "$BUILD_OUT" "2 Lua file"
check_file_exists "multi-file binary exists" "$WORKDIR/multiapp/multiapp"

# Serve the multi-file app
"$WORKDIR/multiapp/multiapp" -p 19871 "$WORKDIR/multiapp/app.lua" >/dev/null 2>&1 &
SERVER_PID=$!

if wait_for_server 19871; then
    RESP=$(curl -s "http://127.0.0.1:19871/")
    check_contains "multi-file GET / works" "$RESP" "Hello, world!"

    RESP=$(curl -s "http://127.0.0.1:19871/health")
    check_contains "multi-file GET /health" "$RESP" "multiapp"
else
    fail "multi-file app did not start"
fi

stop_server

# Verify multi-file signature
VERIFY_OUT=$("$HULL" verify "$WORKDIR/multiapp" 2>&1); RC=$?
check_exit "multi-file verify passes" 0 $RC

# Inspect multi-file app (manifest may be nil if app has require() deps)
INSPECT_OUT=$("$HULL" inspect "$WORKDIR/multiapp" 2>&1); RC=$?
check_contains "multi-file inspect shows files" "$INSPECT_OUT" "app.lua"

# ── Step 11: Built binary subcommands ─────────────────────────────────

echo ""
echo "=== Step 11: Built binary subcommands ==="

# The built binary should support hull subcommands since it links hull_main
"$WORKDIR/multiapp/multiapp" keygen "$WORKDIR/app_key" >/dev/null 2>&1; RC=$?
check_exit "built app keygen exits 0" 0 $RC
check_file_exists "built app keygen creates pubkey" "$WORKDIR/app_key.pub"

MANIFEST_OUT=$("$WORKDIR/multiapp/multiapp" manifest "$WORKDIR/multiapp" 2>&1); RC=$?
check_exit "built app manifest exits 0" 0 $RC
check_contains "built app manifest has env" "$MANIFEST_OUT" "APP_NAME"

# ── Step 12: Error cases ─────────────────────────────────────────────

echo ""
echo "=== Step 12: Error cases ==="

# Build with no .lua files
mkdir -p "$WORKDIR/emptyapp"
BUILD_OUT=$("$HULL" build "$WORKDIR/emptyapp" 2>&1); RC=$?
check_exit "build empty app fails" 1 $RC
check_contains "build reports no files" "$BUILD_OUT" "no .lua files"

# Verify with no hull.sig
VERIFY_OUT=$("$HULL" verify "$WORKDIR/emptyapp" 2>&1); RC=$?
check_exit "verify without hull.sig fails" 1 $RC
check_contains "verify reports missing sig" "$VERIFY_OUT" "no hull.sig"

# Inspect with no hull.sig
INSPECT_OUT=$("$HULL" inspect "$WORKDIR/emptyapp" 2>&1); RC=$?
check_exit "inspect without hull.sig fails" 1 $RC

# Manifest with no app.lua
MANIFEST_OUT=$("$HULL" manifest "$WORKDIR/emptyapp" 2>&1); RC=$?
check_exit "manifest without app.lua fails" 1 $RC

# Sign with nonexistent key
BUILD_OUT=$("$HULL" build --sign "/nonexistent/key.key" "$WORKDIR/myapp" 2>&1); RC=$?
check_exit "build with bad key fails" 1 $RC

# ── Step 13: Self-build chain (hull → hull2 → hull3) ─────────────────

echo ""
echo "=== Step 13: Self-build chain ==="

# Create a minimal app for self-build test
mkdir -p "$WORKDIR/nullapp"
cat > "$WORKDIR/nullapp/app.lua" << 'APPEOF'
app.get("/", function(req, res) res:json({status = "ok"}) end)
APPEOF

# hull → hull2
BUILD_OUT=$("$HULL" build -o "$WORKDIR/hull2" "$WORKDIR/nullapp" 2>&1); RC=$?
check_exit "self-build hull→hull2 exits 0" 0 $RC
check_file_executable "hull2 exists and executable" "$WORKDIR/hull2"

# hull2 keygen (verify subcommands work)
if [ -x "$WORKDIR/hull2" ]; then
    "$WORKDIR/hull2" keygen "$WORKDIR/sb_key" >/dev/null 2>&1; RC=$?
    check_exit "hull2 keygen exits 0" 0 $RC
    check_file_exists "hull2 keygen creates pubkey" "$WORKDIR/sb_key.pub"
else
    fail "hull2 not executable — skipping chain"
fi

# hull2 → hull3
if [ -x "$WORKDIR/hull2" ]; then
    BUILD_OUT=$("$WORKDIR/hull2" build -o "$WORKDIR/hull3" "$WORKDIR/nullapp" 2>&1); RC=$?
    check_exit "self-build hull2→hull3 exits 0" 0 $RC
    check_file_executable "hull3 exists and executable" "$WORKDIR/hull3"
else
    fail "hull2 not executable — skipping hull3 build"
fi

# hull3 keygen (verify the chain)
if [ -x "$WORKDIR/hull3" ]; then
    "$WORKDIR/hull3" keygen "$WORKDIR/sb_key2" >/dev/null 2>&1; RC=$?
    check_exit "hull3 keygen exits 0" 0 $RC
    check_file_exists "hull3 keygen creates pubkey" "$WORKDIR/sb_key2.pub"
else
    fail "hull3 not executable — skipping chain verification"
fi

# ── Step 14: --verify-sig (runtime signature verification) ─────────

echo ""
echo "=== Step 14: --verify-sig ==="

# Start signed app with --verify-sig → should start and serve
"$WORKDIR/myapp/myapp" --verify-sig "$WORKDIR/developer.pub" -p 19872 "$WORKDIR/myapp/app.lua" >/dev/null 2>&1 &
SERVER_PID=$!

if wait_for_server 19872; then
    RESP=$(curl -s "http://127.0.0.1:19872/")
    check_contains "--verify-sig serves OK" "$RESP" "hello from hull build"
    pass "--verify-sig starts with valid sig"
else
    fail "--verify-sig did not start with valid sig"
fi

stop_server

# Tamper with app.lua and try --verify-sig → should refuse
cp "$WORKDIR/myapp/app.lua" "$WORKDIR/myapp/app.lua.bak"
echo '-- tampered' >> "$WORKDIR/myapp/app.lua"

"$WORKDIR/myapp/myapp" --verify-sig "$WORKDIR/developer.pub" -p 19873 "$WORKDIR/myapp/app.lua" >/dev/null 2>&1 &
TPID=$!
sleep 2

# Server should have exited (refused to start)
if kill -0 "$TPID" 2>/dev/null; then
    fail "--verify-sig should refuse tampered app"
    kill "$TPID" 2>/dev/null
    wait "$TPID" 2>/dev/null
else
    wait "$TPID" 2>/dev/null
    TEXIT=$?
    if [ "$TEXIT" -ne 0 ]; then
        pass "--verify-sig refuses tampered app (exit $TEXIT)"
    else
        fail "--verify-sig should exit non-zero for tampered app"
    fi
fi

# Restore
mv "$WORKDIR/myapp/app.lua.bak" "$WORKDIR/myapp/app.lua"

# Test with wrong key → should refuse
"$HULL" keygen "$WORKDIR/wrong_key" >/dev/null 2>&1
"$WORKDIR/myapp/myapp" --verify-sig "$WORKDIR/wrong_key.pub" -p 19874 "$WORKDIR/myapp/app.lua" >/dev/null 2>&1 &
TPID=$!
sleep 2

if kill -0 "$TPID" 2>/dev/null; then
    fail "--verify-sig should refuse wrong key"
    kill "$TPID" 2>/dev/null
    wait "$TPID" 2>/dev/null
else
    wait "$TPID" 2>/dev/null
    TEXIT=$?
    if [ "$TEXIT" -ne 0 ]; then
        pass "--verify-sig refuses wrong key (exit $TEXIT)"
    else
        fail "--verify-sig should exit non-zero for wrong key"
    fi
fi

# ── Step 15: hull.sig enhanced fields (platform/binary/trampoline hashes) ──

echo ""
echo "=== Step 15: hull.sig enhanced fields ==="

if [ -f "$WORKDIR/myapp/hull.sig" ]; then
    SIG_CONTENT=$(cat "$WORKDIR/myapp/hull.sig")
    check_contains "hull.sig has platform_hash" "$SIG_CONTENT" '"platform_hash"'
    check_contains "hull.sig has binary_hash" "$SIG_CONTENT" '"binary_hash"'
    check_contains "hull.sig has trampoline_hash" "$SIG_CONTENT" '"trampoline_hash"'
fi

# ── Step 16: hull new ──────────────────────────────────────────────

echo ""
echo "=== Step 16: hull new ==="

"$HULL" new "$WORKDIR/newapp" >/dev/null 2>&1; RC=$?
check_exit "hull new exits 0" 0 $RC
check_file_exists "new app.lua exists" "$WORKDIR/newapp/app.lua"
check_file_exists "new tests/ exists" "$WORKDIR/newapp/tests/test_app.lua"
check_file_exists "new .gitignore exists" "$WORKDIR/newapp/.gitignore"

# New app should serve
"$HULL" -p 19875 "$WORKDIR/newapp/app.lua" >/dev/null 2>&1 &
SERVER_PID=$!
if wait_for_server 19875; then
    RESP=$(curl -s "http://127.0.0.1:19875/")
    check_contains "new app GET / ok" "$RESP" "ok"
else
    fail "new app did not start"
fi
stop_server

# hull new with existing dir should fail
"$HULL" new "$WORKDIR/newapp" >/dev/null 2>&1; RC=$?
check_exit "hull new existing dir fails" 1 $RC

# hull new with --runtime js
"$HULL" new --runtime js "$WORKDIR/newapp_js" >/dev/null 2>&1; RC=$?
check_exit "hull new --runtime js exits 0" 0 $RC
check_file_exists "new app.js exists" "$WORKDIR/newapp_js/app.js"

# ── Step 17: hull eject ────────────────────────────────────────────

echo ""
echo "=== Step 17: hull eject ==="

# Build platform first (needed by eject)
"$HULL" eject "$WORKDIR/myapp" -o "$WORKDIR/ejected" >/dev/null 2>&1; RC=$?
check_exit "hull eject exits 0" 0 $RC
check_file_exists "ejected Makefile exists" "$WORKDIR/ejected/Makefile"
check_file_exists "ejected app_main.c exists" "$WORKDIR/ejected/src/app_main.c"
check_file_exists "ejected gen_registry.sh exists" "$WORKDIR/ejected/scripts/gen_registry.sh"
check_file_exists "ejected platform lib exists" "$WORKDIR/ejected/platform/libhull_platform.a"
check_file_exists "ejected app.lua exists" "$WORKDIR/ejected/app/app.lua"

# hull eject with existing dir should fail
"$HULL" eject "$WORKDIR/myapp" -o "$WORKDIR/ejected" >/dev/null 2>&1; RC=$?
check_exit "hull eject existing dir fails" 1 $RC

# ── Summary ───────────────────────────────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "$PASS/$TOTAL e2e build pipeline tests passed"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
