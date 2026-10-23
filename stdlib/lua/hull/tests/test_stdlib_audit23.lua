-- test_stdlib_audit23.lua - stdlib regressions from audit 23 (Lua).
--
-- template.compile returns the documented function(data) -> string (it
-- returned the generated (data, escape, filters) function); config refuses a
-- non-finite number; the idempotency default principal reads user ids as the
-- rest of the stdlib does and matches the JS twin; jobs.dead clamps its
-- limit / offset; attachment.sweep takes an integer limit; and the
-- idempotency / pwned helpers are test hooks, not module exports.
--
-- Runs in the caps-bearing state (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): jobs and idempotency need the db.

local template    = require("hull.template")
local config      = require("hull.config")
local idempotency = require("hull.web.middleware.idempotency")
local jobs        = require("hull.jobs")

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

-- ── template.compile ───────────────────────────────────────────────

test("template.compile's function takes data alone", function()
    local T = __test_hooks("hull.template")
    local f = T.compile_string("Hi {{ name }}{% if x %}!{% end %}")
    assert_eq(f({ name = "<b>", x = true }), "Hi &lt;b&gt;!", "escaped")
    assert_eq(f(), "Hi ", "no data is {}")
    assert_eq(f({ name = "y" }, function() return "BAD" end), "Hi y",
              "a caller cannot hand it another escape")
    assert_eq(template.render_string("S {{ v }}", { v = 1 }), "S 1", "render_string")
end)

-- ── config ─────────────────────────────────────────────────────────

test("config: a non-finite number is not a number", function()
    local C = __test_hooks("hull.config")
    assert_eq(type(config.get), "function")
    for _, raw in ipairs({ "1e400", "-1e400" }) do
        assert_eq(pcall(C.coerce, "X", raw, "number"), false, "number " .. raw)
        assert_eq(pcall(C.coerce, "X", raw, "integer"), false, "integer " .. raw)
    end
    assert_eq(C.coerce("X", "42", "integer"), 42)
    assert_eq(C.coerce("X", "1.5", "number"), 1.5)
end)

-- ── idempotency ────────────────────────────────────────────────────

test("idempotency: the default principal normalizes user ids", function()
    local I = __test_hooks("hull.web.middleware.idempotency")
    local function p(ctx) return I.default_principal({ ctx = ctx }) end
    assert_eq(p({ session = { user_id = "" } }), "__anon", "an empty session id is none")
    assert_eq(p({ session = { user_id = 42.0 } }), "session:42", "42.0 is 42")
    assert_eq(p({ session = { user_id = 0 } }), "session:0", "0 is a user")
    assert_eq(p({ session = { user_id = {} }, user = { id = 7 } }), "user:7",
              "a table session id is none")
    assert_eq(p({ user = { sub = "", id = 7 } }), "user:7", "the first JWT id that is one")
    assert_eq(p({ user = { sub = {}, user_id = "u" } }), "user:u")
    assert_eq(p({ user = { sub = 1.5 } }), "__anon", "a fraction is no id")
    assert_eq(p({}), "__anon")
end)

test("idempotency / pwned helpers are not module exports", function()
    assert_eq(idempotency._default_principal, nil)
    assert_eq(idempotency._norm_principal, nil)
    assert_eq(type(__test_hooks("hull.web.middleware.idempotency").norm_principal), "function")
    local pwned = require("hull.web.pwned")
    assert_eq(pwned._sha1_hex, nil)
    assert_eq(pwned._in_local_blocklist, nil)
    local P = __test_hooks("hull.web.pwned")
    assert_eq(P.sha1_hex("password"):upper(), "5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8")
    assert_eq(P.in_local_blocklist(P.sha1_hex("password")), true, "a top password")
end)

-- ── jobs.dead ──────────────────────────────────────────────────────

test("jobs.dead clamps limit to 1..1000 and floors offset at 0", function()
    jobs.init()
    jobs.handler("a23boom", function() error("boom") end)
    for _ = 1, 3 do jobs.enqueue("a23boom", {}, { max_attempts = 1 }) end
    local ok, err = pcall(jobs.work, { batch = 3 })
    assert_eq(ok, true, tostring(err))
    assert_eq(#jobs.dead(), 3, "three dead")
    assert_eq(#jobs.dead({ limit = -1 }), 1, "a negative limit is 1, not 'no limit'")
    assert_eq(#jobs.dead({ limit = 0 }), 1, "0 is 1")
    assert_eq(#jobs.dead({ limit = 0 / 0 }), 3, "NaN is the default")
    assert_eq(#jobs.dead({ limit = math.huge }), 3, "inf is 1000")
    assert_eq(#jobs.dead({ offset = -5 }), 3, "a negative offset is 0")
    assert_eq(#jobs.dead({ offset = 0 / 0 }), 3, "NaN offset is 0")
    assert_eq(#jobs.dead({ limit = 1.9, offset = 1.7 }), 1, "fractions are floored")
end)

-- ── attachment.sweep ───────────────────────────────────────────────

test("attachment.sweep takes an integer limit", function()
    local attachment = require("hull.attachment")
    for _, bad in ipairs({ math.huge, -math.huge, 0 / 0, 1.5, "5", 2 ^ 60 }) do
        assert_eq(pcall(attachment.sweep, { limit = bad }), false, tostring(bad))
    end
    assert_eq(attachment.sweep({ limit = 0 }), 0, "below 1 sweeps nothing")
end)

print(string.format("stdlib audit 23: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
