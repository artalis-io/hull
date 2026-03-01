-- Middleware — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Demonstrates middleware chaining: request ID, logging, rate limiting, CORS

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
-- Simple in-memory rate limiter: max 60 requests per minute per client.
-- Uses a sliding window approximation with per-second buckets.
-- NOTE: resets on server restart. For production, use a DB-backed limiter.

local rate_window = 60   -- window in seconds
local rate_limit = 60    -- max requests per window
local rate_buckets = {}  -- { [client_key] = { count, window_start } }

app.use("*", "/api/*", function(_req, res)
    -- In a real app, key by IP or API key. Here we use a fixed key for demo.
    local key = "global"
    local now = time.now()

    local bucket = rate_buckets[key]
    if not bucket or (now - bucket.window_start) >= rate_window then
        rate_buckets[key] = { count = 1, window_start = now }
        bucket = rate_buckets[key]
    else
        bucket.count = bucket.count + 1
    end

    local remaining = rate_limit - bucket.count
    if remaining < 0 then remaining = 0 end

    res:header("X-RateLimit-Limit", tostring(rate_limit))
    res:header("X-RateLimit-Remaining", tostring(remaining))
    res:header("X-RateLimit-Reset", tostring(bucket.window_start + rate_window))

    if bucket.count > rate_limit then
        res:status(429):json({ error = "rate limit exceeded", retry_after = rate_window })
        return 1
    end

    return 0
end)

-- ── CORS middleware ──────────────────────────────────────────────────
-- Manual CORS implementation: allows configurable origins, methods, headers.

local cors_origins = { "http://localhost:5173", "http://localhost:3001" }
local cors_methods = "GET, POST, PUT, DELETE, OPTIONS"
local cors_headers = "Content-Type, Authorization"
local cors_max_age = "86400"

local function is_allowed_origin(origin)
    for _, o in ipairs(cors_origins) do
        if o == "*" or o == origin then return true end
    end
    return false
end

app.use("*", "/api/*", function(req, res)
    local origin = req.headers["origin"]
    if not origin then return 0 end

    if not is_allowed_origin(origin) then return 0 end

    res:header("Access-Control-Allow-Origin", origin)
    res:header("Access-Control-Allow-Methods", cors_methods)
    res:header("Access-Control-Allow-Headers", cors_headers)
    res:header("Access-Control-Max-Age", cors_max_age)

    -- Handle preflight
    if req.method == "OPTIONS" then
        res:status(204):text("")
        return 1
    end

    return 0
end)

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
