-- Audit 44 M1: the manifest's default under "@state/", a stdlib init() at the
-- app's top level (it opens the internal database before the manifest is
-- read) and a db.default() object captured at top level. After the default
-- is re-pointed, the captured object must drive the state database and the
-- stdlib's _hull_* tables must be in <state>/.hull/internal.db. audit-log is
-- an HTTP-free module on the internal database, so this runs on every flavor
-- with a database.
local db = require("hull.db")
local audit_log = require("hull.web.middleware.audit-log")
app.manifest({
    name = "state-a44-top",
    state = { name = "hull-e2e-a44" },
    modules = { "hull/db@1", "hull/web/middleware/audit-log@1" },
    databases = { named = { default = "@state/data.db" } },
})
audit_log.init({ fingerprint_salt = "e2e-audit44-salt", cleanup = false })
local top = db.default()
app.main(function()
    audit_log.record("u1", "login", nil,
                     { ip = "127.0.0.1", user_agent = "e2e", fingerprint = "fp" })
    print("events=" .. tostring(#audit_log.list("u1")))
    top.exec("INSERT INTO notes (body) VALUES (?)", { "captured" })
    print("notes=" .. tostring(db.default().query("SELECT COUNT(*) AS n FROM notes")[1].n))
    return 0
end)
