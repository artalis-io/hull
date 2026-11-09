import { app } from "hull:app";
import { db as dbModule } from "hull:db";
import { rbac } from "hull:web:middleware:rbac";
import { session } from "hull:web:middleware:session";
// The JS twin of top.lua (audit 44 M1).
app.manifest({
    name: "state-a44-top",
    state: { name: "hull-e2e-a44" },
    modules: ["hull/db@1", "hull/web/middleware/rbac@1", "hull/web/middleware/session@1"],
    databases: { named: { default: "@state/data.db" } },
});
rbac.init();
session.init({});
const top = dbModule.default();
app.main(async () => {
    rbac.defineRole("admin", []);
    rbac.assign("u1", "admin");
    console.log("roles=" + String(rbac.roles("u1").length));
    top.exec("INSERT INTO notes (body) VALUES (?)", ["captured"]);
    console.log("notes=" + String(dbModule.default().query("SELECT COUNT(*) AS n FROM notes")[0].n));
    return 3;   // the middleware modules admit hull/http-server: exit before the serve loop
});
