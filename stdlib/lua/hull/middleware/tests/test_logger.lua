-- test_logger.lua — Tests for hull.middleware.logger
--
-- Tests pure-function helpers (no runtime globals needed).
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local logger = require('hull.middleware.logger')

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

-- ── generate_id ──────────────────────────────────────────────────────

test("generate_id returns hex string", function()
    local id = logger.generate_id()
    assert(type(id) == "string", "expected string")
    assert(#id > 0, "expected non-empty")
end)

test("generate_id increments", function()
    local a = logger.generate_id()
    local b = logger.generate_id()
    assert(a ~= b, "expected different IDs")
end)

-- ── format_line ──────────────────────────────────────────────────────

test("format_line basic pairs", function()
    local line = logger.format_line({
        { "method", "GET" },
        { "path", "/api" },
    })
    assert_eq(line, "method=GET path=/api")
end)

test("format_line quotes values with spaces", function()
    local line = logger.format_line({
        { "ua", "Mozilla Firefox" },
    })
    assert_eq(line, 'ua="Mozilla Firefox"')
end)

-- ── should_skip ──────────────────────────────────────────────────────

test("should_skip matches exact path", function()
    assert_eq(logger.should_skip("/health", {"/health"}), true)
end)

test("should_skip returns false on miss", function()
    assert_eq(logger.should_skip("/api", {"/health"}), false)
end)

test("should_skip returns false for nil list", function()
    assert_eq(logger.should_skip("/health", nil), false)
end)

-- ── middleware factory ───────────────────────────────────────────────

test("middleware returns a function", function()
    local mw = logger.middleware({})
    assert(type(mw) == "function", "expected function")
end)

return {pass = pass, fail = fail}
