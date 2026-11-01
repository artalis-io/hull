-- test_stdlib_audit17.lua - stdlib regressions from audit 17 (Lua).
--
-- etag.text / etag.html with an empty body: etag.compute returns nil, and
-- res:header("ETag", nil) raised (a 500). An empty body now goes out with no
-- ETag and is never answered 304. jobs._config (the live config, settable
-- past jobs.init's checks) and the clock-injectable seams (jobs._tick,
-- _cron_next, _events_drain) are test hooks, no longer on the module.
-- Audit 18 adds outbox.flush: refused inside a transaction, and the runtime's
-- refusal to wait raised rather than counted as a failed attempt.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): etag hashes, jobs needs the db.

local etag = require("hull.web.middleware.etag")
local jobs = require("hull.jobs")

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
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2)
    end
end

local function mock_res(helper)
    local res = { code = 200, sent = false, headers = {} }
    function res:status(code) self.code = code; return self end
    function res:header(name, value)
        if value == nil then error("header value is nil") end
        self.headers[name] = value
        return self
    end
    res[helper] = function(self) self.sent = true; return self end
    return res
end

-- ── etag: an empty body ─────────────────────────────────────────────

for _, helper in ipairs({ "text", "html" }) do
    for _, body in ipairs({ "", false }) do
        local label = helper .. (body == "" and " empty" or " nil") .. " body"
        test(label .. " is sent with no ETag", function()
            local res = mock_res(helper)
            etag[helper]({ method = "GET", headers = {} }, res, body or nil)
            assert_eq(res.sent, true, "sent")
            assert_eq(res.code, 200, "status")
            assert_eq(res.headers.ETag, nil, "no ETag")
        end)
        test(label .. " is never a 304, even for If-None-Match: *", function()
            local res = mock_res(helper)
            etag[helper]({ method = "GET", headers = { ["if-none-match"] = "*" } },
                         res, body or nil)
            assert_eq(res.sent, true, "sent")
            assert_eq(res.code, 200, "status")
        end)
    end
end

test("a non-empty body still gets its ETag", function()
    local res = mock_res("text")
    etag.text({ method = "GET", headers = {} }, res, "hello")
    assert_eq(res.sent, true, "sent")
    assert_eq(type(res.headers.ETag), "string", "ETag")
end)

-- ── jobs: no exported config ────────────────────────────────────────

test("jobs._config is not exported", function()
    assert_eq(jobs._config, nil)
end)

test("the config is a test hook", function()
    local hooks = __test_hooks("hull.jobs")
    assert_eq(type(hooks and hooks.config), "table", "hook")
    assert_eq(type(hooks.config.backoff), "function", "live config")
end)

-- The clock-injectable seams wrote _hull_* rows at a time app code chose
-- (jobs._tick(1e12): every next_run_at decades out; a drain at now = 1e15:
-- the lease held for ever). They are test hooks too.
for _, k in ipairs({ "_tick", "_cron_next", "_events_drain" }) do
    test("jobs." .. k .. " is not exported", function()
        assert_eq(jobs[k], nil)
    end)
end

test("the cron tick, cron math and drain are test hooks", function()
    local hooks = __test_hooks("hull.jobs")
    assert_eq(type(hooks.tick), "function", "tick")
    assert_eq(type(hooks.cron_next), "function", "cron_next")
    assert_eq(type(hooks.events_drain), "function", "events_drain")
end)

-- ── outbox.flush and the runtime's refusal to wait (audit 18) ───────
-- A refusal said where flush was called, not how the destination answered,
-- yet it was recorded as a failed delivery: with max_attempts = 1 one flush
-- from such a place marked the row failed, unsent.

local outbox = require("hull.web.middleware.outbox")
-- The stdlib's own table, to stub what outbox / pwned call (an app's is a
-- read-only view since audit 43).
local http_client = __test_hooks("hull._test_hooks").real("hull.http-client")
local db = require("hull.db").default()

local function counts()
    local s = outbox.stats()
    return s.pending or 0, s.failed or 0
end

test("audit 18: outbox.flush refuses inside a transaction, before any claim", function()
    outbox.init()
    local p0, f0 = counts()
    outbox.enqueue({ kind = "webhook", destination = "http://a18.test/txn",
                     payload = "{}", max_attempts = 1 })
    local ok, err = pcall(db.batch, function() outbox.flush() end)
    assert_eq(ok, false, "refused")
    assert_eq(tostring(err):find("outbox.flush cannot run inside", 1, true) ~= nil,
              true, "message: " .. tostring(err))
    local p, f = counts()
    assert_eq(p, p0 + 1, "still pending")
    assert_eq(f, f0, "not failed")
end)

test("audit 18: a wait refusal is raised, not counted as an attempt", function()
    outbox.init()
    local p0, f0 = counts()
    outbox.enqueue({ kind = "webhook", destination = "http://a18.test/refused",
                     payload = "{}", max_attempts = 1 })
    local real = http_client.async.post
    http_client.async.post = function()
        error("[hull:wait_refused] http.fetch: can only wait in a handler", 0)
    end
    local ok1, err1 = pcall(outbox.flush)
    -- The claim was given back: the next flush takes the row again at once.
    local ok2 = pcall(outbox.flush)
    http_client.async.post = real
    assert_eq(ok1, false, "raised")
    assert_eq(tostring(err1):find("can only wait in a handler", 1, true) ~= nil,
              true, "message: " .. tostring(err1))
    assert_eq(ok2, false, "due again and raised again")
    local p, f = counts()
    assert_eq(p, p0 + 1, "still pending")
    assert_eq(f, f0, "not failed")
end)

-- ── refusal codes (audit 35) ────────────────────────────────────────
-- The stdlib recognised refusals by message text, so audit 31's fetch cap
-- ("too many requests in flight") read as a remote failure: outbox counted
-- an attempt for every claimed row, pwned failed open. Refusals now start
-- with "[hull:<code>] " and the stdlib tests the code.

-- max_attempts = 1: a counted attempt would mark the row failed.
local function outbox_rows(dest)
    outbox.enqueue({ kind = "webhook", destination = dest .. "/1",
                     payload = "{}", max_attempts = 1 })
    outbox.enqueue({ kind = "webhook", destination = dest .. "/2",
                     payload = "{}", max_attempts = 1 })
end

test("audit 35: a busy refusal stops the flush, nothing counted", function()
    outbox.init()
    local p0, f0 = counts()
    outbox_rows("http://a35.test/busy")
    local calls = 0
    local real = http_client.async.post
    http_client.async.post = function()
        calls = calls + 1
        error("[hull:busy] http.fetch: too many requests in flight (at most 64 per VM)", 0)
    end
    local ok1, r1 = pcall(outbox.flush)
    local ok2, r2 = pcall(outbox.flush)
    http_client.async.post = real
    assert_eq(ok1, true, "not raised: " .. tostring(r1))
    assert_eq(ok2, true, "not raised: " .. tostring(r2))
    assert_eq(r1.busy, true, "reported busy")
    assert_eq(r1.failed + r1.retried + r1.delivered, 0, "no outcome")
    -- The flush stopped at the first refusal, and the claim was given back:
    -- the second flush took the same row again.
    assert_eq(calls, 2, "one delivery tried per flush")
    local p, f = counts()
    assert_eq(p, p0 + 2, "both still pending")
    assert_eq(f, f0, "none failed: no attempt counted")
end)

test("audit 35: refusal text without the code is a delivery failure", function()
    outbox.init()
    outbox_rows("http://a35.test/text")
    local p0, f0 = counts()
    local real = http_client.async.post
    -- A remote failure whose message merely contains refusal text (or the
    -- code anywhere but the start) is not a refusal.
    http_client.async.post = function()
        error("upstream said: [hull:busy] too many requests in flight", 0)
    end
    local ok, r = pcall(outbox.flush)
    http_client.async.post = real
    assert_eq(ok, true, "not raised: " .. tostring(r))
    assert_eq(r.busy, nil, "not busy")
    -- Every due row (these two and the ones earlier tests left pending)
    -- was tried and counted.
    assert_eq(r.failed, p0, "all counted")
    local p, f = counts()
    assert_eq(p, 0, "none pending")
    assert_eq(f, f0 + p0, "all failed")
end)

local pwned = require("hull.web.pwned")
local PW = "a35-zq8Xk3v-not-in-any-list-7"

test("audit 35: pwned raises on a busy refusal instead of failing open", function()
    local real = http_client.async.get
    http_client.async.get = function()
        error("[hull:busy] http.fetch: too many requests in flight (at most 64 per VM)", 0)
    end
    local ok, e = pcall(pwned.check, PW)
    http_client.async.get = real
    assert_eq(ok, false, "raised")
    assert_eq(tostring(e):find("^%[hull:busy%] ") ~= nil, true, "code kept: " .. tostring(e))
end)

test("audit 35: pwned raises on a wait refusal", function()
    local real = http_client.async.get
    http_client.async.get = function()
        error("[hull:wait_refused] http.fetch cannot wait while a transaction is open", 0)
    end
    local ok, e = pcall(pwned.check, PW)
    http_client.async.get = real
    assert_eq(ok, false, "raised")
    assert_eq(tostring(e):find("^%[hull:wait_refused%] ") ~= nil, true, "code kept: " .. tostring(e))
end)

return { pass = pass, fail = fail }
