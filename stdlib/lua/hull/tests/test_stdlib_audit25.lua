-- test_stdlib_audit25.lua - stdlib regressions from audit 25 (Lua stdlib).
--
-- jobs.enqueue takes delay / at / run_at as finite numbers of seconds (or
-- numeric strings), whole - the JS twin's rule. outbox.enqueue does the same
-- for delay and holds max_attempts (per call and at init) to a whole number
-- >= 0.
--
-- Runs in the caps-bearing context (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): jobs and outbox need the db.

local jobs   = require("hull.jobs")
local time   = require("hull.time")
local outbox = require("hull.web.middleware.outbox")

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

-- ── jobs ───────────────────────────────────────────────────────────

test("jobs.enqueue: a numeric-string delay is added, whole", function()
    jobs.init()
    local before = time.now()
    local j = jobs.get(jobs.enqueue("a25delay", {}, { queue = "a25q", delay = "60" }))
    assert_eq(math.type(j.run_at), "integer", "run_at is an integer")
    assert_eq(j.run_at >= before + 60 and j.run_at <= time.now() + 60, true,
              "now + 60, got " .. tostring(j.run_at))
end)

test("jobs.enqueue: delay / at / run_at are whole seconds", function()
    jobs.init()
    local before = time.now()
    local j = jobs.get(jobs.enqueue("a25frac", {}, { queue = "a25q", delay = 2.5 }))
    assert_eq(math.type(j.run_at), "integer", "fractional delay rounded")
    assert_eq(j.run_at >= before + 3, true, "rounded up")
    j = jobs.get(jobs.enqueue("a25at", {}, { queue = "a25q", at = 1900000000.2 }))
    assert_eq(j.run_at, 1900000001, "at rounded up")
    j = jobs.get(jobs.enqueue("a25runat", {}, { queue = "a25q", run_at = "1900000000" }))
    assert_eq(j.run_at, 1900000000, "numeric-string run_at")
end)

test("jobs.enqueue: a non-numeric schedule is refused", function()
    jobs.init()
    for _, k in ipairs({ "delay", "at", "run_at" }) do
        for _, bad in ipairs({ "soon", "", math.huge, -math.huge, 0 / 0, {}, true }) do
            local o = { queue = "a25bad" }
            o[k] = bad
            local ok, err = pcall(jobs.enqueue, "a25bad", {}, o)
            assert_eq(ok, false, k .. " = " .. tostring(bad))
            assert_eq(tostring(err):find("finite number", 1, true) ~= nil, true,
                      tostring(err))
        end
    end
end)

-- ── outbox ─────────────────────────────────────────────────────────

test("outbox.enqueue: a numeric-string delay is added, whole", function()
    outbox.init()
    local hooks = __test_hooks("hull.web.middleware.outbox")
    local before = time.now()
    local id = outbox.enqueue({ kind = "webhook", destination = "http://a25.test/",
                                payload = "{}", delay = "60" })
    local r = hooks.rows(id)[1]
    assert_eq(r.next_attempt_at >= before + 60 and r.next_attempt_at <= time.now() + 60,
              true, "now + 60, got " .. tostring(r.next_attempt_at))
    id = outbox.enqueue({ kind = "webhook", destination = "http://a25.test/",
                          payload = "{}", delay = 1.5 })
    r = hooks.rows(id)[1]
    assert_eq(math.type(r.next_attempt_at), "integer", "whole seconds")
    hooks.clear()
end)

test("outbox: a bad delay or max_attempts is refused", function()
    outbox.init()
    local function with(k, v)
        return { kind = "webhook", destination = "http://a25.test/", payload = "{}",
                 [k] = v }
    end
    for _, bad in ipairs({ "soon", math.huge, 0 / 0, {}, true }) do
        assert_eq(pcall(outbox.enqueue, with("delay", bad)), false, "delay " .. tostring(bad))
    end
    for _, bad in ipairs({ "5", 2.5, -1, 0 / 0, math.huge, 2 ^ 31, false }) do
        assert_eq(pcall(outbox.enqueue, with("max_attempts", bad)), false,
                  "max_attempts " .. tostring(bad))
        assert_eq(pcall(outbox.init, { max_attempts = bad }), false,
                  "init max_attempts " .. tostring(bad))
    end
    -- 0 (no retries), an integral float and an ordinary count are fine.
    outbox.enqueue(with("max_attempts", 0))
    outbox.enqueue(with("max_attempts", 3.0))
    outbox.init({ max_attempts = 7 })
    outbox.init({ max_attempts = 5 })
    __test_hooks("hull.web.middleware.outbox").clear()
end)

print(string.format("stdlib audit 25: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
