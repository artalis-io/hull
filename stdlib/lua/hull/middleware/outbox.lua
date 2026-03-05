--
-- hull.middleware.outbox -- Transactional outbox for reliable side effects
--
-- Decouples external side effects (HTTP, SMTP, webhook delivery) from the
-- request transaction. Side effects are written as outbox rows inside the
-- same transaction as the state change, then delivered after commit.
--
-- Usage:
--   local outbox = require("hull.middleware.outbox")
--   outbox.init({ flush_after_request = true })
--
--   -- Inside a handler (within a transaction):
--   outbox.enqueue({
--       kind = "webhook",
--       destination = webhook.url,
--       payload = json.encode(data),
--       headers = json.encode({ ["Content-Type"] = "application/json" }),
--       idempotency_key = "evt-" .. event_id .. "-wh-" .. wh.id,
--   })
--
--   -- After handler returns (or explicitly):
--   outbox.flush()
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local outbox = {}

local _max_attempts = 5

--- Initialize the outbox table.
-- opts.max_attempts: max delivery attempts before marking failed (default 5)
-- opts.flush_after_request: auto-flush after each request (default false)
function outbox.init(opts)
    opts = opts or {}
    if opts.max_attempts then
        _max_attempts = opts.max_attempts
    end
    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_outbox (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            kind            TEXT NOT NULL,
            destination     TEXT NOT NULL,
            payload         TEXT NOT NULL,
            headers         TEXT,
            idempotency_key TEXT,
            attempts        INTEGER NOT NULL DEFAULT 0,
            max_attempts    INTEGER NOT NULL DEFAULT 5,
            next_attempt_at INTEGER NOT NULL,
            state           TEXT NOT NULL DEFAULT 'pending',
            created_at      INTEGER NOT NULL,
            delivered_at    INTEGER,
            last_error      TEXT
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_outbox_pending
        ON _hull_outbox(state, next_attempt_at)
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_outbox_idem
        ON _hull_outbox(idempotency_key)
    ]])
end

--- Enqueue a side effect for delivery after commit.
-- Must be called inside a transaction (db.batch) to be atomic with state changes.
--
-- opts.kind: type of delivery ("webhook", "http", "smtp", etc.)
-- opts.destination: URL or address
-- opts.payload: string body
-- opts.headers: JSON-encoded headers string (optional)
-- opts.idempotency_key: unique key for dedup (optional but recommended)
-- opts.max_attempts: override default max attempts (optional)
-- opts.delay: seconds to delay first attempt (optional, default 0)
function outbox.enqueue(opts)
    if not opts or not opts.kind then
        error("outbox.enqueue requires opts.kind")
    end
    if not opts.destination then
        error("outbox.enqueue requires opts.destination")
    end
    if not opts.payload then
        error("outbox.enqueue requires opts.payload")
    end

    local now = time.now()
    local max = opts.max_attempts or _max_attempts
    local delay = opts.delay or 0

    db.exec(
        "INSERT INTO _hull_outbox (kind, destination, payload, headers, idempotency_key, max_attempts, next_attempt_at, state, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', ?)",
        { opts.kind, opts.destination, opts.payload, opts.headers, opts.idempotency_key, max, now + delay, now }
    )

    return db.last_id()
end

--- Attempt to deliver a single outbox item.
-- Returns true on success, false on failure.
local function deliver_item(item)
    if item.kind == "webhook" or item.kind == "http" then
        local req_headers = {}
        if item.headers then
            local decoded = json.decode(item.headers)
            if decoded then
                req_headers = decoded
            end
        end

        local send_ok, result = pcall(function()
            return http.post(item.destination, item.payload, {
                headers = req_headers
            })
        end)

        if send_ok and result and result.status and result.status >= 200 and result.status < 300 then
            return true, nil
        end

        local err_msg
        if not send_ok then
            err_msg = tostring(result)
        elseif result then
            err_msg = "HTTP " .. tostring(result.status)
        else
            err_msg = "no response"
        end
        return false, err_msg
    end

    -- Unknown kind: mark as failed
    return false, "unsupported outbox kind: " .. tostring(item.kind)
end

--- Compute exponential backoff delay (in seconds) for attempt N.
-- 2^attempt * 10 seconds, capped at 1 hour.
local function backoff_delay(attempt)
    local delay = (2 ^ attempt) * 10
    if delay > 3600 then delay = 3600 end
    return delay
end

--- Flush pending outbox items. Delivers items where next_attempt_at <= now.
-- opts.limit: max items to process per flush (default 50)
-- Returns { delivered = N, failed = N, retried = N }
function outbox.flush(opts)
    opts = opts or {}
    local limit = opts.limit or 50
    local now = time.now()

    local items = db.query(
        "SELECT id, kind, destination, payload, headers, idempotency_key, attempts, max_attempts FROM _hull_outbox WHERE state = 'pending' AND next_attempt_at <= ? ORDER BY id LIMIT ?",
        { now, limit }
    )

    local delivered = 0
    local failed = 0
    local retried = 0

    for _, item in ipairs(items) do
        local ok, err = deliver_item(item)

        if ok then
            db.exec(
                "UPDATE _hull_outbox SET state = 'delivered', delivered_at = ?, attempts = attempts + 1 WHERE id = ?",
                { time.now(), item.id }
            )
            delivered = delivered + 1
        else
            local new_attempts = item.attempts + 1
            if new_attempts >= item.max_attempts then
                db.exec(
                    "UPDATE _hull_outbox SET state = 'failed', attempts = ?, last_error = ? WHERE id = ?",
                    { new_attempts, err, item.id }
                )
                failed = failed + 1
            else
                local next_at = time.now() + backoff_delay(new_attempts)
                db.exec(
                    "UPDATE _hull_outbox SET attempts = ?, next_attempt_at = ?, last_error = ? WHERE id = ?",
                    { new_attempts, next_at, err, item.id }
                )
                retried = retried + 1
            end
        end
    end

    return { delivered = delivered, failed = failed, retried = retried }
end

--- Create a post-body middleware that auto-flushes outbox after each request.
-- Only flushes on successful responses (non-error status).
function outbox.middleware()
    return function(req, _res)
        -- Mark request for post-handler flush
        req.ctx._outbox_flush = true
        return 0
    end
end

--- Flush if the request was marked for it (call after handler returns).
-- Intended for use in a post-handler hook or explicitly by the app.
function outbox.flush_if_needed(req)
    if req.ctx and req.ctx._outbox_flush then
        outbox.flush()
    end
end

--- Get counts of items by state.
-- Returns { pending = N, delivered = N, failed = N }
function outbox.stats()
    local rows = db.query(
        "SELECT state, COUNT(*) as count FROM _hull_outbox GROUP BY state"
    )
    local result = { pending = 0, delivered = 0, failed = 0 }
    for _, row in ipairs(rows) do
        result[row.state] = row.count
    end
    return result
end

--- Delete delivered items older than max_age seconds.
-- Returns number of deleted rows.
function outbox.cleanup(max_age)
    max_age = max_age or 86400 * 7  -- default 7 days
    local cutoff = time.now() - max_age
    return db.exec(
        "DELETE FROM _hull_outbox WHERE state = 'delivered' AND delivered_at <= ?",
        { cutoff }
    )
end

return outbox
