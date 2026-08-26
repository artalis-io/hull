-- test_email.lua - Tests for hull.email
--
-- Tests field validation and provider dispatch (no network I/O).
-- email.send follows the stdlib error convention: it THROWS a coded error
-- table ({ code, message }) on failure and returns true on success.
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

-- Assert email.send(opts) throws a coded error whose code == want_code and
-- (optionally) whose message contains want_msg.
local function expect_code(name, opts, want_code, want_msg)
    test(name, function()
        local ok, err = pcall(email.send, opts)
        assert_eq(ok, false, "expected a throw")
        assert_eq(type(err), "table", "expected an error table")
        assert_eq(err.code, want_code, "code")
        if want_msg then
            assert(tostring(err):find(want_msg, 1, true),
                   "expected message to contain '" .. want_msg .. "', got: " .. tostring(err))
        end
    end)
end

-- ── validation ──────────────────────────────────────────────────────

expect_code("nil opts throws", nil, "invalid_argument", "opts required")

expect_code("missing from throws",
    { to = "x@y.com", subject = "s", body = "b" },
    "invalid_argument", "from required")

expect_code("missing to throws",
    { from = "x@y.com", subject = "s", body = "b" },
    "invalid_argument", "to required")

expect_code("missing subject throws",
    { from = "x@y.com", to = "y@z.com", body = "b" },
    "invalid_argument", "subject required")

expect_code("missing body throws",
    { from = "x@y.com", to = "y@z.com", subject = "s" },
    "invalid_argument", "body required")

expect_code("invalid from address throws",
    { from = "not-an-email", to = "y@z.com", subject = "s", body = "b" },
    "invalid_argument", "invalid from address")

expect_code("invalid to address throws",
    { from = "x@y.com", to = "not-an-email", subject = "s", body = "b" },
    "invalid_argument", "invalid to address")

-- ── provider dispatch ───────────────────────────────────────────────

expect_code("unknown provider throws",
    { provider = "unknown", from = "a@b.com", to = "c@d.com",
      subject = "s", body = "b" },
    "unknown_provider", "unknown provider")

test("default provider is smtp", function()
    -- Dispatches to the smtp adapter (which then fails because smtp is not
    -- configured / localhost is unreachable in the test env). The failure must
    -- NOT be unknown_provider - that would mean it never reached smtp.
    local ok, err = pcall(email.send, {
        from = "a@b.com", to = "c@d.com",
        subject = "s", body = "b",
        smtp_host = "localhost",
    })
    if not ok then
        assert(type(err) ~= "table" or err.code ~= "unknown_provider",
               "should dispatch to smtp, not fail with unknown_provider")
    end
end)

-- ── api provider validation ─────────────────────────────────────────

expect_code("postmark requires api_key",
    { provider = "postmark", from = "a@b.com", to = "c@d.com",
      subject = "s", body = "b" },
    "invalid_argument", "api_key required")

expect_code("sendgrid requires api_key",
    { provider = "sendgrid", from = "a@b.com", to = "c@d.com",
      subject = "s", body = "b" },
    "invalid_argument", "api_key required")

expect_code("resend requires api_key",
    { provider = "resend", from = "a@b.com", to = "c@d.com",
      subject = "s", body = "b" },
    "invalid_argument", "api_key required")

-- ── results ─────────────────────────────────────────────────────────

print(pass .. " passed, " .. fail .. " failed")
if fail > 0 then error(fail .. " test(s) failed") end
