-- test_stdlib_options_audit26.lua - stdlib option validation from audit 26 (Lua).
--
-- audit_log retain_days, attachment max_size, search limit / offset / query
-- caps, jobs max_attempts / priority / throttle / concurrency and
-- cleanup's before / older_than, idempotency / inbox ttl bounds, email
-- addresses with C1 controls (NEL), and htmx form errors read with raw
-- access. A bad option raises; it no longer deletes every row, disables a
-- cap or retries for ever.
--
-- Runs in the caps-bearing context (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): most of these modules need the db.

local audit_log   = require("hull.web.middleware.audit-log")
local attachment  = require("hull.attachment")
local search      = require("hull.search")
local jobs        = require("hull.jobs")
local idempotency = require("hull.web.middleware.idempotency")
local inbox       = require("hull.web.middleware.inbox")
local email       = require("hull.email")
local form        = require("hull.web.htmx.form")

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

local function refused(fn, ...)
    return (pcall(fn, ...)) == false
end

local BAD_NUMS = { "30", "", false, true, {}, 0 / 0, math.huge, -math.huge, 0, -1 }

-- ── audit_log.retain_days ─────────────────────────────────────────

test("audit_log.init: a bad retain_days raises and deletes nothing", function()
    local salt = "a26-salt-0123456789"
    audit_log.init({ fingerprint_salt = salt, cleanup = false })
    audit_log.record(2601, "login", { headers = {}, ip = "10.0.0.1" })
    local before = #audit_log.list(2601)
    assert_eq(before >= 1, true, "a row was recorded")
    for _, bad in ipairs(BAD_NUMS) do
        assert_eq(refused(audit_log.init, { fingerprint_salt = salt, retain_days = bad,
                                            cleanup = false }), true, tostring(bad))
    end
    assert_eq(refused(audit_log.init, { fingerprint_salt = salt, retain_days = 36501,
                                        cleanup = false }), true, "over 100 years")
    -- The default (365) still holds: today's row survives a cleanup.
    audit_log.cleanup()
    assert_eq(#audit_log.list(2601), before, "the row survives")
    audit_log.init({ fingerprint_salt = salt, retain_days = 30, cleanup = false })
    audit_log.cleanup()
    assert_eq(#audit_log.list(2601), before, "the row survives retain_days = 30")
end)

-- ── attachment.max_size ───────────────────────────────────────────

test("attachment.init: max_size must be a finite number > 0", function()
    for _, bad in ipairs(BAD_NUMS) do
        assert_eq(refused(attachment.init, { max_size = bad }), true, tostring(bad))
    end
    attachment.init({ max_size = 1024 })
end)

-- ── search limit / offset / caps ──────────────────────────────────

test("search.query: limit / offset / caps are bounded integers", function()
    search.create_index("a26docs", { "body" })
    search.index("a26docs", "1", { body = "alpha one" })
    search.index("a26docs", "2", { body = "alpha two" })
    search.index("a26docs", "3", { body = "alpha three" })
    assert_eq(#search.query("a26docs", "alpha", { limit = 2 }), 2, "limit 2")
    assert_eq(#search.query("a26docs", "alpha", { limit = 2, offset = 2 }), 1, "offset 2")
    for _, bad in ipairs({ "-1", -1, 0, 1.5, 0 / 0, math.huge, 1001, true }) do
        assert_eq(refused(search.query, "a26docs", "alpha", { limit = bad }), true,
                  "limit " .. tostring(bad))
    end
    for _, bad in ipairs({ "1", -1, 1.5, 0 / 0, math.huge }) do
        assert_eq(refused(search.query, "a26docs", "alpha", { offset = bad }), true,
                  "offset " .. tostring(bad))
    end
    for _, k in ipairs({ "max_query_len", "max_query_terms" }) do
        for _, bad in ipairs({ "1024", 0, -1, 1.5, 0 / 0, math.huge }) do
            assert_eq(refused(search.query, "a26docs", "alpha", { [k] = bad }), true,
                      k .. " " .. tostring(bad))
        end
    end
end)

-- ── jobs ──────────────────────────────────────────────────────────

test("jobs: max_attempts is an integer in 1..10000", function()
    jobs.init()
    for _, bad in ipairs({ "5", 0, -1, 1.5, 0 / 0, math.huge, 10001, true, {} }) do
        assert_eq(refused(jobs.init, { max_attempts = bad }), true, "init " .. tostring(bad))
        assert_eq(refused(jobs.enqueue, "a26", {}, { queue = "a26q", max_attempts = bad }),
                  true, "enqueue " .. tostring(bad))
        assert_eq(refused(jobs.cron, "a26cron", "* * * * *", nil, { max_attempts = bad }),
                  true, "cron " .. tostring(bad))
    end
    -- A valid one is honoured: max_attempts = 1 dead-letters on the first failure.
    jobs.handler("a26fail", function() error("boom") end)
    local id = jobs.enqueue("a26fail", {}, { queue = "a26fq", max_attempts = 1 })
    jobs.work({ queue = "a26fq", batch = 1 })
    assert_eq(jobs.get(id).status, "dead", "dead after one attempt")
end)

test("jobs.enqueue: priority / throttle / concurrency types", function()
    jobs.init()
    for _, bad in ipairs({ "high", 1.5, 0 / 0, math.huge, true }) do
        assert_eq(refused(jobs.enqueue, "a26p", {}, { queue = "a26q", priority = bad }),
                  true, "priority " .. tostring(bad))
    end
    for _, bad in ipairs({ "soon", -1, 0 / 0, math.huge, true, {} }) do
        assert_eq(refused(jobs.enqueue, "a26t", {}, { queue = "a26q", throttle = bad }),
                  true, "throttle " .. tostring(bad))
    end
    for _, bad in ipairs({ "2", -1, 1.5, 0 / 0, true }) do
        assert_eq(refused(jobs.enqueue, "a26c", {}, { queue = "a26q",
                                                      concurrency_key = "k", concurrency = bad }),
                  true, "concurrency " .. tostring(bad))
    end
    assert_eq(type(jobs.enqueue("a26p", {}, { queue = "a26q", priority = -5,
                                              throttle = 0, concurrency_key = "k",
                                              concurrency = 2 })), "number", "valid opts")
end)

test("jobs.cleanup: a non-number before raises and deletes nothing", function()
    jobs.init()
    jobs.handler("a26clean", function() return true end)
    local id = jobs.enqueue("a26clean", {}, { queue = "a26clean" })
    jobs.work({ queue = "a26clean", batch = 1 })
    assert_eq(jobs.get(id).status, "done", "the job ran")
    for _, bad in ipairs({ "x", "", true, {}, 0 / 0, math.huge }) do
        assert_eq(refused(jobs.cleanup, { queue = "a26clean", before = bad }), true,
                  "before " .. tostring(bad))
    end
    for _, bad in ipairs({ "x", -1, 0 / 0, math.huge }) do
        assert_eq(refused(jobs.cleanup, { queue = "a26clean", older_than = bad }), true,
                  "older_than " .. tostring(bad))
    end
    assert_eq(jobs.get(id) ~= nil, true, "the done job survives")
end)

-- ── idempotency / inbox ttl ───────────────────────────────────────

test("idempotency / inbox: ttl is a whole number of seconds <= 366 days", function()
    for _, bad in ipairs({ 31622401, 1e12, 1.5, 0, -1, 0 / 0, math.huge, "60" }) do
        assert_eq(refused(idempotency.init, { ttl = bad }), true, "idem init " .. tostring(bad))
        assert_eq(refused(idempotency.middleware, { ttl = bad }), true,
                  "idem middleware " .. tostring(bad))
        assert_eq(refused(inbox.init, { ttl = bad }), true, "inbox init " .. tostring(bad))
        assert_eq(refused(inbox.check_and_mark, "a26m", "a26", { ttl = bad }), true,
                  "inbox check_and_mark " .. tostring(bad))
    end
    idempotency.init({ ttl = 31622400 })
    inbox.init({ ttl = 31622400 })
    assert_eq(inbox.check_and_mark("a26m", "a26", { ttl = 60 }), false, "new")
end)

-- ── email addresses ───────────────────────────────────────────────

test("email.send: an address with a C1 control (NEL) or U+2028 is refused", function()
    for _, bad in ipairs({ "a\xC2\x85@x.co", "a@x\xC2\x85.co", "a@x.c\xC2\x80o",
                           "a\xE2\x80\xA8@x.co", "a\xE2\x80\xA9@x.co" }) do
        local ok, err = pcall(email.send, { from = bad, to = "y@z.co",
                                            subject = "s", body = "b" })
        assert_eq(ok, false, "refused")
        assert_eq(type(err) == "table" and err.code, "invalid_argument", "code")
        assert_eq(err.message, "invalid from address", "message")
    end
end)

-- ── htmx form ─────────────────────────────────────────────────────

test("htmx form: errors are read with raw access", function()
    local errs = setmetatable({}, { __index = function() return "inherited" end })
    assert_eq(form.field_error(errs, "constructor"), "", "no own error")
    assert_eq(form.field_attrs(errs, "constructor"), "", "no own attrs")
    assert_eq(form.field_error({ name = "bad" }, "name") ~= "", true, "own error renders")
end)

print(string.format("stdlib audit 26: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
