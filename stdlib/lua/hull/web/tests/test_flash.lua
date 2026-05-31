-- test_flash.lua — Tests for hull.web.flash
--
-- Covers input validation and the HX-Trigger path. The session-backed
-- paths (set / consume) require a live SQLite session store; they are
-- exercised end-to-end in examples/hypermedia_todo/tests/test_app.lua.

local flash = require('hull.web.flash')
local json  = require('hull.json')

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

-- Fake res object that captures headers in a table.
local function fake_res()
    local headers = {}
    return {
        header = function(self, k, v) headers[k] = v end,
        _headers = headers,
    }
end

-- ── trigger ──────────────────────────────────────────────────────────

test("trigger sets HX-Trigger header with default kind=info", function()
    local res = fake_res()
    flash.trigger(res, "Saved.")
    local val = res._headers["HX-Trigger"]
    assert(val, "header set")
    local payload = json.decode(val)
    assert_eq(payload.flash.text, "Saved.")
    assert_eq(payload.flash.kind, "info")
end)

test("trigger honors custom kind", function()
    local res = fake_res()
    flash.trigger(res, "Bad input", "error")
    local payload = json.decode(res._headers["HX-Trigger"])
    assert_eq(payload.flash.text, "Bad input")
    assert_eq(payload.flash.kind, "error")
end)

test("trigger coerces non-string text", function()
    local res = fake_res()
    flash.trigger(res, 42)
    local payload = json.decode(res._headers["HX-Trigger"])
    assert_eq(payload.flash.text, "42")
end)

test("trigger rejects missing res", function()
    local ok = pcall(flash.trigger, nil, "x")
    assert_eq(ok, false, "should error")
end)

test("trigger rejects missing text", function()
    local ok = pcall(flash.trigger, fake_res(), nil)
    assert_eq(ok, false, "should error")
end)

-- ── set: input validation (no session needed for the error path) ────

test("set rejects missing text", function()
    local req = { ctx = { session_id = "fake", session = {} } }
    local ok, err = pcall(flash.set, req, nil)
    assert_eq(ok, false, "should error")
    assert(tostring(err):find("text is required"),
           "error mentions text required, got: " .. tostring(err))
end)

test("set errors when no session_id in req.ctx", function()
    local req = { ctx = {} }
    local ok, err = pcall(flash.set, req, "hi")
    assert_eq(ok, false, "should error")
    assert(tostring(err):find("no session"),
           "error mentions session, got: " .. tostring(err))
end)

test("set errors when req is nil", function()
    local ok = pcall(flash.set, nil, "hi")
    assert_eq(ok, false, "should error")
end)

-- ── consume: safe-defaults ───────────────────────────────────────────

test("consume returns empty array when no session", function()
    local msgs = flash.consume({ ctx = {} })
    assert_eq(type(msgs), "table")
    assert_eq(#msgs, 0)
end)

test("consume returns empty when req is nil", function()
    local msgs = flash.consume(nil)
    assert_eq(type(msgs), "table")
    assert_eq(#msgs, 0)
end)

return { pass = pass, fail = fail }
