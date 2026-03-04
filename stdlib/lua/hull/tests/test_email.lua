-- test_email.lua — Tests for hull.email
--
-- Tests field validation and provider dispatch (no network I/O).
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local email = require('hull.email')

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

-- ── validation ──────────────────────────────────────────────────────

test("nil opts returns error", function()
    local r = email.send(nil)
    assert_eq(r.ok, false)
    assert_eq(r.error, "opts required")
end)

test("missing from returns error", function()
    local r = email.send({ to = "x@y.com", subject = "s", body = "b" })
    assert_eq(r.ok, false)
    assert_eq(r.error, "from required")
end)

test("missing to returns error", function()
    local r = email.send({ from = "x@y.com", subject = "s", body = "b" })
    assert_eq(r.ok, false)
    assert_eq(r.error, "to required")
end)

test("missing subject returns error", function()
    local r = email.send({ from = "x@y.com", to = "y@z.com", body = "b" })
    assert_eq(r.ok, false)
    assert_eq(r.error, "subject required")
end)

test("missing body returns error", function()
    local r = email.send({ from = "x@y.com", to = "y@z.com", subject = "s" })
    assert_eq(r.ok, false)
    assert_eq(r.error, "body required")
end)

-- ── provider dispatch ───────────────────────────────────────────────

test("unknown provider returns error", function()
    local r = email.send({
        provider = "unknown",
        from = "a@b.com", to = "c@d.com",
        subject = "s", body = "b",
    })
    assert_eq(r.ok, false)
    assert(r.error:find("unknown provider"), "expected 'unknown provider' in error")
end)

test("default provider is smtp", function()
    -- Will fail because smtp is not configured in test env, but should not
    -- error about "unknown provider" — it dispatches to smtp adapter
    local r = email.send({
        from = "a@b.com", to = "c@d.com",
        subject = "s", body = "b",
        smtp_host = "localhost",
    })
    -- Should either succeed or fail with smtp-related error, not "unknown provider"
    assert(r.error == nil or not r.error:find("unknown provider"),
           "should dispatch to smtp, not fail with unknown provider")
end)

-- ── api provider validation ─────────────────────────────────────────

test("postmark requires api_key", function()
    local r = email.send({
        provider = "postmark",
        from = "a@b.com", to = "c@d.com",
        subject = "s", body = "b",
    })
    assert_eq(r.ok, false)
    assert(r.error:find("api_key required"), "expected api_key required error")
end)

test("sendgrid requires api_key", function()
    local r = email.send({
        provider = "sendgrid",
        from = "a@b.com", to = "c@d.com",
        subject = "s", body = "b",
    })
    assert_eq(r.ok, false)
    assert(r.error:find("api_key required"), "expected api_key required error")
end)

test("resend requires api_key", function()
    local r = email.send({
        provider = "resend",
        from = "a@b.com", to = "c@d.com",
        subject = "s", body = "b",
    })
    assert_eq(r.ok, false)
    assert(r.error:find("api_key required"), "expected api_key required error")
end)

-- ── results ─────────────────────────────────────────────────────────

print(pass .. " passed, " .. fail .. " failed")
if fail > 0 then error(fail .. " test(s) failed") end
