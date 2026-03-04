--
-- hull.middleware.inbox -- Inbox deduplication for incoming events
--
-- Prevents duplicate processing of incoming events (webhook receipts,
-- queue messages, etc.) by tracking processed message IDs in SQLite.
--
-- Usage:
--   local inbox = require("hull.middleware.inbox")
--   inbox.init()
--
--   app.post("/webhooks/receive", function(req, res)
--       local event_id = req.headers["x-webhook-event-id"]
--       if inbox.is_duplicate(event_id, "upstream") then
--           return res:json({ received = true, duplicate = true })
--       end
--       -- Process event...
--       inbox.mark_processed(event_id, "upstream")
--       res:json({ received = true })
--   end)
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local inbox = {}

local _ttl = 604800  -- default 7 days

--- Initialize the inbox table.
-- opts.ttl: how long to remember processed message IDs (default 604800 = 7 days)
function inbox.init(opts)
    opts = opts or {}
    if opts.ttl then
        _ttl = opts.ttl
    end

    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_inbox_processed (
            message_id   TEXT NOT NULL,
            source       TEXT NOT NULL,
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

--- Check if a message has already been processed.
-- Returns true if the message_id from the given source was already processed
-- and has not expired.
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

--- Mark a message as processed.
-- opts.ttl: override module-level TTL for this message (optional)
function inbox.mark_processed(message_id, source, opts)
    if not message_id or message_id == "" then
        return
    end
    source = source or "default"
    opts = opts or {}

    local now = time.now()
    local ttl = opts.ttl or _ttl

    db.exec(
        "INSERT OR REPLACE INTO _hull_inbox_processed (message_id, source, processed_at, expires_at) VALUES (?, ?, ?, ?)",
        { message_id, source, now, now + ttl }
    )
end

--- Check and mark in a single call. Returns true if this is a duplicate
--- (already processed), false if it's new (and marks it as processed).
-- This is the common pattern: check + mark atomically.
function inbox.check_and_mark(message_id, source, opts)
    if inbox.is_duplicate(message_id, source) then
        return true
    end
    inbox.mark_processed(message_id, source, opts)
    return false
end

--- Delete expired inbox records. Returns number of deleted rows.
function inbox.cleanup()
    local now = time.now()
    return db.exec(
        "DELETE FROM _hull_inbox_processed WHERE expires_at <= ?",
        { now }
    )
end

return inbox
