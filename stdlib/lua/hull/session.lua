--
-- hull.session -- Server-side sessions backed by SQLite
--
-- Uses the global `db` object for storage and `crypto` for ID generation.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local session = {}

-- Module-level TTL (seconds), default 24 hours
local _ttl = 86400

--- Initialize the sessions table and configure TTL.
-- opts.ttl: session lifetime in seconds (default 86400)
function session.init(opts)
    opts = opts or {}
    if opts.ttl then
        _ttl = opts.ttl
    end

    db.exec([[
        CREATE TABLE IF NOT EXISTS hull_sessions (
            id TEXT PRIMARY KEY,
            data TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            last_accessed INTEGER NOT NULL,
            expires_at INTEGER NOT NULL
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_sessions_expires
        ON hull_sessions(expires_at)
    ]])
end

--- Generate a 64-character hex session ID from 32 random bytes.
local function generate_id()
    local raw = crypto.random(32)
    local hex = {}
    for i = 1, #raw do
        hex[i] = string.format("%02x", string.byte(raw, i))
    end
    return table.concat(hex)
end

--- Create a new session with the given data table.
-- Returns the session ID (64-char hex string).
function session.create(data)
    local id = generate_id()
    local now = time.now()
    local encoded = json.encode(data or {})

    db.exec(
        "INSERT INTO hull_sessions (id, data, created_at, last_accessed, expires_at) VALUES (?, ?, ?, ?, ?)",
        { id, encoded, now, now, now + _ttl }
    )

    return id
end

--- Load a session by ID.
-- Returns the data table, or nil if the session does not exist or is expired.
-- Updates last_accessed and extends expiry on successful load.
function session.load(session_id)
    if not session_id or session_id == "" then
        return nil
    end

    local now = time.now()
    local rows = db.query(
        "SELECT data, expires_at FROM hull_sessions WHERE id = ?",
        { session_id }
    )

    if #rows == 0 then
        return nil
    end

    local row = rows[1]

    -- Check expiry
    if row.expires_at <= now then
        -- Expired -- clean it up
        db.exec("DELETE FROM hull_sessions WHERE id = ?", { session_id })
        return nil
    end

    -- Update last_accessed and extend expiry
    db.exec(
        "UPDATE hull_sessions SET last_accessed = ?, expires_at = ? WHERE id = ?",
        { now, now + _ttl, session_id }
    )

    return json.decode(row.data)
end

--- Replace session data for an existing session.
function session.update(session_id, data)
    if not session_id or session_id == "" then
        return
    end

    local now = time.now()
    local encoded = json.encode(data or {})

    db.exec(
        "UPDATE hull_sessions SET data = ?, last_accessed = ?, expires_at = ? WHERE id = ?",
        { encoded, now, now + _ttl, session_id }
    )
end

--- Destroy a session by ID.
function session.destroy(session_id)
    if not session_id or session_id == "" then
        return
    end

    db.exec("DELETE FROM hull_sessions WHERE id = ?", { session_id })
end

--- Delete all expired sessions.
-- Returns the number of deleted sessions.
function session.cleanup()
    local now = time.now()
    local count = db.exec(
        "DELETE FROM hull_sessions WHERE expires_at <= ?",
        { now }
    )
    return count
end

return session
