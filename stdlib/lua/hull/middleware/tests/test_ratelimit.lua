-- test_ratelimit.lua — Tests for hull.middleware.ratelimit
--
-- Tests pure-function helpers (no runtime globals needed).
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local ratelimit = require('hull.middleware.ratelimit')

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

-- ── check ────────────────────────────────────────────────────────────

test("check allows first request", function()
    local buckets = {}
    local r = ratelimit.check(buckets, "ip1", 5, 60, 1000)
    assert_eq(r.allowed, true)
    assert_eq(r.remaining, 4)
end)

test("check blocks after limit", function()
    local buckets = {}
    for i = 1, 5 do
        ratelimit.check(buckets, "ip2", 5, 60, 1000)
    end
    local r = ratelimit.check(buckets, "ip2", 5, 60, 1000)
    assert_eq(r.allowed, false)
    assert_eq(r.remaining, 0)
end)

test("check resets after window", function()
    local buckets = {}
    for i = 1, 5 do
        ratelimit.check(buckets, "ip3", 5, 60, 1000)
    end
    local r = ratelimit.check(buckets, "ip3", 5, 60, 1061)
    assert_eq(r.allowed, true)
    assert_eq(r.remaining, 4)
end)

test("check returns remaining count", function()
    local buckets = {}
    ratelimit.check(buckets, "ip4", 10, 60, 1000)
    ratelimit.check(buckets, "ip4", 10, 60, 1000)
    local r = ratelimit.check(buckets, "ip4", 10, 60, 1000)
    assert_eq(r.remaining, 7)
end)

test("check returns reset timestamp", function()
    local buckets = {}
    local r = ratelimit.check(buckets, "ip5", 5, 60, 1000)
    assert_eq(r.reset, 1060)
end)

-- ── middleware factory ───────────────────────────────────────────────

test("middleware returns a function", function()
    local mw = ratelimit.middleware({ limit = 10, window = 60 })
    assert(type(mw) == "function", "expected function")
end)

return {pass = pass, fail = fail}
