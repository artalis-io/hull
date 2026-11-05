-- test_stdlib_audit44.lua - stdlib regressions from audit 44 (Lua).
--
-- A batch of Lows from the Lua / JS stdlib audits: auth-flows checks a reset
-- token before the pwned-password lookup (an outbound fetch); session.update
-- takes only a session id; rbac.define_role takes one permission bare and
-- refuses any other non-table; validate refuses an unknown type; ratelimit
-- checks limit / window at factory time and sends Retry-After; jobs checks
-- poll_ms / concurrency / max_empty_polls / await's interval, queue / type /
-- trace, history / history_retention, keeps a payload of false, stores a
-- payload as results are stored, and parks a cron row whose enqueue cannot
-- succeed; search's snippet / highlight column and tokens are integers;
-- retry.run caps max_attempts and refuses a negative base_ms / cap_ms; and
-- hull._text.storable's ellipsis fits inside max. The JS twin is
-- test_stdlib_audit44.js.
--
-- Each case runs in a fresh caps-bearing state and a fresh :memory: database
-- (run_audit44_case in tests/hull/runtime/lua/test_lua.c), named in the
-- global __audit44_case.

local json = require("hull.json")

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

local function raises(fn, needle, msg)
    local ok, err = pcall(fn)
    if ok then error((msg or "") .. ": expected an error", 2) end
    if needle and not tostring(err):find(needle, 1, true) then
        error((msg or "") .. ": expected an error naming '" .. needle .. "', got "
              .. tostring(err), 2)
    end
end

local function mkres()
    local r = { code = 200, headers = {} }
    function r.status(self, c) self.code = c; return self end
    function r.json(self, v) self.body = v; return self end
    function r.html(self, v) self.body = v; return self end
    function r.text(self, v) self.body = v; return self end
    function r.header(self, k, v) self.headers[k] = v; return self end
    function r.redirect(self, p, c) self.code = c or 302; self.location = p; return self end
    return r
end

local cases = {}

-- Item 1: the reset token is checked (not consumed) before the pwned lookup.
function cases.pwned_reset_order()
    local crypto = require("hull.crypto")
    local af = require("hull.web.auth-flows")
    local AF = __test_hooks("hull.web.auth-flows")
    local USER = { id = "a44", email = "a44@a44.test", email_verified = true,
                   password_hash = crypto.hash_password("old-password-44") }
    AF.reset()
    af.init({
        secret = ("s"):rep(32),
        trust_request_host = true,
        email_rate_limit = false,
        check_pwned_passwords = true,
        email_send = function() end,
        templates = {},
        user_find_by_email = function(e) return e == USER.email and USER or nil end,
        user_get = function(id) return id == USER.id and USER or nil end,
        user_create = function() return 1 end,
        user_set_password = function() end,
        user_set_email = function() end,
        user_set_email_verified = function() end,
        on_login = function(_req, res) res:json({ ok = true }) end,
        on_password_reset = function() end,
    })
    local function req(body)
        return { method = "POST", path = "/auth/password-reset/confirm",
                 headers = { host = "app.test", ["content-type"] = "application/json",
                             ["sec-fetch-site"] = "same-origin" },
                 body = json.encode(body), ctx = {}, remote_addr = "192.0.2.44" }
    end
    -- "password" is on the local breach list: a lookup answers "pwned"
    -- without the network, so the order shows in the answer.
    test("a garbage token is refused before the pwned lookup", function()
        local res = mkres()
        AF.handlers.password_reset_confirm(req({ token = "garbage", password = "password" }), res)
        assert_eq(res.code, 400, "400")
        assert_eq(tostring(res.body and res.body.error):sub(1, 13), "reset failed:",
                  "token error, not the pwned one")
    end)
    test("a used token is refused before the pwned lookup", function()
        local tok = AF.issue_token(USER.id, AF.ACTIONS.password_reset, 600)
        assert_eq(AF.mark_token_used(tok, require("hull.time").now() + 600), true, "marked")
        local res = mkres()
        AF.handlers.password_reset_confirm(req({ token = tok, password = "password" }), res)
        assert_eq(res.body and res.body.error, "reset failed: replayed", "replayed")
    end)
    test("a valid token with a pwned password is refused and not consumed", function()
        local tok = AF.issue_token(USER.id, AF.ACTIONS.password_reset, 600)
        local res = mkres()
        AF.handlers.password_reset_confirm(req({ token = tok, password = "password" }), res)
        assert_eq(res.code, 400, "400")
        assert_eq(tostring(res.body.error):find("breaches", 1, true) ~= nil, true,
                  "the pwned answer")
        assert_eq(AF.token_already_used(tok), false, "token still usable")
    end)
end

-- Item 2: session.update takes only an id session.create makes.
function cases.session_update_sid()
    local session = require("hull.web.middleware.session")
    session.init({ cleanup = false })
    test("an id that is not a session id is refused before anything else", function()
        local ok, r = pcall(session.update, "not-a-session-id\0", {}, { ttl = -1 })
        assert_eq(ok, true, "no raise: " .. tostring(r))
        assert_eq(r, false, "false")
        local ok2, r2 = pcall(session.update, {}, {})
        assert_eq(ok2, true, "a table id: " .. tostring(r2))
        assert_eq(r2, false, "false")
        assert_eq(session.update(("A"):rep(64), {}), false, "uppercase hex")
    end)
    test("a real session still updates", function()
        local sid = session.create({ user_id = "u44" })
        assert_eq(session.update(sid, { user_id = "u44", n = 2 }), true, "updated")
        assert_eq(session.load(sid).n, 2, "data")
    end)
end

-- Item 4: rbac.define_role takes one permission bare.
function cases.rbac_string_permission()
    local rbac = require("hull.web.middleware.rbac")
    rbac.init()
    test("a bare permission name is granted", function()
        rbac.define_role("r44", "users.write")
        rbac.assign("u44", "r44")
        assert_eq(rbac.has_permission("u44", "users.write"), true, "granted")
        assert_eq(rbac.has_permission("u44", "u"), false, "not per character")
    end)
    test("any other non-table is refused", function()
        raises(function() rbac.define_role("r44b", 5) end, "permissions", "number")
        raises(function() rbac.define_role("r44c", true) end, "permissions", "boolean")
        rbac.define_role("r44d")   -- none is fine
        rbac.define_role("r44e", { "a.b" })
    end)
end

-- Item 5: an unknown validate type is a schema error.
function cases.validate_unknown_type()
    local validate = require("hull.validate")
    test("an unknown type raises", function()
        raises(function() validate.check({ x = 1 }, { x = { type = "array" } }) end,
               "unknown type", "array")
        raises(function() validate.check({}, { x = { type = "int" } }) end,
               "unknown type", "even with no value")
    end)
    test("the known types still check", function()
        local ok = validate.check({ a = "s", b = 1.5, c = 2, d = true },
            { a = { type = "string" }, b = { type = "number" },
              c = { type = "integer" }, d = { type = "boolean" } })
        assert_eq(ok, true, "ok")
    end)
end

-- Item 6: ratelimit limit / window checked up front; Retry-After on 429.
function cases.ratelimit_options()
    local ratelimit = require("hull.web.middleware.ratelimit")
    test("a bad limit or window raises at factory time", function()
        for _, bad in ipairs({ 0, -1, 0 / 0, math.huge, "60" }) do
            raises(function() ratelimit.middleware({ limit = bad }) end, "limit",
                   "limit " .. tostring(bad))
        end
        for _, bad in ipairs({ 0, -5, 0 / 0, math.huge, "60" }) do
            raises(function() ratelimit.middleware({ window = bad }) end, "window",
                   "window " .. tostring(bad))
        end
    end)
    test("a refused request carries Retry-After", function()
        local mw = ratelimit.middleware({ limit = 1, window = 60, key = "k44" })
        local req = { method = "GET", path = "/", headers = {}, remote_addr = "192.0.2.1" }
        assert_eq(mw(req, mkres()), 0, "first allowed")
        local res = mkres()
        assert_eq(mw(req, res), 1, "second refused")
        assert_eq(res.code, 429, "429")
        local ra = tonumber(res.headers["Retry-After"])
        assert_eq(ra ~= nil and ra >= 1 and ra <= 60, true,
                  "Retry-After " .. tostring(res.headers["Retry-After"]))
    end)
end

-- Item 7: run_worker's poll_ms / concurrency / max_empty_polls, await interval.
function cases.jobs_poll_options()
    local jobs = require("hull.jobs")
    jobs.init()
    test("a bad poll_ms raises before the loop", function()
        for _, bad in ipairs({ 0 / 0, -1, math.huge, "fast", {} }) do
            raises(function() jobs.run_worker({ poll_ms = bad, drain = true }) end,
                   "poll_ms", "poll_ms " .. tostring(bad))
        end
    end)
    test("a bad await interval or timeout raises", function()
        -- An unknown id: before the check, await answered nil at once
        -- instead of waiting on a job no worker runs.
        local id = 999999
        raises(function() jobs.await(id, { interval = 0 / 0 }) end, "interval", "NaN")
        raises(function() jobs.await(id, { timeout = 0 / 0 }) end, "timeout", "NaN timeout")
    end)
    test("a small poll_ms and odd counts still run", function()
        jobs.handler("a44p", function() return true end)
        jobs.enqueue("a44p", {}, { queue = "p44" })
        local n = jobs.run_worker({ queue = "p44", poll_ms = 0, max_empty_polls = "1",
                                    concurrency = 1.5 })
        assert_eq(n, 1, "processed")
    end)
end

-- Items 8 + 15: queue / type / trace checks; payload false; payload storage.
function cases.jobs_key_options()
    local jobs = require("hull.jobs")
    jobs.init()
    test("queue, type and trace are checked", function()
        raises(function() jobs.enqueue("t", {}, { queue = 5 }) end, "queue", "numeric queue")
        raises(function() jobs.enqueue("t", {}, { queue = {} }) end, "queue", "table queue")
        raises(function() jobs.enqueue("t", {}, { queue = ("q"):rep(256) }) end,
               "queue", "long queue")
        raises(function() jobs.enqueue(("t"):rep(256), {}) end, "type", "long type")
        raises(function() jobs.enqueue("t", {}, { trace = {} }) end, "trace", "table trace")
        raises(function() jobs.enqueue("t", {}, { trace = ("x"):rep(256) }) end,
               "trace", "long trace")
        raises(function() jobs.cron("c44", "* * * * *", nil, { queue = 7 }) end,
               "queue", "cron queue")
        raises(function() jobs.pause(9) end, "queue", "pause")
        raises(function() jobs.purge({}) end, "queue", "purge")
        raises(function() jobs.claim({ queues = { "ok", 3 } }) end, "queue", "claim list")
    end)
    test("a payload of false reaches the handler as false", function()
        local seen = "unset"
        jobs.handler("a44false", function(job) seen = job.data end)
        jobs.enqueue("a44false", false, { queue = "f44" })
        assert_eq(pcall(jobs.work, { batch = 1, queue = "f44" }), true, "work")
        assert_eq(seen, false, "false kept")
    end)
    test("a payload with bytes that are not UTF-8 is stored and reads back", function()
        local BIN = "a\0b caf\xc3 \255"
        local seen
        jobs.handler("a44bin", function(job) seen = job.data and job.data.b end)
        local id = jobs.enqueue("a44bin", { b = BIN }, { queue = "b44", trace = "00-abc" })
        assert_eq(__test_hooks("hull.jobs").job_payload(id):sub(1, 4), "b64:", "stored as b64")
        assert_eq(pcall(jobs.work, { batch = 1, queue = "b44" }), true, "work")
        assert_eq(seen, BIN, "byte-exact")
    end)
end

-- Item 9: history / history_retention are checked.
function cases.jobs_history_options()
    local jobs = require("hull.jobs")
    test("a bad history_retention raises", function()
        for _, bad in ipairs({ 0 / 0, -1, math.huge, "week", {} }) do
            raises(function() jobs.init({ history_retention = bad }) end,
                   "history_retention", tostring(bad))
        end
    end)
    test("a bad history raises", function()
        raises(function() jobs.init({ history = "yes" }) end, "history", "string")
        raises(function() jobs.init({ history = { queues = { 1 } } }) end, "history", "queues")
    end)
    test("good values are kept", function()
        jobs.init({ history = { queues = { "h44" } }, history_retention = 3600 })
        jobs.init({ history = true, history_retention = false })
    end)
end

-- Item 10: a cron row whose enqueue cannot succeed is parked.
function cases.jobs_cron_park()
    local jobs = require("hull.jobs")
    local hooks = __test_hooks("hull.jobs")
    jobs.init({ events = true })
    test("a stored row with an empty type is parked with its error", function()
        jobs.cron("park44", "* * * * *")
        hooks.cron_set_field("park44", "type", "")
        hooks.cron_set_next_run_at("park44", 1000)
        hooks.tick(2000)
        local r = hooks.cron_row("park44")
        assert_eq(r.next_run_at, hooks.CRON_DISABLED_AT, "parked")
        assert_eq(type(r.last_error) == "string"
                  and r.last_error:find("type", 1, true) ~= nil, true,
                  "last_error: " .. tostring(r.last_error))
        local evs = jobs.events({ types = { "cron_disabled" } })
        assert_eq(#evs >= 1, true, "cron_disabled emitted")
    end)
    test("a stored max_attempts of 0 is parked too", function()
        jobs.cron("park44b", "* * * * *")
        hooks.cron_set_field("park44b", "max_attempts", 0)
        hooks.cron_set_next_run_at("park44b", 1000)
        hooks.tick(2000)
        assert_eq(hooks.cron_row("park44b").next_run_at, hooks.CRON_DISABLED_AT, "parked")
    end)
    test("a good row still fires", function()
        jobs.cron("fire44", "* * * * *", nil, { queue = "c44" })
        hooks.cron_set_next_run_at("fire44", 1000)
        hooks.tick(2000)
        assert_eq(hooks.cron_row("fire44").next_run_at ~= hooks.CRON_DISABLED_AT, true,
                  "not parked")
        assert_eq(jobs.stats({ queue = "c44" }).pending, 1, "enqueued")
    end)
end

-- Item 12: snippet / highlight column and tokens are integers.
function cases.search_snippet_options()
    local search = require("hull.search")
    search.create_index("a44docs", { "title", "body" })
    search.index("a44docs", 1, { title = "hello", body = "a needle here" })
    test("a non-integer column or tokens raises", function()
        for _, bad in ipairs({ math.huge, 0 / 0, 1e300, 1.5, "1", -1 }) do
            raises(function()
                search.query("a44docs", "needle", { snippet = { column = bad } })
            end, "column", "snippet column " .. tostring(bad))
            raises(function()
                search.query("a44docs", "needle", { highlight = { column = bad } })
            end, "column", "highlight column " .. tostring(bad))
        end
        for _, bad in ipairs({ math.huge, 0 / 0, 0, 65, 2.5, "8" }) do
            raises(function()
                search.query("a44docs", "needle", { snippet = { column = 2, tokens = bad } })
            end, "tokens", "tokens " .. tostring(bad))
        end
    end)
    test("integers still work", function()
        local rows = search.query("a44docs", "needle", { snippet = { column = 2, tokens = 8 } })
        assert_eq(#rows, 1, "one row")
        assert_eq(rows[1].snippet:find("needle", 1, true) ~= nil, true, "snippet")
    end)
end

-- Item 13: retry.run caps max_attempts and refuses negative base / cap.
function cases.retry_options()
    local retry = require("hull.retry")
    test("max_attempts = math.huge is capped", function()
        local calls = 0
        local ok = pcall(retry.run, function() calls = calls + 1; error("x", 0) end,
            { max_attempts = math.huge, base_ms = 0, cap_ms = 0,
              retry_on_error = function() return calls < 1001 end })
        assert_eq(ok, false, "raised")
        assert_eq(calls, 1000, "at most 1000 attempts")
    end)
    test("a negative or infinite base_ms / cap_ms raises before fn runs", function()
        for _, k in ipairs({ "base_ms", "cap_ms" }) do
            for _, bad in ipairs({ -1, math.huge, 0 / 0, "5" }) do
                local calls = 0
                raises(function()
                    retry.run(function() calls = calls + 1 end, { [k] = bad })
                end, k, k .. " " .. tostring(bad))
                assert_eq(calls, 0, "fn not run")
            end
            raises(function() retry.backoff(1, { [k] = -1 }) end, k, "backoff " .. k)
        end
    end)
end

-- Item 14: hull._text.storable edge cases.
function cases.text_storable()
    local T = __test_hooks("hull._test_hooks").real("hull._text")
    local storable = T.storable
    test("the ellipsis fits inside max", function()
        local s = storable(("a"):rep(100), 10, true)
        assert_eq(#s, 10, "10 bytes")
        assert_eq(s, "aaaaaaa...", "cut at max - 3")
        assert_eq(storable(("a"):rep(10), 10, true), ("a"):rep(10), "no cut, no ellipsis")
        -- a cut through a code point keeps the result inside max
        local e = storable(("\u{20AC}"):rep(10), 10, true)
        assert_eq(#e <= 10 and utf8.len(e) ~= nil, true, "code point: " .. e)
        assert_eq(e, "\u{20AC}\u{20AC}...", "two euros and the ellipsis")
        assert_eq(storable(("a"):rep(10), 2, true), "aa", "a cap under 3 takes none")
    end)
    test("NULs are dropped", function()
        assert_eq(storable("a\0b\0c"), "abc", "NUL")
    end)
    test("overlong and surrogate encodings become U+FFFD", function()
        local s = storable("x\xc0\xafy\xed\xa0\x80z")
        assert_eq(utf8.len(s) ~= nil, true, "valid UTF-8")
        assert_eq(s:find("x\239\191\189", 1, true) == 1, true, "overlong: " .. s)
        assert_eq(s:find("\xed\xa0\x80", 1, true), nil, "surrogate gone")
        assert_eq(s:sub(-1), "z", "kept the tail")
    end)
    test("the cut is on a code point", function()
        local s = storable("ab\u{20AC}", 4)
        assert_eq(s, "ab", "no half character")
    end)
    test("a raising __tostring gives the fallback", function()
        local v = setmetatable({}, { __tostring = function() error("no") end })
        assert_eq(storable(v), T.UNCONVERTIBLE, "default fallback")
        assert_eq(storable(v, 100, false, "fb"), "fb", "caller fallback")
    end)
end

test("the case is known", function()
    assert_eq(type(cases[__audit44_case]), "function", tostring(__audit44_case))
end)
if cases[__audit44_case] then cases[__audit44_case]() end

return { pass = pass, fail = fail }
