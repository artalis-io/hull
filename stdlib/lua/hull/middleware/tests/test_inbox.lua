-- test_inbox.lua — Tests for hull.middleware.inbox
--
-- Requires db, time globals (run via hull test harness).

local inbox = require('hull.middleware.inbox')

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

-- ── init ─────────────────────────────────────────────────────────────

test("init creates inbox table", function()
    inbox.init({ ttl = 60 })
end)

-- ── is_duplicate ─────────────────────────────────────────────────────

test("new message is not a duplicate", function()
    assert_eq(inbox.is_duplicate("msg-new-1", "test"), false)
end)

test("nil message_id returns false", function()
    assert_eq(inbox.is_duplicate(nil, "test"), false)
end)

test("empty message_id returns false", function()
    assert_eq(inbox.is_duplicate("", "test"), false)
end)

-- ── mark_processed ───────────────────────────────────────────────────

test("mark_processed then is_duplicate returns true", function()
    inbox.mark_processed("msg-mark-1", "test")
    assert_eq(inbox.is_duplicate("msg-mark-1", "test"), true)
end)

test("different source is not duplicate", function()
    inbox.mark_processed("msg-src-1", "source-A")
    assert_eq(inbox.is_duplicate("msg-src-1", "source-B"), false)
end)

-- ── check_and_mark ───────────────────────────────────────────────────

test("check_and_mark returns false for new message", function()
    local dup = inbox.check_and_mark("msg-cam-1", "test")
    assert_eq(dup, false, "first call should not be duplicate")
end)

test("check_and_mark returns true for second call", function()
    inbox.check_and_mark("msg-cam-2", "test")
    local dup = inbox.check_and_mark("msg-cam-2", "test")
    assert_eq(dup, true, "second call should be duplicate")
end)

-- ── default source ───────────────────────────────────────────────────

test("default source works", function()
    inbox.mark_processed("msg-default-1")
    assert_eq(inbox.is_duplicate("msg-default-1"), false)
    assert_eq(inbox.is_duplicate("msg-default-1", "default"), true)
end)

-- ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", function()
    local count = inbox.cleanup()
    assert(type(count) == "number", "expected number")
end)

return {pass = pass, fail = fail}
