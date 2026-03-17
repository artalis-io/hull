// Background Timers — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Test: curl localhost:3000/health
//       curl localhost:3000/heartbeats
//       curl localhost:3000/counter
//
// Demonstrates app.every() and app.daily() — background timers
// that fire on the event loop thread with full async support.

import { app } from "hull:app";
import { db } from "hull:db";
import { log } from "hull:log";
import { time } from "hull:time";

app.manifest({
    database: "timers.db",
});

// ── Repeating timer: insert heartbeat every 500ms ──────────────────

app.every(500, () => {
    db.exec("INSERT INTO heartbeat (ts, source) VALUES (?, ?)",
            [time.nowMs(), "every"]);
});

// ── Self-cancelling timer: count to 3, then stop ───────────────────

let cancelCount = 0;

app.every(200, () => {
    cancelCount++;
    db.exec("INSERT INTO heartbeat (ts, source) VALUES (?, ?)",
            [time.nowMs(), `cancel-${cancelCount}`]);
    if (cancelCount >= 3) {
        return false;  // stop the timer
    }
});

// ── Routes ─────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok", runtime: "quickjs" });
});

// Return all heartbeats (populated by timers)
app.get("/heartbeats", (_req, res) => {
    const rows = db.query("SELECT * FROM heartbeat ORDER BY id");
    res.json(rows);
});

// Return heartbeat count by source
app.get("/counter", (_req, res) => {
    const rows = db.query(
        "SELECT source, COUNT(*) as count FROM heartbeat GROUP BY source ORDER BY source");
    res.json(rows);
});

log.info("Timers example loaded — background timers registered");
