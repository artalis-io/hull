-- test_json.lua — Tests for vendored rxi/json.lua
--
-- Tests the raw vendor library in our sandbox.
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local json = require('vendor.json')

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

-- ── Encode tests ──────────────────────────────────────────────────────

test("encode empty object", function()
    assert_eq(json.encode({}), "{}")
end)

test("encode object", function()
    local s = json.encode({name = "hull"})
    -- Should be valid JSON with the key
    assert(string.find(s, '"name"'), "missing key")
    assert(string.find(s, '"hull"'), "missing value")
end)

test("encode array", function()
    assert_eq(json.encode({1, 2, 3}), "[1,2,3]")
end)

test("encode nested", function()
    local s = json.encode({items = {1, 2}})
    assert(string.find(s, '"items"'), "missing key")
    assert(string.find(s, '[1,2]', 1, true), "missing array")
end)

test("encode string escaping", function()
    local s = json.encode({msg = 'hello "world"\nnewline'})
    assert(string.find(s, '\\"world\\"', 1, true), "missing escaped quotes")
    assert(string.find(s, '\\n', 1, true), "missing escaped newline")
end)

test("encode numbers", function()
    assert_eq(json.encode(42), "42")
    assert_eq(json.encode(0), "0")
    assert_eq(json.encode(-1), "-1")
end)

test("encode booleans", function()
    assert_eq(json.encode(true), "true")
    assert_eq(json.encode(false), "false")
end)

test("encode nil", function()
    assert_eq(json.encode(nil), "null")
end)

-- ── Decode tests ──────────────────────────────────────────────────────

test("decode object", function()
    local t = json.decode('{"a":1}')
    assert_eq(t.a, 1)
end)

test("decode array", function()
    local t = json.decode('[1,2,3]')
    assert_eq(#t, 3)
    assert_eq(t[1], 1)
    assert_eq(t[3], 3)
end)

test("decode nested", function()
    local t = json.decode('{"items":[1,2],"meta":{"count":2}}')
    assert_eq(#t.items, 2)
    assert_eq(t.meta.count, 2)
end)

test("decode booleans and null", function()
    local t = json.decode('{"a":true,"b":false,"c":null}')
    assert_eq(t.a, true)
    assert_eq(t.b, false)
    assert_eq(t.c, nil)
end)

test("decode string escapes", function()
    local t = json.decode('{"msg":"hello\\nworld"}')
    assert_eq(t.msg, "hello\nworld")
end)

test("decode unicode", function()
    local t = json.decode('{"c":"\\u0041"}')
    assert_eq(t.c, "A")
end)

test("decode numbers", function()
    local t = json.decode('{"i":42,"f":3.14,"n":-1}')
    assert_eq(t.i, 42)
    assert(math.abs(t.f - 3.14) < 0.001, "float mismatch")
    assert_eq(t.n, -1)
end)

test("decode invalid JSON raises error", function()
    local ok = pcall(json.decode, '{invalid}')
    assert(not ok, "expected error for invalid JSON")
end)

-- ── Roundtrip ─────────────────────────────────────────────────────────

test("roundtrip object", function()
    local original = {name = "hull", version = 1, active = true}
    local s = json.encode(original)
    local decoded = json.decode(s)
    assert_eq(decoded.name, "hull")
    assert_eq(decoded.version, 1)
    assert_eq(decoded.active, true)
end)

test("roundtrip array", function()
    local original = {1, "two", true, false}
    local s = json.encode(original)
    local decoded = json.decode(s)
    assert_eq(#decoded, 4)
    assert_eq(decoded[1], 1)
    assert_eq(decoded[2], "two")
    assert_eq(decoded[3], true)
    assert_eq(decoded[4], false)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
