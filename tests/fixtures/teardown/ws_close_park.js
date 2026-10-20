// Shutdown fixture (tests/e2e_teardown.sh): a WebSocket on_close that is
// still parked on hull.sleep when the server is torn down. Its continuation
// is cancelled, and the cancel must run the deferred conn teardown the
// completion would have run: skipped, the conn object stayed alive and
// QuickJS aborted in JS_FreeRuntime (audit 19).
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import { app } from "hull:app";

app.manifest({ modules: ["hull/http-server@1", "hull/web/ws-server@1"] });

app.get("/ping", (req, res) => res.text("pong"));

app.ws("/ws", {
    onOpen(conn) { conn.send("hello"); },
    async onClose(conn) {
        console.log("teardown-ws: on_close parked");
        await hull.sleep(60000);
        console.log("teardown-ws: on_close resumed");
    },
});
