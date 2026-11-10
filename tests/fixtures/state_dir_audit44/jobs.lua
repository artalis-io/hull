-- Audit 44: a top-level jobs.init() / jobs.enqueue() made _hull_jobs (and a
-- row) in the implicit default before the manifest's "@state/" default
-- replaced it; the tables are carried into the new default.
local jobs = require("hull.jobs")
app.manifest({
    name = "state-a44-jobs",
    state = { name = "hull-e2e-a44j" },
    modules = { "hull/db@1", "hull/jobs@1" },
    databases = { named = { default = "@state/data.db" } },
})
jobs.init()
jobs.enqueue("top", { n = 1 })
app.main(function()
    jobs.enqueue("main", { n = 2 })
    print("enqueued=2")
    return 0
end)
