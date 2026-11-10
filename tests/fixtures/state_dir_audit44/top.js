import { app } from "hull:app";
import { db as dbModule } from "hull:db";
import { auditLog } from "hull:web:middleware:audit-log";
// The JS twin of top.lua (audit 44 M1).
app.manifest({
    name: "state-a44-top",
    state: { name: "hull-e2e-a44" },
    modules: ["hull/db@1", "hull/web/middleware/audit-log@1"],
    databases: { named: { default: "@state/data.db" } },
});
auditLog.init({ fingerprintSalt: "e2e-audit44-salt", cleanup: false });
const top = dbModule.default();
app.main(async () => {
    auditLog.record("u1", "login", null,
                    { ip: "127.0.0.1", user_agent: "e2e", fingerprint: "fp" });
    console.log("events=" + String(auditLog.list("u1").length));
    top.exec("INSERT INTO notes (body) VALUES (?)", ["captured"]);
    console.log("notes=" + String(dbModule.default().query("SELECT COUNT(*) AS n FROM notes")[0].n));
    return 0;
});
