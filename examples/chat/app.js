// Chat — WebSocket + SSE example (JavaScript)
//
// Run: hull app.js -p 3000
//
// Endpoints:
//   WS  /ws/chat          — WebSocket chat (broadcast messages)
//   SSE /sse/events       — SSE event stream (sends tick every 2s)
//   GET /health           — health check
//   GET /ws/connections   — current WebSocket connection count

import { app } from "hull:app";
import { log } from "hull:log";
import { time } from "hull:time";
import { ws } from "hull:ws";

app.manifest({});

// ── WebSocket chat ────────────────────────────────────────────────

app.ws("/ws/chat", {
    onOpen(conn) {
        log.info(`ws: client connected id=${conn.id}`);
        ws.broadcast("/ws/chat", JSON.stringify({
            type: "join",
            id: conn.id,
            connections: ws.connections("/ws/chat"),
        }));
    },

    onMessage(conn, msg) {
        ws.broadcast("/ws/chat", JSON.stringify({
            type: "message",
            from: conn.id,
            data: msg,
        }));
    },

    onClose(conn, code) {
        log.info(`ws: client disconnected id=${conn.id} code=${code}`);
        ws.broadcast("/ws/chat", JSON.stringify({
            type: "leave",
            id: conn.id,
            code: code,
            connections: ws.connections("/ws/chat"),
        }));
    },
});

// ── SSE event stream ──────────────────────────────────────────────

app.sse("/sse/events", async (_req, stream) => {
    stream.comment("connected");
    stream.event("welcome", JSON.stringify({ time: time.datetime() }));

    for (let i = 1; i <= 3; i++) {
        await hull.sleep(2000);
        stream.event("tick", JSON.stringify({
            seq: i,
            time: time.datetime(),
        }), String(i));
    }

    stream.event(null, "stream complete");
    stream.close();
});

// ── HTTP endpoints ────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok", runtime: "js" });
});

app.get("/ws/connections", (_req, res) => {
    res.json({ count: ws.connections("/ws/chat") });
});

log.info("Chat app loaded — routes registered");
