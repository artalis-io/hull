// Health check + ETag example
//
// Demonstrates hull:web:middleware:health and hull:web:middleware:etag

import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { etag } from "hull:web:middleware:etag";
import { health } from "hull:web:middleware:health";

app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/crypto@1",
        "hull/db@1",
        "hull/time@1",
        "hull/web/middleware/etag@1",
        "hull/web/middleware/health@1",
    ],
});

// Register a custom health check
health.register("app_ready", () => true);

// Pass db module to health (optional - only needed if app has a database)
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
