// WebSocket server close fixture (tests/e2e_ws_close.sh, audit 44 M5): the
// app closes the connection itself - from onOpen on /open, from onMessage on
// /msg - and its onClose must still run once Keel delivers the close, with
// the node removed from the registry (wsServer.connections back to 0).
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import { app } from "hull:app";
import { wsServer } from "hull:web:ws-server";

app.manifest({ modules: ["hull/http-server@1", "hull/web/ws-server@1"] });

let closes = 0;

app.get("/ping", (_req, res) => res.text("pong"));

app.ws("/open", {
    onOpen(conn) { conn.close(1000, "bye"); },
    onClose(_conn) { closes++; },
});

app.ws("/msg", {
    onMessage(conn, _msg) { conn.close(4000, "done"); },
    onClose(_conn) { closes++; },
});

app.get("/state", (_req, res) => {
    const sent = wsServer.broadcast("/open", "x") + wsServer.broadcast("/msg", "x");
    res.text(`closes=${closes} open=${wsServer.connections("/open")}`
             + ` msg=${wsServer.connections("/msg")} sent=${sent}`);
});
