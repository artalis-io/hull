-- Audit convergence §7.2: a duckdb:// connection with a root grant (mode B)
-- must not reach the app's .hull, where Hull keeps its internal database.
-- GET /probe answers "IDB NOLEAK" when every attempt was refused, then
-- "READS OK <rows> <files>" when ordinary mode-B file access still works
-- (or "READS REFUSED: <err>" without a file policy, as in a composed app).
app.manifest({
    modules = { "hull/web/middleware/session@1", "hull/db@1", "hull/http-server@1" },
    databases = { named = { ana = "duckdb://ana.duckdb" } },
    fs = { read = { "." }, write = { "." } },
})
local session = require("hull.web.middleware.session")
local db = require("hull.db")
session.init({})
app.get("/probe", function(req, res)
    session.create({ user_id = "alice" })       -- .hull/internal.db exists
    local c = db.connect("ana")
    local leaked = {}
    for _, sql in ipairs({
        "SELECT size FROM read_blob('.hull/internal.db')",
        "SELECT size FROM read_blob('.HULL/internal.db')",
        "SELECT size FROM read_blob('./.hull/internal.db')",
        "SELECT size FROM read_blob('data/../.hull/internal.db')",
        "SELECT size FROM read_blob('.hull//internal.db-wal')",
        "SELECT size FROM read_blob('*/internal.db')",
        "SELECT size FROM read_blob('**/*.db')",
        "SELECT size FROM read_blob('.h*/internal.db')",
        "SELECT file FROM glob('**')",
        "SELECT * FROM '.hull/internal.db'",
    }) do
        if pcall(c.query, sql) then leaked[#leaked + 1] = sql end
    end
    for _, sql in ipairs({
        "COPY (SELECT 1 AS a) TO '.hull/x.csv'",
        "COPY (SELECT 1 AS a) TO '.hull/internal.db-journal'",
        "COPY (SELECT 1 AS a) TO 'late.csv'",   -- made at the root after startup
    }) do
        if pcall(c.exec, sql) then leaked[#leaked + 1] = sql end
    end
    if #leaked > 0 then
        return res:text("LEAKED " .. table.concat(leaked, " | "))
    end
    local ok, out = pcall(function()
        local n = c.query("SELECT count(*) AS n FROM read_csv('data/x.csv')")[1].n
        local g = c.query("SELECT count(*) AS n FROM glob('data/*.csv')")[1].n
        c.exec("COPY (SELECT 7 AS a) TO 'data/out.csv'")
        return "READS OK " .. tostring(n) .. " " .. tostring(g)
    end)
    res:text("IDB NOLEAK " .. (ok and out or ("READS REFUSED: " .. tostring(out))))
end)
