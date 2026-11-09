-- Audit 44 M3: `hull migrate` moves the stdlib's _hull_* tables of a state
-- default into <state>/.hull/internal.db - the file the runner reads.
-- (audit-log: an HTTP-free module whose table is on the internal database.)
local audit_log = require("hull.web.middleware.audit-log")
app.manifest({
    name = "state-a44-migr",
    state = { name = "hull-e2e-a44m" },
    modules = { "hull/db@1", "hull/web/middleware/audit-log@1" },
    databases = { named = { default = "@state/data.db" } },
})
app.main(function(ctx)
    audit_log.init({ fingerprint_salt = "e2e-audit44-salt", cleanup = false })
    if ctx.args[1] == "seed" then
        audit_log.record("u1", "login", nil,
                         { ip = "127.0.0.1", user_agent = "e2e", fingerprint = "fp" })
    end
    print("events=" .. tostring(#audit_log.list("u1")))
    return 0
end)
