-- test_jobs_hooks.lua - hull.jobs through its clock-injectable test hooks (Lua).
--
-- The cron tick, the cron math and the subscription drain take a caller-chosen
-- clock. Exported on the module (jobs._tick / _cron_next / _events_drain), app
-- code wrote _hull_* rows at a time it chose: jobs._tick(1e12) pushed every
-- schedule's next_run_at decades out, a drain at now = 1e15 held its
-- subscription's lease for ever, and app SQL cannot repair either (audit 17).
-- They are hull._test_hooks entries now, reached here through the harness's
-- __test_hooks. These cases moved from tests/e2e_jobs.sh, which can no longer
-- reach them: v1.1 cron, durable events Phase 2 / Phase 3, the v1.5 polish
-- tz-offset hour, audit 9 HORIZON and audit 10 CRONTZ / STRICT.
--
-- Each case needs a fresh database (event ids, log depth), so the C harness
-- (tests/hull/runtime/lua/test_lua.c, run_jobs_hooks_case) runs this script
-- once per case in a new state, naming the case in the global __jobs_case.

local jobs = require("hull.jobs")
local time = require("hull.time")

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

local hooks = __test_hooks("hull.jobs")
local cases = {}

-- v1.1 cron + v1.5 polish + audit 10 CRONTZ / STRICT.
function cases.cron()
    jobs.init()
    test("cron_next math on a fixed epoch", function()
        local b = 1609459200   -- 2021-01-01 00:00:00 UTC
        assert_eq(hooks.cron_next("*/15 * * * *", b) - b, 900, "*/15")
        assert_eq(hooks.cron_next("0 0 * * *", b) - b, 86400, "midnight")
        assert_eq(hooks.cron_next("30 2 * * *", b) - b, 9000, "02:30")
    end)
    test("a fixed-offset tz moves the hour", function()
        local nx = hooks.cron_next("0 9 * * *", 1735689600, 2 * 3600)
        assert_eq(math.floor((nx % 86400) / 3600), 7, "09:00+02:00 in UTC")
    end)
    test("a tz offset skips on local day / hour boundaries", function()
        local b = 1735689600   -- 2025-01-01T00:00Z
        assert_eq(hooks.cron_next("0 9 * * *", b, 19800) - b, 12600, "+05:30")
        assert_eq(hooks.cron_next("0 0 * * 1", b, 7200) - b, 424800, "Mon +02:00")
        local chain, t = {}, b
        for _ = 1, 5 do
            t = hooks.cron_next("0 9 * * 1-5", t, -18000)
            chain[#chain + 1] = t - b
        end
        assert_eq(table.concat(chain, ","), "50400,136800,223200,482400,568800", "1-5 -05:00")
        assert_eq(hooks.cron_next("0 1 * * 0", b, -10800) - b, 360000, "Sun -03:00")
    end)
    test("cron fields are plain decimal digits", function()
        local b = 1735689600
        for _, s in ipairs({ "5abc * * * *", "0x5 * * * *", "5.0 * * * *", "1e1 * * * *",
                             "1,,2 * * * *", ",1 * * * *", "*/5/2 * * * *", "1-2-3 * * * *" }) do
            assert_eq(hooks.cron_next(s, b), nil, s)
        end
        assert_eq(hooks.cron_next("5,10-12 */2 * * 1-5", b) ~= nil, true, "a valid spec")
    end)
    test("a bad tz is refused", function()
        for _, tz in ipairs({ 0/0, 1/0, 2000, 1.5, "+25:00", "+05:75", "+18:59", "-1801" }) do
            assert_eq(pcall(jobs.cron, "tzx", "0 9 * * *", nil, { tz = tz }), false, tostring(tz))
        end
    end)
    test("a due schedule fires once; a second tick does not double-fire", function()
        jobs.cron("beep", "* * * * *")
        hooks.tick(time.now() + 120)
        local c1 = jobs.claim({ batch = 10 })
        assert_eq(#c1, 1, "fired")
        assert_eq(c1[1].type, "beep", "type")
        hooks.tick(time.now() + 120)
        assert_eq(#jobs.claim({ batch = 10 }), 0, "no double fire")
        assert_eq(jobs.uncron("beep"), true, "uncron")
    end)
end

-- durable events Phase 2: capture, at-least-once, lease reclaim, retention.
function cases.events()
    jobs.init({ events = true })
    local future = time.now() + 100   -- past every event ts
    jobs.enqueue("ok", {}); jobs.enqueue("ok", {}); jobs.enqueue("ok", {})
    test("a drain delivers in id order, once", function()
        local seen = {}
        jobs.subscribe("cap", function(ev) seen[#seen + 1] = ev.id end, { from = "beginning" })
        assert_eq(hooks.events_drain("cap", { now = 1000 }).delivered, 3, "delivered")
        assert_eq(hooks.events_drain("cap", { now = 1000 }).delivered, 0, "recaught")
        assert_eq(table.concat(seen, ","), "1,2,3", "order")
    end)
    test("a crash before the cursor write re-delivers", function()
        local dc = 0
        jobs.subscribe("dup", function() dc = dc + 1 end, { from = "beginning" })
        hooks.events_drain("dup", { now = 1000, commit_cursor = false })
        hooks.events_drain("dup", { now = 1000 })
        assert_eq(dc, 6, "delivered twice")
    end)
    test("a dead drainer's lease is reclaimed once it expires", function()
        jobs.subscribe("lease", function() end, { from = "beginning" })
        hooks.events_drain("lease", { now = 1000, commit_cursor = false, release_lease = false })
        assert_eq(hooks.events_drain("lease", { now = 1000 }).leased, false, "held")
        assert_eq(hooks.events_drain("lease", { now = 1040 }).delivered, 3, "reclaimed")
    end)
    test("retention never passes the slowest cursor", function()
        jobs.subscribe("lag", function() end, { from = "beginning" })
        local before = #jobs.events({ limit = 100 })
        jobs.cleanup({ before = future })
        assert_eq(#jobs.events({ limit = 100 }), before, "retained")
        hooks.events_drain("lag", { now = 1000 })
        jobs.cleanup({ before = future })
        assert_eq(#jobs.events({ limit = 100 }), 0, "purged")
    end)
end

-- durable events Phase 3: poison-event skip-after-N + metrics.
function cases.poison()
    jobs.init({ events = true })
    jobs.enqueue("ok", {}); jobs.enqueue("ok", {}); jobs.enqueue("ok", {})
    test("a poison event is skipped after max_failures", function()
        local seen = {}
        jobs.subscribe("p", function(ev)
            if ev.id == 2 then error("poison") end
            seen[#seen + 1] = ev.id
        end, { from = "beginning", max_failures = 2 })
        local d1 = hooks.events_drain("p", { now = 1000 })
        local d2 = hooks.events_drain("p", { now = 1000 })
        hooks.events_drain("p", { now = 1000 })
        assert_eq(d1.delivered, 1, "d1")
        assert_eq(d2.skipped, true, "d2 skipped")
        assert_eq(table.concat(seen, ","), "1,3,4", "seen")
        assert_eq(#jobs.events({ types = { "subscription_skipped" } }), 1, "skip event")
        local m = jobs.metrics()
        local sub = m.events and m.events.subscriptions[1]
        assert_eq(sub and sub.lag, 0, "lag")
        assert_eq(sub and sub.failures, 0, "failures")
        assert_eq(m.events and m.events.log_depth, 4, "log depth")
    end)
end

-- audit 9 HORIZON: a drain holds back events younger than the grace.
function cases.horizon()
    jobs.init({ events = true })
    test("events younger than the grace are held back", function()
        jobs.subscribe("h", function() end, { from = "now" })
        jobs.enqueue("e1", {}); jobs.enqueue("e2", {}); jobs.enqueue("e3", {})
        local now = time.now()
        assert_eq(hooks.events_drain("h", { now = now, grace = 5 }).delivered, 0, "young")
        assert_eq(hooks.events_drain("h", { now = now + 60, grace = 5 }).delivered, 3, "old")
    end)
    test("events_grace sets the default", function()
        jobs.init({ events_grace = 5 })
        jobs.subscribe("h2", function() end, { from = "now" })
        jobs.enqueue("e4", {})
        assert_eq(hooks.events_drain("h2", { now = time.now() }).delivered, 0, "cfg young")
    end)
end

-- Audit 20: enqueue wrote its row before a bad dependency edge failed; the
-- JS twin's metrics / cron split are fixed to match this one.
function cases.audit20()
    jobs.init()
    test("depends_on with a duplicate or a non-id is refused, no row written", function()
        local a = jobs.enqueue("dep", {})
        local before = jobs.metrics().totals.blocked
        for _, bad in ipairs({ { a, a }, { "x" }, { a, 1.5 }, { -1 } }) do
            local ok, err = pcall(jobs.enqueue, "child", {}, { depends_on = bad })
            assert_eq(ok, false, "refused")
            assert_eq(tostring(err):find("distinct job ids", 1, true) ~= nil, true,
                      tostring(err))
        end
        assert_eq(jobs.metrics().totals.blocked, before, "no blocked row left")
        local id = jobs.enqueue("child", {}, { depends_on = { a } })
        assert_eq(math.type(id), "integer", "a valid depends_on still enqueues")
        assert_eq(jobs.metrics().totals.blocked, before + 1, "one blocked row")
    end)
    test("metrics with a queue named constructor", function()
        jobs.enqueue("t", {}, { queue = "constructor" })
        assert_eq(jobs.metrics().queues["constructor"].pending, 1, "bucket")
    end)
    test("a21: a non-list or holed depends_on, or an id given as a string, is refused", function()
        local a = jobs.enqueue("dep", {})
        local before = jobs.metrics().totals.pending
        for _, bad in ipairs({ a, tostring(a), { a, nil, a + 1 }, { [2] = a },
                               { tostring(a) } }) do
            local ok, err = pcall(jobs.enqueue, "child", {}, { depends_on = bad })
            assert_eq(ok, false, "refused")
            local msg = tostring(err)
            assert_eq(msg:find("list of job ids", 1, true) ~= nil
                      or msg:find("distinct job ids", 1, true) ~= nil, true, msg)
        end
        assert_eq(jobs.metrics().totals.pending, before, "no pending row left")
    end)
end

-- Audit 22: only the handler ran under pcall. A raise after it returned - a
-- result json.encode refuses, a park a NaN made unwritable - escaped jobs.work
-- (the worker loop died, the job stayed 'running'); JS retries it.
function cases.audit22()
    jobs.init()
    test("a result json.encode refuses is retried, not raised", function()
        jobs.handler("nanres", function() return { ratio = 0/0 } end)
        local id = jobs.enqueue("nanres", {})
        local ok, err = pcall(jobs.work, { batch = 1 })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.status ~= "running", true, "left " .. tostring(j.status))
        assert_eq(type(j.last_error) == "string" and #j.last_error > 0, true,
                  "the error is recorded")
    end)
    test("a NaN ctx.sleep sleeps for no time", function()
        jobs.workflow("nansleep", function(w) w.sleep(0/0); return { ok = true } end)
        local id = jobs.start("nansleep", {})
        local ok, err = pcall(jobs.work, { batch = 1 })
        assert_eq(ok, true, tostring(err))
        assert_eq(jobs.workflow_status(id).status, "done", "finished")
    end)
end

-- Audit 23: run_at is INTEGER, and a fractional second went to Postgres as
-- a double it refuses (SQLite stores it as REAL, which shows here as a float
-- run_at); the failure path ran app code - the backoff, an error's
-- __tostring - unprotected, so a raise there escaped jobs.work.
function cases.audit23()
    jobs.init()
    test("a fractional ctx.sleep parks on a whole second", function()
        jobs.workflow("halfsleep", function(w) w.sleep(0.5); return { ok = true } end)
        local id = jobs.start("halfsleep", {})
        local ok, err = pcall(jobs.work, { batch = 1 })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.status, "pending", "parked")
        assert_eq(math.type(j.run_at), "integer", "run_at " .. tostring(j.run_at))
    end)
    test("a fractional wait_signal timeout parks on a whole second", function()
        jobs.workflow("halfwait", function(w)
            w.wait_signal("go", { timeout = 1.5 }); return { ok = true } end)
        local id = jobs.start("halfwait", {})
        local ok, err = pcall(jobs.work, { batch = 1 })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.status, "waiting", "parked")
        assert_eq(math.type(j.run_at), "integer", "run_at " .. tostring(j.run_at))
    end)
    test("a fractional backoff retries on a whole second", function()
        jobs.init({ backoff = function() return 2.5 end })
        jobs.handler("fails1", function() error("boom") end)
        local id = jobs.enqueue("fails1", {})
        local ok, err = pcall(jobs.work, { batch = 1 })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.status, "pending", "retried")
        assert_eq(math.type(j.run_at), "integer", "run_at " .. tostring(j.run_at))
    end)
    test("a raising or NaN backoff falls back to the default", function()
        for i, bad in ipairs({ function() error("bad backoff") end,
                               function() return 0/0 end,
                               function() return "soon" end }) do
            jobs.init({ backoff = bad })
            jobs.handler("fails_b" .. i, function() error("boom") end)
            local id = jobs.enqueue("fails_b" .. i, {})
            local ok, err = pcall(jobs.work, { batch = 1 })
            assert_eq(ok, true, "backoff " .. i .. ": " .. tostring(err))
            local j = jobs.get(id)
            assert_eq(j.status, "pending", "retried " .. i)
            assert_eq(math.type(j.run_at), "integer", "run_at " .. i)
        end
        jobs.init({ backoff = function() return 1 end })
    end)
    test("init refuses a backoff that is not a function", function()
        assert_eq(pcall(jobs.init, { backoff = 5 }), false, "refused")
    end)
    test("an error whose __tostring raises is still recorded", function()
        local inner = setmetatable({}, { __tostring = function() error("again") end })
        local bad = setmetatable({}, { __tostring = function() error(inner) end })
        jobs.handler("badtostr", function() error(bad) end)
        local id = jobs.enqueue("badtostr", {})
        local ok = pcall(jobs.work, { batch = 1 })
        assert_eq(ok, true, "jobs.work raised")
        local j = jobs.get(id)
        assert_eq(j.status, "pending", "retried")
        assert_eq(type(j.last_error), "string", "recorded")
    end)
end

-- The harness sets the case as a global before it runs the script.
local case = rawget(_G, "__jobs_case")
test("the case is known", function()
    assert_eq(type(cases[case]), "function", tostring(case))
end)
if cases[case] then cases[case]() end

return { pass = pass, fail = fail }
