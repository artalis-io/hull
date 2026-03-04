-- test_idempotency.lua — Tests for hull.middleware.idempotency
--
-- Requires db, crypto, time, json globals (run via hull test harness).

local idempotency = require('hull.middleware.idempotency')

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

-- ── Helpers ─────────────────────────────────────────────────────────────

-- Mock response object that captures status and body
local function mock_res()
    local r = {
        _status = 200,
        _body = nil,
        _headers = {},
    }
    function r:status(code) self._status = code; return self end
    function r:json(data) self._body = json.encode(data); return self end
    function r:text(data) self._body = data; return self end
    function r:header(name, value) self._headers[name] = value; return self end
    return r
end

local function mock_req(method, path, body, headers)
    return {
        method = method or "POST",
        path = path or "/api/test",
        body = body or '{"data":"test"}',
        headers = headers or {},
        ctx = {},
    }
end

-- ── init ─────────────────────────────────────────────────────────────

test("init creates idempotency table", function()
    idempotency.init({ ttl = 60 })
end)

-- ── middleware: no key ───────────────────────────────────────────────

test("no idempotency key proceeds normally", function()
    local mw = idempotency.middleware()
    local req = mock_req("POST", "/api/test", '{"x":1}')
    local res = mock_res()
    local result = mw(req, res)
    assert_eq(result, 0, "should continue")
end)

-- ── middleware: GET skipped ──────────────────────────────────────────

test("GET requests are not intercepted", function()
    local mw = idempotency.middleware()
    local req = mock_req("GET", "/api/test", nil, { ["idempotency-key"] = "key-get" })
    local res = mock_res()
    local result = mw(req, res)
    assert_eq(result, 0)
end)

-- ── middleware: first request with key ──────────────────────────────

test("first request with key inserts inflight record", function()
    local mw = idempotency.middleware()
    local req = mock_req("POST", "/api/items", '{"name":"A"}', { ["idempotency-key"] = "key-first-1" })
    local res = mock_res()
    local result = mw(req, res)
    assert_eq(result, 0, "should continue to handler")
    assert_eq(req.ctx._idem_key, "key-first-1")
    assert_eq(req.ctx._idem_principal, "__anon")

    -- Verify inflight record exists
    local rows = db.query(
        "SELECT state FROM _hull_idempotency_keys WHERE key = ?",
        { "key-first-1" }
    )
    assert_eq(#rows, 1)
    assert_eq(rows[1].state, "inflight")
end)

-- ── respond + cache hit ──────────────────────────────────────────────

test("respond caches response and retry returns cached", function()
    local mw = idempotency.middleware()

    -- First request
    local req1 = mock_req("POST", "/api/items", '{"name":"B"}', { ["idempotency-key"] = "key-cache-1" })
    local res1 = mock_res()
    mw(req1, res1)
    idempotency.respond(req1, res1, 201, { id = 42 })

    -- Retry with same key and body
    local req2 = mock_req("POST", "/api/items", '{"name":"B"}', { ["idempotency-key"] = "key-cache-1" })
    local res2 = mock_res()
    local result = mw(req2, res2)
    assert_eq(result, 1, "should short-circuit")
    assert_eq(res2._status, 201, "cached status")
    assert_eq(res2._headers["X-Idempotency-Replay"], "true")
end)

-- ── different body = 409 ─────────────────────────────────────────────

test("same key with different body returns 409", function()
    local mw = idempotency.middleware()

    -- First request
    local req1 = mock_req("POST", "/api/items", '{"name":"C"}', { ["idempotency-key"] = "key-conflict-1" })
    local res1 = mock_res()
    mw(req1, res1)
    idempotency.respond(req1, res1, 201, { id = 99 })

    -- Retry with same key but different body
    local req2 = mock_req("POST", "/api/items", '{"name":"D"}', { ["idempotency-key"] = "key-conflict-1" })
    local res2 = mock_res()
    local result = mw(req2, res2)
    assert_eq(result, 1, "should short-circuit")
    assert_eq(res2._status, 409, "conflict")
end)

-- ── inflight = 409 ───────────────────────────────────────────────────

test("concurrent request with same key returns 409", function()
    local mw = idempotency.middleware()

    -- First request (in-flight, not yet completed)
    local req1 = mock_req("POST", "/api/items", '{"name":"E"}', { ["idempotency-key"] = "key-inflight-1" })
    local res1 = mock_res()
    mw(req1, res1)
    -- Don't call respond — still in-flight

    -- Second request with same key
    local req2 = mock_req("POST", "/api/items", '{"name":"E"}', { ["idempotency-key"] = "key-inflight-1" })
    local res2 = mock_res()
    local result = mw(req2, res2)
    assert_eq(result, 1, "should short-circuit")
    assert_eq(res2._status, 409)
end)

-- ── complete without respond ─────────────────────────────────────────

test("complete marks key done without caching response", function()
    local mw = idempotency.middleware()

    local req = mock_req("POST", "/api/items", '{"name":"F"}', { ["idempotency-key"] = "key-complete-1" })
    local res = mock_res()
    mw(req, res)
    idempotency.complete(req)

    -- Verify state is complete but no status cached
    local rows = db.query(
        "SELECT state, status FROM _hull_idempotency_keys WHERE key = ?",
        { "key-complete-1" }
    )
    assert_eq(#rows, 1)
    assert_eq(rows[1].state, "complete")
end)

-- ── principal scoping ────────────────────────────────────────────────

test("different principals can use same key", function()
    local mw = idempotency.middleware({
        get_principal = function(req) return req.ctx.user_id or "__anon" end,
    })

    -- User A
    local req1 = mock_req("POST", "/api/items", '{"name":"G"}', { ["idempotency-key"] = "key-scope-1" })
    req1.ctx.user_id = "user-A"
    local res1 = mock_res()
    mw(req1, res1)
    idempotency.respond(req1, res1, 201, { id = 1 })

    -- User B with same key
    local req2 = mock_req("POST", "/api/items", '{"name":"G"}', { ["idempotency-key"] = "key-scope-1" })
    req2.ctx.user_id = "user-B"
    local res2 = mock_res()
    local result = mw(req2, res2)
    assert_eq(result, 0, "different principal, should continue")
end)

-- ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", function()
    local count = idempotency.cleanup()
    assert(type(count) == "number", "expected number")
end)

return {pass = pass, fail = fail}
