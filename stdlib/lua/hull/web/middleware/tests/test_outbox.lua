-- test_outbox.lua - Tests for hull.web.middleware.outbox
--
-- Requires db, time, json, http globals (run via hull test harness).
-- Note: outbound HTTP calls will fail in test mode (no actual server).

local outbox = require('hull.web.middleware.outbox')
local H = __test_hooks('hull.web.middleware.outbox')

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

    local rows = H.rows(id)
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
    local rows = H.rows(id)
    assert_eq(rows[1].idempotency_key, "evt-1-wh-1")
end)

test("enqueue with delay", function()
    local id = outbox.enqueue({
        kind = "http",
        destination = "http://127.0.0.1:9999/delayed",
        payload = "{}",
        delay = 300,
    })
    local rows = H.rows(id)
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
    H.clear()

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

-- Audit 23: an app header "idempotency-key" was sent beside the row's
-- "Idempotency-Key" - two headers.
test("flush adds no second Idempotency-Key in another case", function()
    H.clear()
    outbox.enqueue({
        kind = "webhook",
        destination = "http://127.0.0.1:9999/test-flush",
        payload = '{"event":"case"}',
        headers = require('hull.json').encode({ ["idempotency-key"] = "app-key" }),
        idempotency_key = "row-key",
    })
    local seen
    H.set_post(function(_url, _body, opts)
        seen = opts.headers
        return { status = 200 }
    end)
    local ok, err = pcall(outbox.flush)
    H.set_post(nil)
    assert(ok, tostring(err))
    assert(seen, "posted")
    local n = 0
    for k in pairs(seen) do if k:lower() == "idempotency-key" then n = n + 1 end end
    assert_eq(n, 1, "one Idempotency-Key")
    assert_eq(seen["idempotency-key"], "app-key", "the app's own")
end)

-- Audit 39: the delivery error is remote / runtime text; stored raw, a NUL or
-- bytes that are not UTF-8 failed the outcome UPDATE on Postgres (and over
-- 64 KiB on MySQL), and the row was re-leased for ever, uncounted.
test("audit 39: a hostile delivery error is stored as safe text", function()
    H.clear()
    local id = outbox.enqueue({
        kind = "webhook",
        destination = "http://127.0.0.1:9999/a39",
        payload = '{"event":"a39"}',
    })
    H.set_post(function()
        error("upstream: a\0b caf\xc3 " .. string.rep("y", 70000), 0)
    end)
    local ok, err = pcall(outbox.flush)
    H.set_post(nil)
    assert(ok, tostring(err))
    local row = H.rows(id)[1]
    assert_eq(row.attempts, 1, "attempt counted")
    local le = row.last_error
    assert(type(le) == "string" and not le:find("\0", 1, true) and utf8.len(le)
           and #le <= 4099, "storable last_error")
    assert(le:find("caf\239\191\189", 1, true), "U+FFFD")
end)

test("audit 39: an idempotency_key the column cannot store is refused", function()
    for _, k in ipairs({ "a\0b", "caf\xc3", string.rep("k", 256) }) do
        local ok, err = pcall(outbox.enqueue, {
            kind = "webhook", destination = "http://127.0.0.1:9999/k",
            payload = "{}", idempotency_key = k,
        })
        assert_eq(ok, false, "refused")
        assert(tostring(err):find("idempotency_key", 1, true), tostring(err))
    end
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
