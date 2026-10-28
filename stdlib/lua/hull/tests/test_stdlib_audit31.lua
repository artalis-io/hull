-- test_stdlib_audit31.lua - stdlib regressions from audit 31 (Lua).
--
-- The lockout counters decide whether to insert their row with a plain read
-- (an UPDATE matching no row took a MySQL gap lock, and two new keys in one
-- gap deadlocked); a failed password check bumps the account row before the
-- per-IP row, so a failure on the IP row still counts the account; a TOTP
-- per-IP failure keeps a lock another instance set; and /login counts an
-- unknown address under a key of its own, with the same reads and writes as
-- a known one. The JS twin is test_stdlib_audit31.js.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): everything here needs the db.

local json   = require("hull.json")
local crypto = require("hull.crypto")
local af     = require("hull.web.auth-flows")
local totp   = require("hull.web.middleware.totp")

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

local function mkreq(body, ip)
    return { method = "POST", path = "/auth/login",
             headers = { host = "app.test", ["content-type"] = "application/json",
                         ["sec-fetch-site"] = "same-origin" },
             body = json.encode(body or {}), ctx = {},
             remote_addr = ip or "192.0.2.31" }
end

local USERS = {
    ["known@a31.test"] = { id = "a31_known", email = "known@a31.test",
                           email_verified = true,
                           password_hash = crypto.hash_password("right-password-31") },
}

local AF = __test_hooks("hull.web.auth-flows")
AF.reset()
af.init({
    secret = ("s"):rep(32),
    trust_request_host = true,
    email_rate_limit = false,
    max_failed_logins = 2,
    email_send = function() end,
    templates = {},
    user_find_by_email = function(e) return USERS[e:lower()] end,
    user_get = function() return nil end,
    user_create = function() return 1 end,
    user_set_password = function() end,
    user_set_email = function() end,
    user_set_email_verified = function() end,
    on_login = function(_req, res) res:json({ ok = true }) end,
})

test("lockout: adjacent new keys of one account are each counted", function()
    local a = "a31_adj" .. "\31" .. "198.51.100.1"
    local b = "a31_adj" .. "\31" .. "198.51.100.2"
    AF.bump_failed_login(a, 5)
    AF.bump_failed_login(b, 5)
    AF.bump_failed_login(a, 5)
    assert_eq(AF.login_failed_count(a), 2, "a")
    assert_eq(AF.login_failed_count(b), 1, "b")
end)

test("lockout: the account row is bumped before the per-IP row", function()
    local saved = AF.state.max_failed_logins
    -- The per-IP bump fails (a table is not a SQL parameter); the account's
    -- bump, made first, stays counted.
    AF.state.max_failed_logins = {}
    local ok = pcall(AF.bump_failed_logins, "a31_order", "a31_order\31" .. "203.0.113.9")
    AF.state.max_failed_logins = saved
    assert_eq(ok, false, "the per-IP bump failed")
    assert_eq(AF.login_failed_count("a31_order"), 1, "account counted")
    assert_eq(AF.login_failed_count("a31_order\31" .. "203.0.113.9"), 0,
              "the per-IP batch rolled back")
end)

test("login: an unknown address is counted under its own key", function()
    local key = AF.unknown_login_key("Nobody@A31.test")
    assert_eq(key, AF.unknown_login_key("nobody@a31.test"), "ASCII case folded")
    assert_eq(key:sub(1, 1), "\31", "prefix no account key has")
    assert_eq(#key, #"\31unknown\31" + 64, "sha256 hex")
    local ip_key = key .. "\31" .. "192.0.2.31"
    local res = mkres()
    AF.handlers.login(mkreq({ email = "nobody@a31.test", password = "wrong-password" }), res)
    assert_eq(res.code, 401, "401")
    assert_eq(res.body.error, "invalid credentials")
    assert_eq(AF.login_failed_count(key), 1, "account twin")
    assert_eq(AF.login_failed_count(ip_key), 1, "per-IP twin")
    AF.handlers.login(mkreq({ email = "NOBODY@a31.test", password = "wrong-password" }), mkres())
    assert_eq(AF.login_failed_count(ip_key), 2, "same key, case folded")
    assert_eq(AF.lockout_remaining(ip_key) > 0, true, "locked at max_failed_logins")
    -- Locked: the same answer, and no further count.
    local res3 = mkres()
    AF.handlers.login(mkreq({ email = "nobody@a31.test", password = "wrong-password" }), res3)
    assert_eq(res3.code, 401, "locked 401")
    assert_eq(res3.body.error, "invalid credentials")
    assert_eq(AF.login_failed_count(ip_key), 2, "not counted while locked")
end)

test("login: both runtimes derive the same unknown-address key", function()
    -- sha256("nobody@a31.test"), as the JS twin asserts it.
    assert_eq(AF.unknown_login_key("nobody@a31.test"),
              "\31unknown\31"
              .. "6fe6ced6c91c198a591c9a0da895f2ce80321cc9324983f3ecffad3967be7d66",
              "the key")
end)

test("login: a known address does the same bookkeeping", function()
    local ip_key = "a31_known\31" .. "192.0.2.31"
    local res = mkres()
    AF.handlers.login(mkreq({ email = "known@a31.test", password = "wrong-password" }), res)
    assert_eq(res.code, 401)
    assert_eq(res.body.error, "invalid credentials")
    assert_eq(AF.login_failed_count("a31_known"), 1, "account")
    assert_eq(AF.login_failed_count(ip_key), 1, "per-IP")
    local ok = mkres()
    AF.handlers.login(mkreq({ email = "known@a31.test", password = "right-password-31" }), ok)
    assert_eq(ok.code, 200, "right password")
    assert_eq(AF.login_failed_count("a31_known"), 0, "cleared")
end)

local T = __test_hooks("hull.web.middleware.totp")
T.reset()
totp.init({ issuer = "T", cleanup = false, recovery_codes = 1,
            max_failed_attempts_per_ip = 2, lockout_duration_per_ip = 60 })

test("totp: a per-IP failure after the lock keeps it", function()
    local ip = "203.0.113.31"
    T.bump_failed_attempt_ip(ip, "a31_x")
    T.bump_failed_attempt_ip(ip, "a31_y")
    local r1 = T.lockout_remaining_ip(ip)
    assert_eq(r1 > 0, true, "locked at the threshold")
    -- A failure that passed the check before another instance locked it.
    T.bump_failed_attempt_ip(ip, "a31_x")
    assert_eq(T.lockout_remaining_ip(ip) > 0, true, "still locked")
    T.clear_failed_attempts_ip(ip)
    assert_eq(T.lockout_remaining_ip(ip), 0, "cleared")
end)

test("totp: counters on new and existing rows", function()
    local ip = "203.0.113.32"
    T.bump_failed_attempt_ip(ip, "a31_z")
    assert_eq(T.ip_failed_count(ip), 1, "new row")
    T.credit_success_ip(ip, "a31_z")
    assert_eq(T.ip_failed_count(ip), 0, "credited")
    T.credit_success_ip("203.0.113.99", "a31_z")   -- no row: nothing to credit
    T.bump_failed_attempt("a31_u")
    T.bump_failed_attempt("a31_u")
    assert_eq(T.lockout_remaining("a31_u"), 0, "under the per-user max")
    T.clear_failed_attempts("a31_u")
    T.clear_failed_attempts_ip(ip)
end)

return { pass = pass, fail = fail }
