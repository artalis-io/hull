-- Middleware — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Demonstrates middleware chaining: request ID, logging, rate limiting, CORS

local cors = require("hull.cors")
local ratelimit = require("hull.ratelimit")

-- ── Request ID middleware ────────────────────────────────────────────
-- Assigns a unique ID to every request, available via req.ctx.request_id
-- and returned in the X-Request-ID response header.

local request_counter = 0

app.use("*", "/*", function(req, res)
    request_counter = request_counter + 1
    local id = string.format("%x-%x", time.now(), request_counter)
    req.ctx.request_id = id
    res:header("X-Request-ID", id)
    return 0
end)

-- ── Request logging middleware ───────────────────────────────────────
-- Logs method, path, and request ID for every request.

app.use("*", "/*", function(req, _res)
    log.info(string.format("%s %s [%s]", req.method, req.path, req.ctx.request_id or "-"))
    return 0
end)

-- ── Rate limiting middleware ─────────────────────────────────────────
-- 60 requests per minute, keyed globally (demo).

app.use("*", "/api/*", ratelimit.middleware({
    limit = 60, window = 60,
}))

-- ── CORS middleware ──────────────────────────────────────────────────

app.use("*", "/api/*", cors.middleware({
    origins = { "http://localhost:5173", "http://localhost:3001" },
}))

-- OPTIONS route for CORS preflight (router requires a route to exist
-- so middleware can run — the CORS middleware above handles the response)
app.options("/api/items", function(_req, res)
    res:status(204):text("")
end)

-- ── Routes ──────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- Public route (no rate limit — only /api/* is rate limited)
app.get("/", function(req, res)
    res:json({
        message = "Middleware example",
        request_id = req.ctx.request_id,
    })
end)

-- API routes (rate limited + CORS)
app.get("/api/items", function(req, res)
    res:json({
        items = { "apple", "banana", "cherry" },
        request_id = req.ctx.request_id,
    })
end)

app.post("/api/items", function(req, res)
    local body = json.decode(req.body)
    res:status(201):json({
        created = body,
        request_id = req.ctx.request_id,
    })
end)

-- Route to inspect middleware state
app.get("/api/debug", function(req, res)
    res:json({
        request_id = req.ctx.request_id,
        total_requests = request_counter,
    })
end)

log.info("Middleware example loaded — routes registered")
