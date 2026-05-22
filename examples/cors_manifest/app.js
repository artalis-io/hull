// CORS via Manifest — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Demonstrates CORS configuration via app.manifest() and server.stats() API.
// CORS headers are handled automatically by Keel — no middleware code needed.

import { app } from "hull:app";
import { log } from "hull:log";
import { server } from "hull:server";

app.manifest({
    cors: {
        origins: ["http://localhost:5173", "http://localhost:3001"],
        methods: "GET, POST, PUT, DELETE",
        credentials: true,
        maxAge: 3600,
    },
    modules: [
    "hull/log@1",
        "hull/server@1",
    ],
});

// ── Routes ──────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Returns live server stats (connection counts)
app.get("/api/data", (_req, res) => {
    let stats;
    try { stats = server.stats(); } catch (_e) {
        stats = { activeConnections: 0, maxConnections: 0, asyncSuspended: 0 };
    }
    res.json({
        connections: stats.activeConnections,
        max: stats.maxConnections,
        suspended: stats.asyncSuspended,
    });
});

app.post("/api/data", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        res.status(400);
        res.json({ error: "invalid JSON" });
        return;
    }
    res.status(201).json({ created: body });
});

// OPTIONS for CORS preflight (route must exist for middleware to run)
app.options("/api/data", (_req, res) => {
    res.status(204).text("");
});

log.info("CORS manifest example loaded — routes registered");
