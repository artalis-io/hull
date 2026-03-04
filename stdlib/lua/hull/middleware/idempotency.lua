--
-- hull.middleware.idempotency -- Idempotency-Key middleware for safe retries
--
-- Prevents duplicate side effects when clients retry POST requests by caching
-- responses keyed by (principal_id, idempotency_key). Uses SHA-256 fingerprint
-- to detect key reuse with different request bodies (409 Conflict).
--
-- Usage:
--   local idempotency = require("hull.middleware.idempotency")
--   idempotency.init({ ttl = 86400 })
--   app.use_post("POST", "/api/*", idempotency.middleware({
--       get_principal = function(req) return req.ctx.session and req.ctx.session.user_id or "__anon" end,
--   }))
--
-- Handlers capture responses via idempotency.respond(req, res, status, body):
--   app.post("/api/events", function(req, res)
--       -- ... do work inside db.batch() ...
--       idempotency.respond(req, res, 201, { event_id = id })
--   end)
--
-- On retry with same key+body: cached response is returned, handler never runs.
-- On retry with same key+different body: 409 Conflict.
-- Without Idempotency-Key header: handler runs normally (no caching).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local idempotency = {}

local _ttl = 86400  -- default 24 hours
local _header_name = "idempotency-key"

--- Initialize the idempotency table.
-- opts.ttl: key lifetime in seconds (default 86400)
function idempotency.init(opts)
    opts = opts or {}
    if opts.ttl then
        _ttl = opts.ttl
    end

    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_idempotency_keys (
            key            TEXT NOT NULL,
            principal_id   TEXT NOT NULL DEFAULT '__anon',
            fingerprint    TEXT NOT NULL,
            endpoint       TEXT NOT NULL,
            status         INTEGER,
            response_body  TEXT,
            response_headers TEXT,
            state          TEXT NOT NULL DEFAULT 'inflight',
            created_at     INTEGER NOT NULL,
            expires_at     INTEGER NOT NULL,
            PRIMARY KEY (principal_id, key)
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_idem_expires
        ON _hull_idempotency_keys(expires_at)
    ]])
end

--- Compute a request fingerprint: SHA-256(method + path + body).
local function compute_fingerprint(req)
    local data = (req.method or "") .. "\0" .. (req.path or "") .. "\0" .. (req.body or "")
    return crypto.sha256(data)
end

--- Create the idempotency post-body middleware.
-- opts.get_principal: function(req) -> string (default: req.ctx.session.user_id or "__anon")
-- opts.ttl: override module-level TTL for this middleware instance
-- opts.header_name: header to read key from (default "idempotency-key")
-- opts.methods: table of methods to intercept (default {"POST"})
function idempotency.middleware(opts)
    opts = opts or {}

    local get_principal = opts.get_principal or function(req)
        if req.ctx and req.ctx.session and req.ctx.session.user_id then
            return tostring(req.ctx.session.user_id)
        end
        return "__anon"
    end

    local ttl = opts.ttl or _ttl
    local header_name = opts.header_name or _header_name

    -- Build methods lookup
    local method_list = opts.methods or { "POST" }
    local methods = {}
    for _, m in ipairs(method_list) do
        methods[m] = true
    end

    return function(req, res)
        -- Only intercept configured methods
        if not methods[req.method] then
            return 0
        end

        -- Read idempotency key from header
        local key = req.headers[header_name]
        if not key or key == "" then
            return 0  -- no key, proceed normally
        end

        local principal_id = get_principal(req)
        local fingerprint = compute_fingerprint(req)
        local endpoint = req.method .. " " .. req.path
        local now = time.now()

        -- Clean up expired keys opportunistically (1 in 100 requests)
        if math.random(1, 100) == 1 then
            db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", { now })
        end

        -- Check for existing key
        local rows = db.query(
            "SELECT fingerprint, state, status, response_body, response_headers, expires_at FROM _hull_idempotency_keys WHERE principal_id = ? AND key = ?",
            { principal_id, key }
        )

        if #rows > 0 then
            local row = rows[1]

            -- Expired: delete and treat as new
            if row.expires_at <= now then
                db.exec("DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND key = ?",
                        { principal_id, key })
            else
                -- Fingerprint mismatch: different request body with same key
                if row.fingerprint ~= fingerprint then
                    res:status(409):json({
                        error = "idempotency key already used with different request body"
                    })
                    return 1
                end

                -- Still in-flight (concurrent request with same key)
                if row.state == "inflight" then
                    res:status(409):json({
                        error = "request with this idempotency key is already in progress"
                    })
                    return 1
                end

                -- Completed: return cached response
                if row.state == "complete" and row.status then
                    res:status(row.status)
                    -- Restore cached headers
                    if row.response_headers then
                        local headers = json.decode(row.response_headers)
                        if headers then
                            for k, v in pairs(headers) do
                                res:header(k, v)
                            end
                        end
                    end
                    res:header("X-Idempotency-Replay", "true")
                    if row.response_body then
                        res:header("Content-Type", "application/json")
                        res:text(row.response_body)
                    else
                        res:text("")
                    end
                    return 1
                end

                -- State is "complete" but no status cached: let handler re-run
                -- (handler didn't use idempotency.respond() last time)
                db.exec("DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND key = ?",
                        { principal_id, key })
            end
        end

        -- Insert in-flight record
        db.exec(
            "INSERT INTO _hull_idempotency_keys (key, principal_id, fingerprint, endpoint, state, created_at, expires_at) VALUES (?, ?, ?, ?, 'inflight', ?, ?)",
            { key, principal_id, fingerprint, endpoint, now, now + ttl }
        )

        -- Store key info in context for idempotency.respond() to use
        req.ctx._idem_key = key
        req.ctx._idem_principal = principal_id

        return 0
    end
end

--- Send a response and cache it for idempotency replay.
-- Must be called from within a handler that has an idempotency key active.
-- data is encoded as JSON.
--
-- Usage:
--   idempotency.respond(req, res, 201, { event_id = 42 })
function idempotency.respond(req, res, status_code, data, extra_headers)
    local body_str = json.encode(data)

    -- Send the actual response
    res:status(status_code)
    if extra_headers then
        for k, v in pairs(extra_headers) do
            res:header(k, v)
        end
    end
    res:json(data)

    -- Cache if idempotency key is active
    if req.ctx._idem_key then
        local headers_str = nil
        if extra_headers then
            headers_str = json.encode(extra_headers)
        end

        db.exec(
            "UPDATE _hull_idempotency_keys SET state = 'complete', status = ?, response_body = ?, response_headers = ? WHERE principal_id = ? AND key = ?",
            { status_code, body_str, headers_str, req.ctx._idem_principal, req.ctx._idem_key }
        )
    end
end

--- Mark an idempotency key as complete without caching a response.
-- Useful when the handler sends a response via res:json() directly.
-- Retries will re-execute the handler (the key prevents concurrent duplicates).
function idempotency.complete(req)
    if req.ctx._idem_key then
        db.exec(
            "UPDATE _hull_idempotency_keys SET state = 'complete' WHERE principal_id = ? AND key = ?",
            { req.ctx._idem_principal, req.ctx._idem_key }
        )
    end
end

--- Delete expired idempotency keys. Returns the number of deleted rows.
-- Call periodically (e.g., alongside session.cleanup()).
function idempotency.cleanup()
    local now = time.now()
    return db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", { now })
end

return idempotency
