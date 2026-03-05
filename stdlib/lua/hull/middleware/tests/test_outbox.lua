-- test_outbox.lua — Tests for hull.middleware.outbox
--
-- Requires db, time, json, http globals (run via hull test harness).
-- Note: outbound HTTP calls will fail in test mode (no actual server).

local outbox = require('hull.middleware.outbox')

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

test("init creates outbox table", function()
    outbox.init({ max_attempts = 3 })
end)

-- ── enqueue ─────────────────────────────────────────────────────────

test("enqueue inserts a pending item", function()
    local id = outbox.enqueue({
        kind = "webhook",
        destination = "http://127.0.0.1:9999/hook",
        payload = '{"event":"test"}',
    })
    assert(id > 0, "expected positive ID")

    local rows = db.query("SELECT state, kind, destination FROM _hull_outbox WHERE id = ?", { id })
    assert_eq(#rows, 1)
    assert_eq(rows[1].state, "pending")
    assert_eq(rows[1].kind, "webhook")
end)

test("enqueue with idempotency key", function()
    local id = outbox.enqueue({
        kind = "webhook",
        destination = "http://127.0.0.1:9999/hook",
        payload = '{"event":"test2"}',
        idempotency_key = "evt-1-wh-1",
    })
    local rows = db.query("SELECT idempotency_key FROM _hull_outbox WHERE id = ?", { id })
    assert_eq(rows[1].idempotency_key, "evt-1-wh-1")
end)

test("enqueue with delay", function()
    local id = outbox.enqueue({
        kind = "http",
        destination = "http://127.0.0.1:9999/delayed",
        payload = "{}",
        delay = 300,
    })
    local rows = db.query("SELECT next_attempt_at, created_at FROM _hull_outbox WHERE id = ?", { id })
    assert(rows[1].next_attempt_at > rows[1].created_at, "delay should push next_attempt_at")
end)

test("enqueue requires kind", function()
    local ok, _err = pcall(function()
        outbox.enqueue({ destination = "http://x", payload = "{}" })
    end)
    assert_eq(ok, false)
end)

test("enqueue requires destination", function()
    local ok, _err = pcall(function()
        outbox.enqueue({ kind = "webhook", payload = "{}" })
    end)
    assert_eq(ok, false)
end)

test("enqueue requires payload", function()
    local ok, _err = pcall(function()
        outbox.enqueue({ kind = "webhook", destination = "http://x" })
    end)
    assert_eq(ok, false)
end)

-- ── flush (delivery fails in test mode) ──────────────────────────────

test("flush processes pending items", function()
    -- Clear outbox
    db.exec("DELETE FROM _hull_outbox")

    outbox.enqueue({
        kind = "webhook",
        destination = "http://127.0.0.1:9999/test-flush",
        payload = '{"event":"flush"}',
    })

    -- Flush will fail delivery (no server) and retry
    local result = outbox.flush()
    assert(type(result.delivered) == "number", "has delivered count")
    assert(type(result.failed) == "number", "has failed count")
    assert(type(result.retried) == "number", "has retried count")
end)

-- ── stats ───────────────────────────────────────────────────────────

test("stats returns counts by state", function()
    local s = outbox.stats()
    assert(type(s.pending) == "number", "has pending")
    assert(type(s.delivered) == "number", "has delivered")
    assert(type(s.failed) == "number", "has failed")
end)

-- ── middleware ───────────────────────────────────────────────────────

test("middleware sets _outbox_flush flag", function()
    local mw = outbox.middleware()
    local req = { ctx = {} }
    local res = {}
    local result = mw(req, res)
    assert_eq(result, 0)
    assert_eq(req.ctx._outbox_flush, true)
end)

-- ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", function()
    local count = outbox.cleanup(0)  -- 0 max_age = delete all delivered
    assert(type(count) == "number", "expected number")
end)

return {pass = pass, fail = fail}
