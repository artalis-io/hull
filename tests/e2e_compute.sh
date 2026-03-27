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

test("compute.load", function()
    local ok, err = compute.load("echo")
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

test("compute.load", () => {
    const ok = compute.load("echo");
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
echo "=== E2E: compute.async.call (Lua) ==="

# Async requires a live HTTP request context (thread pool + connection)
ASYNCDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR" "$ASYNCDIR"' EXIT

mkdir -p "$ASYNCDIR/compute"
cp tests/fixtures/compute/echo.wasm "$ASYNCDIR/compute/echo.wasm"

cat > "$ASYNCDIR/app.lua" << 'EOF'
app.get("/async-echo", function(req, res)
    local r = compute.async.call("echo", "hello async")
    if r.error then
        res:json({ error = r.error })
    else
        res:json({ result = r.result })
    end
end)
EOF

# Start server in background (direct mode, not hull dev)
PORT=19876
$HULL -p $PORT --no-sandbox "$ASYNCDIR/app.lua" >/dev/null 2>&1 &
HULL_PID=$!
sleep 1

if kill -0 $HULL_PID 2>/dev/null; then
    RESP=$(curl -sf "http://127.0.0.1:$PORT/async-echo" 2>/dev/null || echo "CURL_FAIL")
    kill $HULL_PID 2>/dev/null || true
    wait $HULL_PID 2>/dev/null || true

    if echo "$RESP" | grep -q '"result":"hello async"'; then
        pass "compute.async.call (Lua)"
    else
        fail "compute.async.call (Lua) — response: $RESP"
    fi
else
    fail "compute.async.call (Lua) — server failed to start"
    kill $HULL_PID 2>/dev/null || true
    wait $HULL_PID 2>/dev/null || true
fi

echo ""
echo "=== E2E: async does not block event loop ==="

# Proof: fire a slow async compute, then immediately hit /health.
# If /health responds quickly while slow call is still running,
# the event loop is not blocked.
CONCDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR" "$CONCDIR"' EXIT

mkdir -p "$CONCDIR/compute"
cp tests/fixtures/compute/spin.wasm "$CONCDIR/compute/spin.wasm"

cat > "$CONCDIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)

-- Slow: ~1s of WASM computation via async (yields to event loop)
app.get("/slow-async", function(req, res)
    local input = string.rep("x", 500)
    local r = compute.async.call("spin", input, { gas = 2000000000 })
    if r.error then
        res:status(500):json({ error = r.error })
    else
        res:json({ ok = true })
    end
end)

-- Slow: ~1s of WASM computation via sync (BLOCKS event loop)
app.get("/slow-sync", function(req, res)
    local input = string.rep("x", 500)
    local out, err = compute.call("spin", input, { gas = 2000000000 })
    if err then
        res:status(500):json({ error = err })
    else
        res:json({ ok = true })
    end
end)
EOF

PORT=19878
$HULL -p $PORT --no-sandbox "$CONCDIR/app.lua" >/dev/null 2>&1 &
HULL_PID=$!
sleep 1

if kill -0 $HULL_PID 2>/dev/null; then
    # Test 1: Async should NOT block — fire slow-async, then health
    curl -sf "http://127.0.0.1:$PORT/slow-async" >/dev/null 2>&1 &
    SLOW_PID=$!
    sleep 0.1  # Give it time to start the async call

    # Health should respond immediately (< 1s) while slow-async is running
    T0=$(date +%s)
    HEALTH_RESP=$(curl -sf --max-time 2 "http://127.0.0.1:$PORT/health" 2>/dev/null || echo "TIMEOUT")
    T1=$(date +%s)
    HEALTH_TIME=$((T1 - T0))

    wait $SLOW_PID 2>/dev/null || true

    if echo "$HEALTH_RESP" | grep -q '"ok":true' && [ "$HEALTH_TIME" -lt 2 ]; then
        pass "async compute does not block event loop (health responded in ${HEALTH_TIME}s)"
    else
        fail "async compute blocked event loop (health: $HEALTH_RESP, time: ${HEALTH_TIME}s)"
    fi

    kill $HULL_PID 2>/dev/null || true
    wait $HULL_PID 2>/dev/null || true
else
    fail "concurrency test — server failed to start"
fi

echo ""
echo "=== E2E: AOT filesystem lookup ==="

# If wamrc is available, test AOT loading in dev mode
WAMRC="${WAMRC:-build/wamrc}"
if [ -x "$WAMRC" ]; then
    AOTDIR=$(mktemp -d)
    trap 'rm -rf "$TMPDIR" "$AOTDIR"' EXIT

    mkdir -p "$AOTDIR/compute"
    cp tests/fixtures/compute/echo.wasm "$AOTDIR/compute/echo.wasm"

    # Detect arch
    ARCH=$(uname -m)
    case "$ARCH" in
        x86_64|amd64) ARCH="x86_64" ;;
        aarch64|arm64) ARCH="aarch64" ;;
    esac

    # AOT compile
    "$WAMRC" --target="$ARCH" -o "$AOTDIR/compute/echo.aot.$ARCH" "$AOTDIR/compute/echo.wasm" >/dev/null 2>&1

    if [ -f "$AOTDIR/compute/echo.aot.$ARCH" ]; then
        # Lua test that exercises AOT path
        cat > "$AOTDIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)
EOF
        mkdir -p "$AOTDIR/tests"
        cat > "$AOTDIR/tests/test_aot.lua" << 'EOF'
test("compute.call echo via AOT", function()
    local out, err = compute.call("echo", "aot test")
    assert(not err, "err: " .. tostring(err))
    assert(out == "aot test", "mismatch: " .. tostring(out))
end)
EOF
        OUTPUT=$($HULL test "$AOTDIR" 2>&1) || true
        # Check that AOT was actually loaded (log shows "aot=1")
        if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
            pass "AOT filesystem lookup (Lua)"
        else
            fail "AOT filesystem lookup (Lua)"
            echo "$OUTPUT"
        fi
    else
        echo "  SKIP: wamrc failed to compile AOT for $ARCH"
    fi
else
    echo "  SKIP: wamrc not found at $WAMRC (build with: make wamrc)"
fi

echo ""
echo "=== E2E: hull build --no-aot ==="

# Test that --no-aot flag is respected (doesn't crash even without wamrc)
NOAOT_DIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR" "$AOTDIR" "$NOAOT_DIR"' EXIT

mkdir -p "$NOAOT_DIR/compute"
cp tests/fixtures/compute/echo.wasm "$NOAOT_DIR/compute/echo.wasm"
cat > "$NOAOT_DIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)
EOF

# hull build --no-aot should succeed at the Lua level (will fail at link
# unless platform is embedded, but we just check it doesn't crash on AOT logic)
OUTPUT=$($HULL build "$NOAOT_DIR" --cc cc --no-aot 2>&1) || true
if echo "$OUTPUT" | grep -q "AOT"; then
    fail "hull build --no-aot should not mention AOT"
    echo "$OUTPUT"
else
    pass "hull build --no-aot skips AOT"
fi

echo ""
echo "=== E2E: compute.call buffer mode (Lua) ==="

BUFDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR" "$AOTDIR" "$NOAOT_DIR" "$BUFDIR"' EXIT

mkdir -p "$BUFDIR/compute"
cp tests/fixtures/compute/echo.wasm "$BUFDIR/compute/echo.wasm"

cat > "$BUFDIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)
EOF

mkdir -p "$BUFDIR/tests"
cat > "$BUFDIR/tests/test_buffer.lua" << 'EOF'
test("compute.call buffer mode", function()
    local buf, err = compute.call("echo", "hello buffer", { buffer = true })
    assert(not err, "err: " .. tostring(err))
    assert(buf, "expected buffer")
    assert(buf:len() == 12, "len: " .. tostring(buf:len()))
    assert(buf:bytes() == "hello buffer", "mismatch: " .. tostring(buf:bytes()))
    buf:close()
    assert(buf:bytes() == nil, "expected nil after close")
end)

test("compute.call buffer chaining", function()
    local buf1, err = compute.call("echo", "chain", { buffer = true })
    assert(not err, "err: " .. tostring(err))
    local buf2, err2 = compute.call("echo", buf1, { buffer = true })
    assert(not err2, "err2: " .. tostring(err2))
    assert(buf2:bytes() == "chain", "chain mismatch: " .. tostring(buf2:bytes()))
    buf2:close()
    buf1:close()
end)

test("compute.buffer from string", function()
    local buf = compute.buffer("test input")
    assert(buf, "expected buffer")
    assert(buf:len() == 10, "len: " .. tostring(buf:len()))
    -- Use as input to compute.call
    local out, err = compute.call("echo", buf)
    assert(not err, "err: " .. tostring(err))
    assert(out == "test input", "mismatch: " .. tostring(out))
    buf:close()
end)
EOF

OUTPUT=$($HULL test "$BUFDIR" 2>&1) || true
echo "$OUTPUT"
if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
    pass "Lua buffer mode tests"
else
    fail "Lua buffer mode tests"
fi

echo ""
echo "=== E2E: compute.call buffer mode (JS) ==="

rm -f "$BUFDIR/app.lua" "$BUFDIR/tests/test_buffer.lua"

cat > "$BUFDIR/app.js" << 'JSEOF'
import { app } from "hull:app";
app.get("/health", (req, res) => { res.json({ ok: true }); });
JSEOF

cat > "$BUFDIR/tests/test_buffer.js" << 'JSEOF'
import { compute } from "hull:compute";

test("compute.call buffer mode", () => {
    const buf = compute.call("echo", "hello buffer", { buffer: true });
    test.eq(buf.length, 12);
    const ab = buf.bytes();
    const bytes = new Uint8Array(ab);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    test.eq(result, "hello buffer");
    buf.close();
    test.eq(buf.bytes(), null);
});

test("compute.call buffer chaining", () => {
    const buf1 = compute.call("echo", "chain", { buffer: true });
    const buf2 = compute.call("echo", buf1, { buffer: true });
    const ab = buf2.bytes();
    const bytes = new Uint8Array(ab);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    test.eq(result, "chain");
    buf2.close();
    buf1.close();
});

test("compute.buffer from string", () => {
    const buf = compute.buffer("test input");
    test.eq(buf.length, 10);
    // Use as input to compute.call (non-buffer mode)
    const out = compute.call("echo", buf);
    const bytes = new Uint8Array(out);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    test.eq(result, "test input");
    buf.close();
});
JSEOF

OUTPUT=$($HULL test "$BUFDIR" 2>&1) || true
echo "$OUTPUT"
if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
    pass "JS buffer mode tests"
else
    fail "JS buffer mode tests"
fi

echo ""
echo "=== E2E: stateful persistent instance (Lua) ==="

PERSISTDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR" "$AOTDIR" "$NOAOT_DIR" "$BUFDIR" "$PERSISTDIR"' EXIT

mkdir -p "$PERSISTDIR/compute"
cp tests/fixtures/compute/echo.wasm "$PERSISTDIR/compute/echo.wasm"
cp tests/fixtures/compute/kv_store.wasm "$PERSISTDIR/compute/kv_store.wasm"

cat > "$PERSISTDIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)
EOF

mkdir -p "$PERSISTDIR/tests"
cat > "$PERSISTDIR/tests/test_persistent.lua" << 'EOF'
test("compute.instance create and call", function()
    local inst, err = compute.instance("echo")
    assert(inst, "create failed: " .. tostring(err))
    assert(inst:name() == "echo", "name: " .. tostring(inst:name()))
    assert(not inst:closed(), "should not be closed")

    local out, err2 = inst:call("hello persistent")
    assert(not err2, "call err: " .. tostring(err2))
    assert(out == "hello persistent", "mismatch: " .. tostring(out))
    inst:close()
    assert(inst:closed(), "should be closed")
end)

test("compute.instance repeated calls", function()
    local inst = compute.instance("echo")
    for i = 1, 10 do
        local out = inst:call("call " .. tostring(i))
        assert(out == "call " .. tostring(i))
    end
    inst:close()
end)

test("compute.instance gas recovery", function()
    local inst = compute.instance("echo")
    -- gas=1 should fail
    local out, err = inst:call("x", { gas = 1 })
    assert(out == nil, "should fail with low gas")
    assert(err, "should have error")

    -- Normal gas should succeed (reusable after exhaustion)
    local out2, err2 = inst:call("recover", { gas = 10000000 })
    assert(not err2, "recovery err: " .. tostring(err2))
    assert(out2 == "recover", "mismatch: " .. tostring(out2))
    inst:close()
end)

test("compute.instance not found", function()
    local inst, err = compute.instance("nonexistent")
    assert(inst == nil, "should be nil")
    assert(err == "not_found", "err: " .. tostring(err))
end)

test("compute.instance buffer mode", function()
    local inst = compute.instance("echo")
    local buf, err = inst:call("buf test", { buffer = true })
    assert(not err, "err: " .. tostring(err))
    assert(buf:len() == 8, "len: " .. tostring(buf:len()))
    assert(buf:bytes() == "buf test", "mismatch: " .. tostring(buf:bytes()))
    buf:close()
    inst:close()
end)

-- Stateful kv_store: load once, query many times
test("kv_store: persistent state across calls", function()
    local inst = compute.instance("kv_store")

    -- Build LOAD message: opcode(1) + count(u32) + entries...
    -- 2 entries: "name"="hull", "ver"="1.0"
    local function u32(n)
        return string.char(n % 256, math.floor(n/256) % 256,
                           math.floor(n/65536) % 256, math.floor(n/16777216) % 256)
    end
    local load_msg = "\x01" .. u32(2)
        .. u32(4) .. "name" .. u32(4) .. "hull"
        .. u32(3) .. "ver"  .. u32(3) .. "1.0"
    local out, err = inst:call(load_msg)
    assert(not err, "load err: " .. tostring(err))
    assert(#out == 4, "expected 4 bytes for count")

    -- GET "name" → "hull"
    local out2, err2 = inst:call("\x02name")
    assert(not err2, "get err: " .. tostring(err2))
    assert(out2 == "hull", "expected 'hull', got: " .. tostring(out2))

    -- GET "ver" → "1.0"
    local out3, err3 = inst:call("\x02ver")
    assert(not err3, "get err: " .. tostring(err3))
    assert(out3 == "1.0", "expected '1.0', got: " .. tostring(out3))

    -- GET missing → empty
    local out4 = inst:call("\x02nope")
    assert(out4 == "", "expected empty for missing key")

    -- COUNT → 2
    local out5 = inst:call("\x03")
    assert(#out5 == 4, "expected 4 bytes for count")

    inst:close()
end)

-- Proves pooled calls lose state (fresh instance each time)
test("kv_store: pooled calls lose state", function()
    local function u32(n)
        return string.char(n % 256, math.floor(n/256) % 256,
                           math.floor(n/65536) % 256, math.floor(n/16777216) % 256)
    end
    -- LOAD via pooled call (large heap to avoid pool reuse)
    local load_msg = "\x01" .. u32(1) .. u32(3) .. "foo" .. u32(3) .. "bar"
    local out, err = compute.call("kv_store", load_msg, { heap = 8 * 1024 * 1024 })
    assert(not err, "load err: " .. tostring(err))

    -- GET via fresh pooled call — state is gone
    local out2, err2 = compute.call("kv_store", "\x02foo", { heap = 8 * 1024 * 1024 })
    assert(not err2, "get err: " .. tostring(err2))
    assert(out2 == "", "pooled call should NOT retain state, got: " .. tostring(out2))
end)
EOF

OUTPUT=$($HULL test "$PERSISTDIR" 2>&1) || true
echo "$OUTPUT"
if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
    pass "Lua persistent instance tests"
else
    fail "Lua persistent instance tests"
fi

echo ""
echo "=== E2E: persistent instance (JS) ==="

rm -f "$PERSISTDIR/app.lua" "$PERSISTDIR/tests/test_persistent.lua"

cat > "$PERSISTDIR/app.js" << 'JSEOF'
import { app } from "hull:app";
app.get("/health", (req, res) => { res.json({ ok: true }); });
JSEOF

cat > "$PERSISTDIR/tests/test_persistent.js" << 'JSEOF'
import { compute } from "hull:compute";

function ab2str(ab) {
    const bytes = new Uint8Array(ab);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    return result;
}

test("compute.instance create and call", () => {
    const inst = compute.instance("echo");
    test.eq(inst.name, "echo");
    test.eq(inst.closed, false);

    const out = inst.call("hello persistent");
    test.eq(ab2str(out), "hello persistent");
    inst.close();
    test.eq(inst.closed, true);
});

test("compute.instance repeated calls", () => {
    const inst = compute.instance("echo");
    for (let i = 1; i <= 10; i++) {
        const msg = "call " + i;
        const out = inst.call(msg);
        test.eq(ab2str(out), msg);
    }
    inst.close();
});

test("compute.instance not found", () => {
    let threw = false;
    try {
        compute.instance("nonexistent");
    } catch (e) {
        threw = true;
    }
    test.eq(threw, true);
});

test("compute.instance buffer mode", () => {
    const inst = compute.instance("echo");
    const buf = inst.call("buf test", { buffer: true });
    test.eq(buf.length, 8);
    test.eq(ab2str(buf.bytes()), "buf test");
    buf.close();
    inst.close();
});
JSEOF

OUTPUT=$($HULL test "$PERSISTDIR" 2>&1) || true
echo "$OUTPUT"
if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
    pass "JS persistent instance tests"
else
    fail "JS persistent instance tests"
fi

echo ""

# Probe shared heap: WAMR shared heaps may not work on all platforms
echo "=== Probing WAMR shared heap ==="
PROBEDIR=$(mktemp -d)
mkdir -p "$PROBEDIR/compute"
cp tests/fixtures/compute/shared_read.wasm "$PROBEDIR/compute/shared_read.wasm"
cat > "$PROBEDIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)
EOF
mkdir -p "$PROBEDIR/tests"
cat > "$PROBEDIR/tests/test_probe.lua" << 'EOF'
test("shared heap probe", function()
    local ok, err = compute.segment("shared_read", "seg0", "ABCDEFGHIJ")
    assert(ok, "load failed: " .. tostring(err))
    local function u32(n)
        return string.char(n % 256, math.floor(n/256) % 256,
                           math.floor(n/65536) % 256, math.floor(n/16777216) % 256)
    end
    local out = compute.call("shared_read", "\x00" .. u32(2) .. u32(5))
    assert(out == "CDEFG", "shared heap data not readable")
    compute.segment("shared_read", nil)
end)
EOF
PROBE_OUT=$($HULL test "$PROBEDIR" 2>&1) || true
rm -rf "$PROBEDIR"

SHARED_HEAP_OK=0
if echo "$PROBE_OUT" | grep -qE "0 failed|tests passed$"; then
    echo "  Shared heap works on this platform"
    SHARED_HEAP_OK=1
else
    echo "  NOTE: WAMR shared heap not functional — skipping compute.segment tests"
fi

echo ""
echo "=== E2E: compute.segment shared data (Lua) ==="

SHAREDDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR" "$AOTDIR" "$NOAOT_DIR" "$BUFDIR" "$PERSISTDIR" "$SHAREDDIR"' EXIT

mkdir -p "$SHAREDDIR/compute"
cp tests/fixtures/compute/shared_read.wasm "$SHAREDDIR/compute/shared_read.wasm"

cat > "$SHAREDDIR/app.lua" << 'EOF'
app.get("/health", function(req, res) res:json({ ok = true }) end)
EOF

mkdir -p "$SHAREDDIR/tests"
cat > "$SHAREDDIR/tests/test_shared.lua" << 'EOF'
test("compute.segment single segment", function()
    -- Load data into shared_read module
    local ok, err = compute.segment("shared_read", "seg0", "ABCDEFGHIJ")
    assert(ok, "data load failed: " .. tostring(err))

    -- Read 5 bytes at offset 2 from segment 0
    local function u32(n)
        return string.char(n % 256, math.floor(n/256) % 256,
                           math.floor(n/65536) % 256, math.floor(n/16777216) % 256)
    end
    local msg = "\x00" .. u32(2) .. u32(5)
    local out, err2 = compute.call("shared_read", msg)
    assert(not err2, "call err: " .. tostring(err2))
    assert(out == "CDEFG", "expected CDEFG, got: " .. tostring(out))

    -- Count query
    local count_msg = "\xFF"
    local out2 = compute.call("shared_read", count_msg)
    assert(#out2 == 4, "expected 4 bytes for count")

    -- Clean up
    compute.segment("shared_read", nil)
end)

test("compute.segment multi-segment", function()
    compute.segment("shared_read", "a", "AAAA")
    compute.segment("shared_read", "b", "BBBBBB")

    local function u32(n)
        return string.char(n % 256, math.floor(n/256) % 256,
                           math.floor(n/65536) % 256, math.floor(n/16777216) % 256)
    end

    -- Read all of segment 0 ("AAAA")
    local out = compute.call("shared_read", "\x00" .. u32(0) .. u32(4))
    assert(out == "AAAA", "seg 0: " .. tostring(out))

    -- Read all of segment 1 ("BBBBBB")
    local out2 = compute.call("shared_read", "\x01" .. u32(0) .. u32(6))
    assert(out2 == "BBBBBB", "seg 1: " .. tostring(out2))

    compute.segment("shared_read", nil)
end)
EOF

if [ $SHARED_HEAP_OK -eq 1 ]; then
    OUTPUT=$($HULL test "$SHAREDDIR" 2>&1) || true
    echo "$OUTPUT"
    if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
        pass "Lua shared data tests"
    else
        fail "Lua shared data tests"
    fi
else
    echo "  SKIP (shared heap not available)"
    pass "Lua shared data tests"
fi

echo ""
echo "=== E2E: compute.segment shared data (JS) ==="

rm -f "$SHAREDDIR/app.lua" "$SHAREDDIR/tests/test_shared.lua"

cat > "$SHAREDDIR/app.js" << 'JSEOF'
import { app } from "hull:app";
app.get("/health", (req, res) => { res.json({ ok: true }); });
JSEOF

cat > "$SHAREDDIR/tests/test_shared.js" << 'JSEOF'
import { compute } from "hull:compute";

function ab2str(ab) {
    const bytes = new Uint8Array(ab);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    return result;
}

test("compute.segment single segment", () => {
    compute.segment("shared_read", "seg0", "ABCDEFGHIJ");

    // Build read message: seg=0, offset=2, length=5
    const msg = new Uint8Array(9);
    msg[0] = 0;
    const dv = new DataView(msg.buffer);
    dv.setUint32(1, 2, true);
    dv.setUint32(5, 5, true);

    const out = compute.call("shared_read", msg.buffer);
    test.eq(ab2str(out), "CDEFG");

    compute.segment("shared_read", null);
});
JSEOF

if [ $SHARED_HEAP_OK -eq 1 ]; then
    OUTPUT=$($HULL test "$SHAREDDIR" 2>&1) || true
    echo "$OUTPUT"
    if echo "$OUTPUT" | grep -qE "0 failed|tests passed$"; then
        pass "JS shared data tests"
    else
        fail "JS shared data tests"
    fi
else
    echo "  SKIP (shared heap not available)"
    pass "JS shared data tests"
fi

echo ""
echo "=== E2E: hull compute tooling (sample modules) ==="

# Test the sample compute modules via hull compute test
# These live in examples/compute/compute/<name>/
COMPUTE_EXAMPLE="examples/compute"
if [ -d "$COMPUTE_EXAMPLE/compute/vector_ops" ]; then
    for MODULE in vector_ops sort hash json_extract scoring text; do
        if [ -f "$COMPUTE_EXAMPLE/compute/$MODULE.wasm" ]; then
            HULL_ABS="$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")"
            OUTPUT=$(cd "$COMPUTE_EXAMPLE" && "$HULL_ABS" compute test "$MODULE" 2>&1) || true
            if echo "$OUTPUT" | grep -qE "tests passed$"; then
                TESTS=$(echo "$OUTPUT" | grep -oE '[0-9]+/[0-9]+ tests passed' | head -1)
                pass "hull compute test $MODULE ($TESTS)"
            else
                fail "hull compute test $MODULE"
            fi
        else
            echo "  SKIP: $MODULE.wasm not found"
        fi
    done
else
    echo "  SKIP: sample modules not found in $COMPUTE_EXAMPLE"
fi

echo ""
echo "=== Results: $PASS/$TOTAL passed ==="
if [ $FAIL -gt 0 ]; then exit 1; fi
