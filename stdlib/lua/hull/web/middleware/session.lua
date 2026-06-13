--- Server-side sessions backed by SQLite.
--
-- @module hull.web.middleware.session
-- @license AGPL-3.0-or-later
--
-- Storage: `_hull_sessions` table (id, data, created_at, last_accessed,
-- expires_at). Uses `db.*` for persistence and `crypto.random` for the
-- session id (32 bytes hex). Cookie-based auth pairs this with
-- @{hull.web.middleware.auth.session_middleware}.

local json = require("hull.json")
local crypto = require("hull.crypto")
local db = require("hull.db")
local time = require("hull.time")

local session = {}

-- Module-level TTL (seconds), default 24 hours.
-- Singleton TTL — session.init() must be called exactly once per application.
local _ttl = 86400

--- Initialize the sessions table.
--
-- Creates the `_hull_sessions` table if absent and sets the module-level
-- TTL. **Must be called once at startup** before any other session
-- function. Calling twice with different TTLs is silently allowed but
-- the latter overrides.
--
-- @tparam[opt] table opts  `{ ttl = integer }` (seconds, default `86400`).
--
-- @usage
-- local session = require("hull.web.middleware.session")
-- session.init({ ttl = 7 * 24 * 3600 })  -- 1 week
function session.init(opts)
    opts = opts or {}
    -- M-2: explicit nil check; `if opts.ttl then` would override the
    -- default even when the caller passes ttl=0 (0 is truthy in Lua).
    if opts.ttl ~= nil then
        _ttl = opts.ttl
    end

    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_sessions (
            id TEXT PRIMARY KEY,
            data TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            last_accessed INTEGER NOT NULL,
            expires_at INTEGER NOT NULL
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx__hull_sessions_expires
        ON _hull_sessions(expires_at)
    ]])
    -- Additive migration for the device-management helpers
    -- (session.list_for_user, destroy_others, destroy_all). Pre-
    -- existing rows keep working; they just carry NULL in the
    -- new columns. SQLite has no ALTER TABLE ADD COLUMN IF NOT
    -- EXISTS, so PRAGMA-check first.
    local existing = {}
    for _, r in ipairs(db.query("PRAGMA table_info(_hull_sessions)") or {}) do
        existing[r.name] = true
    end
    if not existing.user_id then
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN user_id TEXT")
    end
    if not existing.ip then
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN ip TEXT")
    end
    if not existing.user_agent then
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN user_agent TEXT")
    end
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx__hull_sessions_user_id "
        .. "ON _hull_sessions(user_id)")
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

--- Create a new session.
--
-- @tparam[opt] table data  Initial session data. JSON-encoded for storage.
-- @tparam[opt] table opts  `{ ttl = integer }` — override module-level TTL.
-- @treturn string  Session id (64-char hex). Persist to the client via
--   a cookie (see @{hull.web.middleware.auth.login}).
function session.create(data, opts)
    local id = generate_id()
    local now = time.now()
    local ttl = (opts and opts.ttl) or _ttl
    local encoded = json.encode(data or {})

    -- Capture device columns for hull/web/middleware/audit-log
    -- + session.list_for_user. user_id is taken from the data
    -- blob (the standard auth-flows pattern is to put it there);
    -- ip + ua come from opts.req if the caller passes it.
    local user_id = (type(data) == "table" and data.user_id) or nil
    local ip, ua
    if opts and opts.req then
        local h = opts.req.headers
        local xff = h and h["x-forwarded-for"]
        if xff then
            ip = (xff:match("^([^,]+)") or xff):gsub("^%s+", ""):gsub("%s+$", "")
        else
            ip = opts.req.remote_addr
        end
        ua = h and h["user-agent"]
    end

    db.exec(
        "INSERT INTO _hull_sessions "
        .. "(id, data, created_at, last_accessed, expires_at, "
        .. " user_id, ip, user_agent) "
        .. "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        { id, encoded, now, now, now + ttl, user_id, ip, ua }
    )

    return id
end

--- Load a session by id; refresh expiry on hit (sliding TTL).
--
-- Validates that `session_id` matches the 64-char hex shape before
-- hitting the DB. On hit, updates `last_accessed` and extends
-- `expires_at`. Expired sessions return `nil` and are NOT deleted
-- (call @{session.cleanup} for that).
--
-- @tparam string session_id  Session id (from the auth cookie).
-- @tparam[opt] table opts  `{ ttl = integer }` for the extended expiry.
-- @treturn ?table  Session data, or `nil` if absent / expired / malformed.
function session.load(session_id, opts)
    if not session_id or session_id == "" then
        return nil
    end
    -- Validate format: must be 64-char hex (from generate_id)
    if #session_id ~= 64 or not session_id:match("^%x+$") then
        return nil
    end

    local now = time.now()
    local rows = db.query(
        "SELECT data, expires_at FROM _hull_sessions WHERE id = ?",
        { session_id }
    )

    if #rows == 0 then
        return nil
    end

    local row = rows[1]

    -- Check expiry
    if row.expires_at <= now then
        -- Expired -- clean it up
        db.exec("DELETE FROM _hull_sessions WHERE id = ?", { session_id })
        return nil
    end

    -- Update last_accessed and extend expiry
    local ttl = (opts and opts.ttl) or _ttl
    db.exec(
        "UPDATE _hull_sessions SET last_accessed = ?, expires_at = ? WHERE id = ?",
        { now, now + ttl, session_id }
    )

    local decoded = json.decode(row.data)
    if not decoded then
        -- Corrupted session data — destroy and return nil
        db.exec("DELETE FROM _hull_sessions WHERE id = ?", { session_id })
        return nil
    end
    return decoded
end

--- Replace the data for an existing session and refresh expiry.
--
-- @tparam string session_id
-- @tparam table data  Replacement payload (JSON-encoded).
-- @tparam[opt] table opts  `{ ttl = integer }`.
function session.update(session_id, data, opts)
    if not session_id or session_id == "" then
        return
    end

    local now = time.now()
    local ttl = (opts and opts.ttl) or _ttl
    local encoded = json.encode(data or {})

    db.exec(
        "UPDATE _hull_sessions SET data = ?, last_accessed = ?, expires_at = ? WHERE id = ?",
        { encoded, now, now + ttl, session_id }
    )
end

--- Destroy a session by id.
--
-- @tparam string session_id  Empty / nil is a no-op.
function session.destroy(session_id)
    if not session_id or session_id == "" then
        return
    end

    db.exec("DELETE FROM _hull_sessions WHERE id = ?", { session_id })
end

--- Delete all expired sessions.
--
-- Run periodically — typically `app.every(3600_000, session.cleanup)`.
--
-- @treturn integer  Number of rows deleted.
function session.cleanup()
    local now = time.now()
    local count = db.exec(
        "DELETE FROM _hull_sessions WHERE expires_at <= ?",
        { now }
    )
    return count
end

--- List the currently active sessions for a user.
-- Returns rows `{ id, created_at, last_accessed, ip, user_agent }`,
-- newest-accessed first. Excludes expired rows. The session
-- DATA blob is NOT included (apps don't typically need it for a
-- device list — they want ip/ua/recency).
-- @tparam string user_id
-- @treturn table  Array (possibly empty).
function session.list_for_user(user_id)
    if type(user_id) ~= "string" or user_id == "" then return {} end
    local now = time.now()
    return db.query(
        "SELECT id, created_at, last_accessed, ip, user_agent "
        .. "FROM _hull_sessions "
        .. "WHERE user_id = ? AND expires_at > ? "
        .. "ORDER BY last_accessed DESC",
        { user_id, now }) or {}
end

--- Destroy every session for a user EXCEPT `current_sid`.
-- Standard "sign out everywhere else" UX. Returns the count.
function session.destroy_others(current_sid, user_id)
    if type(user_id) ~= "string" or user_id == "" then return 0 end
    return db.exec(
        "DELETE FROM _hull_sessions WHERE user_id = ? AND id != ?",
        { user_id, current_sid or "" }) or 0
end

--- Destroy every session for a user.
-- Used by hull/web/auth-flows on a successful password reset
-- when opts.revoke_sessions_on_password_reset is true (default).
function session.destroy_all(user_id)
    if type(user_id) ~= "string" or user_id == "" then return 0 end
    return db.exec(
        "DELETE FROM _hull_sessions WHERE user_id = ?",
        { user_id }) or 0
end

return session
