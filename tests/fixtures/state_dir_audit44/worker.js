import { app } from "hull:app";
import { db as dbModule } from "hull:db";
import { worker } from "hull:worker";
// The JS twin of worker.lua (audit 44 M2).
app.manifest({
    name: "state-a44-worker",
    state: { name: "hull-e2e-a44w" },
    modules: ["hull/db@1", "hull/worker@1"],
    databases: { named: { default: "@state/data.db" } },
});
app.main(async () => {
    let ok = true;
    try {
        await worker.dispatch(() => {
            db.exec("INSERT INTO notes (body) VALUES ('from-worker')");
            return { n: db.query("SELECT COUNT(*) AS n FROM notes")[0].n };
        }, {});
    } catch (e) {
        ok = false;
        console.log("worker_err=" + String(e && e.message));
    }
    console.log("worker_ok=" + String(ok));
    console.log("main_notes=" + String(dbModule.default().query("SELECT COUNT(*) AS n FROM notes")[0].n));
    return 0;
});
