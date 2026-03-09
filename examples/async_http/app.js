// Async HTTP Client — Hull + QuickJS example
//
// Run: hull app.js -p 3000 --no-sandbox
// Test: curl localhost:3000/health
//       curl localhost:3000/sleep
//       curl localhost:3000/async-fetch
//
// Demonstrates hull.sleep() and http.fetch() — both return Promises
// that pause the handler while the event loop serves other connections.
// Compare /sync-fetch (blocks event loop) vs /async-fetch (non-blocking
// via KlWatcher).

import { app } from "hull:app";
import { db } from "hull:db";
import { log } from "hull:log";
import { http } from "hull:http";

// Allow outbound HTTP to self (localhost) for the fetch demos
app.manifest({
    hosts: ["127.0.0.1"],
});

// ── Routes ─────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Mock slow API endpoint (uses async sleep internally)
app.get("/api/slow", async (_req, res) => {
    await hull.sleep(50);
    res.json({ value: 42, source: "mock" });
});

// Sync HTTP: blocks the event loop while waiting for response
app.get("/sync-fetch", (req, res) => {
    const host = req.headers.host || "127.0.0.1:3000";
    const port = host.split(":")[1] || "3000";
    const resp = http.get("http://127.0.0.1:" + port + "/api/slow");
    res.json({ mode: "sync", status: resp.status, body: resp.body });
});

// Async HTTP: yields via await, event loop stays responsive
app.get("/async-fetch", async (req, res) => {
    const host = req.headers.host || "127.0.0.1:3000";
    const port = host.split(":")[1] || "3000";
    const resp = await http.fetch("GET", "http://127.0.0.1:" + port + "/api/slow");
    res.json({ mode: "async", status: resp.status, body: resp.body });
});

// Async POST with body and headers
app.get("/async-post", async (req, res) => {
    const host = req.headers.host || "127.0.0.1:3000";
    const port = host.split(":")[1] || "3000";
    const resp = await http.fetch("POST", "http://127.0.0.1:" + port + "/echo", {
        body: '{"greeting":"hello"}',
        headers: { "Content-Type": "application/json" },
    });
    res.json({ mode: "async", status: resp.status, body: resp.body });
});

// Echo body back as JSON (target for async-post)
app.post("/echo", (req, res) => {
    res.json({ body: req.body });
});

// Sleep demo (existing async primitive)
app.get("/sleep", async (_req, res) => {
    await hull.sleep(100);
    res.json({ slept: true });
});

log.info("Async HTTP example loaded — routes registered");
