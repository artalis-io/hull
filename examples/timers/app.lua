-- Background Timers — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Test: curl localhost:3000/health
--       curl localhost:3000/heartbeats
--       curl localhost:3000/counter
--
-- Demonstrates app.every() and app.daily() — background timers
-- that fire on the event loop thread with full async support.

local db   = require("hull.db")
local time = require("hull.time")

local log = require("hull.log")
app.manifest({
    database = "timers.db",
    modules = {
        "hull/log@1",
        "hull/db@1",
        "hull/time@1",
    },
})

-- ── Repeating timer: insert heartbeat every 500ms ──────────────────

app.every(500, function()
    db.exec("INSERT INTO heartbeat (ts, source) VALUES (?, ?)",
            {time.now_ms(), "every"})
end)

-- ── Self-cancelling timer: count to 3, then stop ───────────────────

local cancel_count = 0

app.every(200, function()
    cancel_count = cancel_count + 1
    db.exec("INSERT INTO heartbeat (ts, source) VALUES (?, ?)",
            {time.now_ms(), "cancel-" .. cancel_count})
    if cancel_count >= 3 then
        return false  -- stop the timer
    end
end)

-- ── Routes ─────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok", runtime = "lua" })
end)

-- Return all heartbeats (populated by timers)
app.get("/heartbeats", function(_req, res)
    local rows = db.query("SELECT * FROM heartbeat ORDER BY id")
    res:json(rows)
end)

-- Return heartbeat count by source
app.get("/counter", function(_req, res)
    local rows = db.query(
        "SELECT source, COUNT(*) as count FROM heartbeat GROUP BY source ORDER BY source")
    res:json(rows)
end)

log.info("Timers example loaded — background timers registered")
