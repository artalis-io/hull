-- test_form.lua — Tests for hull.form
--
-- Tests pure-function URL-encoded body parsing (no runtime globals needed).
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local form = require('hull.form')

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

-- ── basic parsing ────────────────────────────────────────────────────

test("simple key-value pair", function()
    local r = form.parse("name=alice")
    assert_eq(r.name, "alice")
end)

test("multiple pairs", function()
    local r = form.parse("email=a%40b.com&pass=secret")
    assert_eq(r.email, "a@b.com")
    assert_eq(r.pass, "secret")
end)

test("plus decoded to space", function()
    local r = form.parse("msg=hello+world")
    assert_eq(r.msg, "hello world")
end)

test("percent decoding", function()
    local r = form.parse("q=%E4%BD%A0%E5%A5%BD")
    assert_eq(r.q, "\xE4\xBD\xA0\xE5\xA5\xBD")
end)

-- ── edge cases ───────────────────────────────────────────────────────

test("nil returns empty table", function()
    local r = form.parse(nil)
    assert(next(r) == nil, "expected empty table")
end)

test("empty string returns empty table", function()
    local r = form.parse("")
    assert(next(r) == nil, "expected empty table")
end)

test("non-string returns empty table", function()
    local r = form.parse(42)
    assert(next(r) == nil, "expected empty table")
end)

test("pair without = is skipped", function()
    local r = form.parse("noequals&key=val")
    assert_eq(r.key, "val")
    assert_eq(r.noequals, nil)
end)

test("empty key is skipped", function()
    local r = form.parse("=value&key=val")
    assert_eq(r[""], nil)
    assert_eq(r.key, "val")
end)

test("empty value is preserved", function()
    local r = form.parse("key=")
    assert_eq(r.key, "")
end)

test("duplicate keys: last wins", function()
    local r = form.parse("a=1&a=2&a=3")
    assert_eq(r.a, "3")
end)

test("value with = sign", function()
    local r = form.parse("data=a=b=c")
    assert_eq(r.data, "a=b=c")
end)

test("multiple & separators", function()
    local r = form.parse("a=1&&b=2")
    assert_eq(r.a, "1")
    assert_eq(r.b, "2")
end)

test("key with percent encoding", function()
    local r = form.parse("my%20key=value")
    assert_eq(r["my key"], "value")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
