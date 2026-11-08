import { app } from "hull:app";
import { db as dbModule } from "hull:db";
// The JS twin of app.lua: the default database under "@state/" (gap 3, PR 2).
app.manifest({
    name: "state-db-e2e",
    state: { name: "hull-e2e-state-db" },
    modules: ["hull/db@1"],
    databases: { named: { default: "@state/data.db", cache: "@state/cache.db" } },
});
app.main(async () => {
    const d = dbModule.default();
    d.exec("INSERT INTO notes (body) VALUES (?)", ["note"]);
    console.log("count=" + String(d.query("SELECT COUNT(*) AS n FROM notes")[0].n));
    const c = dbModule.connect("cache");
    c.exec("CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT)");
    c.exec("INSERT OR REPLACE INTO kv (k, v) VALUES ('a', 'b')");
    console.log("cache_rows=" + String(c.query("SELECT COUNT(*) AS n FROM kv")[0].n));
    return 0;
});
