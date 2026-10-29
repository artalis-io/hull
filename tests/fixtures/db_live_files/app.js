// Audit 33: an app cannot truncate a database file SQLite holds open.
// Run with -d data/app.db and data/shmlink -> app.db-shm (e2e_db_live_files.sh).
import { app } from "hull:app";
import { db } from "hull:db";
import { fs } from "hull:fs";
import { compute } from "hull:compute";

app.manifest({
    modules: ["hull/db@1", "hull/fs@1", "hull/compute@1"],
    fs: { read: ["data"], write: ["data"] },
});

function attempt(label, fn) {
    let ok = false, err = null;
    try {
        const r = fn();
        if (Array.isArray(r)) { ok = !!r[0]; err = r[1]; }
        else ok = r !== null && r !== undefined && r !== false;
    } catch (e) {
        err = String(e && e.message ? e.message : e);
    }
    console.log(label + " " + ok + " " + err);
    return ok;
}

app.main(() => {
    const c = db.default();
    c.exec("CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, v TEXT)");
    c.exec("INSERT INTO t (v) VALUES ('a')");

    const s = attempt("STREAM", () => compute.stream("echo", "y", { file: "data/app.db-shm" }));
    const w = attempt("LINK", () => fs.write("data/shmlink", "y"));
    const r = attempt("WAL", () => fs.write("data/app.db-wal", "y"));

    const n = c.query("SELECT count(*) AS n FROM t")[0].n;
    console.log("AFTER " + n);
    return (s || w || r || n !== 1) ? 1 : 0;
});
