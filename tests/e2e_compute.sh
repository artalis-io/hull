#!/bin/sh
# e2e_compute.sh — E2E tests for WASM compute capability
#
# Tests compute.call() from both Lua and JS runtimes.
# Requires: build/hull, tests/fixtures/compute/echo.wasm
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PASS=0
FAIL=0
TOTAL=0

pass() { PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); echo "  FAIL: $1"; }

# Create temporary app directory
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Copy echo.wasm fixture
mkdir -p "$TMPDIR/compute"
cp tests/fixtures/compute/echo.wasm "$TMPDIR/compute/echo.wasm"

echo "=== E2E: compute.call (Lua) ==="

# Lua app + tests
cat > "$TMPDIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)
EOF

mkdir -p "$TMPDIR/tests"
cat > "$TMPDIR/tests/test_compute.lua" << 'EOF'
test("compute.call echo", function()
    local out, err = compute.call("echo", "hello wasm")
    assert(not err, "err: " .. tostring(err))
    assert(out == "hello wasm", "mismatch: " .. tostring(out))
end)

test("compute.call empty", function()
    local out, err = compute.call("echo", "")
    assert(not err, "err: " .. tostring(err))
    assert(out == "", "expected empty")
end)

test("compute.call not found", function()
    local out, err = compute.call("nonexistent", "x")
    assert(out == nil)
    assert(err == "not_found")
end)

test("compute.preload", function()
    local ok, err = compute.preload("echo")
    assert(ok, "preload failed: " .. tostring(err))
end)

test("compute.call with opts", function()
    local out, err = compute.call("echo", "test", {
        gas = 1000000,
        max_input = 1024,
        max_output = 1024,
    })
    assert(not err, "err: " .. tostring(err))
    assert(out == "test")
end)
EOF

OUTPUT=$($HULL test "$TMPDIR" 2>&1) || true
echo "$OUTPUT"
if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
    pass "Lua compute tests"
else
    fail "Lua compute tests"
fi

echo ""
echo "=== E2E: compute.call (JS) ==="

# JS app + tests
rm -f "$TMPDIR/app.lua" "$TMPDIR/tests/test_compute.lua"

cat > "$TMPDIR/app.js" << 'JSEOF'
import { app } from "hull:app";
app.get("/health", (req, res) => { res.json({ ok: true }); });
JSEOF

cat > "$TMPDIR/tests/test_compute.js" << 'JSEOF'
import { compute } from "hull:compute";

test("compute.call echo", () => {
    const out = compute.call("echo", "hello");
    // out is ArrayBuffer — convert to string via Uint8Array
    const bytes = new Uint8Array(out);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    test.eq(result, "hello");
});

test("compute.preload", () => {
    const ok = compute.preload("echo");
    test.eq(ok, true);
});
JSEOF

OUTPUT=$($HULL test "$TMPDIR" 2>&1) || true
echo "$OUTPUT"
if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
    pass "JS compute tests"
else
    fail "JS compute tests"
fi

echo ""
echo "=== Results: $PASS/$TOTAL passed ==="
if [ $FAIL -gt 0 ]; then exit 1; fi
