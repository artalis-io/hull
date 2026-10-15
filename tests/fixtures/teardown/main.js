// app.main that returns with work still in flight (tests/e2e_teardown.sh):
// worker.dispatch and db.async calls nobody awaits, an unawaited hull.sleep,
// and - given a port - a detached http.fetch that never answers. Each left a
// continuation behind, and QuickJS aborted in JS_FreeRuntime (audit 14 G1).
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import { app } from "hull:app";
import { worker } from "hull:worker";
import { db as dbModule } from "hull:db";
import { httpClient } from "hull:http-client";

const db = dbModule.default();
app.manifest({
    modules: ["hull/worker@1", "hull/db@1", "hull/http-client@1"],
    hosts: ["127.0.0.1"],
});

app.main(async (ctx) => {
    if (ctx.args[0])
        httpClient.async.get(`http://127.0.0.1:${ctx.args[0]}/slow`).catch(() => {});
    for (let i = 0; i < 16; i++) {
        worker.dispatch(() => {
            let x = 0;
            for (let j = 0; j < 3000000; j++) x += j;
            return x;
        }).catch(() => {});
        db.async.query("SELECT ? AS value", [i]).catch(() => {});
    }
    hull.sleep(60000);
    await hull.sleep(50);
    ctx.stdout.write("teardown-main: left work behind\n");
    return 0;
});
