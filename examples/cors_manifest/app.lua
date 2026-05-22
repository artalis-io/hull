-- CORS via Manifest — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Demonstrates CORS configuration via app.manifest() and http_server.stats() API.
-- CORS headers are handled automatically by Keel — no middleware code needed.

local http_server = require("hull.http-server")

local log = require("hull.log")
local json = require("hull.json")
app.manifest({
    cors = {
        origins = {"http://localhost:5173", "http://localhost:3001"},
        methods = "GET, POST, PUT, DELETE",
        credentials = true,
        max_age = 3600,
    },
    modules = {
    "hull/http-server@1",
    "hull/json@1",
    "hull/log@1",
    },
})

-- ── Routes ──────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- Returns live server stats (connection counts)
app.get("/api/data", function(_req, res)
    local ok, stats = pcall(http_server.stats)
    if ok then
        res:json({
            connections = stats.active_connections,
            max = stats.max_connections,
            suspended = stats.async_suspended,
        })
    else
        res:json({ connections = 0, max = 0, suspended = 0 })
    end
end)

app.post("/api/data", function(req, res)
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end
    res:status(201):json({ created = body })
end)

-- OPTIONS for CORS preflight (route must exist for middleware to run)
app.options("/api/data", function(_req, res)
    res:status(204):text("")
end)

log.info("CORS manifest example loaded — routes registered")
