--- Idempotency-Key middleware for safe POST retries.
--
-- Prevents duplicate side effects when clients retry the same request by
-- caching the response keyed by `(principal_id, idempotency_key)`. A
-- `SHA-256(method || path || body)` fingerprint detects key reuse with a
-- different body (returns `409 Conflict`).
--
-- @par State table
--   `_hull_idempotency_keys`. Schema is created by `idempotency.init()`.
--   Entries auto-expire after `opts.ttl` seconds. Call
--   `idempotency.cleanup()` periodically to reclaim space; the middleware
--   also opportunistically deletes expired rows every 100 invocations.
--
-- @par Replay semantics
--   - Same key, same body → cached response returned (handler skipped).
--   - Same key, different body → `409`.
--   - Same key, still inflight → `409` (`request already in progress`).
--   - No `Idempotency-Key` header → handler runs normally (no caching).
--
-- @par Header allowlist (M-3)
--   Only safe response headers are persisted/replayed. `Set-Cookie`,
--   `Authorization`, `Cookie`, and a deny-list of `X-*` credential
--   patterns (`x-auth`, `x-api-key`, …) are never cached, so a stored
--   replay can't outlive a revoked session.
--
-- @module hull.web.middleware.idempotency
-- @license AGPL-3.0-or-later
-- @usage
--   local idempotency = require("hull.web.middleware.idempotency")
--   idempotency.init({ ttl = 86400 })
--   app.use_post("POST", "/api/*", idempotency.middleware({
--       get_principal = function(req)
--           return req.ctx.session and req.ctx.session.user_id or "__anon"
--       end,
--   }))
--
--   app.post("/api/events", function(req, res)
--       -- ... do work inside db.batch() ...
--       idempotency.respond(req, res, 201, { event_id = id })
--   end)
--

local json = require("hull.json")
local crypto = require("hull.crypto")
local db = require("hull.db")
local time = require("hull.time")

local idempotency = {}

local _ttl = 86400  -- default 24 hours

-- M-3: allowlist of headers safe to replay/cache. Excludes credential
-- and session-binding headers (Set-Cookie, WWW-Authenticate, etc.) so a
-- stored Set-Cookie can't outlive a revoked session.
--
-- Phase 6 audit M-2: previously this allowlist included a blanket `x-*`
-- catch-all, which permitted X-Auth-Token, X-API-Key, X-CSRF-Token,
-- X-Forwarded-Authorization, X-Amz-Security-Token and other common
-- credential-bearing headers. Removed the blanket and replaced with an
-- explicit allowlist of safe X-* headers + an explicit deny-list of
-- credential X-* patterns.
local REPLAYABLE_HEADERS = {
    ["content-type"]        = true,
    ["content-language"]    = true,
    ["content-encoding"]    = true,
    ["location"]            = true,
    ["etag"]                = true,
    ["last-modified"]       = true,
    ["cache-control"]       = true,
    ["vary"]                = true,
    -- Safe X-* headers (stdlib-emitted or widely-used non-credential):
    ["x-request-id"]        = true,
    ["x-ratelimit-limit"]   = true,
    ["x-ratelimit-remaining"] = true,
    ["x-ratelimit-reset"]   = true,
    ["x-idempotency-replay"] = true,
    ["x-content-type-options"] = true,
    ["x-frame-options"]     = true,
    -- Other safe response-shaping headers:
    ["strict-transport-security"] = true,
    ["content-security-policy"]   = true,
    ["referrer-policy"]     = true,
    ["permissions-policy"]  = true,
}

-- X-* credential substrings we explicitly deny. Module-level so the
-- table is created once, not per-call.
local X_CREDENTIAL_PATTERNS = {
    "x-auth", "x-api-key", "x-csrf", "x-token",
    "x-forwarded-authorization", "x-amz-security-token",
    "x-aws-", "x-google-", "x-vault-", "x-jwt-",
}

local function is_replayable_header(name)
    if type(name) ~= "string" or name == "" then return false end
    local lc = name:lower()
    -- Always-deny credential headers (run FIRST so an accidental
    -- allowlist addition can't override the deny — JS parity).
    if lc == "set-cookie" or lc == "authorization" or lc == "cookie" or
       lc == "proxy-authenticate" or lc == "www-authenticate" then
        return false
    end
    for _, deny in ipairs(X_CREDENTIAL_PATTERNS) do
        if lc:find(deny, 1, true) then return false end
    end
    -- Then check allowlist.
    if REPLAYABLE_HEADERS[lc] then return true end
    -- Anything not on the allowlist is denied (no blanket X-*).
    return false
end

local function header_value_safe(v)
    if type(v) ~= "string" then return false end
    -- Reject CRLF / NUL injection.
    return v:find("[\r\n%z]") == nil
end
local _header_name = "idempotency-key"

--- Initialize the `_hull_idempotency_keys` SQLite table.
--
-- Idempotent — safe to call on every boot.
--
-- @function idempotency.init
-- @tparam[opt] table opts
-- @tparam[opt=86400] number opts.ttl Key lifetime in seconds.
function idempotency.init(opts)
    opts = opts or {}
    -- M-2: explicit nil check; opts.ttl == 0 must override the default.
    if opts.ttl ~= nil then
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

--- Build a post-body idempotency middleware.
--
-- Reads the idempotency key from `req.headers[header_name]`. On retry
-- with the same key+body the cached response is replayed; with a
-- different body a `409` is returned. Without the header the handler
-- runs normally.
--
-- @function idempotency.middleware
-- @tparam[opt] table opts
-- @tparam[opt] function(req)->string opts.get_principal
--   Returns a stable per-user key for scoping. Default:
--   `req.ctx.session.user_id` or `"__anon"`.
-- @tparam[opt] number opts.ttl  Override module-level TTL for this instance.
-- @tparam[opt="idempotency-key"] string opts.header_name  Header to read.
-- @tparam[opt={"POST"}] table opts.methods  Methods to intercept.
-- @treturn function(req, res) -> 0|1
function idempotency.middleware(opts)
    opts = opts or {}

    local get_principal = opts.get_principal or function(req)
        if req.ctx and req.ctx.session and req.ctx.session.user_id then
            return tostring(req.ctx.session.user_id)
        end
        return "__anon"
    end

    -- L-3: dropped the leading underscore — Lua convention reserves `_x`
    -- for unused locals; this counter is actively mutated below.
    local cleanup_counter = 0
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

        -- Clean up expired keys periodically (every 100 requests)
        cleanup_counter = cleanup_counter + 1
        if cleanup_counter >= 100 then
            cleanup_counter = 0
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
                -- Fingerprint comparison. Fingerprints are SHA-256(public
                -- inputs) — not secrets — so timing leaks are not exploitable.
                -- We still use a constant-time loop for consistency with jwt.lua
                -- and csrf.lua so the comparison contract is the same everywhere
                -- in the stdlib.
                local diff = (row.fingerprint and #row.fingerprint or 0) == #fingerprint and 0 or 1
                local n = math.min(#fingerprint, row.fingerprint and #row.fingerprint or 0)
                for i = 1, n do
                    diff = diff | (string.byte(row.fingerprint, i) ~ string.byte(fingerprint, i))
                end
                if diff ~= 0 then
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
                    -- Restore cached headers (M-3: allowlist + CRLF reject).
                    -- Content-Type is one of these — respond() and
                    -- respond_html() both stash it in the headers blob —
                    -- so the cached value drives the replayed mime type.
                    local replayed_ct = nil
                    if row.response_headers then
                        local headers = json.decode(row.response_headers)
                        if headers then
                            for k, v in pairs(headers) do
                                if is_replayable_header(k) and header_value_safe(v) then
                                    res:header(k, v)
                                    if k:lower() == "content-type" then
                                        replayed_ct = v
                                    end
                                end
                            end
                        end
                    end
                    res:header("X-Idempotency-Replay", "true")
                    if row.response_body then
                        -- Default to application/json for back-compat with
                        -- old cached rows from before respond_html() existed.
                        if not replayed_ct then
                            res:header("Content-Type", "application/json")
                        end
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

        -- Insert in-flight record (OR IGNORE prevents race with concurrent request)
        local inserted = db.exec(
            "INSERT OR IGNORE INTO _hull_idempotency_keys (key, principal_id, fingerprint, endpoint, state, created_at, expires_at) VALUES (?, ?, ?, ?, 'inflight', ?, ?)",
            { key, principal_id, fingerprint, endpoint, now, now + ttl }
        )
        if inserted == 0 then
            -- Another request claimed this key between our SELECT and INSERT
            res:status(409):json({
                error = "request with this idempotency key is already in progress"
            })
            return 1
        end

        -- Store key info in context for idempotency.respond() to use
        req.ctx._idem_key = key
        req.ctx._idem_principal = principal_id

        return 0
    end
end

--- Send a JSON response and cache it for idempotency replay.
--
-- Call from inside a handler that has an idempotency key active (see
-- `idempotency.middleware`). `data` is JSON-encoded; on cache hit the
-- middleware will replay the encoded body verbatim.
--
-- `extra_headers` is filtered through the same allowlist used by the
-- replay path, so credential headers never persist on disk.
--
-- @function idempotency.respond
-- @tparam table  req           The request object.
-- @tparam table  res           The response object.
-- @tparam number status_code   HTTP status to send + cache.
-- @tparam any    data          Body data (JSON-encoded).
-- @tparam[opt]  table extra_headers Map of `Header-Name -> value`.
-- @usage
--   idempotency.respond(req, res, 201, { event_id = 42 })
-- Shared cache-write path. `body_str` is the literal response bytes;
-- `content_type` is folded into the cached headers blob so the replay
-- path serves the response with the correct mime.
local function cache_and_send(req, res, status_code, body_str, content_type, extra_headers, send_fn)
    res:status(status_code)
    if extra_headers then
        for k, v in pairs(extra_headers) do
            if is_replayable_header(k) and header_value_safe(v) then
                res:header(k, v)
            end
        end
    end
    send_fn(res, body_str)

    if req.ctx._idem_key then
        -- Phase 6 audit M-3: filter headers through the allowlist
        -- before writing to SQLite so credential headers never
        -- persist on disk for the TTL.
        --
        -- All keys are normalised to lowercase before stash. The
        -- replay path matches case-insensitively on read, so a
        -- caller passing `Content-Type` (mixed) and our own
        -- `content-type` (lower) would otherwise both land in the
        -- blob and emit two headers on replay. Lowercase keeps the
        -- blob clean and matches the convention of every HTTP
        -- spec since 7230.
        local filtered = { ["content-type"] = content_type }
        if extra_headers then
            for k, v in pairs(extra_headers) do
                if is_replayable_header(k) and header_value_safe(v) then
                    filtered[k:lower()] = v
                end
            end
        end
        local headers_str = json.encode(filtered)
        db.exec(
            "UPDATE _hull_idempotency_keys SET state = 'complete', status = ?, response_body = ?, response_headers = ? WHERE principal_id = ? AND key = ?",
            { status_code, body_str, headers_str, req.ctx._idem_principal, req.ctx._idem_key }
        )
    end
end

function idempotency.respond(req, res, status_code, data, extra_headers)
    cache_and_send(req, res, status_code,
        json.encode(data), "application/json",
        extra_headers,
        function(r, _) r:json(data) end)
end

--- Send an HTML response and cache it for idempotency replay.
--
-- HTMX form retries against the same `Idempotency-Key` get the cached
-- HTML fragment back verbatim, with `Content-Type: text/html;
-- charset=utf-8`. Without this helper the replay path would serve the
-- cached body as JSON.
--
-- `extra_headers` is filtered through the same allowlist used by the
-- replay path, so credential headers never persist on disk.
--
-- @function idempotency.respond_html
-- @tparam table  req           The request object.
-- @tparam table  res           The response object.
-- @tparam number status_code   HTTP status to send + cache.
-- @tparam string html          Response body (HTML).
-- @tparam[opt]  table extra_headers Map of `Header-Name -> value`.
function idempotency.respond_html(req, res, status_code, html, extra_headers)
    cache_and_send(req, res, status_code,
        html, "text/html; charset=utf-8",
        extra_headers,
        function(r, body) r:html(body) end)
end

--- Mark an idempotency key as complete without caching a response.
--
-- Useful when the handler sends a response via `res:json()` directly.
-- The key prevents concurrent duplicates but retries will re-execute the
-- handler (no cached body to replay).
--
-- @function idempotency.complete
-- @tparam table req The request object.
function idempotency.complete(req)
    if req.ctx._idem_key then
        db.exec(
            "UPDATE _hull_idempotency_keys SET state = 'complete' WHERE principal_id = ? AND key = ?",
            { req.ctx._idem_principal, req.ctx._idem_key }
        )
    end
end

--- Delete expired idempotency keys.
--
-- Call periodically (e.g. inside `app.every(3600, idempotency.cleanup)`).
--
-- @function idempotency.cleanup
-- @treturn number Count of deleted rows.
function idempotency.cleanup()
    local now = time.now()
    return db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", { now })
end

return idempotency
