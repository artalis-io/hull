-- test_stdlib_audit24.lua - stdlib regressions from audit 24 (Lua).
--
-- csv.encode writes a nil cell as an empty field instead of dropping it and
-- every cell after it; the jobs / audit-log / outbox LIMITs are clamped
-- integers; jobs.purge / cleanup copy their status list strictly;
-- session.list_for_user names sessions by ref (never by id) and
-- session.destroy_ref signs one out; outbox claims outlast http.fetch's
-- 10 min ceiling; jwt refuses a non-finite exp / nbf; totp.init
-- refuses malformed counts; validate's number type and bounds refuse NaN /
-- inf; a trusted_hosts entry no longer matches a non-default port; and the
-- email-change handler refuses an id that is not one.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): jobs, session, audit-log and outbox
-- need the db.

local json      = require("hull.json")
local crypto    = require("hull.crypto")
local encoding  = require("hull.encoding")
local csv       = require("hull.csv")
local jobs      = require("hull.jobs")
local jwt       = require("hull.jwt")
local validate  = require("hull.validate")
local session   = require("hull.web.middleware.session")
local audit_log = require("hull.web.middleware.audit-log")
local outbox    = require("hull.web.middleware.outbox")
local totp      = require("hull.web.middleware.totp")
local cookie    = require("hull.web.cookie")
local af        = require("hull.web.auth-flows")

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

-- ── csv ────────────────────────────────────────────────────────────

test("csv.encode: a nil cell is an empty field, not the end of the row", function()
    local out = csv.encode({ { a = 1, b = 2, c = 3 }, { a = 1, c = 3 }, { c = 3 } },
                           { headers = true })
    assert_eq(out, "a,b,c\n1,2,3\n1,,3\n,,3\n", "headers mode")
    assert_eq(csv.encode({ { 1, nil, 3 } }), "1,,3\n", "array row with a hole")
    assert_eq(csv.encode({ { n = 3, "x" } }), "x,,\n", "the row's n is its count")
    assert_eq(csv.encode({ { a = 1, b = 2 } }, { headers = true, columns = { "b", "a" } }),
              "b,a\n2,1\n", "columns order")
    assert_eq(pcall(csv.encode, { { a = 1 } }, { headers = true, columns = { "a", {} } }),
              false, "a non-name column is refused")
end)

-- ── jobs ───────────────────────────────────────────────────────────

test("jobs: limits / batches are clamped integers", function()
    jobs.init()
    for _ = 1, 3 do jobs.enqueue("a24claim", {}, { queue = "a24q" }) end
    assert_eq(#jobs.claim({ queue = "a24q", batch = -5 }), 1, "a negative batch is 1")
    assert_eq(#jobs.claim({ queue = "a24q", batch = 0 / 0 }), 2, "NaN is the default")
    assert_eq(#jobs.events({ limit = -1 }) <= 1, true, "a negative events limit is 1")
end)

test("jobs.purge / cleanup: statuses are strings, empty is nothing", function()
    jobs.init()
    jobs.enqueue("a24purge", {}, { queue = "a24p" })
    assert_eq(jobs.purge("a24p", { statuses = {} }), 0, "empty purges nothing")
    assert_eq(pcall(jobs.purge, "a24p", { statuses = { 1 } }), false, "a number")
    assert_eq(pcall(jobs.purge, "a24p", { statuses = { "pending", nil, "dead" } }),
              false, "a hole")
    assert_eq(pcall(jobs.purge, "a24p", { statuses = "pending" }), false, "not a table")
    assert_eq(jobs.cleanup({ statuses = {} }), 0, "empty cleans nothing, no SQL error")
    assert_eq(pcall(jobs.cleanup, { statuses = { "done", false } }), false, "a boolean")
    assert_eq(jobs.purge("a24p", { statuses = { "pending" } }), 1, "a real list")
end)

-- ── audit-log ──────────────────────────────────────────────────────

test("audit_log: list limit is clamped, window_days must be finite > 0", function()
    audit_log.init({ fingerprint_salt = "a24-salt-0123456789" })
    local req = { headers = { ["user-agent"] = "curl/8" }, remote_addr = "10.0.0.1" }
    for _ = 1, 3 do audit_log.record(2401, "login", req) end
    assert_eq(#audit_log.list(2401), 3)
    assert_eq(#audit_log.list(2401, { limit = -1 }), 1, "a negative limit is 1")
    assert_eq(#audit_log.list(2401, { limit = 0 / 0 }), 3, "NaN is the default")
    assert_eq(#audit_log.list(2401, { limit = 1.5 }), 1, "floored")
    for _, bad in ipairs({ 0 / 0, math.huge, -1, 0, "30", true }) do
        assert_eq(pcall(audit_log.list_devices, 2401, { window_days = bad }), false,
                  "list_devices " .. tostring(bad))
        assert_eq(pcall(audit_log.is_new_device, 2401, req, { window_days = bad }), false,
                  "is_new_device " .. tostring(bad))
    end
    assert_eq(#audit_log.list_devices(2401, { window_days = 1 }), 1)
    assert_eq(audit_log.is_new_device(2401, req, { window_days = 1 }), false)
end)

-- ── session device list ────────────────────────────────────────────

test("session.list_for_user: refs, not ids; destroy_ref is per user", function()
    session.init({ ttl = 3600, cleanup = false })
    local a = session.create({ user_id = 2402 })
    local b = session.create({ user_id = 2402 })
    local other = session.create({ user_id = 2403 })
    local rows = session.list_for_user(2402, a)
    assert_eq(#rows, 2)
    local current, ref_b = 0, nil
    for _, r in ipairs(rows) do
        assert_eq(r.id, nil, "no id")
        assert_eq(type(r.ref) == "string" and #r.ref == 32 and not r.ref:find("[^0-9a-f]"),
                  true, "a 32-hex ref")
        assert_eq(r.ref ~= a and r.ref ~= b, true, "the ref is not the id")
        if r.current then current = current + 1 else ref_b = r.ref end
    end
    assert_eq(current, 1, "one current session")
    -- The request form of `current`.
    local n = 0
    for _, r in ipairs(session.list_for_user(2402, { ctx = { session_id = b } })) do
        if r.current then n = n + 1; assert_eq(r.ref, ref_b, "b is current") end
    end
    assert_eq(n, 1)
    local other_ref = session.list_for_user(2403)[1].ref
    assert_eq(session.destroy_ref(2402, other_ref), 0, "another user's ref")
    assert_eq(session.load(other) ~= nil, true, "the other user's session lives")
    assert_eq(session.destroy_ref(2402, "zz"), 0, "a malformed ref")
    assert_eq(session.destroy_ref(2402, ref_b), 1, "its own ref")
    assert_eq(session.load(b), nil, "b is gone")
    assert_eq(session.load(a) ~= nil, true, "a lives")
end)

-- ── outbox ─────────────────────────────────────────────────────────

test("outbox.flush: limit clamped, the lease outlasts any delivery", function()
    outbox.init()
    local hooks = __test_hooks("hull.web.middleware.outbox")
    hooks.clear()
    for i = 1, 3 do
        outbox.enqueue({ kind = "webhook", destination = "http://a24.test/" .. i,
                         payload = "{}" })
    end
    local seen = {}
    hooks.set_post(function(_url, _body, opts)
        seen[#seen + 1] = opts.timeout_ms or false
        return { status = 200 }
    end)
    local ok, r = pcall(outbox.flush, { limit = -1 })
    local ok2, r2 = pcall(outbox.flush, { limit = 0 / 0 })
    hooks.set_post(nil)
    assert_eq(ok, true, tostring(r))
    assert_eq(r.delivered, 1, "a negative limit is 1")
    assert_eq(ok2, true, tostring(r2))
    assert_eq(r2.delivered, 2, "NaN is the default")
    assert_eq(#seen, 3)
    for i = 1, 3 do
        assert_eq(seen[i], false, "the app's own http timeout is kept")
    end
    assert_eq(hooks.claim_lease > 600, true, "the lease outlasts http.fetch's 10 min ceiling")
end)

-- ── jwt ────────────────────────────────────────────────────────────

local function raw_hs256(payload_json, secret)
    local function b64(s) return encoding.base64.encode(s, { url = true }) end
    local input = b64('{"alg":"HS256","typ":"JWT"}') .. "." .. b64(payload_json)
    return input .. "." .. b64(crypto.hmac_sha256(input, secret))
end

test("jwt.verify: a non-finite exp / nbf is invalid", function()
    local secret = ("k"):rep(32)
    local ok_tok = raw_hs256('{"sub":"a","exp":4000000000}', secret)
    assert_eq(jwt.verify(ok_tok, secret) ~= nil, true, "a finite exp verifies")
    for _, p in ipairs({ '{"sub":"a","exp":1e999}', '{"sub":"a","nbf":-1e999}' }) do
        local claims = jwt.verify(raw_hs256(p, secret), secret)
        assert_eq(claims, nil, p)
    end
end)

-- ── totp ───────────────────────────────────────────────────────────

test("totp.init: counts are whole numbers > 0, window 0..10", function()
    for _, kv in ipairs({
        { "max_failed_attempts", 0 / 0 }, { "max_failed_attempts", 0 },
        { "max_failed_attempts_per_ip", 1.5 }, { "lockout_duration", "900" },
        { "recovery_codes", -1 }, { "period", math.huge },
        { "window", 1.5 }, { "window", -1 }, { "window", 11 }, { "window", "1" },
    }) do
        local ok = pcall(totp.init, { [kv[1]] = kv[2] })
        assert_eq(ok, false, kv[1] .. " = " .. tostring(kv[2]))
    end
end)

-- ── validate ───────────────────────────────────────────────────────

test("validate: number refuses NaN / inf, bounds refuse NaN", function()
    for _, v in ipairs({ 0 / 0, math.huge, -math.huge }) do
        local ok = validate.check({ x = v }, { x = { type = "number" } })
        assert_eq(ok, false, "type number " .. tostring(v))
    end
    assert_eq(validate.check({ x = 0 / 0 }, { x = { min = 1 } }), false, "NaN vs min")
    assert_eq(validate.check({ x = 0 / 0 }, { x = { max = 1 } }), false, "NaN vs max")
    assert_eq(validate.check({ x = 5 }, { x = { min = 0 / 0 } }), false, "NaN min")
    assert_eq(validate.check({ x = 5 }, { x = { type = "number", min = 1, max = 9 } }), true)
end)

-- ── trusted hosts and ports ────────────────────────────────────────

test("session.logout_handler: a bare trusted host means its default port", function()
    session.init({ cleanup = false })
    local lo = session.logout_handler(cookie, { trusted_hosts = { "app.example.com",
                                                                  "admin.example.com:8443" } })
    local function run(origin)
        local res = mkres()
        lo({ method = "POST", headers = { host = "internal", origin = origin } }, res)
        return res.code
    end
    assert_eq(run("https://app.example.com"), 200)
    assert_eq(run("https://app.example.com:443"), 200, "https default port")
    assert_eq(run("http://app.example.com:80"), 200, "http default port")
    assert_eq(run("https://app.example.com:8443"), 403, "another port")
    assert_eq(run("http://app.example.com:443"), 403, "443 is not http's default")
    assert_eq(run("https://admin.example.com:8443"), 200, "host:port entry")
    assert_eq(run("https://admin.example.com"), 403, "host:port entry, default port")
end)

-- ── auth-flows ─────────────────────────────────────────────────────

local users = {}
local function af_init(extra)
    __test_hooks("hull.web.auth-flows").reset()
    local opts = {
        secret = ("s"):rep(32),
        public_origin = "https://app.example.com",
        trusted_hosts = { "app.example.com" },
        email_rate_limit = false,
        email_send = function() end,
        templates = {
            email_change        = function(c) return { subject = "e", text = c.link } end,
            email_change_notify = function(c) return { subject = "n", text = c.revoke_url } end,
        },
        user_find_by_email = function(e)
            for _, u in pairs(users) do if u.email == e then return u end end
        end,
        user_get = function(id) return users[tostring(id)] end,
        user_create = function() return nil end,
        user_set_password = function() end,
        user_set_email = function(id, e) users[tostring(id)].email = e end,
        user_set_email_verified = function() end,
        on_login = function(_req, res) res:json({ ok = true }) end,
        on_password_reset = function() end,
        email_change_reauth = function() return true end,
    }
    for k, v in pairs(extra or {}) do opts[k] = v end
    af.init(opts)
end

test("auth-flows: trusted_hosts matches a non-default port only exactly", function()
    af_init()
    local so = __test_hooks("hull.web.auth-flows").same_origin_request
    local function run(origin)
        return so({ headers = { host = "internal", origin = origin,
                                ["content-type"] = "application/json" } })
    end
    assert_eq(run("https://app.example.com:443"), true, "default port")
    assert_eq(run("https://app.example.com:8443"), false, "another port")
end)

test("auth-flows: email change refuses an id that is not one", function()
    users = { ["0"] = { id = 0, email = "zero@a24.test", email_verified = true } }
    af_init()
    local H = __test_hooks("hull.web.auth-flows").handlers
    local function run(uid)
        local res = mkres()
        H.email_change({ method = "POST", path = "/auth/email-change",
                         headers = { host = "app.example.com",
                                     ["content-type"] = "application/json",
                                     ["sec-fetch-site"] = "same-origin" },
                         body = json.encode({ new_email = "new0@a24.test" }),
                         ctx = { user_id = uid }, remote_addr = "192.0.2.9" }, res)
        return res.code, res.body
    end
    assert_eq((run("")), 401, "an empty id")
    assert_eq((run({})), 401, "a table id")
    assert_eq((run(1.5)), 401, "a fraction")
    local code, body = run(0)
    assert_eq(code, 200, "user 0 is a user")
    assert_eq(body and body.ok, true)
end)

print(string.format("stdlib audit 24: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
