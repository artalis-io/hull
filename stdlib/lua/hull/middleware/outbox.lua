--- Transactional outbox for reliable side-effect delivery.
--
-- Decouples external side effects (webhooks, HTTP calls, SMTP) from the
-- request transaction. The handler enqueues an outbox row inside the same
-- `db.batch` that commits the state change; the outbox flusher then
-- delivers the row after commit with exponential backoff retries.
--
-- Delivery is **at-least-once** — a crash between deliver-and-mark can
-- re-deliver an item. Design receivers to be idempotent (e.g. with
-- `hull.middleware.inbox`).
--
-- @par Backoff schedule
--   `2^attempt * 10s`, capped at 1 hour. After `max_attempts` (default
--   5) the row is marked `failed`.
--
-- @par State table
--   `_hull_outbox`. Schema is created by `outbox.init()`.
--
-- @module hull.middleware.outbox
-- @license AGPL-3.0-or-later
-- @usage
--   local outbox = require("hull.middleware.outbox")
--   outbox.init({ max_attempts = 5 })
--
--   -- Inside a handler, within a transaction:
--   outbox.enqueue({
--       kind            = "webhook",
--       destination     = webhook.url,
--       payload         = json.encode(data),
--       headers         = json.encode({ ["Content-Type"] = "application/json" }),
--       idempotency_key = "evt-" .. event_id .. "-wh-" .. wh.id,
--   })
--
--   -- Periodic flush from a timer:
--   app.every(30000, outbox.flush)
--

local json = require("hull.json")
local db = require("hull.db")
local http = require("hull.http-client")
local time = require("hull.time")

local outbox = {}

local _max_attempts = 5

--- Initialize the `_hull_outbox` table.
--
-- Idempotent — safe to call on every boot.
--
-- @function outbox.init
-- @tparam[opt] table opts
-- @tparam[opt=5] number opts.max_attempts  Max delivery attempts before
--   the row is marked `failed`.
function outbox.init(opts)
    opts = opts or {}
    -- M-2: explicit nil check; opts.max_attempts == 0 (no retries) must
    -- override the default.
    if opts.max_attempts ~= nil then
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
--
-- Must be called inside a `db.batch` to be atomic with the state change.
-- Returns the new row id (use it to enforce per-row idempotency in the
-- receiver).
--
-- @function outbox.enqueue
-- @tparam table opts
-- @tparam string opts.kind         `"webhook"`, `"http"`, `"smtp"`, ...
-- @tparam string opts.destination  URL or address.
-- @tparam string opts.payload      Body bytes.
-- @tparam[opt]  string opts.headers          JSON-encoded headers.
-- @tparam[opt]  string opts.idempotency_key  Dedup key (recommended).
-- @tparam[opt]  number opts.max_attempts     Override module default.
-- @tparam[opt=0] number opts.delay           Seconds before first attempt.
-- @treturn number  New `_hull_outbox.id`.
-- @raise If `kind`, `destination`, or `payload` is missing.
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
            return http.async.post(item.destination, item.payload, {
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

--- Flush pending outbox items whose `next_attempt_at <= now`.
--
-- Successful deliveries are marked `delivered`. Failures bump the
-- attempt counter and schedule the next retry; after `max_attempts`
-- failures the row is marked `failed`.
--
-- @function outbox.flush
-- @tparam[opt] table opts
-- @tparam[opt=50] number opts.limit  Max items to process per call.
-- @treturn table  `{ delivered = N, failed = N, retried = N }`.
-- @note Delivery is at-least-once — design receivers to be idempotent.
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

--- Build a post-body middleware that marks the request for auto-flush.
--
-- Sets `req.ctx._outbox_flush = true`; call `outbox.flush_if_needed(req)`
-- from a post-handler hook to trigger the actual flush.
--
-- @function outbox.middleware
-- @treturn function(req, res) -> 0
function outbox.middleware()
    return function(req, _res)
        -- Mark request for post-handler flush
        req.ctx._outbox_flush = true
        return 0
    end
end

--- Flush if `outbox.middleware()` marked the request.
--
-- Call from a post-handler hook (or explicitly from the handler).
--
-- @function outbox.flush_if_needed
-- @tparam table req  The request object.
function outbox.flush_if_needed(req)
    if req.ctx and req.ctx._outbox_flush then
        outbox.flush()
    end
end

--- Count outbox rows per state.
--
-- @function outbox.stats
-- @treturn table  `{ pending = N, delivered = N, failed = N }`.
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

--- Delete delivered rows older than `max_age` seconds.
--
-- @function outbox.cleanup
-- @tparam[opt=604800] number max_age  Age in seconds (default = 7 days).
-- @treturn number  Count of deleted rows.
function outbox.cleanup(max_age)
    max_age = max_age or 86400 * 7  -- default 7 days
    local cutoff = time.now() - max_age
    return db.exec(
        "DELETE FROM _hull_outbox WHERE state = 'delivered' AND delivered_at <= ?",
        { cutoff }
    )
end

return outbox
