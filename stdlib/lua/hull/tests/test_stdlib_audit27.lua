-- test_stdlib_audit27.lua - stdlib regressions from audit 27 (Lua).
--
-- csv header order (Lua stays sorted), search paging clamps instead of
-- raising, jobs time options as whole seconds and null / numeric-string
-- parity with the JS twin, attachment max_size = false, and address checks
-- that refuse invalid UTF-8.
--
-- Runs in the caps-bearing context (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): search / jobs need the db, email the
-- http-client + smtp caps.

local csv        = require("hull.csv")
local search     = require("hull.search")
local jobs       = require("hull.jobs")
local attachment = require("hull.attachment")
local email      = require("hull.email")
local time       = require("hull.time")
local _af        = require("hull.web.auth-flows")   -- registers its test hooks

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

-- ── csv ───────────────────────────────────────────────────────────

test("csv.encode: Lua headers stay the sorted union; columns honoured", function()
    assert_eq(csv.encode({ { title = "t", id = 1, done = 0 } }, { headers = true }),
              "done,id,title\n0,1,t\n", "sorted")
    assert_eq(csv.encode({ { title = "t", id = 1, done = 0 } },
                         { headers = true, columns = { "title", "id", "done" } }),
              "title,id,done\nt,1,0\n", "columns order")
end)

-- ── search paging ─────────────────────────────────────────────────

test("search.query: limit / offset clamp instead of raising", function()
    search.create_index("a27docs", { "body" })
    for i = 1, 5 do search.index("a27docs", tostring(i), { body = "alpha " .. i }) end
    local function n(opts) return #search.query("a27docs", "alpha", opts) end
    assert_eq(n({ limit = 5000 }), 5, "limit 5000 clamps to 1000")
    assert_eq(n({ limit = 0 }), 1, "limit 0 clamps to 1")
    assert_eq(n({ limit = -3 }), 1, "negative clamps to 1")
    assert_eq(n({ limit = "2" }), 2, "numeric string")
    assert_eq(n({ limit = " 3 " }), 3, "numeric string with spaces")
    assert_eq(n({ limit = 2.9 }), 2, "fraction floors")
    assert_eq(n({ limit = "abc" }), 5, "garbage takes the default (20)")
    assert_eq(n({ limit = 0 / 0 }), 5, "NaN takes the default")
    assert_eq(n({ limit = true }), 5, "boolean takes the default")
    assert_eq(n({ limit = math.huge }), 5, "inf clamps to 1000")
    assert_eq(n({ offset = "3" }), 2, "query-string offset")
    assert_eq(n({ offset = -1 }), 5, "negative offset clamps to 0")
    assert_eq(n({ offset = "x" }), 5, "garbage offset takes the default (0)")
    assert_eq(n({ offset = math.huge }), 0, "inf offset clamps to the max")
    assert_eq(n({ offset = "Infinity" }), 5, "\"Infinity\" is not a number (tonumber)")
    -- The query caps still raise on a bad value.
    assert_eq(refused(search.query, "a27docs", "alpha", { max_query_len = "1024" }),
              true, "max_query_len stays strict")
end)

-- ── jobs ──────────────────────────────────────────────────────────

test("jobs.enqueue: a fractional throttle works (whole seconds)", function()
    jobs.init()
    local id = jobs.enqueue("a27thr", {}, { queue = "a27thr", throttle = 0.5 })
    assert_eq(type(id), "number", "first enqueue")
    assert_eq(jobs.enqueue("a27thr", {}, { queue = "a27thr", throttle = 2.5 }), nil,
              "throttled")
end)

test("jobs.cleanup: fractional before / older_than are accepted", function()
    jobs.init()
    jobs.handler("a27clean", function() return true end)
    local id = jobs.enqueue("a27clean", {}, { queue = "a27clean" })
    jobs.work({ queue = "a27clean", batch = 1 })
    assert_eq(jobs.get(id).status, "done", "ran")
    assert_eq(jobs.cleanup({ queue = "a27clean", older_than = 0.5 }), 0, "young row kept")
    assert_eq(jobs.cleanup({ queue = "a27clean", before = time.now() - 0.5 }), 0,
              "before rounds up, row kept")
    assert_eq(jobs.cleanup({ queue = "a27clean", before = time.now() + 1.5 }), 1,
              "before in the future deletes")
end)

test("jobs: count options read numbers as tonumber does", function()
    jobs.init()
    jobs.handler("a27dead", function() error("boom") end)
    jobs.enqueue("a27dead", {}, { queue = "a27dq", max_attempts = 1 })
    jobs.work({ queue = "a27dq", batch = 1 })
    local function dead(o) o.queue = "a27dq"; return #jobs.dead(o) end
    assert_eq(dead({}), 1, "one dead")
    assert_eq(dead({ offset = "Infinity" }), 1, "\"Infinity\" takes the default offset")
    assert_eq(dead({ offset = "0x0" }), 1, "hex string")
    assert_eq(dead({ offset = math.huge }), 0, "inf clamps")
end)

test("jobs: nil max_attempts is unset in init / enqueue / cron", function()
    jobs.init({ max_attempts = nil })
    assert_eq(type(jobs.enqueue("a27ma", {}, { queue = "a27ma", max_attempts = nil })),
              "number", "enqueue")
    jobs.cron("a27macron", "* * * * *", nil, { max_attempts = nil })
    jobs.uncron("a27macron")
end)

-- ── attachment ────────────────────────────────────────────────────

test("attachment.init: max_size = false resets the cap to unlimited", function()
    attachment.init({ max_size = 10 })
    attachment.init({ max_size = false })
    assert_eq(refused(attachment.init, { max_size = true }), true, "true refused")
    assert_eq(refused(attachment.init, { max_size = 0 }), true, "0 refused")
end)

-- ── addresses: valid UTF-8 only ───────────────────────────────────

local BAD_UTF8 = { "a\xff@x.co", "a@x\xc3.co", "\xc0\xafa@x.co", "a@x.c\xed\xa0\x80o" }

test("email.send: an address that is not valid UTF-8 is refused", function()
    for _, bad in ipairs(BAD_UTF8) do
        local ok, err = pcall(email.send, { from = bad, to = "y@z.co",
                                            subject = "s", body = "b" })
        assert_eq(ok, false, "refused")
        assert_eq(type(err) == "table" and err.code, "invalid_argument", "code")
    end
end)

test("auth-flows is_email_ish: invalid UTF-8 refused, valid UTF-8 kept", function()
    local is_email_ish = __test_hooks('hull.web.auth-flows').is_email_ish
    for _, bad in ipairs(BAD_UTF8) do
        assert_eq(is_email_ish(bad), false, "refused")
    end
    assert_eq(is_email_ish("jos\xc3\xa9@x.co"), true, "valid UTF-8 accepted")
    assert_eq(is_email_ish("a@x.co"), true, "ascii accepted")
end)

print(string.format("stdlib audit 27: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
