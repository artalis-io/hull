-- Audit 33: an app cannot truncate a database file SQLite holds open.
-- Run with -d data/app.db and data/shmlink -> app.db-shm (e2e_db_live_files.sh).
app.manifest({
    modules = { "hull/db@1", "hull/fs@1", "hull/compute@1" },
    fs = { read = { "data" }, write = { "data" } },
})
local db = require("hull.db")
local fs = require("hull.fs")
local compute = require("hull.compute")

app.main(function(ctx)
    local c = db.default()
    c.exec("CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)")
    c.exec("INSERT INTO t (v) VALUES ('a')")

    -- compute.stream's file output truncated the -shm in place: SIGBUS.
    local ok, err = compute.stream("echo", "y", { file = "data/app.db-shm" })
    print("STREAM " .. tostring(ok) .. " " .. tostring(err))
    -- fs.write through an in-root symlink writes in place.
    local wok, werr = fs.write("data/shmlink", "y")
    print("LINK " .. tostring(wok) .. " " .. tostring(werr))
    -- A replace of the live -wal (rename over it).
    local rok, rerr = fs.write("data/app.db-wal", "y")
    print("WAL " .. tostring(rok) .. " " .. tostring(rerr))

    local n = c.query("SELECT count(*) AS n FROM t")[1].n
    print("AFTER " .. tostring(n))
    if ok or wok or rok or n ~= 1 then return 1 end
    return 0
end)
