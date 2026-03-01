// Middleware — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Demonstrates middleware chaining: request ID, logging, rate limiting, CORS

import { app } from "hull:app";
import { log } from "hull:log";
import { time } from "hull:time";
import { cors } from "hull:cors";
import { ratelimit } from "hull:ratelimit";

// ── Request ID middleware ────────────────────────────────────────────
// Assigns a unique ID to every request, available via req.ctx.request_id
// and returned in the X-Request-ID response header.

let requestCounter = 0;

app.use("*", "/*", (req, res) => {
    requestCounter++;
    const id = `${time.now().toString(16)}-${requestCounter.toString(16)}`;
    if (!req.ctx) req.ctx = {};
    req.ctx.request_id = id;
    res.header("X-Request-ID", id);
    return 0;
});

// ── Request logging middleware ───────────────────────────────────────
// Logs method, path, and request ID for every request.

app.use("*", "/*", (req, _res) => {
    log.info(`${req.method} ${req.path} [${req.ctx.request_id || "-"}]`);
    return 0;
});

// ── Rate limiting middleware ─────────────────────────────────────────
// 60 requests per minute, keyed globally (demo).

app.use("*", "/api/*", ratelimit.middleware({
    limit: 60, window: 60,
}));

// ── CORS middleware ──────────────────────────────────────────────────

app.use("*", "/api/*", cors.middleware({
    origins: ["http://localhost:5173", "http://localhost:3001"],
}));

// OPTIONS route for CORS preflight (router requires a route to exist
// so middleware can run — the CORS middleware above handles the response)
app.options("/api/items", (_req, res) => {
    res.status(204).text("");
});

// ── Routes ──────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Public route (no rate limit — only /api/* is rate limited)
app.get("/", (req, res) => {
    res.json({
        message: "Middleware example",
        request_id: req.ctx ? req.ctx.request_id : null,
    });
});

// API routes (rate limited + CORS)
app.get("/api/items", (req, res) => {
    res.json({
        items: ["apple", "banana", "cherry"],
        request_id: req.ctx ? req.ctx.request_id : null,
    });
});

app.post("/api/items", (req, res) => {
    const body = JSON.parse(req.body);
    res.status(201).json({
        created: body,
        request_id: req.ctx ? req.ctx.request_id : null,
    });
});

// Route to inspect middleware state
app.get("/api/debug", (req, res) => {
    res.json({
        request_id: req.ctx ? req.ctx.request_id : null,
        total_requests: requestCounter,
    });
});

log.info("Middleware example loaded — routes registered");
