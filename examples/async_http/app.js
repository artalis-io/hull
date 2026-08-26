// Async HTTP Client - Hull + QuickJS example
//
// Run: hull app.js -p 3000 --no-sandbox
// Test: curl localhost:3000/health
//       curl localhost:3000/sleep
//       curl localhost:3000/async-fetch
//       curl localhost:3000/async-db
//       curl localhost:3000/worker-dispatch
//
// Demonstrates hull.sleep(), httpClient.async.get(), db.async.query(),
// and worker.dispatch() - all return Promises that pause the handler
// while the event loop serves other connections.

import { app } from "hull:app";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { httpClient } from "hull:http-client";
import { log } from "hull:log";
import { worker } from "hull:worker";

// Allow outbound HTTP to self (localhost) for the fetch demos
app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/db@1",
        "hull/http-client@1",
        "hull/log@1",
        "hull/worker@1",
    ],
    hosts: ["127.0.0.1"],
    database: "async_demo.db",
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
    const resp = httpClient.get(`http://127.0.0.1:${port}/api/slow`);
    res.json({ mode: "sync", status: resp.status, body: resp.body });
});

// Async HTTP: yields via await, event loop stays responsive
app.get("/async-fetch", async (req, res) => {
    const host = req.headers.host || "127.0.0.1:3000";
    const port = host.split(":")[1] || "3000";
    const resp = await httpClient.async.get(`http://127.0.0.1:${port}/api/slow`);
    res.json({ mode: "async", status: resp.status, body: resp.body });
});

// Async POST with body and headers
app.get("/async-post", async (req, res) => {
    const host = req.headers.host || "127.0.0.1:3000";
    const port = host.split(":")[1] || "3000";
    const resp = await httpClient.async.post(`http://127.0.0.1:${port}/echo`,
        '{"greeting":"hello"}',
        { headers: { "Content-Type": "application/json" } });
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

// Async DB: offloads query to thread pool, event loop stays responsive
app.get("/async-db", async (_req, res) => {
    const rows = await db.async.query("SELECT 1 + 1 as result");
    res.json({ rows });
});

// Worker dispatch: runs JS function on a worker thread with its own VM
app.get("/worker-dispatch", async (_req, res) => {
    const result = await worker.dispatch((ctx) => {
        db.exec("CREATE TABLE IF NOT EXISTS async_test (id INTEGER PRIMARY KEY, val TEXT)");
        db.exec("INSERT INTO async_test (val) VALUES (?)", [ctx.value]);
        const rows = db.query("SELECT COUNT(*) as n FROM async_test");
        return { count: rows[0].n };
    }, { value: "hello" });
    res.json(result);
});

log.info("Async HTTP example loaded - routes registered");
