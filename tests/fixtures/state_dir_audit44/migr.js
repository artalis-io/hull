import { app } from "hull:app";
import { rbac } from "hull:web:middleware:rbac";
// The JS twin of migr.lua (audit 44 M3).
app.manifest({
    name: "state-a44-migr",
    state: { name: "hull-e2e-a44m" },
    modules: ["hull/db@1", "hull/web/middleware/rbac@1"],
    databases: { named: { default: "@state/data.db" } },
});
app.main(async (ctx) => {
    rbac.init();
    if (ctx.args[0] === "seed") {
        rbac.defineRole("admin", []);
        rbac.assign("u1", "admin");
    }
    console.log("roles=" + String(rbac.roles("u1").length));
    return 3;   // the middleware modules admit hull/http-server: exit before the serve loop
});
