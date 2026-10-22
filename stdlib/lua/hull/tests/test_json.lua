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

-- ── Number round trip (audit 15) ────────────────────────────────────
-- Every number used to go out as "%.14g": an integer of 1e14 or more (a
-- snowflake user id) came back as a different number.

test("integers encode exactly and round-trip", function()
    for _, n in ipairs({ 0, -1, 42, (1 << 53) - 1, 1000000000000001,
                         1234567890123456789, math.maxinteger, math.mininteger }) do
        local s = json.encode(n)
        assert_eq(s, string.format("%d", n), "text of " .. tostring(n))
        local back = json.decode(s)
        assert_eq(math.type(back), "integer", "integer back for " .. s)
        assert_eq(back, n, "value of " .. s)
    end
    local t = json.decode(json.encode({ user_id = 1234567890123456789 }))
    assert_eq(t.user_id, 1234567890123456789, "inside an object")
end)

test("floats encode in the shortest form that round-trips", function()
    assert_eq(json.encode(0.1), "0.1")
    assert_eq(json.encode(1.5), "1.5")
    for _, x in ipairs({ 0.1, 1 / 3, 2 / 3, 0.1 + 0.2, 1e15 + 0.5, 1e-300,
                         123456.789, -2.5e-7, 2.0 ^ 53 - 1 }) do
        assert_eq(json.decode(json.encode(x)), x, "round trip of " .. string.format("%.17g", x))
    end
    assert_eq(#json.encode(1 / 3) <= 19, true, "no more than 17 digits")
end)

test("NaN and infinities are still refused", function()
    for _, x in ipairs({ 0 / 0, math.huge, -math.huge }) do
        local ok = pcall(json.encode, x)
        assert_eq(ok, false, tostring(x))
    end
end)

-- ── audit 23: the decoder takes the JSON grammar only ─────────────────

test("numbers outside the JSON grammar are refused", function()
    for _, s in ipairs({ "0x10", "01", "-01", "1.", ".5", "-", "+1", "1e", "1e+",
                         "1.e5", "0x1p4", "1_000", "--1", "1e400", "-1e400",
                         "[1e999]", "[0x10]" }) do
        local ok = pcall(json.decode, s)
        assert_eq(ok, false, s)
    end
end)

test("numbers in the JSON grammar still decode", function()
    local cases = { ["0"] = 0, ["-0"] = 0, ["7"] = 7, ["-12"] = -12, ["1.5"] = 1.5,
                    ["-0.25"] = -0.25, ["1e3"] = 1000.0, ["2E-2"] = 0.02,
                    ["1.5e+2"] = 150.0, ["10"] = 10 }
    for s, want in pairs(cases) do
        assert_eq(json.decode(s), want, s)
    end
    assert_eq(json.decode("[1,2.5]")[2], 2.5, "inside an array")
end)

test("a surrogate pair decodes; a lone surrogate is an error", function()
    assert_eq(json.decode('"\\ud83d\\ude00"'), "\240\159\152\128", "U+1F600")
    assert_eq(json.decode('"\\u00e9"'), "\195\169", "U+00E9")
    for _, s in ipairs({ '"\\ud83d"', '"\\ud83dx"', '"\\ude00"', '"\\ud83d\\u0041"',
                         '"\\ud83d\\ud83d"', '"a\\udfffb"' }) do
        local ok = pcall(json.decode, s)
        assert_eq(ok, false, s)
    end
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
