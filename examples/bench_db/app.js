// bench_db — SQLite performance benchmark endpoints
//
// Run: hull app.js -p 3000
//
// Workloads:
//   GET  /read        — read-heavy: SELECT 20 rows by indexed column
//   POST /write       — write-heavy: INSERT single row
//   POST /write-batch — write-heavy: INSERT 10 rows in one request
//   GET  /mixed       — mixed: 1 INSERT + 1 SELECT (20 rows)
//   GET  /health      — baseline (no DB)

import { app } from "hull:app";
import { db } from "hull:db";
import { log } from "hull:log";
import { time } from "hull:time";

// Schema
db.exec("CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, payload TEXT, ts INTEGER NOT NULL)");
db.exec("CREATE INDEX IF NOT EXISTS idx_events_ts ON events (ts DESC)");

// Seed data for reads (1000 rows)
const count = db.query("SELECT count(*) AS n FROM events");
if (count[0].n < 1000) {
    db.batch(() => {
        for (let i = 1; i <= 1000; i++) {
            db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
                    ["seed", "payload-" + i, 1700000000 + i]);
        }
    });
}

// GET /health — no DB baseline
app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// GET /read — read-heavy: SELECT 20 most recent events
app.get("/read", (_req, res) => {
    const rows = db.query("SELECT id, kind, payload, ts FROM events ORDER BY ts DESC LIMIT 20");
    res.json(rows);
});

// POST /write — write-heavy: single INSERT per request
app.post("/write", (_req, res) => {
    const n = db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
                      ["bench", "data", time.now()]);
    res.json({ inserted: n });
});

// POST /write-batch — 10 INSERTs in a single transaction
app.post("/write-batch", (_req, res) => {
    db.batch(() => {
        for (let i = 1; i <= 10; i++) {
            db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
                    ["batch", "data-" + i, time.now()]);
        }
    });
    res.json({ inserted: 10 });
});

// GET /mixed — 1 write + 1 read per request
app.get("/mixed", (_req, res) => {
    db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
            ["mixed", "data", time.now()]);
    const rows = db.query("SELECT id, kind, payload, ts FROM events ORDER BY ts DESC LIMIT 20");
    res.json(rows);
});

log.info("bench_db loaded — endpoints: /health /read /write /mixed");
