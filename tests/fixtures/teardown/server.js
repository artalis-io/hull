// Shutdown fixture (tests/e2e_teardown.sh): at SIGTERM it holds a request
// parked on hull.sleep, a WebSocket that never answers the close, and a
// detached http.fetch started from a timer - each of which the teardown used
// to reach through a freed runtime (audit 14 G1 / audit 15).
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { wsServer } from "hull:web:ws-server";

app.manifest({
    modules: ["hull/http-server@1", "hull/http-client@1", "hull/timers@1",
              "hull/web/ws-server@1"],
    hosts: ["127.0.0.1"],
});

let target = null;
let fired = false;

// A detached fetch: no request holds it.
app.every(50, async () => {
    if (!target || fired) return;
    fired = true;
    try { await httpClient.async.get(target); } catch (e) { /* cancelled */ }
});

app.get("/ping", (req, res) => res.text("pong"));
app.get("/arm/:port", (req, res) => {
    target = `http://127.0.0.1:${req.params.port}/slow`;
    res.text("armed");
});
app.get("/fired", (req, res) => res.text(fired ? "yes" : "no"));
app.get("/slow", async (req, res) => {
    await hull.sleep(60000);
    res.text("late");
});

app.ws("/ws", {
    onOpen(conn) { conn.send("hello"); },
    onMessage(conn, msg) { conn.send(msg); },
    // Runs when a client closes normally. A WebSocket still open when the
    // server is torn down closes without it: the loop is gone by then.
    onClose(conn) { wsServer.broadcast("/ws", "left"); },
});
