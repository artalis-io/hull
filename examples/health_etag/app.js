// Health check + ETag example
//
// Demonstrates hull:middleware:health and hull:middleware:etag

import { db } from "hull:db";
import { etag } from "hull:middleware:etag";
import { health } from "hull:middleware:health";

// Register a custom health check
health.register("app_ready", () => true);

// Pass db module to health (optional — only needed if app has a database)
health.setDb(db);

// Register health middleware on all GET paths
app.use("GET", "/*", health.middleware());

// API routes with ETag support
app.get("/api/items", (req, res) => {
    const items = db.query("SELECT * FROM items ORDER BY id");
    etag.json(req, res, { items });
});

app.get("/api/greeting", (req, res) => {
    const name = req.query.name || "World";
    etag.text(req, res, `Hello, ${name}!`);
});

app.get("/api/page", (req, res) => {
    etag.html(req, res, "<h1>Hello</h1>");
});

// POST route (ETag skipped for non-GET)
app.post("/api/items", (req, res) => {
    const body = JSON.parse(req.body);
    db.exec("INSERT INTO items (name) VALUES (?)", [body.name]);
    res.status(201).json({ ok: true });
});
