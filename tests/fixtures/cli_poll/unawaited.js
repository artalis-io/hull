import { app } from "hull:app";
import { worker } from "hull:worker";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
app.manifest({ modules: ["hull/worker@1", "hull/db@1"] });
// Audit 14 G1: main returns with work nobody awaited - finished, running and
// still queued. poll_pool_free delivers or cancels each one, so no
// continuation outlives the runtime (QuickJS aborted in JS_FreeRuntime: 134).
app.main(async (ctx) => {
    for (let i = 0; i < 16; i++) {
        worker.dispatch(() => {
            let x = 0;
            for (let j = 0; j < 3000000; j++) x += j;
            return x;
        }).catch(() => {});
        db.async.query("SELECT ? AS value", [i]).catch(() => {});
    }
    hull.sleep(60000);
    await hull.sleep(20);
    ctx.stdout.write("poll-cli: left work behind\n");
    return 0;
});
