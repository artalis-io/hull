-- test_flash.lua - Tests for hull.web.flash
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

-- ── session-backed round trip ───────────────────────────────────────

local session = require('hull.web.middleware.session')
session.init({ ttl = 60 })

local function ctx_for_new_session()
    local sid = session.create({})
    return { ctx = { session_id = sid } }
end

test("set + consume round-trip persists and clears in one cycle", function()
    local req = ctx_for_new_session()
    flash.set(req, "First message.", "info")
    flash.set(req, "Second message.", "success")
    local msgs = flash.consume(req)
    assert_eq(#msgs, 2)
    assert_eq(msgs[1].text, "First message.")
    assert_eq(msgs[1].kind, "info")
    assert_eq(msgs[2].text, "Second message.")
    assert_eq(msgs[2].kind, "success")
    -- Second consume on the same req → empty (one-shot semantics).
    local empty = flash.consume(req)
    assert_eq(#empty, 0)
end)

test("consume across separate requests in the same session", function()
    local sid = session.create({})
    -- Request A: stash.
    flash.set({ ctx = { session_id = sid } }, "across requests")
    -- Request B (separate req table, same session id): consume.
    local msgs = flash.consume({ ctx = { session_id = sid } })
    assert_eq(#msgs, 1)
    assert_eq(msgs[1].text, "across requests")
end)

test("set bounded at FLASH_MAX_ENTRIES (16); excess silently dropped", function()
    local req = ctx_for_new_session()
    for i = 1, 25 do
        flash.set(req, "msg-" .. i, "info")
    end
    local msgs = flash.consume(req)
    assert_eq(#msgs, 16, "queue capped at 16")
    assert_eq(msgs[1].text, "msg-1", "first-in preserved")
    assert_eq(msgs[16].text, "msg-16", "later messages dropped")
end)

test("set truncates text exceeding 4 KiB", function()
    local req = ctx_for_new_session()
    flash.set(req, string.rep("a", 5000), "info")
    local msgs = flash.consume(req)
    assert_eq(#msgs, 1)
    assert_eq(#msgs[1].text, 4096)
end)

return { pass = pass, fail = fail }
