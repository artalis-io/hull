-- test_idempotency.lua - Tests for hull.web.middleware.idempotency
--
-- Runs in the C harness's caps-bearing runtime (lua_stdlib.middleware_*).
local json = require('hull.json')

local idempotency = require('hull.web.middleware.idempotency')
local H = __test_hooks('hull.web.middleware.idempotency')

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
    function r:html(data) self._body = data; return self end
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
    local rows = H.rows("key-first-1")
    assert_eq(#rows, 1)
    assert_eq(rows[1].state, "inflight")
end)

-- Audit 23: req.headers keys are lowercase, and a mixed-case header_name
-- never matched them.
test("a mixed-case header_name matches the lowercased header", function()
    local mw = idempotency.middleware({ header_name = "X-Idem-Key" })
    local req = mock_req("POST", "/api/items", '{"name":"M"}', { ["x-idem-key"] = "key-mixed-1" })
    local res = mock_res()
    assert_eq(mw(req, res), 0, "should continue to handler")
    assert_eq(req.ctx._idem_key, "key-mixed-1", "the key was read")
    assert_eq(#H.rows("key-mixed-1"), 1, "inflight record")
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
    -- Don't call respond - still in-flight

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
    local rows = H.rows("key-complete-1")
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

-- ── audit 39: stored byte-exact, before sending; a failed store ─────

test("audit 39: a body that is not UTF-8 / holds a NUL replays byte-exact", function()
    local mw = idempotency.middleware()
    local body = "<p>caf\xe9 \0 \xff\xfe</p>"
    local req1 = mock_req("POST", "/a39", "x", { ["idempotency-key"] = "a39-bytes" })
    local res1 = mock_res()
    assert_eq(mw(req1, res1), 0)
    idempotency.respond_html(req1, res1, 201, body)
    assert_eq(res1._body, body, "first response")
    local row = H.rows("a39-bytes")[1]
    assert_eq(row.state, "complete")
    assert(row.response_headers:sub(1, 3) == "v2:", "stored in the v2 format")
    assert(utf8.len(row.response_body) and not row.response_body:find("\0", 1, true),
           "stored body is plain text")
    local req2 = mock_req("POST", "/a39", "x", { ["idempotency-key"] = "a39-bytes" })
    local res2 = mock_res()
    assert_eq(mw(req2, res2), 1)
    assert_eq(res2._status, 201)
    assert_eq(res2._body, body, "replayed bytes")
    assert_eq(res2._headers["content-type"], "text/html; charset=utf-8")
    assert_eq(res2._headers["X-Idempotency-Replay"], "true")
end)

test("audit 39: a failed store marks the key processed, never re-runnable", function()
    local mw = idempotency.middleware()
    local runs = 0
    local function handle(req, res)
        runs = runs + 1
        idempotency.respond(req, res, 201, { n = runs })
    end
    local req1 = mock_req("POST", "/a39f", "x", { ["idempotency-key"] = "a39-fail" })
    local res1 = mock_res()
    assert_eq(mw(req1, res1), 0)
    H.fail_store(true)
    local ok, err = pcall(handle, req1, res1)
    H.fail_store(false)
    assert(ok, "the handler must not raise: " .. tostring(err))
    assert_eq(res1._status, 201, "the response is still sent")
    assert_eq(H.rows("a39-fail")[1].state, "unstored")
    local req2 = mock_req("POST", "/a39f", "x", { ["idempotency-key"] = "a39-fail" })
    local res2 = mock_res()
    assert_eq(mw(req2, res2), 1, "a retry never reaches the handler")
    assert_eq(res2._status, 409)
    assert_eq(runs, 1)
end)

test("audit 40: headers past the store cap mark the key unstored", function()
    local mw = idempotency.middleware()
    local req1 = mock_req("POST", "/a40h", "x", { ["idempotency-key"] = "a40-hdr" })
    local res1 = mock_res()
    assert_eq(mw(req1, res1), 0)
    idempotency.respond(req1, res1, 201, { ok = true },
                        { ["Content-Language"] = string.rep("a", 300 * 1024) })
    assert_eq(res1._status, 201, "the response is still sent")
    assert_eq(H.rows("a40-hdr")[1].state, "unstored")
    local res2 = mock_res()
    assert_eq(mw(mock_req("POST", "/a40h", "x", { ["idempotency-key"] = "a40-hdr" }), res2), 1)
    assert_eq(res2._status, 409)
end)

test("audit 39: a row in the old format still replays", function()
    local mw = idempotency.middleware()
    local req = mock_req("POST", "/legacy", "x", { ["idempotency-key"] = "a39-legacy" })
    H.put_legacy("a39-legacy", "__anon", H.fingerprint(req), 200, '{"old":true}',
                 '{"content-type":"application/json"}')
    local res = mock_res()
    assert_eq(mw(req, res), 1)
    assert_eq(res._status, 200)
    assert_eq(res._body, '{"old":true}')
end)

test("audit 39: a key that is not printable ASCII is a 400", function()
    local mw = idempotency.middleware()
    for _, k in ipairs({ "caf\xc3\xa9", "a\0b", "tab\tkey", "\xff" }) do
        local res = mock_res()
        assert_eq(mw(mock_req("POST", "/k", "x", { ["idempotency-key"] = k }), res), 1)
        assert_eq(res._status, 400, "key " .. (k:gsub("%c", "?")))
    end
end)

test("audit 39: a principal the column cannot store is hashed", function()
    local p = H.norm_principal("u\xff\0")
    assert(p:sub(1, 7) == "sha256:", p)
end)

-- ── cleanup ─────────────────────────────────────────────────────────

test("cleanup returns count", function()
    local count = idempotency.cleanup()
    assert(type(count) == "number", "expected number")
end)

return {pass = pass, fail = fail}
