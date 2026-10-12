import { app } from "hull:app";
import { worker } from "hull:worker";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
app.manifest({ modules: ["hull/worker@1", "hull/db@1"] });
app.main(async (ctx) => {
    let total = 0;
    for (let i = 1; i <= 32; i++) {
        await hull.sleep(1);
        const result = await worker.dispatch((input) => input.value + 1, { value: i });
        if (result !== i + 1) throw new Error("worker result mismatch");
        const rows = await db.async.query("SELECT ? AS value", [result]);
        if (rows[0].value !== result) throw new Error("async DB result mismatch");
        total += result;
    }
    if (total !== 560) throw new Error("total mismatch");
    ctx.stdout.write("poll-cli: timers=32 workers=32 db=32 total=560\n");
    return ctx.args[0] === "exit7" ? 7 : 0;
});
