-- test_totp_audit28.lua - TOTP regressions from audit 28 (Lua).
--
-- A successful verify takes only that user's own failures from an IP back
-- out of the IP's count. Audit 26 kept them all (an attacker could otherwise
-- reset its count by verifying its own account), so behind a proxy or NAT
-- ordinary typos from every user added up and locked 2FA for everyone.
-- Failures spread over other accounts still count.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): everything here needs the db. Each
-- enrolment hashes ten recovery codes, so the suite enrolls few accounts and
-- records most failures through the bump hook.

local totp = require("hull.web.middleware.totp")

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

local H = function() return __test_hooks("hull.web.middleware.totp") end

-- Enroll + confirm a user; returns its raw secret.
local function enroll(uid)
    local r = totp.enroll(uid)
    local secret = H().base32_decode(r.secret_base32)
    local code = H().totp_at_step(secret, H().current_step(), 6)
    assert_eq(totp.confirm(uid, code), true, "confirm " .. uid)
    return secret, r.recovery_codes
end

-- A code at step current + k (confirm used the current one).
local function code_at(secret, k)
    return H().totp_at_step(secret, H().current_step() + k, 6)
end

test("totp: a user's success takes its own failures off the IP count", function()
    H().reset()
    totp.init({ issuer = "T", cleanup = false, recovery_codes = 1, max_failed_attempts_per_ip = 5 })
    local sa, rc = enroll("a28_a1")
    local ip = "198.51.100.1"
    local req = { remote_addr = ip, headers = {} }
    assert_eq(totp.verify("a28_a1", "000000x", req), false)
    assert_eq(totp.verify("a28_a1", "000000y", req), false)
    assert_eq(H().ip_failed_count(ip), 2, "two failures counted")
    assert_eq(totp.verify("a28_a1", code_at(sa, 1), req), true, "own code verifies")
    assert_eq(H().ip_failed_count(ip), 0, "the user's own failures came off")
    -- The recovery-code path credits the same way.
    H().bump_failed_attempt_ip(ip, "a28_other")
    assert_eq(totp.verify("a28_a1", "000000z", req), false)
    assert_eq(H().ip_failed_count(ip), 2)
    assert_eq(totp.verify("a28_a1", rc[1], req), true, "recovery code verifies")
    assert_eq(H().ip_failed_count(ip), 1, "only a1's failure came off")
end)

test("totp: typos from many users, each then verifying, never lock the IP", function()
    H().reset()
    totp.init({ issuer = "T", cleanup = false, recovery_codes = 1, max_failed_attempts_per_ip = 3 })
    local ip = "198.51.100.5"
    -- Record each user's typo, then credit that user's success: the count
    -- never reaches 3.
    for i = 1, 10 do
        H().bump_failed_attempt_ip(ip, "a28_t" .. i)
        H().credit_success_ip(ip, "a28_t" .. i)
    end
    assert_eq(H().ip_failed_count(ip), 0)
    assert_eq(H().lockout_remaining_ip(ip), 0, "no lockout from honest typos")
end)

test("totp: another account's failures stay after a success", function()
    H().reset()
    totp.init({ issuer = "T", cleanup = false, recovery_codes = 1, max_failed_attempts_per_ip = 5 })
    local sa = enroll("a28_a2")
    local ip = "198.51.100.2"
    local req = { remote_addr = ip, headers = {} }
    for _ = 1, 3 do H().bump_failed_attempt_ip(ip, "a28_b2") end
    assert_eq(totp.verify("a28_a2", "000000z", req), false)
    assert_eq(H().ip_failed_count(ip), 4)
    assert_eq(totp.verify("a28_a2", code_at(sa, 1), req), true, "attacker's own code")
    assert_eq(H().ip_failed_count(ip), 3, "only a2's one failure came off")
    -- The attacker's success left b2's share, and its next failure locks.
    assert_eq(totp.verify("a28_a2", "000000v", req), false)
    assert_eq(totp.verify("a28_a2", "000000u", req), false)
    assert_eq(H().lockout_remaining_ip(ip) > 0, true,
              "b2's failures still count toward the lockout")
end)

test("totp: failures from distinct users with no success still lock the IP", function()
    H().reset()
    totp.init({ issuer = "T", cleanup = false, recovery_codes = 1, max_failed_attempts_per_ip = 20 })
    local ip = "198.51.100.3"
    for i = 1, 19 do
        H().bump_failed_attempt_ip(ip, "a28_d" .. i)
        assert_eq(H().lockout_remaining_ip(ip), 0, "not locked after " .. i)
    end
    assert_eq(H().ip_failed_count(ip), 19)
    H().bump_failed_attempt_ip(ip, "a28_d20")
    assert_eq(H().lockout_remaining_ip(ip) > 0, true, "20 failures lock the IP")
end)

test("totp: a lockout drops the shares, so a later success takes nothing", function()
    H().reset()
    totp.init({ issuer = "T", cleanup = false, recovery_codes = 1, max_failed_attempts_per_ip = 3 })
    local ip = "198.51.100.4"
    for _ = 1, 3 do H().bump_failed_attempt_ip(ip, "a28_e1") end
    assert_eq(H().lockout_remaining_ip(ip) > 0, true, "locked")
    assert_eq(H().ip_failed_count(ip), 0, "the count starts again")
    H().credit_success_ip(ip, "a28_e1")
    assert_eq(H().ip_failed_count(ip), 0)
    -- A new count after the lockout is not reduced by a stale share either.
    H().clear_failed_attempts_ip(ip)
    H().bump_failed_attempt_ip(ip, "a28_e2")
    H().credit_success_ip(ip, "a28_e1")
    assert_eq(H().ip_failed_count(ip), 1, "e1 has no share in the new count")
end)

print(string.format("totp audit 28: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
