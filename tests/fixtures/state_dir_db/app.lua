-- The app's default database under the state directory (gap 3, PR 2):
-- databases.named.default names it, no -d is given, and the migrations run
-- on it. `cache` is a second named connection there.
local db = require("hull.db")
app.manifest({
    name = "state-db-e2e",
    state = { name = "hull-e2e-state-db" },
    modules = { "hull/db@1" },
    databases = { named = { default = "@state/data.db", cache = "@state/cache.db" } },
})
app.main(function()
    local d = db.default()
    d.exec("INSERT INTO notes (body) VALUES (?)", { "note" })
    print("count=" .. tostring(d.query("SELECT COUNT(*) AS n FROM notes")[1].n))
    local c = db.connect("cache")
    c.exec("CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT)")
    c.exec("INSERT OR REPLACE INTO kv (k, v) VALUES ('a', 'b')")
    print("cache_rows=" .. tostring(c.query("SELECT COUNT(*) AS n FROM kv")[1].n))
    return 0
end)
