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
    test("a result json.encode refuses is recorded, not raised", function()
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
        -- Its own queue: parked, it falls due at the next whole second, and a
        -- later batch-1 claim on "default" in that second took it (lower id)
        -- instead of the job that case was about.
        local before = time.now()
        local id = jobs.start("halfsleep", {}, { queue = "halfsleep" })
        local ok, err = pcall(jobs.work, { batch = 1, queue = "halfsleep" })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.status, "pending", "parked")
        assert_eq(math.type(j.run_at), "integer", "run_at " .. tostring(j.run_at))
        assert_eq(j.run_at >= before + 1, true, "ran and parked: run_at " .. tostring(j.run_at))
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

-- audit 30: jobs.cron at every start must not drop a tick that came due
-- while no worker ran; a changed spec / tz or a parked schedule recomputes.
function cases.audit30()
    jobs.init()
    local SPEC = "0 0 * * *"
    test("re-registering keeps a due tick, which fires exactly once", function()
        jobs.cron("nightly", SPEC)
        local due = time.now() - 30
        hooks.cron_set_next_run_at("nightly", due)
        jobs.cron("nightly", SPEC, { v = 2 }, { queue = "q30" })
        assert_eq(hooks.cron_next_run_at("nightly"), due, "kept")
        hooks.tick(time.now())
        local c = jobs.claim({ batch = 10, queue = "q30" })
        assert_eq(#c, 1, "fired once")
        assert_eq(c[1].type, "nightly", "type")
        assert_eq(c[1].data.v, 2, "the new payload")
        hooks.tick(time.now())
        assert_eq(#jobs.claim({ batch = 10, queue = "q30" }), 0, "no second fire")
        assert_eq(hooks.cron_next_run_at("nightly") > time.now(), true, "advanced")
    end)
    test("a changed spec recomputes next_run_at", function()
        local due = time.now() - 30
        hooks.cron_set_next_run_at("nightly", due)
        jobs.cron("nightly", "30 1 * * *")
        local nx = hooks.cron_next_run_at("nightly")
        assert_eq(nx > time.now(), true, "recomputed")
        assert_eq(nx, hooks.cron_next("30 1 * * *", time.now()), "the new spec")
    end)
    test("a changed tz recomputes next_run_at", function()
        hooks.cron_set_next_run_at("nightly", time.now() - 30)
        jobs.cron("nightly", "30 1 * * *", nil, { tz = "+02:00" })
        assert_eq(hooks.cron_next_run_at("nightly") > time.now(), true, "recomputed")
    end)
    test("re-registering a parked schedule re-enables it", function()
        hooks.cron_set_next_run_at("nightly", hooks.CRON_DISABLED_AT)
        jobs.cron("nightly", "30 1 * * *", nil, { tz = "+02:00" })
        local nx = hooks.cron_next_run_at("nightly")
        assert_eq(nx < hooks.CRON_DISABLED_AT and nx > time.now(), true, "re-enabled")
    end)
    test("a fresh schedule gets its next occurrence", function()
        jobs.cron("fresh", SPEC)
        assert_eq(hooks.cron_next_run_at("fresh"), hooks.cron_next(SPEC, time.now()), "computed")
    end)
end

-- Audit 38: a handler's or subscriber's error text went to the database as
-- it was, and Postgres refuses a NUL or bytes that are not UTF-8 (MySQL also
-- more than 64 KiB in TEXT): the write raised out of jobs.work / the drain,
-- leaving the batch 'running' and the worker dead. SQLite stores such text,
-- so here the checks are on what is stored: storable text, the worker alive,
-- the batch-mates done, the poison skipped (tests/e2e_postgres.sh and
-- e2e_mysql.sh run the same against the servers that refused it).
function cases.audit38()
    jobs.init({ events = true, history = true, backoff = function() return 0 end })
    local function storable(s)
        return type(s) == "string" and not s:find("\0", 1, true)
            and utf8.len(s) ~= nil and #s <= 4096 + 3
    end
    test("a handler error the database would refuse is stored as safe text", function()
        jobs.handler("poison", function()
            error("upstream: a\0b caf\xc3 " .. string.rep("y", 100000), 0)
        end)
        jobs.handler("fine", function() return true end)
        local p = jobs.enqueue("poison", {})
        local a, b = jobs.enqueue("fine", {}), jobs.enqueue("fine", {})
        local ok, err = pcall(jobs.work, { batch = 10 })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(p)
        assert_eq(j.status, "pending", "re-pended")
        assert_eq(storable(j.last_error), true, "storable last_error")
        assert_eq(j.last_error:sub(1, 13), "upstream: ab ", "NUL dropped")
        assert_eq(j.last_error:find("caf\239\191\189", 1, true) ~= nil, true, "U+FFFD")
        assert_eq(j.last_error:sub(-3), "...", "capped")
        assert_eq(jobs.get(a).status, "done", "batch-mate a")
        assert_eq(jobs.get(b).status, "done", "batch-mate b")
        local h = jobs.history(p)
        assert_eq(#h, 1, "attempt recorded")
        assert_eq(storable(h[1].error), true, "storable attempt error")
    end)
    test("an error whose __tostring raises is recorded as a fixed text", function()
        jobs.handler("weird", function()
            error(setmetatable({}, { __tostring = function() error("no") end }))
        end)
        local id = jobs.enqueue("weird", {}, { queue = "w38" })
        local ok, err = pcall(jobs.work, { batch = 1, queue = "w38" })
        assert_eq(ok, true, tostring(err))
        assert_eq(jobs.get(id).last_error,
            "(an error value that could not be converted to text)", "fixed text")
    end)
    -- Audit 39: round 38 dead-lettered a result whose JSON is not UTF-8 on
    -- every backend; it is stored byte-exact now and reads back equal.
    local BIN = "\255\254\0abc caf\xc3"
    test("a result holding bytes that are not UTF-8 completes and reads back", function()
        jobs.handler("binres", function() return { digest = BIN } end)
        local id = jobs.enqueue("binres", {}, { queue = "bin39" })
        local ok, err = pcall(jobs.work, { batch = 1, queue = "bin39" })
        assert_eq(ok, true, tostring(err))
        local r = jobs.result(id)
        assert_eq(r.status, "done", "done: " .. tostring(jobs.get(id).last_error))
        assert_eq(r.result and r.result.digest, BIN, "byte-exact result")
    end)
    test("a dependent receives a binary result byte-exact", function()
        jobs.handler("binsrc", function() return BIN end)
        local seen
        jobs.handler("bindst", function(job) seen = job.deps and job.deps[1] end)
        local a = jobs.enqueue("binsrc", {}, { queue = "bind39" })
        jobs.enqueue("bindst", {}, { queue = "bind39", depends_on = { a } })
        assert_eq(pcall(jobs.work, { batch = 5, queue = "bind39" }), true, "work")
        assert_eq(pcall(jobs.work, { batch = 5, queue = "bind39" }), true, "work")
        assert_eq(seen, BIN, "dependency result")
    end)
    test("a ctx.step result holding bytes that are not UTF-8 replays", function()
        local runs = 0
        jobs.workflow("binstep", function(w)
            local v = w.step("b", function() return { d = BIN } end)
            runs = runs + 1
            if runs == 1 then error("again", 0) end
            return { same = (v.d == BIN) }
        end)
        local id = jobs.start("binstep", {}, { queue = "bs39" })
        assert_eq(pcall(jobs.work, { batch = 1, queue = "bs39" }), true, "first run")
        assert_eq(pcall(jobs.work, { batch = 1, queue = "bs39" }), true, "replay")
        local r = jobs.result(id)
        assert_eq(r.status, "done", "done: " .. tostring(jobs.get(id).last_error))
        assert_eq(runs, 2, "re-ran")
        assert_eq(r.result.same, true, "the stored step value replayed byte-exact")
    end)
    test("a result that cannot be stored dead-letters the job once", function()
        jobs.handler("badres", function() return { f = function() end } end)
        local id = jobs.enqueue("badres", {}, { queue = "r38" })
        local ok, err = pcall(jobs.work, { batch = 1, queue = "r38" })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.status, "dead", "dead")
        assert_eq(j.attempts, 1, "ran once")
        assert_eq(j.last_error:find("job result cannot be stored", 1, true) ~= nil, true,
                  tostring(j.last_error))
    end)
    test("a runtime busy refusal is handed back without spending an attempt", function()
        jobs.handler("busy", function() error("[hull:busy] fetch cap full", 0) end)
        local id = jobs.enqueue("busy", {}, { queue = "b38", max_attempts = 1 })
        local ok, err = pcall(jobs.work, { batch = 1, queue = "b38" })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.status, "pending", "pending")
        assert_eq(j.attempts, 0, "uncounted")
        assert_eq(j.run_at > time.now() - 1, true, "deferred")
    end)
    test("a poison subscription event with such text is skipped", function()
        local seen = {}
        jobs.subscribe("p38", function(ev)
            if ev.type == "dead" then error("forward failed: caf\xc3\0", 0) end
            seen[#seen + 1] = ev.id
        end, { from = "beginning", max_failures = 1 })
        jobs.subscribe("q38", function(ev)
            if ev.type == "dead" then
                error(setmetatable({}, { __tostring = function() error("no") end }))
            end
        end, { from = "beginning", max_failures = 1 })
        local d = hooks.events_drain("p38", { now = time.now() + 100 })
        assert_eq(d.skipped, true, "p38 skipped")
        local d2 = hooks.events_drain("q38", { now = time.now() + 100 })
        assert_eq(d2.skipped, true, "q38 skipped")
        local sk = jobs.events({ types = { "subscription_skipped" } })
        assert_eq(#sk, 2, "two skip events")
        for _, e in ipairs(sk) do
            assert_eq(storable(e.data and e.data.error), true, "storable skip text")
        end
        -- The cursor moved past the poison: the next drain goes on from it.
        local d3 = hooks.events_drain("p38", { now = time.now() + 100 })
        assert_eq(d3.leased, true, "lease released")
    end)
    test("options that wedged a worker are refused up front", function()
        assert_eq(pcall(jobs.cron, "c38", "* * * * *", nil, { type = 5 }), false, "cron type")
        jobs.cron("c38", "* * * * *", nil, { type = "" })   -- falls back to the name
        assert_eq(pcall(jobs.limit, "q", { rate = 1.5 }), false, "fractional rate")
        assert_eq(pcall(jobs.limit, "q", { rate = 0/0 }), false, "NaN rate")
        assert_eq(pcall(jobs.limit, "q", { rate = 2, per = 0 }), false, "per 0")
        assert_eq(pcall(jobs.limit, "q", { rate = 2, per = "x" }), false, "per string")
        assert_eq(pcall(jobs.limit, "q", { rate = 2, per = 0.5 }), true, "per 0.5")
        jobs.limit("q", nil)
        assert_eq(pcall(jobs.metrics, { window = 0 }), false, "window 0")
        assert_eq(pcall(jobs.metrics, { window = "300" }), false, "window string")
        assert_eq(pcall(jobs.metrics, { window = 60 }), true, "window 60")
    end)
    test("ctx.patched refuses an id too long for its step key", function()
        jobs.workflow("longpatch", function(w) w.patched(string.rep("p", 300)); return true end)
        local id = jobs.start("longpatch", {}, { queue = "l38" })
        local ok, err = pcall(jobs.work, { batch = 1, queue = "l38" })
        assert_eq(ok, true, tostring(err))
        local j = jobs.get(id)
        assert_eq(j.last_error:find("too long", 1, true) ~= nil, true, tostring(j.last_error))
    end)
end

-- Audit 39: a busy refusal is never compensated (the round-38 hand-back
-- re-ran a workflow whose last attempt had compensated, and it replayed the
-- undone steps' memos to 'done'), a compensated step is never replayed as
-- done, and busy hand-backs are bounded (an app can forge the code).
function cases.audit39()
    jobs.init({ backoff = function() return 0 end })
    local charged, refunded, ship = 0, 0, 0
    jobs.workflow("order39", function(c)
        c.step("charge", function() charged = charged + 1; return "ch_1" end,
               { compensate = function() refunded = refunded + 1 end })
        c.step("ship", function()
            ship = ship + 1
            if ship == 1 then error("[hull:busy] http.fetch: 64 requests in flight", 0) end
            return "shipped"
        end)
        return "ok"
    end)
    test("a busy step on the last attempt is handed back, not compensated", function()
        local id = jobs.start("order39", {}, { queue = "o39", max_attempts = 1 })
        assert_eq(pcall(jobs.work, { batch = 1, queue = "o39" }), true, "work 1")
        local j = jobs.get(id)
        assert_eq(j.status, "pending", "handed back")
        assert_eq(j.attempts, 0, "uncounted")
        assert_eq(refunded, 0, "not compensated")
        hooks.make_due(id)
        assert_eq(pcall(jobs.work, { batch = 1, queue = "o39" }), true, "work 2")
        assert_eq(jobs.get(id).status, "done", "done")
        assert_eq(charged, 1, "charged once")
        assert_eq(refunded, 0, "still not compensated")
        assert_eq(ship, 2, "ship re-ran")
    end)
    test("busy hand-backs are bounded: past the streak cap the attempt counts", function()
        jobs.handler("busy39", function() error("[hull:busy] forged", 0) end)
        local id = jobs.enqueue("busy39", {}, { queue = "b39", max_attempts = 1 })
        hooks.busy_streak(id, 20)
        assert_eq(pcall(jobs.work, { batch = 1, queue = "b39" }), true, "work")
        local j = jobs.get(id)
        assert_eq(j.status, "dead", "counted")
        assert_eq(j.attempts, 1, "one attempt")
        assert_eq(hooks.busy_streak(id), nil, "streak cleared")
    end)
    test("a workflow past the cap compensates and carries no refusal code", function()
        local r = 0
        jobs.workflow("cap39", function(c)
            c.step("a", function() return 1 end, { compensate = function() r = r + 1 end })
            c.step("b", function() error("[hull:busy] forged", 0) end)
        end)
        local id = jobs.start("cap39", {}, { queue = "c39", max_attempts = 1 })
        hooks.busy_streak(id, 20)
        assert_eq(pcall(jobs.work, { batch = 1, queue = "c39" }), true, "work")
        local j = jobs.get(id)
        assert_eq(j.status, "dead", "dead")
        assert_eq(r, 1, "compensated")
        assert_eq(j.last_error:sub(1, 17), "workflow failed: ", tostring(j.last_error))
    end)
    test("a compensated step is not replayed as done", function()
        local r = 0
        local calls = 0
        jobs.workflow("undone39", function(c)
            c.step("a", function() return 1 end, { compensate = function() r = r + 1 end })
            c.step("b", function()
                calls = calls + 1
                if calls == 1 then error("transient", 0) end
                return 2
            end)
            return "ok"
        end)
        local id = jobs.start("undone39", {}, { queue = "u39", max_attempts = 3 })
        assert_eq(pcall(jobs.work, { batch = 1, queue = "u39" }), true, "work 1")
        assert_eq(jobs.get(id).status, "pending", "retrying")
        hooks.mark_step_compensated(id, "a")
        hooks.make_due(id)
        assert_eq(pcall(jobs.work, { batch = 1, queue = "u39" }), true, "work 2")
        local j = jobs.get(id)
        assert_eq(j.status, "dead", "dead, not done")
        assert_eq(j.last_error:find("was compensated", 1, true) ~= nil, true,
                  tostring(j.last_error))
        assert_eq(calls, 1, "b not run past the undone step")
        assert_eq(r, 0, "the compensated step is not compensated again")
    end)
end

-- Audit 41: a ctx.step whose result cannot be stored (a cyclic table here;
-- on MySQL a result over 64 KiB, covered by tests/e2e_mysql.sh) ran its side
-- effect again on every retry, until max_attempts.
function cases.audit41()
    jobs.init({ backoff = function() return 0 end })
    local function cyclic()
        local r = { ok = true }
        r.self = r
        return r
    end
    test("an unstorable step result runs its side effect once and dead-letters", function()
        local charged, refunded = 0, 0
        jobs.workflow("cyc41", function(c)
            c.step("charge", function() charged = charged + 1; return cyclic() end,
                   { compensate = function() refunded = refunded + 1 end })
            return "done"
        end)
        local id = jobs.start("cyc41", {}, { queue = "cy41", max_attempts = 5 })
        for _ = 1, 6 do
            assert_eq(pcall(jobs.work, { batch = 1, queue = "cy41" }), true, "work")
            hooks.make_due(id)
        end
        local j = jobs.get(id)
        assert_eq(j.status, "dead", "dead")
        assert_eq(j.attempts, 1, "dead-lettered on the first attempt")
        assert_eq(charged, 1, "the side effect ran once")
        assert_eq(refunded, 1, "its own compensation ran")
        assert_eq(j.last_error:find("step 'charge' ran but its result cannot be stored", 1, true)
                  ~= nil, true, tostring(j.last_error))
    end)
    test("a body that swallows the error still never re-runs the step", function()
        local runs = 0
        jobs.workflow("swallow41", function(c)
            pcall(c.step, "s", function() runs = runs + 1; return cyclic() end)
            error("body failed", 0)
        end)
        local id = jobs.start("swallow41", {}, { queue = "sw41", max_attempts = 3 })
        for _ = 1, 4 do
            assert_eq(pcall(jobs.work, { batch = 1, queue = "sw41" }), true, "work")
            hooks.make_due(id)
        end
        assert_eq(jobs.get(id).status, "dead", "dead after its attempts")
        assert_eq(runs, 1, "the step ran once across retries")
    end)
    test("a storable step still memoizes and replays", function()
        local runs, attempts = 0, 0
        local seen
        jobs.workflow("memo41", function(c)
            local v = c.step("s", function() runs = runs + 1; return { n = 42 } end)
            attempts = attempts + 1
            if attempts == 1 then error("again", 0) end
            seen = v.n
            return "ok"
        end)
        local id = jobs.start("memo41", {}, { queue = "m41", max_attempts = 3 })
        assert_eq(pcall(jobs.work, { batch = 1, queue = "m41" }), true, "work 1")
        hooks.make_due(id)
        assert_eq(pcall(jobs.work, { batch = 1, queue = "m41" }), true, "work 2")
        assert_eq(jobs.get(id).status, "done", "done")
        assert_eq(runs, 1, "ran once")
        assert_eq(seen, 42, "replayed")
    end)
    test("an operator's jobs.retry runs the unstored step again", function()
        local runs = 0
        local bad = true
        jobs.workflow("retry41", function(c)
            c.step("s", function()
                runs = runs + 1
                if bad then return cyclic() end
                return 1
            end)
            return "ok"
        end)
        local id = jobs.start("retry41", {}, { queue = "r41", max_attempts = 3 })
        assert_eq(pcall(jobs.work, { batch = 1, queue = "r41" }), true, "work 1")
        assert_eq(jobs.get(id).status, "dead", "dead")
        bad = false
        assert_eq(jobs.retry(id), true, "requeued")
        assert_eq(pcall(jobs.work, { batch = 1, queue = "r41" }), true, "work 2")
        assert_eq(jobs.get(id).status, "done", "done")
        assert_eq(runs, 2, "ran again after the retry")
    end)
end

-- The harness sets the case as a global before it runs the script.
local case = rawget(_G, "__jobs_case")
test("the case is known", function()
    assert_eq(type(cases[case]), "function", tostring(case))
end)
if cases[case] then cases[case]() end

return { pass = pass, fail = fail }
