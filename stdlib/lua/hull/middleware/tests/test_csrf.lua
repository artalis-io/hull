-- test_csrf.lua — Tests for hull.middleware.csrf
--
-- Requires crypto and time globals (run via hull test harness).

local csrf = require('hull.middleware.csrf')

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
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

-- ── generate ─────────────────────────────────────────────────────────

test("generate returns a string with dot separator", function()
    local token = csrf.generate("sess123", "secret")
    assert(type(token) == "string", "expected string")
    assert(token:find(".", 1, true), "expected dot in token")
end)

-- ── verify ──────────────────────────────────────────────────────────

test("verify accepts valid token", function()
    local token = csrf.generate("sess456", "mysecret")
    assert_eq(csrf.verify(token, "sess456", "mysecret", 3600), true)
end)

test("verify rejects wrong session", function()
    local token = csrf.generate("sess_a", "mysecret")
    assert_eq(csrf.verify(token, "sess_b", "mysecret", 3600), false)
end)

test("verify rejects wrong secret", function()
    local token = csrf.generate("sess_c", "secret1")
    assert_eq(csrf.verify(token, "sess_c", "secret2", 3600), false)
end)

test("verify rejects nil token", function()
    assert_eq(csrf.verify(nil, "sess", "secret", 3600), false)
end)

test("verify rejects malformed token", function()
    assert_eq(csrf.verify("no-dot-here", "sess", "secret", 3600), false)
end)

return {pass = pass, fail = fail}
