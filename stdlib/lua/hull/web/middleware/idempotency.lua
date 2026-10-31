--- Idempotency-Key middleware for safe POST retries.
--
-- Prevents duplicate side effects when clients retry the same request by
-- caching the response keyed by `(principal_id, idempotency_key)`. A
-- `SHA-256(method || path || query || body)` fingerprint detects key reuse with a
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
local encoding = require("hull.encoding")
local db = require("hull.db._internal").connection()
local time = require("hull.time")
local log = require("hull.log")
local _request = require("hull.web._request")
local _text = require("hull._text")

local idempotency = {}

local _ttl = 86400  -- default 24 hours

-- An in-flight key expires after this many seconds (or the TTL, if shorter).
-- A handler that raised never completes its row, and with the full TTL every
-- retry got 409 for a day - defeating the point of an idempotent retry. A
-- completed response keeps the full TTL (completed_expiry).
local INFLIGHT_LEASE = 300

local function completed_expiry(req)
    return time.now() + (req.ctx._idem_ttl or _ttl)
end

-- A completion writes only the claim it belongs to: still in flight, with this
-- request's fingerprint and claim time. A handler that ran past its lease had
-- its row reclaimed by a retry (possibly with another body); completing later,
-- it overwrote that row, so replays of the retry returned the first
-- handler's response.
local CLAIM_GUARD = " AND state = 'inflight' AND fingerprint = ? AND created_at = ?"

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
    -- allowlist addition can't override the deny - JS parity).
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
-- Matches the `key` column width (VARCHAR(255)); keys over this are rejected.
local MAX_KEY_LEN = 255

-- The `key` column, quoted for the connection's dialect (audit 39): KEY is a
-- reserved word in MySQL, where every statement naming it bare - the CREATE
-- TABLE included - was a syntax error, so the middleware never worked there.
-- Quoting keeps the column name existing SQLite / Postgres tables have.
-- Resolved per call: the internal connection is wired after module load.
local function key_col()
    return db.quote_identifier("key")
end

-- The stored response (audit 39). It used to be written AFTER it was sent,
-- as raw text: a body Postgres refuses (a NUL, bytes that are not UTF-8 - a
-- respond_html echoing request bytes) made that UPDATE raise after the
-- handler's side effect had committed. The client got a 500, the row stayed
-- 'inflight' for its lease (a retry got 409), and a retry after the lease ran
-- the handler AGAIN. Now the body and the headers blob are stored base64
-- encoded - byte-exact, and text every backend accepts - BEFORE the response
-- is sent, and a write that still fails marks the row STATE_UNSTORED, whose
-- retries get a fixed 409 instead of running the handler again.
--
-- A row in this format has response_headers = STORED_V2 .. base64(JSON of
-- the headers) and response_body = base64(body bytes). A row an earlier Hull
-- wrote (response_headers a bare JSON object, or NULL) replays as before.
local STORED_V2 = "v2:"
local STATE_UNSTORED = "unstored"
-- The largest response body (bytes) / headers blob stored for replay (audit
-- 40, parity with the JS twin): a larger one marks the row unstored before
-- anything is encoded.
local MAX_STORED_BYTES = 32 * 1024 * 1024
local MAX_STORED_HEADER_BYTES = 256 * 1024
-- Test seam (hull._test_hooks fail_store): makes the store raise.
local _fail_store = false

-- A ttl is a positive, finite NUMBER of seconds (audit 15): a string one
-- ("86400") was coerced here while the JS twin concatenated it onto the
-- expiry. Same rule as the JS twin and auth.login; anything else raises.
--
-- It is also a whole number of seconds no larger than MAX_TTL (366 days;
-- audit 26): an unbounded one made `now + ttl` overflow a 32-bit INTEGER
-- expiry column on PostgreSQL / MySQL (and run past 2038).
local MAX_TTL = 31622400
local function check_ttl(who, v)
    local n = type(v) == "number" and math.tointeger(v) or nil
    if not (n and n > 0 and n <= MAX_TTL) then
        error(who .. " must be a whole number of seconds in [1, "
              .. MAX_TTL .. "]", 3)
    end
    return n
end

--- Initialize the `_hull_idempotency_keys` SQLite table.
--
-- Idempotent - safe to call on every boot.
--
-- @function idempotency.init
-- @tparam[opt] table opts
-- @tparam[opt=86400] number opts.ttl Key lifetime in seconds.
function idempotency.init(opts)
    opts = opts or {}
    if opts.ttl ~= nil then
        _ttl = check_ttl("idempotency.init: ttl", opts.ttl)
    end

    -- MySQL's TEXT holds 64 KiB; a stored body (base64, 4/3 of the bytes)
    -- needs more there. MEDIUMTEXT holds 16 MiB; a body past even that is
    -- marked unstored rather than truncated (strict mode refuses it).
    -- `state` is a VARCHAR: MySQL gives no TEXT column a DEFAULT.
    local body_type = db.backend_name == "mysql" and "MEDIUMTEXT" or "TEXT"
    local k = key_col()
    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_idempotency_keys (
            ]] .. k .. [[ VARCHAR(255) NOT NULL,
            principal_id   VARCHAR(255) NOT NULL DEFAULT '__anon',
            fingerprint    TEXT NOT NULL,
            endpoint       TEXT NOT NULL,
            status         INTEGER,
            response_body  ]] .. body_type .. [[,
            response_headers ]] .. body_type .. [[,
            state          VARCHAR(16) NOT NULL DEFAULT 'inflight',
            created_at     INTEGER NOT NULL,
            expires_at     INTEGER NOT NULL,
            PRIMARY KEY (principal_id, ]] .. k .. [[)
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_idem_expires
        ON _hull_idempotency_keys(expires_at)
    ]])
end

--- The query string as fingerprint input: its pairs sorted by name, each
-- length-prefixed so that no two different queries encode alike.
local function query_part(q)
    if type(q) ~= "table" then return "" end
    local names = {}
    for k in pairs(q) do
        if type(k) == "string" then names[#names + 1] = k end
    end
    table.sort(names)
    local parts = {}
    for _, k in ipairs(names) do
        local v = tostring(q[k])
        parts[#parts + 1] = #k .. ":" .. k .. #v .. ":" .. v
    end
    return table.concat(parts)
end

--- Compute a request fingerprint: SHA-256(method + path + query + body).
-- The query is part of it: without it, reusing a key for
-- `POST /transfer?to=alice` and then `?to=bob` (same body) replayed
-- alice's response instead of answering 409, and bob's transfer was never
-- made.
--
-- Version 2 (audit 41) length-prefixes every field: joined with "\0", a path
-- or query holding a NUL moved bytes from one field to the next, so two
-- different requests hashed alike and a reused key replayed the first one's
-- response. The stored value carries its version ("2:" .. hex); a row an
-- earlier version stored (bare hex) is compared with the old form until it
-- expires, so an upgrade does not answer a client's retry 409.
local function lp(v)
    v = v or ""
    return #v .. ":" .. v
end
local function compute_fingerprint(req)
    local data = "v2\0" .. lp(req.method) .. lp(req.path) .. lp(query_part(req.query))
        .. lp(req.body)
    return "2:" .. encoding.hex.encode(crypto.sha256(data))
end
local function legacy_fingerprint(req)
    local data = (req.method or "") .. "\0" .. (req.path or "") .. "\0"
        .. query_part(req.query) .. "\0" .. (req.body or "")
    return encoding.hex.encode(crypto.sha256(data))
end
-- Whether a stored fingerprint is this request's (either version).
local function fingerprint_matches(stored, fingerprint, req)
    stored = stored or ""
    if stored:sub(1, 2) ~= "2:" then
        return crypto.constant_time_eq(stored, legacy_fingerprint(req))
    end
    return crypto.constant_time_eq(stored, fingerprint)
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
--   Returns a stable per-user key for scoping. Default: `"session:" ..` the
--   session's `user_id`, else `"user:" ..` the JWT user's `sub` / `id` /
--   `user_id` (`req.ctx.user`, set by auth.jwt_middleware), else `"__anon"` -
--   which every anonymous caller shares, so mount it after authentication
--   (it logs a warning once when 20 keyed requests in a row were all anonymous).
--   A CUSTOM get_principal that returns nil / false / "" does not share
--   "__anon" (audit 11): the request skips the idempotency layer - the
--   handler runs, nothing is stored or replayed - and a warning is logged
--   once per middleware. Return "__anon" explicitly to share one scope.
-- @tparam[opt] number opts.ttl  Override module-level TTL for this instance.
-- @tparam[opt="idempotency-key"] string opts.header_name  Header to read.
-- @tparam[opt={"POST"}] table opts.methods  Methods to intercept.
-- @treturn function(req, res) -> 0|1
-- The default principal: "session:<user_id>" for a session user, else
-- "user:<id>" for a JWT user (`req.ctx.user`, auth.jwt_middleware), else
-- "__anon". The two kinds of user are prefixed apart (audit 9): a session
-- user_id was used bare, so a session user whose id was the string "user:5"
-- shared JWT user 5's principal and could replay or squat their keys.
--
-- Both ids go through hull.web._request.user_id, as the JS twin's do (audit
-- 23): "" is no id, 42.0 is "42", a table is no id. The JWT id is the first
-- of sub / id / user_id that is one.
local function default_principal(req)
    local ctx = req.ctx
    local sess = type(ctx) == "table" and ctx.session or nil
    if type(sess) == "table" then
        local sid = _request.user_id(sess.user_id)
        if sid then return "session:" .. sid end
    end
    -- A JWT-authenticated user (auth.jwt_middleware). Without this every
    -- bearer-token user shared "__anon", so one could replay another's
    -- stored response, or squat their key, by knowing key and body.
    local u = type(ctx) == "table" and ctx.user or nil
    if type(u) == "table" then
        for _, k in ipairs({ "sub", "id", "user_id" }) do
            local id = _request.user_id(u[k])
            if id then return "user:" .. id end
        end
    end
    return "__anon"
end

-- The principal as the principal_id column (VARCHAR(255)) stores it, for the
-- default and a custom get_principal alike (audit 10). A number reached the
-- column as a number, and MySQL then compared the VARCHAR column numerically
-- ('5abc' = 5); a principal over 255 characters failed the insert on
-- Postgres and was truncated by a lax MySQL - two principals sharing a
-- prefix shared their keys. So: nil / false / "" is "__anon", an integer is
-- its decimal string, anything over the column width (or already spelled
-- like a hashed one) becomes "sha256:<hex>" of itself, and any other type is
-- an error.
local function norm_principal(p)
    if p == nil or p == false or p == "" then return "__anon" end
    if type(p) == "number" then
        p = math.type(p) == "integer" and tostring(p) or string.format("%.17g", p)
    elseif type(p) ~= "string" then
        error("idempotency: get_principal must return a string or a number, got "
              .. type(p))
    end
    -- Text the column cannot store as is (a NUL, bytes that are not UTF-8;
    -- audit 39) is hashed too: Postgres / MySQL refused the claim's INSERT.
    if #p > MAX_KEY_LEN or p:sub(1, 7) == "sha256:" or p:find("\0", 1, true)
       or not utf8.len(p) then
        return "sha256:" .. encoding.hex.encode(crypto.sha256(p))
    end
    return p
end

-- How many keyed requests in a row may all be "__anon" before the default
-- principal warns (once per middleware) that it is mounted where no
-- authentication ran: every caller then shares one principal.
local ANON_WARN_AFTER = 20

function idempotency.middleware(opts)
    opts = opts or {}

    local anon_run, anon_warned = 0, false
    local get_principal = opts.get_principal or function(req)
        local p = default_principal(req)
        if p ~= "__anon" then
            anon_run = 0
        elseif not anon_warned then
            anon_run = anon_run + 1
            if anon_run >= ANON_WARN_AFTER then
                anon_warned = true
                log.warn("idempotency: the last " .. ANON_WARN_AFTER .. " keyed "
                    .. "requests had no session or JWT user, so they all share the "
                    .. "'__anon' principal (one caller can replay another's stored "
                    .. "response). Mount the middleware after authentication, or "
                    .. "pass get_principal.")
            end
        end
        return p
    end

    -- L-3: dropped the leading underscore - Lua convention reserves `_x`
    -- for unused locals; this counter is actively mutated below.
    local cleanup_counter = 0
    local ttl = opts.ttl ~= nil and check_ttl("idempotency.middleware: ttl", opts.ttl) or _ttl
    -- req.headers keys are lowercased by the runtime, so a mixed-case
    -- header_name ("Idempotency-Key", what JS's req.header() accepts) never
    -- matched: every retry ran the handler again (audit 23).
    local header_name = opts.header_name or _header_name
    if type(header_name) ~= "string" or header_name == "" then
        error("idempotency.middleware: header_name must be a non-empty string")
    end
    header_name = header_name:lower()

    -- Build methods lookup
    local method_list = opts.methods or { "POST" }
    local methods = {}
    for _, m in ipairs(method_list) do
        methods[m] = true
    end

    local custom = opts.get_principal ~= nil
    local custom_warned = false

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
        -- The key is the PK's client-supplied half (VARCHAR(255)). Reject an
        -- over-length key rather than let MySQL silently truncate it, which
        -- would collide two distinct keys sharing a 255-char prefix and replay
        -- the wrong cached response. Uniform across every backend.
        if #key > MAX_KEY_LEN then
            res:status(400):json({ error = "idempotency-key too long (max " .. MAX_KEY_LEN .. ")" })
            return 1
        end
        -- Printable ASCII only (audit 39): a key with a NUL or bytes that
        -- are not UTF-8 failed the claim's INSERT on Postgres / MySQL - a
        -- 500 for a client error. Keys are opaque client tokens (UUIDs in
        -- practice); obs-text has no business in one.
        if key:find("[^\32-\126]") then
            res:status(400):json({ error = "idempotency-key must be printable ASCII" })
            return 1
        end

        local raw_principal = get_principal(req)
        -- A custom get_principal with no answer (audit 11): sharing "__anon"
        -- let every caller it could not identify replay or squat each
        -- other's keys - most likely a lookup that failed, not a choice. The
        -- request runs without idempotency instead.
        if custom and (raw_principal == nil or raw_principal == false
                       or raw_principal == "") then
            if not custom_warned then
                custom_warned = true
                log.warn("idempotency: get_principal returned "
                    .. tostring(raw_principal) .. "; the request runs without "
                    .. "idempotency (return \"__anon\" to share one scope)")
            end
            return 0
        end
        local principal_id = norm_principal(raw_principal)
        local fingerprint = compute_fingerprint(req)
        -- The path is request text: stored only for inspection, as text
        -- every backend accepts (audit 39).
        local endpoint = _text.storable(tostring(req.method) .. " " .. tostring(req.path), 1024, true)
        local now = time.now()
        local K = key_col()

        -- Clean up expired keys periodically (every 100 requests)
        cleanup_counter = cleanup_counter + 1
        if cleanup_counter >= 100 then
            cleanup_counter = 0
            db.exec("DELETE FROM _hull_idempotency_keys WHERE expires_at <= ?", { now })
        end

        -- Check for existing key
        local rows = db.query(
            "SELECT fingerprint, state, status, response_body, response_headers, expires_at FROM _hull_idempotency_keys WHERE principal_id = ? AND " .. K .. " = ?",
            { principal_id, key }
        )

        if #rows > 0 then
            local row = rows[1]

            -- Expired: delete and treat as new. Only while it is still
            -- expired: a concurrent request may have replaced it with a fresh
            -- claim since the SELECT, and an unguarded DELETE removed that.
            if row.expires_at <= now then
                db.exec("DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND " .. K .. " = ? "
                        .. "AND expires_at <= ?",
                        { principal_id, key, now })
            else
                -- Fingerprint comparison. Fingerprints are SHA-256(public
                -- inputs) - not secrets - so timing leaks are not exploitable.
                -- We still use the C constant-time compare (crypto.constant_time_eq)
                -- for consistency with jwt.lua/csrf.lua so the comparison contract
                -- is one audited implementation everywhere in the stdlib.
                if not fingerprint_matches(row.fingerprint, fingerprint, req) then
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

                -- Ran, but its response could not be stored (audit 39): the
                -- side effect happened, so the handler must not run again,
                -- and there is nothing to replay.
                if row.state == STATE_UNSTORED then
                    res:status(409):json({
                        error = "request with this idempotency key was already "
                            .. "processed; its response could not be stored for replay"
                    })
                    return 1
                end

                -- Completed: return cached response
                if row.state == "complete" and row.status then
                    -- A v2 row holds both blobs base64 encoded; an older row
                    -- the raw text. An undecodable v2 row (tampered) replays
                    -- an empty body rather than raising.
                    local headers_json, body = row.response_headers, row.response_body
                    if type(headers_json) == "string"
                       and headers_json:sub(1, #STORED_V2) == STORED_V2 then
                        headers_json = encoding.base64.decode(headers_json:sub(#STORED_V2 + 1))
                        body = type(body) == "string" and encoding.base64.decode(body) or nil
                    end
                    res:status(row.status)
                    -- Restore cached headers (M-3: allowlist + CRLF reject).
                    -- Content-Type is one of these - respond() and
                    -- respond_html() both stash it in the headers blob -
                    -- so the cached value drives the replayed mime type.
                    local replayed_ct = nil
                    if type(headers_json) == "string" then
                        local okd, headers = pcall(json.decode, headers_json)
                        if okd and type(headers) == "table" then
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
                    if body then
                        -- Default to application/json for back-compat with
                        -- old cached rows from before respond_html() existed.
                        if not replayed_ct then
                            res:header("Content-Type", "application/json")
                        end
                        res:text(body)
                    else
                        res:text("")
                    end
                    return 1
                end

                -- State is "complete" but no status cached: let handler re-run
                -- (handler didn't use idempotency.respond() last time)
                db.exec("DELETE FROM _hull_idempotency_keys WHERE principal_id = ? AND " .. K .. " = ? "
                        .. "AND state = 'complete' AND status IS NULL",
                        { principal_id, key })
            end
        end

        -- Insert in-flight record (insert-if-absent guards the race with
        -- a concurrent request claiming the same key).
        local inserted = db.insert_if_absent(
            "_hull_idempotency_keys",
            { "principal_id", K },
            { K, "principal_id", "fingerprint", "endpoint",
              "state", "created_at", "expires_at" },
            { key, principal_id, fingerprint, endpoint,
              "inflight", now, now + math.min(ttl, INFLIGHT_LEASE) }
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
        req.ctx._idem_ttl = ttl
        -- This request's claim, for the completing UPDATE (see claim_guard).
        req.ctx._idem_fingerprint = fingerprint
        req.ctx._idem_created = now

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
        -- Stored BEFORE the response is sent, base64 encoded (audit 39; see
        -- STORED_V2). A write that still fails (a body past the column, a
        -- lost connection) marks the row unstored - a retry gets 409, never
        -- a second run of the handler - and the response is sent anyway.
        local where = " WHERE principal_id = ? AND " .. key_col() .. " = ?" .. CLAIM_GUARD
        local p, k = req.ctx._idem_principal, req.ctx._idem_key
        local fp, created = req.ctx._idem_fingerprint, req.ctx._idem_created
        local ok, err = pcall(function()
            if _fail_store then error("injected store failure", 0) end
            local body_raw = type(body_str) == "string" and body_str or tostring(body_str or "")
            if #body_raw > MAX_STORED_BYTES then error("response too large to store", 0) end
            local headers_raw = json.encode(filtered)
            if #headers_raw > MAX_STORED_HEADER_BYTES then
                error("response headers too large to store", 0)
            end
            local headers_str = STORED_V2 .. encoding.base64.encode(headers_raw)
            local body_enc = encoding.base64.encode(body_raw)
            db.exec(
                "UPDATE _hull_idempotency_keys SET state = 'complete', status = ?, "
                .. "response_body = ?, response_headers = ?, expires_at = ?" .. where,
                { status_code, body_enc, headers_str, completed_expiry(req),
                  p, k, fp, created })
        end)
        if not ok then
            log.warn("idempotency: the response could not be stored for replay ("
                .. _text.storable(err, 512, true) .. "); retries of this key get 409")
            -- Called by name inside a closure: the _hull_* namespace check
            -- admits a stdlib call only when it names the method.
            local ok2, err2 = pcall(function()
                db.exec(
                    "UPDATE _hull_idempotency_keys SET state = '" .. STATE_UNSTORED .. "', "
                    .. "status = NULL, response_body = NULL, response_headers = NULL, "
                    .. "expires_at = ?" .. where,
                    { completed_expiry(req), p, k, fp, created })
            end)
            if not ok2 then
                log.error("idempotency: the key could not be marked processed ("
                    .. _text.storable(err2, 512, true) .. "); a retry after the "
                    .. INFLIGHT_LEASE .. " s lease runs the handler again")
            end
        end
    end

    res:status(status_code)
    if extra_headers then
        for k, v in pairs(extra_headers) do
            if is_replayable_header(k) and header_value_safe(v) then
                res:header(k, v)
            end
        end
    end
    send_fn(res, body_str)
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
            "UPDATE _hull_idempotency_keys SET state = 'complete', expires_at = ? WHERE principal_id = ? AND "
            .. key_col() .. " = ?" .. CLAIM_GUARD,
            { completed_expiry(req), req.ctx._idem_principal, req.ctx._idem_key,
              req.ctx._idem_fingerprint, req.ctx._idem_created }
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

-- Test-only hooks, registered with the stdlib-internal hull._test_hooks
-- rather than exported (audit 16): the suite reads a key's row back from
-- inside the module, where the _hull_* namespace check admits the query.
require("hull._test_hooks").register("hull.web.middleware.idempotency", {
    rows = function(key)
        return db.query("SELECT state, status, response_body, response_headers "
                        .. "FROM _hull_idempotency_keys WHERE " .. key_col() .. " = ?",
                        { key })
    end,
    -- The principal helpers (audit 23: they were exported on the module).
    default_principal = default_principal,
    norm_principal = norm_principal,
    fingerprint = compute_fingerprint,
    -- Make the response store fail (audit 39: a failed store must leave the
    -- key processed, never re-runnable).
    fail_store = function(on) _fail_store = on and true or false end,
    -- A completed row in the format an earlier Hull wrote (raw body, bare
    -- JSON headers), for the backward-compatibility check.
    put_legacy = function(key, principal, fingerprint, status, body, headers_json)
        local now = time.now()
        db.exec("INSERT INTO _hull_idempotency_keys (" .. key_col() .. ", principal_id, "
            .. "fingerprint, endpoint, status, response_body, response_headers, state, "
            .. "created_at, expires_at) VALUES (?, ?, ?, 'POST /legacy', ?, ?, ?, "
            .. "'complete', ?, ?)",
            { key, principal, fingerprint, status, body, headers_json, now, now + 3600 })
    end,
})

return idempotency
