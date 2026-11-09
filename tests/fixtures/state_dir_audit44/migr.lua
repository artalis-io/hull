-- Audit 44 M3: `hull migrate` moves the stdlib's _hull_* tables of a state
-- default into <state>/.hull/internal.db - the file the runner reads.
local rbac = require("hull.web.middleware.rbac")
app.manifest({
    name = "state-a44-migr",
    state = { name = "hull-e2e-a44m" },
    modules = { "hull/db@1", "hull/web/middleware/rbac@1" },
    databases = { named = { default = "@state/data.db" } },
})
app.main(function(ctx)
    rbac.init()
    if ctx.args[1] == "seed" then
        rbac.define_role("admin", {})
        rbac.assign("u1", "admin")
    end
    print("roles=" .. tostring(#rbac.roles("u1")))
    return 3   -- the middleware modules admit hull/http-server: exit before the serve loop
end)
