--- Transactional outbox for reliable side-effect delivery.
--
-- Decouples external side effects (webhooks, HTTP calls, SMTP) from the
-- request transaction. The handler enqueues an outbox row inside the same
-- `db.batch` that commits the state change; the outbox flusher then
-- delivers the row after commit with exponential backoff retries.
--
-- Delivery is **at-least-once** - a crash between deliver-and-mark can
-- re-deliver an item. Design receivers to be idempotent (e.g. with
-- `hull.web.middleware.inbox`).
--
-- @par Backoff schedule
--   `2^attempt * 10s`, capped at 1 hour. After `max_attempts` (default
--   5) the row is marked `failed`.
--
-- @par State table
--   `_hull_outbox`. Schema is created by `outbox.init()`.
--
-- @module hull.web.middleware.outbox
-- @license AGPL-3.0-or-later
-- @usage
--   local outbox = require("hull.web.middleware.outbox")
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
local db = require("hull.db").default()
local http_client = require("hull.http-client")

-- Whether @p headers already names @p want, in any case: the app's own
-- "idempotency-key" was sent beside the row's "Idempotency-Key" as two
-- headers (audit 23).
local function has_header(headers, want)
    want = want:lower()
    for k in pairs(headers) do
        if type(k) == "string" and k:lower() == want then return true end
    end
    return false
end
local time = require("hull.time")
local refusal = require("hull._refusal")

local outbox = {}

local _max_attempts = 5

-- max_attempts lands in an INTEGER column and is compared with the attempt
-- count: a whole number, 0 (no retries) up to int4's maximum - the JS
-- twin's rule (audit 25; a string or a fraction was bound as given).
local MAX_ATTEMPTS_MAX = 2147483647
local function check_max_attempts(v, where)
    local n = type(v) == "number" and math.tointeger(v) or nil
    if n == nil or n < 0 or n > MAX_ATTEMPTS_MAX then
        error(where .. ": max_attempts must be a whole number >= 0", 3)
    end
    return n
end

-- The first attempt's delay: a finite number of seconds or a numeric
-- string, as whole seconds (next_attempt_at is an INTEGER column; a
-- fractional delay went to Postgres as a double; audit 25).
local DELAY_MAX = 2 ^ 52
local function delay_secs(v)
    if v == nil then return 0 end
    local n = (type(v) == "number" or type(v) == "string") and tonumber(v) or nil
    if n == nil or n ~= n or n == math.huge or n == -math.huge then
        error("outbox.enqueue: delay must be a finite number of seconds", 3)
    end
    if n > DELAY_MAX then n = DELAY_MAX end
    if n < -DELAY_MAX then n = -DELAY_MAX end
    return math.ceil(n)
end

--- Initialize the `_hull_outbox` table.
--
-- Idempotent - safe to call on every boot.
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
        _max_attempts = check_max_attempts(opts.max_attempts, "outbox.init")
    end
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_outbox ("
        .. "id              " .. db.autoincrement_id_ddl .. ", "
        .. "kind            TEXT NOT NULL,"
        .. "destination     TEXT NOT NULL,"
        .. "payload         TEXT NOT NULL,"
        .. "headers         TEXT,"
        .. "idempotency_key VARCHAR(255),"
        .. "attempts        INTEGER NOT NULL DEFAULT 0,"
        .. "max_attempts    INTEGER NOT NULL DEFAULT 5,"
        .. "next_attempt_at INTEGER NOT NULL,"
        .. "state           VARCHAR(32) NOT NULL DEFAULT 'pending',"
        .. "created_at      INTEGER NOT NULL,"
        .. "delivered_at    INTEGER,"
        .. "last_error      TEXT)")
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
    local max = _max_attempts
    if opts.max_attempts ~= nil then
        max = check_max_attempts(opts.max_attempts, "outbox.enqueue")
    end
    local delay = delay_secs(opts.delay)

    local sql = "INSERT INTO _hull_outbox (kind, destination, payload, headers, idempotency_key, max_attempts, next_attempt_at, state, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', ?)"
    local vals = { opts.kind, opts.destination, opts.payload, opts.headers, opts.idempotency_key, max, now + delay, now }
    -- The new id from the INSERT itself where the backend can (SQLite,
    -- Postgres): last_id has no Postgres implementation, so enqueue returned
    -- nothing there. MySQL has no RETURNING; its last_id is per connection.
    if db.dialect and db.dialect.supports_returning then
        local rows = db.query(sql .. " RETURNING id", vals)
        return rows and rows[1] and rows[1].id
    end
    db.exec(sql, vals)
    return db.last_id()
end

--- Attempt to deliver a single outbox item.
-- Returns true on success, false on failure, and `false, err, code` when the
-- runtime refused the delivery (code is its hull._refusal code: "busy" or
-- "wait_refused"). A refusal says where or when flush ran, not how the
-- destination answered, so it is never recorded as a failed attempt
-- (audit 18: every flush from such a place burned an attempt until the row
-- was 'failed', unsent; audit 35: so did every flush while the VM's 64
-- fetches were in flight, and the stdlib matched only the older refusal
-- messages - it tests the code now).
-- How long a flush's claim on a row lasts before another flush may take it
-- (seconds): longer than any delivery can run - http.fetch's ceiling
-- (HL_HTTP_FETCH_MAX_TIMEOUT_MS, 10 min) plus a minute. It was 300 s, under
-- the 10 min a manifest `http.timeout_ms` may give a delivery, so another
-- flush took the row and sent it again while the first was still waiting
-- (audit 24). The app's own timeout still bounds each delivery.
local CLAIM_LEASE = 660

-- The delivery call; the suite stands in for it through the test hooks
-- (set_post), as the JS twin's setPost.
local function real_post(url, body, opts)
    return http_client.async.post(url, body, opts)
end
local post = real_post

local function deliver_item(item)
    if item.kind == "webhook" or item.kind == "http" then
        local req_headers = {}
        if item.headers then
            -- Protected: a stored value that is not JSON raised here, out of
            -- the whole flush, with the row still leased.
            local ok, decoded = pcall(json.decode, item.headers)
            if ok and type(decoded) == "table" then
                req_headers = decoded
            end
        end
        -- Delivery is at-least-once, so give the receiver what it needs to
        -- drop a repeat: the row's idempotency key, unless the app set the
        -- header itself.
        if type(item.idempotency_key) == "string" and item.idempotency_key ~= ""
           and not has_header(req_headers, "Idempotency-Key") then
            req_headers["Idempotency-Key"] = item.idempotency_key
        end

        local send_ok, result = pcall(function()
            return post(item.destination, item.payload, {
                headers = req_headers,
            })
        end)

        if send_ok and result and result.status and result.status >= 200 and result.status < 300 then
            return true, nil
        end
        local refused = not send_ok and refusal.code(result) or nil
        if refused then
            return false, result, refused
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
-- @treturn table  `{ delivered = N, failed = N, retried = N }`, plus
--   `busy = true` when the runtime refused a delivery as busy (the VM's
--   fetch cap): the flush stopped there, the rest stay pending, unattempted.
-- @note Delivery is at-least-once - design receivers to be idempotent.
function outbox.flush(opts)
    -- Delivery waits on http.fetch, which the runtime refuses inside a
    -- transaction; refused here, before any row is claimed (audit 18).
    if db.in_transaction() then
        error("outbox.flush cannot run inside db.batch or an open transaction: "
              .. "delivery waits on the network. Call it after the commit", 2)
    end
    opts = opts or {}
    -- 1..1000 (default 50): a number or numeric string counts, anything
    -- else and NaN take the default; a negative LIMIT is "no limit" on
    -- SQLite (audit 24).
    local limit = tonumber(opts.limit) or 50
    if limit ~= limit then limit = 50 end
    limit = math.floor(math.max(1, math.min(limit, 1000)))
    local now = time.now()

    local items = db.query(
        "SELECT id, kind, destination, payload, headers, idempotency_key, attempts, max_attempts FROM _hull_outbox WHERE state = 'pending' AND next_attempt_at <= ? ORDER BY id LIMIT ?",
        { now, limit }
    )

    local delivered = 0
    local failed = 0
    local retried = 0

    for _, item in ipairs(items) do
        -- Claim the row before sending it. Delivery yields to the event loop,
        -- so another request's flush could select the same pending rows and
        -- send them again. The claim pushes next_attempt_at out by a lease;
        -- only the flush whose UPDATE changed the row delivers it. If this
        -- process dies mid-send the lease runs out and the row is retried.
        -- The lease starts at the claim, not at the flush: items are claimed
        -- one at a time, each before its own awaited delivery, so with slow
        -- deliveries ahead of it a later item got a lease that had already
        -- run out - another flush reclaimed it at once and sent it again.
        local claim_now = time.now()
        local lease = claim_now + CLAIM_LEASE
        local claimed = db.exec(
            "UPDATE _hull_outbox SET next_attempt_at = ? WHERE id = ? " ..
            "AND state = 'pending' AND next_attempt_at <= ?",
            { lease, item.id, claim_now })
        if claimed == 1 then
            local ok, err, refused = deliver_item(item)
            if refused then
                -- Give the claim back (the row is due again at once):
                -- nothing was sent, so no attempt is counted. A busy refusal
                -- (the VM's fetch cap, its off-heap copy total) is transient
                -- and would refuse every row after this one too, so the flush
                -- stops and reports it (`busy = true`); the rows stay pending
                -- for the next flush. A refusal to wait is the caller's bug:
                -- raised.
                db.exec(
                    "UPDATE _hull_outbox SET next_attempt_at = ? WHERE id = ? " ..
                    "AND state = 'pending' AND next_attempt_at = ?",
                    { claim_now, item.id, lease })
                if refused == refusal.BUSY then
                    return { delivered = delivered, failed = failed,
                             retried = retried, busy = true }
                end
                error(err, 0)
            end

            -- Every outcome is written only while the row still holds THIS
            -- flush's lease. A delivery that outran it was reclaimed and sent
            -- again by another flush; a late write here then overwrote that
            -- flush's outcome (and counted the row twice).
            local mine = " AND state = 'pending' AND next_attempt_at = ?"
            if ok then
                local n = db.exec(
                    "UPDATE _hull_outbox SET state = 'delivered', delivered_at = ?, attempts = attempts + 1 WHERE id = ?" .. mine,
                    { time.now(), item.id, lease }
                )
                if n == 1 then delivered = delivered + 1 end
            else
                local new_attempts = item.attempts + 1
                if new_attempts >= item.max_attempts then
                    local n = db.exec(
                        "UPDATE _hull_outbox SET state = 'failed', attempts = ?, last_error = ? WHERE id = ?" .. mine,
                        { new_attempts, err, item.id, lease }
                    )
                    if n == 1 then failed = failed + 1 end
                else
                    local next_at = time.now() + backoff_delay(new_attempts)
                    local n = db.exec(
                        "UPDATE _hull_outbox SET attempts = ?, next_attempt_at = ?, last_error = ? WHERE id = ?" .. mine,
                        { new_attempts, next_at, err, item.id, lease }
                    )
                    if n == 1 then retried = retried + 1 end
                end
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

-- Test-only hooks, registered with the stdlib-internal hull._test_hooks
-- rather than exported (audit 16): the suite reads its rows back from inside
-- the module, where the _hull_* namespace check admits the query.
require("hull._test_hooks").register("hull.web.middleware.outbox", {
    rows = function(id)
        return db.query("SELECT state, kind, destination, idempotency_key, "
                        .. "next_attempt_at, created_at FROM _hull_outbox WHERE id = ?",
                        { id })
    end,
    clear = function() db.exec("DELETE FROM _hull_outbox") end,
    set_post = function(fn) post = fn or real_post end,
    claim_lease = CLAIM_LEASE,
})

return outbox
