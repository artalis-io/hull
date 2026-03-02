-- test_cors.lua — Tests for hull.middleware.cors
--
-- Tests pure-function helpers (no runtime globals needed).
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local cors = require('hull.middleware.cors')

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

-- ── is_allowed_origin ────────────────────────────────────────────────

test("is_allowed_origin exact match", function()
    assert_eq(cors.is_allowed_origin("http://a.com", {"http://a.com"}), true)
end)

test("is_allowed_origin wildcard", function()
    assert_eq(cors.is_allowed_origin("http://any.com", {"*"}), true)
end)

test("is_allowed_origin nil origin", function()
    assert_eq(cors.is_allowed_origin(nil, {"*"}), false)
end)

test("is_allowed_origin nil origins list", function()
    assert_eq(cors.is_allowed_origin("http://a.com", nil), false)
end)

test("is_allowed_origin miss", function()
    assert_eq(cors.is_allowed_origin("http://b.com", {"http://a.com"}), false)
end)

-- ── middleware factory ───────────────────────────────────────────────

test("middleware returns a function", function()
    local mw = cors.middleware({ origins = {"*"} })
    assert(type(mw) == "function", "expected function")
end)

return {pass = pass, fail = fail}
