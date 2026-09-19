-- test_json.lua - Tests for Hull JSON wrapper (hull.json)
--
-- Tests the Hull wrapper API contract.
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local json = require('hull.json')

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

-- ── API contract tests ────────────────────────────────────────────────

test("require returns table with encode and decode", function()
    assert(type(json) == "table", "expected table")
    assert(type(json.encode) == "function", "expected encode function")
    assert(type(json.decode) == "function", "expected decode function")
end)

test("encode empty table is an array", function()
    -- Lua cannot distinguish an empty object from an empty array: both are
    -- `{}`. The vendored encoder resolves the ambiguity toward ARRAY --
    -- `if rawget(val, 1) ~= nil or next(val) == nil` (stdlib/lua/vendor/json.lua)
    -- -- so `{}` round-trips to `[]`, not `{}`. This test asserted `{}` and
    -- never ran, so nothing caught the mismatch. Asserting the real behaviour
    -- rather than patching a vendored library to match a stale expectation.
    assert_eq(json.encode({}), "[]")
end)

test("decode empty object", function()
    local t = json.decode('{}')
    assert(type(t) == "table", "expected table")
    assert(next(t) == nil, "expected empty table")
end)

test("roundtrip via hull namespace", function()
    local original = {name = "hull", count = 42}
    local s = json.encode(original)
    local decoded = json.decode(s)
    assert_eq(decoded.name, "hull")
    assert_eq(decoded.count, 42)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
