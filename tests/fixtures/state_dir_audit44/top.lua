-- Audit 44 M1: the manifest's default under "@state/", a stdlib init() at the
-- app's top level (it opens the internal database before the manifest is
-- read) and a db.default() object captured at top level. After the default
-- is re-pointed, the captured object must drive the state database and the
-- stdlib's _hull_* tables must be in <state>/.hull/internal.db.
local db = require("hull.db")
local rbac = require("hull.web.middleware.rbac")
local session = require("hull.web.middleware.session")
app.manifest({
    name = "state-a44-top",
    state = { name = "hull-e2e-a44" },
    modules = { "hull/db@1", "hull/web/middleware/rbac@1", "hull/web/middleware/session@1" },
    databases = { named = { default = "@state/data.db" } },
})
rbac.init()
session.init({})
local top = db.default()
app.main(function()
    rbac.define_role("admin", {})
    rbac.assign("u1", "admin")
    print("roles=" .. tostring(#rbac.roles("u1")))
    top.exec("INSERT INTO notes (body) VALUES (?)", { "captured" })
    print("notes=" .. tostring(db.default().query("SELECT COUNT(*) AS n FROM notes")[1].n))
    return 3   -- the middleware modules admit hull/http-server: exit before the serve loop
end)
