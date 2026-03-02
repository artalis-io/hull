-- test_session.lua — Tests for hull.middleware.session
--
-- Requires db, crypto, time, json globals (run via hull test harness).

local session = require('hull.middleware.session')

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

test("init creates sessions table", function()
    session.init({ ttl = 60 })
    -- Should not error — table created
end)

-- ── create + load ────────────────────────────────────────────────────

test("create returns 64-char hex ID", function()
    local id = session.create({ user_id = 1 })
    assert(type(id) == "string", "expected string")
    assert_eq(#id, 64, "length")
end)

test("load returns stored data", function()
    local id = session.create({ user_id = 42 })
    local data = session.load(id)
    assert(data, "expected data")
    assert_eq(data.user_id, 42)
end)

-- ── destroy ──────────────────────────────────────────────────────────

test("destroy removes session", function()
    local id = session.create({ user_id = 99 })
    session.destroy(id)
    local data = session.load(id)
    assert_eq(data, nil)
end)

-- ── cleanup ──────────────────────────────────────────────────────────

test("cleanup returns count", function()
    local count = session.cleanup()
    assert(type(count) == "number", "expected number")
end)

return {pass = pass, fail = fail}
