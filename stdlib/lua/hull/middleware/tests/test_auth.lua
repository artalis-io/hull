-- test_auth.lua — Tests for hull.middleware.auth
--
-- Requires db, crypto, time, json globals (run via hull test harness).

local auth = require('hull.middleware.auth')

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

-- ── session_middleware ────────────────────────────────────────────────

test("session_middleware returns a function", function()
    local mw = auth.session_middleware({ optional = true })
    assert(type(mw) == "function", "expected function")
end)

-- ── jwt_middleware ───────────────────────────────────────────────────

test("jwt_middleware returns a function", function()
    local mw = auth.jwt_middleware({ secret = "test-secret" })
    assert(type(mw) == "function", "expected function")
end)

test("jwt_middleware requires secret", function()
    local ok, _ = pcall(function()
        auth.jwt_middleware({})
    end)
    assert(not ok, "expected error without secret")
end)

return {pass = pass, fail = fail}
