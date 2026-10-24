-- test_totp_audit26.lua - TOTP regressions from audit 26 (Lua).
--
-- A secret opened without its account context - an unbound v2 blob or a
-- legacy v1 one - must be exactly 20 bytes and not a bound frame, and the v1
-- path is gated by allow_unbound_secrets like the unbound v2 one: a v2 blob
-- with its 4-byte version stripped (nonce || box) read as v1 under the
-- encryption_key shorthand's legacy key, so another account's bound blob
-- became this account's secret. A successful verify no longer clears the
-- per-IP failure count. max_lockout_duration and pending_ttl are numbers.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): everything here needs the db.

local totp    = require("hull.web.middleware.totp")
local sealbox = require("hull.crypto.sealbox")

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
    assert_eq(ok, false, (msg or "") .. ": raises")
    if needle then
        assert_eq(tostring(err):find(needle, 1, true) ~= nil, true,
                  (msg or "") .. ": message names " .. needle .. " (" .. tostring(err) .. ")")
    end
end

local H = function() return __test_hooks("hull.web.middleware.totp") end
local KEY = ("k"):rep(32)

-- Enroll + confirm a user; returns its raw secret.
local function enroll(uid)
    local r = totp.enroll(uid)
    local secret = H().base32_decode(r.secret_base32)
    local code = H().totp_at_step(secret, H().current_step(), 6)
    assert_eq(totp.confirm(uid, code), true, "confirm " .. uid)
    return secret
end

-- ── Cross-account transplant through the legacy v1 path ────────────

test("totp: a stripped bound blob does not open as another account's v1 row", function()
    H().reset()
    totp.init({ issuer = "T", encryption_key = KEY, cleanup = false })
    enroll("attacker")
    enroll("victim")
    local blob = H().get_blob("attacker")
    local stripped = blob:sub(5)    -- drop the 4-byte version: nonce || box
    assert_eq(H().decrypt_secret(stripped, 1, "victim"), nil,
              "a stripped bound blob is refused")
    -- End to end: the attacker writes it into the victim's row and computes
    -- codes from the frame it would have opened to.
    local ring = sealbox.keyring({ keys = { [1] = KEY }, current = 1 })
    local frame = sealbox.open_unversioned(ring, 1, stripped)
    assert_eq(type(frame), "string", "the stripped blob is a v1-shaped box")
    H().set_blob("victim", stripped, 1)
    local step = H().current_step()
    local ok = false
    for off = -1, 1 do
        if totp.verify("victim", H().totp_at_step(frame, step + off, 6)) then ok = true end
    end
    assert_eq(ok, false, "the transplanted frame is not the victim's secret")
end)

test("totp: a stripped unbound blob is refused while allow_unbound_secrets is off", function()
    H().reset()
    totp.init({ issuer = "T", encryption_key = KEY, cleanup = false,
                allow_unbound_secrets = false })
    local ring = sealbox.keyring({ keys = { [1] = KEY }, current = 1 })
    local secret = ("S"):rep(20)
    local v1 = sealbox.seal(ring, secret):sub(5)   -- a legacy v1 blob
    assert_eq(H().decrypt_secret(v1, 1, "u1"), nil, "v1 is unbound: refused")
    -- rekey_status still counts it (as unbound).
    local pt, v, unbound = H().decrypt_secret(v1, 1, "u1", true)
    assert_eq(pt, secret, "counted when allowed explicitly")
    assert_eq(v, 0); assert_eq(unbound, true)
end)

test("totp: a legacy v1 row still opens while allowed, and only at 20 bytes", function()
    H().reset()
    totp.init({ issuer = "T", encryption_key = KEY, cleanup = false })
    local ring = sealbox.keyring({ keys = { [1] = KEY }, current = 1 })
    local secret = ("S"):rep(20)
    local pt, v, unbound = H().decrypt_secret(sealbox.seal(ring, secret):sub(5), 1, "u1")
    assert_eq(pt, secret, "a genuine v1 row opens")
    assert_eq(v, 0, "version 0 = legacy v1")
    assert_eq(unbound, true, "flagged for a bound re-seal")
    assert_eq(H().decrypt_secret(sealbox.seal(ring, ("S"):rep(21)):sub(5), 1, "u1"), nil,
              "a v1 plaintext of another length is refused")
    assert_eq(H().decrypt_secret(sealbox.seal(ring, ("S"):rep(19)), 1, "u1"), nil,
              "an unbound v2 plaintext of another length is refused")
    local bound_frame = string.pack(">s4>s4", "totp", "x") .. ("S"):rep(10)
    assert_eq(H().decrypt_secret(sealbox.seal(ring, bound_frame):sub(5), 1, "u1"), nil,
              "a v1 plaintext that is a bound frame is refused")
end)

-- ── The per-IP count survives a success ────────────────────────────

test("totp: a successful verify does not reset the per-IP failure count", function()
    H().reset()
    totp.init({ issuer = "T", cleanup = false, max_failed_attempts_per_ip = 3 })
    local sa = enroll("ip_a")
    enroll("ip_b")
    local req = { remote_addr = "203.0.113.9", headers = {} }
    assert_eq(totp.verify("ip_b", "000000x", req), false)
    assert_eq(totp.verify("ip_b", "000000y", req), false)
    -- The attacker's own account, at a step confirm did not use.
    assert_eq(totp.verify("ip_a", H().totp_at_step(sa, H().current_step() + 1, 6), req),
              true, "own code verifies")
    assert_eq(totp.verify("ip_b", "000000z", req), false)
    assert_eq(H().lockout_remaining_ip("203.0.113.9") > 0, true,
              "the third failure locks the IP")
end)

-- ── Numeric options ────────────────────────────────────────────────

test("totp: max_lockout_duration must be a finite number >= 0", function()
    for _, bad in ipairs({ "3600", -1, 0/0, math.huge, true, {} }) do
        H().reset()
        raises(function() totp.init({ max_lockout_duration = bad, cleanup = false }) end,
               "max_lockout_duration", tostring(bad))
    end
    H().reset()
    totp.init({ max_lockout_duration = 0, cleanup = false })
    H().reset()
    totp.init({ max_lockout_duration = 3600, cleanup = false })
end)

test("totp: pending_ttl must be a finite number or false", function()
    for _, bad in ipairs({ "3600", 0/0, math.huge, -math.huge, true, {} }) do
        H().reset()
        raises(function() totp.init({ pending_ttl = bad, cleanup = false }) end,
               "pending_ttl", tostring(bad))
    end
    H().reset()
    totp.init({ pending_ttl = false, cleanup = false })
    H().reset()
    totp.init({ pending_ttl = 60, cleanup = false })
end)

print(string.format("totp audit 26: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
