-- test_session.lua - Tests for hull.web.middleware.session
--
-- Requires db, crypto, time, json globals (run via hull test harness).

local session = require('hull.web.middleware.session')

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
    -- Should not error - table created
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

-- ── audit 39: request text is stored safely ─────────────────────────

local function storable(s, max)
    return type(s) == "string" and not s:find("\0", 1, true)
        and utf8.len(s) ~= nil and #s <= max
end

test("audit 39: a hostile User-Agent / IP is stored as safe text", function()
    local ua = string.rep("A", 511) .. "\xc3\xa9" .. "Moz\xffilla\0"
    local req = { headers = { ["user-agent"] = ua }, remote_addr = "10.0.0.\xff" }
    local id = session.create({ user_id = "a39" }, { req = req })
    local list = session.list_for_user("a39")
    assert_eq(#list, 1, "one session")
    assert(storable(list[1].user_agent, 512), "storable ua")
    assert_eq(list[1].user_agent, string.rep("A", 511), "cut on a code point")
    assert(storable(list[1].ip, 64), "storable ip")
    session.destroy(id)
end)

test("audit 39: destroy of an id that is not a session id is a no-op", function()
    assert_eq(session.destroy("a\0b"), false)
    assert_eq(session.destroy(string.rep("z", 64)), false)
    assert_eq(session.destroy({}), false)
    assert_eq(session.destroy_others("x\0", "nobody"), 0)
end)

test("audit 39: audit_log records a hostile User-Agent device", function()
    local audit_log = require("hull.web.middleware.audit-log")
    audit_log.init({ fingerprint_salt = "a39-salt-salt" })
    local req = { headers = { ["user-agent"] = "Moz\xffilla caf\xc3 \0" },
                  remote_addr = "10.1.2.3" }
    audit_log.record("a39u", "login", req, { metadata = { note = "caf\xc3" } })
    local devs = audit_log.list_devices("a39u")
    assert_eq(#devs, 1, "device recorded")
    assert(storable(devs[1].user_agent, 512), "storable ua")
    assert_eq(audit_log.is_new_device("a39u", req), false, "the device is known")
end)

-- ── cleanup ──────────────────────────────────────────────────────────

test("cleanup returns count", function()
    local count = session.cleanup()
    assert(type(count) == "number", "expected number")
end)

return {pass = pass, fail = fail}
