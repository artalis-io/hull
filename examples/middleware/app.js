// Middleware — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Demonstrates middleware chaining: request ID, logging, rate limiting, CORS

import { app } from "hull:app";
import { log } from "hull:log";
import { time } from "hull:time";

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
// Simple in-memory rate limiter: max 60 requests per minute per client.
// Uses a sliding window approximation with per-second buckets.
// NOTE: resets on server restart. For production, use a DB-backed limiter.

const RATE_WINDOW = 60;   // window in seconds
const RATE_LIMIT = 60;    // max requests per window
const rateBuckets = {};    // { [clientKey]: { count, windowStart } }

app.use("*", "/api/*", (_req, res) => {
    // In a real app, key by IP or API key. Here we use a fixed key for demo.
    const key = "global";
    const now = time.now();

    let bucket = rateBuckets[key];
    if (!bucket || (now - bucket.windowStart) >= RATE_WINDOW) {
        rateBuckets[key] = { count: 1, windowStart: now };
        bucket = rateBuckets[key];
    } else {
        bucket.count++;
    }

    const remaining = Math.max(0, RATE_LIMIT - bucket.count);

    res.header("X-RateLimit-Limit", String(RATE_LIMIT));
    res.header("X-RateLimit-Remaining", String(remaining));
    res.header("X-RateLimit-Reset", String(bucket.windowStart + RATE_WINDOW));

    if (bucket.count > RATE_LIMIT) {
        res.status(429).json({ error: "rate limit exceeded", retry_after: RATE_WINDOW });
        return 1;
    }

    return 0;
});

// ── CORS middleware ──────────────────────────────────────────────────
// Manual CORS implementation: allows configurable origins, methods, headers.

const CORS_ORIGINS = ["http://localhost:5173", "http://localhost:3001"];
const CORS_METHODS = "GET, POST, PUT, DELETE, OPTIONS";
const CORS_HEADERS = "Content-Type, Authorization";
const CORS_MAX_AGE = "86400";

app.use("*", "/api/*", (req, res) => {
    const origin = req.headers.origin;
    if (!origin) return 0;

    const allowed = CORS_ORIGINS.indexOf(origin) !== -1;
    if (!allowed) return 0;

    res.header("Access-Control-Allow-Origin", origin);
    res.header("Access-Control-Allow-Methods", CORS_METHODS);
    res.header("Access-Control-Allow-Headers", CORS_HEADERS);
    res.header("Access-Control-Max-Age", CORS_MAX_AGE);

    // Handle preflight
    if (req.method === "OPTIONS") {
        res.status(204).text("");
        return 1;
    }

    return 0;
});

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
