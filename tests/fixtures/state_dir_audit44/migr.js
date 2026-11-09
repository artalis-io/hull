import { app } from "hull:app";
import { auditLog } from "hull:web:middleware:audit-log";
// The JS twin of migr.lua (audit 44 M3).
app.manifest({
    name: "state-a44-migr",
    state: { name: "hull-e2e-a44m" },
    modules: ["hull/db@1", "hull/web/middleware/audit-log@1"],
    databases: { named: { default: "@state/data.db" } },
});
app.main(async (ctx) => {
    auditLog.init({ fingerprintSalt: "e2e-audit44-salt", cleanup: false });
    if (ctx.args[0] === "seed") {
        auditLog.record("u1", "login", null,
                        { ip: "127.0.0.1", user_agent: "e2e", fingerprint: "fp" });
    }
    console.log("events=" + String(auditLog.list("u1").length));
    return 0;
});
