--- Inbox deduplication for incoming events.
--
-- Prevents duplicate processing of incoming events (webhook receipts,
-- queue messages, retries) by tracking processed `(source, message_id)`
-- pairs in SQLite.
--
-- Counterpart to `hull.web.middleware.outbox`: outbox provides at-least-once
-- delivery, inbox lets receivers deduplicate.
--
-- @par State table
--   `_hull_inbox_processed`. Schema is created by `inbox.init()`.
--
-- @module hull.web.middleware.inbox
-- @license AGPL-3.0-or-later
-- @usage
--   local inbox = require("hull.web.middleware.inbox")
--   inbox.init()
--
--   app.post("/webhooks/receive", function(req, res)
--       local event_id = req.headers["x-webhook-event-id"]
--       if inbox.check_and_mark(event_id, "upstream") then
--           return res:json({ received = true, duplicate = true })
--       end
--       -- ... process event ...
--       res:json({ received = true })
--   end)
--

local db = require("hull.db").default()
local time = require("hull.time")

local inbox = {}

local _ttl = 604800  -- default 7 days

--- Initialize the `_hull_inbox_processed` SQLite table.
--
-- Idempotent — safe to call on every boot.
--
-- @function inbox.init
-- @tparam[opt] table opts
-- @tparam[opt=604800] number opts.ttl  Record lifetime in seconds (default = 7 days).
function inbox.init(opts)
    opts = opts or {}
    -- M-2: explicit nil check; opts.ttl == 0 must override the default.
    if opts.ttl ~= nil then
        _ttl = opts.ttl
    end

    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_inbox_processed (
            message_id   VARCHAR(255) NOT NULL,
            source       VARCHAR(255) NOT NULL,
            processed_at INTEGER NOT NULL,
            expires_at   INTEGER NOT NULL,
            PRIMARY KEY (source, message_id)
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_inbox_expires
        ON _hull_inbox_processed(expires_at)
    ]])
end

--- Has `(source, message_id)` been processed within its TTL?
--
-- Expired rows are deleted lazily and treated as not-yet-processed.
--
-- @function inbox.is_duplicate
-- @tparam string message_id  Caller-provided message identifier.
-- @tparam[opt="default"] string source  Source name (scopes the namespace).
-- @treturn boolean
function inbox.is_duplicate(message_id, source)
    if not message_id or message_id == "" then
        return false
    end
    source = source or "default"

    local now = time.now()
    local rows = db.query(
        "SELECT expires_at FROM _hull_inbox_processed WHERE source = ? AND message_id = ?",
        { source, message_id }
    )

    if #rows == 0 then
        return false
    end

    -- Expired: clean up and treat as new
    if rows[1].expires_at <= now then
        db.exec(
            "DELETE FROM _hull_inbox_processed WHERE source = ? AND message_id = ?",
            { source, message_id }
        )
        return false
    end

    return true
end

--- Record `(source, message_id)` as processed.
--
-- Uses `INSERT OR REPLACE` so repeated calls slide the expiry forward.
--
-- @function inbox.mark_processed
-- @tparam string message_id
-- @tparam[opt="default"] string source
-- @tparam[opt] table opts
-- @tparam[opt] number opts.ttl  Override module-level TTL for this message.
function inbox.mark_processed(message_id, source, opts)
    if not message_id or message_id == "" then
        return
    end
    source = source or "default"
    opts = opts or {}

    local now = time.now()
    local ttl = opts.ttl or _ttl

    db.upsert(
        "_hull_inbox_processed",
        { "source", "message_id" },
        { "message_id", "source", "processed_at", "expires_at" },
        { message_id, source, now, now + ttl }
    )
end

--- Atomic check-and-mark.
--
-- Returns `true` if `(source, message_id)` was already processed.
-- Otherwise marks it as processed and returns `false`.
--
-- @function inbox.check_and_mark
-- @tparam string message_id
-- @tparam[opt="default"] string source
-- @tparam[opt] table opts  Same as `inbox.mark_processed`.
-- @treturn boolean  `true` if duplicate, `false` if new (and marked).
function inbox.check_and_mark(message_id, source, opts)
    local is_dup = false
    db.batch(function()
        if inbox.is_duplicate(message_id, source) then
            is_dup = true
            return
        end
        inbox.mark_processed(message_id, source, opts)
    end)
    return is_dup
end

--- Delete expired inbox records.
--
-- @function inbox.cleanup
-- @treturn number  Count of deleted rows.
function inbox.cleanup()
    local now = time.now()
    return db.exec(
        "DELETE FROM _hull_inbox_processed WHERE expires_at <= ?",
        { now }
    )
end

return inbox
