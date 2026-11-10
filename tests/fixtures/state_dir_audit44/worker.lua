-- Audit 44 M2: worker.dispatch's `db` is the manifest's default (the state
-- database), not the implicit <app>/data.db the re-point removed.
local hdb = require("hull.db")
local worker = require("hull.worker")
app.manifest({
    name = "state-a44-worker",
    state = { name = "hull-e2e-a44w" },
    modules = { "hull/db@1", "hull/worker@1" },
    databases = { named = { default = "@state/data.db" } },
})
app.main(function()
    local ok, r = pcall(worker.dispatch, function()
        local db = _G.db
        db.exec("INSERT INTO notes (body) VALUES ('from-worker')")
        return { n = db.query("SELECT COUNT(*) AS n FROM notes")[1].n }
    end, {})
    print("worker_ok=" .. tostring(ok))
    if not ok then print("worker_err=" .. tostring(r)) end
    print("main_notes=" .. tostring(hdb.default().query("SELECT COUNT(*) AS n FROM notes")[1].n))
    return 0
end)
