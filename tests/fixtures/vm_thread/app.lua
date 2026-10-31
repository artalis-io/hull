-- --vm-thread fixture (tests/e2e_vm_thread.sh). hull._test_native_sleep is a
-- native call that ignores the run's stop flag (cap/vm_thread.h); it exists
-- only under HULL_TEST_NATIVE_SLEEP=1.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

app.manifest({
    modules = { "hull/http-server@1", "hull/timers@1" },
})

local timer_runs, timer_stuck = 0, false

app.use("*", "/mw/*", function(req, _res)
    if req.query.ms then hull._test_native_sleep(tonumber(req.query.ms)) end
    return 0
end)

app.get("/ping", function(_req, res) res:json({ ok = true }) end)

-- A handler stuck in a native call for ?ms.
app.get("/stuck", function(req, res)
    hull._test_native_sleep(tonumber(req.query.ms) or 4000)
    res:json({ done = true })
end)

-- Stuck in a middleware instead.
app.get("/mw/stuck", function(_req, res) res:json({ handler = true }) end)

-- A handler that parks (hull.sleep) and resumes: still works on the thread.
app.get("/park", function(_req, res)
    hull.sleep(20)
    res:json({ parked = true })
end)

-- A timer that gets stuck once, on request.
app.get("/timer/stick", function(_req, res)
    timer_stuck = true
    res:json({ armed = true })
end)
app.get("/timer/runs", function(_req, res) res:json({ runs = timer_runs }) end)
app.every(100, function()
    timer_runs = timer_runs + 1
    if timer_stuck then
        timer_stuck = false
        hull._test_native_sleep(3000)
    end
end)
