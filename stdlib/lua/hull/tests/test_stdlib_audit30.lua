-- test_stdlib_audit30.lua - stdlib regressions from audit 30 (Lua).
--
-- The login lockout counter and TOTP's attempt counters are incremented by
-- the database (read in the stdlib and written back, concurrent attempts on
-- Postgres / MySQL all wrote N+1); their semantics are unchanged: the count
-- increments, locks at the threshold, and after an expired login lockout
-- starts again at 1. rbac.has_any_role / has_any_permission take a string
-- as one name. (The cron re-registration fix is in test_jobs_hooks.lua,
-- case audit30.)
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): everything here needs the db.

local af   = require("hull.web.auth-flows")
local totp = require("hull.web.middleware.totp")
local rbac = require("hull.web.middleware.rbac")

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

local AF = __test_hooks("hull.web.auth-flows")
AF.reset()
af.init({
    secret = ("s"):rep(32),
    trust_request_host = true,
    email_rate_limit = false,
    email_send = function() end,
    templates = {},
    user_find_by_email = function() return nil end,
    user_get = function() return nil end,
    user_create = function() return 1 end,
    user_set_password = function() end,
    user_set_email = function() end,
    user_set_email_verified = function() end,
    on_login = function(_req, res) res:json({ ok = true }) end,
})

test("login lockout: counts, locks at max", function()
    AF.bump_failed_login("a30_u1", 3)
    AF.bump_failed_login("a30_u1", 3)
    assert_eq(AF.login_failed_count("a30_u1"), 2, "two counted")
    assert_eq(AF.lockout_remaining("a30_u1"), 0, "not locked yet")
    AF.bump_failed_login("a30_u1", 3)
    assert_eq(AF.login_failed_count("a30_u1"), 3, "three counted")
    local rem = AF.lockout_remaining("a30_u1")
    assert_eq(rem > 0 and rem <= AF.state.lockout_duration, true, "locked " .. rem)
end)

test("login lockout: the count starts again after an expired window", function()
    local saved = AF.state.lockout_duration
    AF.state.lockout_duration = -5   -- the lock it sets is already over
    local ok, err = pcall(function()
        for _ = 1, 3 do AF.bump_failed_login("a30_u2", 3) end
        assert_eq(AF.login_failed_count("a30_u2"), 3, "locked count")
        assert_eq(AF.lockout_remaining("a30_u2"), 0, "window over")
        AF.state.lockout_duration = saved
        AF.bump_failed_login("a30_u2", 3)
        assert_eq(AF.login_failed_count("a30_u2"), 1, "restarted")
        assert_eq(AF.lockout_remaining("a30_u2"), 0, "the old lock cleared")
        AF.bump_failed_login("a30_u2", 3)
        AF.bump_failed_login("a30_u2", 3)
        assert_eq(AF.lockout_remaining("a30_u2") > 0, true, "locks again at max")
    end)
    AF.state.lockout_duration = saved
    if not ok then error(err, 0) end
end)

test("login lockout: max 1 locks on the first failure", function()
    AF.bump_failed_login("a30_u3", 1)
    assert_eq(AF.login_failed_count("a30_u3"), 1)
    assert_eq(AF.lockout_remaining("a30_u3") > 0, true, "locked")
end)

local T = __test_hooks("hull.web.middleware.totp")
local lockouts = {}
T.reset()
totp.init({ issuer = "T", cleanup = false, recovery_codes = 1,
            max_failed_attempts = 2, lockout_duration = 60,
            max_lockout_duration = 600,
            on_lockout = function(uid, until_, n) lockouts[#lockouts + 1] = { uid, until_, n } end })

test("totp: the per-user count locks every max-th failure, escalating", function()
    T.bump_failed_attempt("a30_t1")
    assert_eq(T.lockout_remaining("a30_t1"), 0, "one failure")
    assert_eq(#lockouts, 0)
    T.bump_failed_attempt("a30_t1")
    local r1 = T.lockout_remaining("a30_t1")
    assert_eq(r1 > 0 and r1 <= 60, true, "first lockout " .. r1)
    assert_eq(#lockouts, 1, "on_lockout")
    assert_eq(lockouts[1][3], 1, "n = 1")
    T.bump_failed_attempt("a30_t1")
    assert_eq(#lockouts, 1, "not at the threshold")
    T.bump_failed_attempt("a30_t1")
    assert_eq(#lockouts, 2, "second lockout")
    assert_eq(lockouts[2][3], 2, "n = 2")
    local r2 = T.lockout_remaining("a30_t1")
    assert_eq(r2 > 60 and r2 <= 120, true, "doubled " .. r2)
    T.clear_failed_attempts("a30_t1")
    assert_eq(T.lockout_remaining("a30_t1"), 0, "cleared")
end)

test("totp: the per-IP count and its per-user shares", function()
    local ip = "203.0.113.30"
    T.bump_failed_attempt_ip(ip, "a30_x")
    T.bump_failed_attempt_ip(ip, "a30_x")
    T.bump_failed_attempt_ip(ip, "a30_y")
    assert_eq(T.ip_failed_count(ip), 3, "three counted")
    T.credit_success_ip(ip, "a30_x")
    assert_eq(T.ip_failed_count(ip), 1, "x's two came off")
    T.credit_success_ip(ip, "a30_x")
    assert_eq(T.ip_failed_count(ip), 1, "credited once")
    T.clear_failed_attempts_ip(ip)
end)

rbac.init()
rbac.assign("a30_r1", "admin")
rbac.assign("a30_r2", "d")
rbac.define_role("editor", { "posts.write" })
rbac.assign("a30_r3", "editor")

test("rbac: has_any_role takes a string as one role", function()
    assert_eq(rbac.has_any_role("a30_r1", "admin"), true, "admin")
    assert_eq(rbac.has_any_role("a30_r2", "admin"), false, "a role named 'd' is not admin")
    assert_eq(rbac.has_any_role("a30_r1", { "x", "admin" }), true, "list")
end)

test("rbac: has_any_permission takes a string as one permission", function()
    assert_eq(rbac.has_any_permission("a30_r3", "posts.write"), true, "string")
    assert_eq(rbac.has_any_permission("a30_r3", "p"), false, "no such permission")
    assert_eq(rbac.has_any_permission("a30_r3", { "posts.write" }), true, "list")
end)

return { pass = pass, fail = fail }
