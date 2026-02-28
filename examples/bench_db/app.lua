-- bench_db — SQLite performance benchmark endpoints
--
-- Workloads:
--   GET  /read        — read-heavy: SELECT 20 rows by indexed column
--   POST /write       — write-heavy: INSERT single row
--   POST /write-batch — write-heavy: INSERT 10 rows in one request
--   GET  /mixed       — mixed: 1 INSERT + 1 SELECT (20 rows)
--   GET  /health      — baseline (no DB)

-- Schema
db.exec("CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, payload TEXT, ts INTEGER NOT NULL)")
db.exec("CREATE INDEX IF NOT EXISTS idx_events_ts ON events (ts DESC)")

-- Seed data for reads (1000 rows)
local count = db.query("SELECT count(*) AS n FROM events")
if count[1].n < 1000 then
    for i = 1, 1000 do
        db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
                {"seed", "payload-" .. i, 1700000000 + i})
    end
end

-- GET /health — no DB baseline
app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- GET /read — read-heavy: SELECT 20 most recent events
app.get("/read", function(_req, res)
    local rows = db.query("SELECT id, kind, payload, ts FROM events ORDER BY ts DESC LIMIT 20")
    res:json(rows)
end)

-- POST /write — write-heavy: single INSERT per request
app.post("/write", function(_req, res)
    local n = db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
                      {"bench", "data", time.now()})
    res:json({ inserted = n })
end)

-- POST /write-batch — 10 INSERTs in a single transaction
app.post("/write-batch", function(_req, res)
    db.batch(function()
        for i = 1, 10 do
            db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
                    {"batch", "data-" .. i, time.now()})
        end
    end)
    res:json({ inserted = 10 })
end)

-- GET /mixed — 1 write + 1 read per request
app.get("/mixed", function(_req, res)
    db.exec("INSERT INTO events (kind, payload, ts) VALUES (?, ?, ?)",
            {"mixed", "data", time.now()})
    local rows = db.query("SELECT id, kind, payload, ts FROM events ORDER BY ts DESC LIMIT 20")
    res:json(rows)
end)

log.info("bench_db loaded — endpoints: /health /read /write /mixed")
