-- test_stdlib_audit22.lua - stdlib regressions from audit 22 (Lua).
--
-- With keys configured, a plaintext row (encrypted = 0) - one a
-- writer of _hull_totp planted, say - verified and was then sealed bound to
-- the account. allow_plaintext_secrets = false refuses it.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): everything here needs the db.

local totp    = require("hull.web.middleware.totp")

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

local T = __test_hooks("hull.web.middleware.totp")
local KEY = ("k"):rep(32)

-- Enroll and confirm `uid`; returns the raw secret and the confirmed step.
local function enrolled(uid)
    local r = totp.enroll(uid)
    local secret = T.base32_decode(r.secret_base32)
    local step = T.current_step()
    assert_eq(totp.confirm(uid, T.totp_at_step(secret, step, 6)), true, "confirm " .. tostring(uid))
    return secret, step
end

test("allow_plaintext_secrets = false refuses a plaintext row", function()
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY })
    local secret, step = enrolled("u23")
    T.set_blob("u23", secret, 0)
    assert_eq(totp.rekey_status().plaintext, 1, "counted")
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY,
                           allow_plaintext_secrets = false })
    assert_eq(totp.verify("u23", T.totp_at_step(secret, step + 1, 6)), false, "refused")
    assert_eq(totp.rekey_status().plaintext, 1, "not re-sealed")
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY })
    assert_eq(totp.verify("u23", T.totp_at_step(secret, step + 1, 6)), true,
              "the default still lets it verify")
    assert_eq(totp.rekey_status().plaintext, 0, "and seals it")
    local ok = pcall(totp.init, { encryption_key = KEY, allow_plaintext_secrets = 1 })
    assert_eq(ok, false, "a non-boolean is refused")
    T.reset(); totp.init({ issuer = "T", encryption_key = KEY })
end)

print(string.format("stdlib audit 22: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
