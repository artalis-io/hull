-- test_stdlib_audit16.lua - stdlib regressions from audit 16 (Lua).
--
-- A module's test hooks are no longer exported to app code: `inbox._test`
-- un-marked any processed message and `totp._test` decrypted any user's
-- stored secret, from a stdlib frame the _hull_* check admits. They live in
-- the stdlib-internal hull._test_hooks, which this script (app code, by its
-- chunk name) cannot require; the harness reads them through __test_hooks.
-- totp.init's allow_unbound_secrets = false refuses a secret sealed before
-- audit 15 (unbound: it opens for any account), and rekey_status() counts
-- such rows. An integral float user id (42.0) is the integer, as in JS.
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

-- ── Test hooks are not app-reachable ───────────────────────────────

test("no stdlib module exports _test", function()
    for _, m in ipairs({ "hull.web.middleware.inbox", "hull.web.middleware.totp",
                         "hull.web.middleware.oauth", "hull.web.middleware.audit-log",
                         "hull.web.auth-flows" }) do
        assert_eq(require(m)._test, nil, m)
    end
end)

test("app code cannot require hull._test_hooks", function()
    local ok, err = pcall(function() return require("hull._test_hooks") end)
    assert_eq(ok, false, "refused")
    assert_eq(tostring(err):find("internal to the Hull stdlib", 1, true) ~= nil, true,
              tostring(err))
end)

-- ── TOTP ───────────────────────────────────────────────────────────

local T = __test_hooks("hull.web.middleware.totp")
local KEY = ("k"):rep(32)
local RING = sealbox.keyring({ keys = { [1] = KEY }, current = 1 })

-- Enroll and confirm `uid`; returns the raw secret and the confirmed step.
local function enrolled(uid)
    local r = totp.enroll(uid)
    local secret = T.base32_decode(r.secret_base32)
    local step = T.current_step()
    assert_eq(totp.confirm(uid, T.totp_at_step(secret, step, 6)), true, "confirm " .. tostring(uid))
    return secret, step
end

test("rekey_status counts unbound rows; a verify re-seals one bound", function()
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY })
    local before = totp.rekey_status().unbound
    local secret, step = enrolled("u20")
    T.set_blob("u20", sealbox.seal(RING, secret))   -- sealed before audit 15
    assert_eq(totp.rekey_status().unbound, before + 1, "counted")
    assert_eq(totp.verify("u20", T.totp_at_step(secret, step + 1, 6)), true,
              "opens while allowed")
    assert_eq(totp.rekey_status().unbound, before, "re-sealed bound")
end)

test("allow_unbound_secrets = false refuses an unbound secret", function()
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY })
    local secret, step = enrolled("u21")
    T.set_blob("u21", sealbox.seal(RING, secret))
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY, allow_unbound_secrets = false })
    local unbound = totp.rekey_status().unbound
    assert_eq(unbound >= 1, true, "still counted when refused")
    assert_eq(totp.verify("u21", T.totp_at_step(secret, step + 1, 6)), false, "refused")
    assert_eq(totp.rekey_status().unbound, unbound, "not re-sealed")
    -- A bound secret is unaffected.
    local s2, st2 = enrolled("u22")
    assert_eq(totp.verify("u22", T.totp_at_step(s2, st2 + 1, 6)), true, "bound still works")
    T.reset()
    local ok = pcall(totp.init, { encryption_key = KEY, allow_unbound_secrets = "no" })
    assert_eq(ok, false, "a non-boolean is refused")
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY })
end)

test("an integral float user id is the integer (as in JS)", function()
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY })
    local r = totp.enroll(42)
    local secret = T.base32_decode(r.secret_base32)
    local step = T.current_step()
    assert_eq(totp.confirm(42.0, T.totp_at_step(secret, step, 6)), true, "confirm as 42.0")
    assert_eq(totp.enrolled(42), true, "enrolled as 42")
    assert_eq(totp.verify(42.0, T.totp_at_step(secret, step + 1, 6)), true, "verify as 42.0")
    -- The seal context agrees too: 42.0 seals as "42".
    local blob = T.encrypt_secret(secret, 42.0)
    assert_eq(T.decrypt_secret(blob, 1, "42"), secret, "42.0 sealed as 42")
    -- A fractional float is still no id.
    assert_eq(totp.verify(42.5, "123456"), false, "42.5 is no id")
end)

print(string.format("stdlib audit 16: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
