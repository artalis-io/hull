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

-- Set by session.init(). Factories like session.login_handler use
-- this to fail loudly when an app forgets the init ordering
-- (typical error: calling authflows.init -> wires login_handler ->
-- request comes in -> session.create fails with a missing-table
-- SQL error). With the guard the failure is at init time with a
-- one-line fix.
local _initialized = false

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
    -- new columns. Neither SQLite nor Postgres has ALTER TABLE
    -- ADD COLUMN IF NOT EXISTS in every supported version, so
    -- ask the backend for the current column set first.
    local existing = {}
    for _, name in ipairs(db.table_columns("_hull_sessions") or {}) do
        existing[name] = true
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
    _initialized = true
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
-- @treturn boolean  true if a row was updated, false otherwise
-- (unknown / expired session id). Mirrors JS.
function session.update(session_id, data, opts)
    if not session_id or session_id == "" then
        return false
    end

    local now = time.now()
    local ttl = (opts and opts.ttl) or _ttl
    local encoded = json.encode(data or {})

    -- AND expires_at > ? prevents reviving a session that the user
    -- already let expire. Matches the JS guard so apps that check
    -- the return get the same semantics on both runtimes.
    local affected = db.exec(
        "UPDATE _hull_sessions SET data = ?, last_accessed = ?, expires_at = ? "
        .. "WHERE id = ? AND expires_at > ?",
        { encoded, now, now + ttl, session_id, now }
    )
    return (affected or 0) > 0
end

--- Destroy a session by id.
--
-- @tparam string session_id  Empty / nil is a no-op.
-- @treturn boolean  true if a row was deleted, false otherwise.
function session.destroy(session_id)
    if not session_id or session_id == "" then
        return false
    end

    local affected = db.exec(
        "DELETE FROM _hull_sessions WHERE id = ?", { session_id })
    return (affected or 0) > 0
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

-- ── Session-fixation defense + on_login factories ────────────────

--- Rotate the session id. Destroys @p old_sid (if non-empty) and
-- creates a fresh session row with the supplied data. Use during
-- login to defend against session-fixation attacks: a pre-login
-- session ID known to an attacker can't be elevated to an
-- authenticated session because the id changes the moment
-- credentials verify.
-- @tparam ?string old_sid  Existing session to destroy (nil ok).
-- @tparam table   data
-- @tparam[opt] table opts  Same shape as session.create.
-- @treturn string  New session id.
function session.rotate(old_sid, data, opts)
    if old_sid and old_sid ~= "" then session.destroy(old_sid) end
    return session.create(data, opts)
end

-- Sensible defaults for the cookie helper used by login_handler /
-- logout_handler. Apps override via the opts table.
local DEFAULT_LOGIN_HANDLER_OPTS = {
    name        = "hull_session",
    cookie_opts = { path = "/", httponly = true, samesite = "Lax" },
}

--- Build a standard on_login(req, res, user) callback for
-- hull/web/auth-flows or hull/web/middleware/oauth. Eliminates the
-- per-app boilerplate of "create session -> serialize cookie ->
-- set Set-Cookie -> respond with JSON". Session-fixation defense
-- (session.rotate) is on by default.
--
-- @tparam table cookie_mod  hull.web.cookie module reference.
-- @tparam[opt] table opts
--   * `name`        — cookie name (default "hull_session", matching
--                     hull/web/middleware/auth's session_middleware).
--   * `cookie_opts` — extra opts forwarded to cookie.serialize
--                     (default { path="/", httponly=true, samesite="Lax" }).
--   * `extract_data(user)` — fields to embed in the session row
--                     (default `{ user_id = user.id, email = user.email }`).
--   * `respond(res, user, sid)` — write the response body (default
--                     `res:json({ ok = true, user_id = user.id,
--                                  email = user.email })`).
--   * `rotate`      — bool, rotate any prior session in the cookie
--                     (default true; turn off only if your app
--                     deliberately tracks pre- and post-login
--                     state in one session).
-- @treturn function  on_login compatible with auth-flows + oauth.
--
-- Additional opts (audit + new-device, shared across all login sources):
--   * `audit_log`      — module ref (e.g. require("hull.web.middleware.audit-log")).
--                        When set, records a login event after the session
--                        is rotated/created. Off by default.
--   * `audit_kind`     — event kind string (default "login").
--   * `audit_metadata(user, ctx)` — table emitted as the event metadata.
--                        Default derives `{ factors = ctx.factors }` (auth-flows)
--                        or `{ factors = "oauth:" .. ctx.provider }` (oauth)
--                        or `{ factors = "unknown" }` (no ctx).
--   * `on_new_device(req, res, user)` — called before audit_log.record when
--                        audit_log.is_new_device(user_id, req) returns true.
--                        Requires audit_log. Wrapped in pcall so callback
--                        bugs cannot fail the login.
function session.login_handler(cookie_mod, opts)
    if not _initialized then
        error("session.login_handler: call session.init() first")
    end
    if type(cookie_mod) ~= "table" or type(cookie_mod.serialize) ~= "function" then
        error("session.login_handler: cookie module required")
    end
    opts = opts or {}
    local name        = opts.name        or DEFAULT_LOGIN_HANDLER_OPTS.name
    local cookie_opts = opts.cookie_opts or DEFAULT_LOGIN_HANDLER_OPTS.cookie_opts
    local rotate      = opts.rotate
    if rotate == nil then rotate = true end
    local extract_data = opts.extract_data or function(user)
        return { user_id = user.id, email = user.email }
    end
    local respond = opts.respond or function(res, user, _sid)
        res:json({ ok = true, user_id = user.id, email = user.email })
    end
    local audit_log    = opts.audit_log
    local audit_kind   = opts.audit_kind or "login"
    local on_new_dev   = opts.on_new_device
    local audit_meta   = opts.audit_metadata or function(_user, ctx)
        if type(ctx) == "table" then
            if type(ctx.factors) == "string" then
                return { factors = ctx.factors }
            elseif type(ctx.provider) == "string" then
                return { factors = "oauth:" .. ctx.provider }
            end
        end
        return { factors = "unknown" }
    end
    -- Defense in depth: scrub keys that must never reach the audit
    -- store, no matter what the app's audit_metadata returned. OAuth
    -- ctx (claims, tokens) is passed straight through to apps via
    -- on_login's 4th arg, which makes `audit_metadata = function(_,c)
    -- return c end` a one-keystroke leak of access_token /
    -- refresh_token / raw ID-token into _hull_audit_log.metadata
    -- (where it persists for opts.retain_days — default 365). The
    -- list mirrors the JS half and covers OAuth, password, and TOTP
    -- secret surfaces. If you need to log claim details, pull them
    -- out by name in a custom audit_metadata — never pass the raw
    -- ctx through.
    local SCRUB_KEYS = {
        tokens         = true, token        = true,
        access_token   = true, refresh_token = true,
        id_token       = true, claims       = true,
        password       = true, password_hash = true,
        pwhash         = true, secret       = true,
    }
    local function scrub(meta)
        if type(meta) ~= "table" then return meta end
        local out = {}
        for k, v in pairs(meta) do
            if not SCRUB_KEYS[k] then out[k] = v end
        end
        return out
    end

    return function(req, res, user, ctx)
        if type(user) ~= "table" or user.id == nil or user.id == "" then
            error("session.login_handler: user.id is required")
        end
        local data = extract_data(user) or {}
        local sid
        if rotate then
            local existing
            if req.ctx and req.ctx.session_id then
                existing = req.ctx.session_id
            else
                local cookies = cookie_mod.parse(req.headers.cookie or "")
                existing = cookies[name]
            end
            sid = session.rotate(existing, data, { req = req })
        else
            sid = session.create(data, { req = req })
        end
        res:header("Set-Cookie", cookie_mod.serialize(name, sid, cookie_opts))

        -- New-device + audit emission happen AFTER the session row exists
        -- so the new-device check sees prior history (not the row we just
        -- created), and the audit row carries the canonical session_id.
        if audit_log then
            if on_new_dev then
                local ok, is_new = pcall(audit_log.is_new_device, user.id, req)
                if ok and is_new then pcall(on_new_dev, req, res, user) end
            end
            pcall(audit_log.record, user.id, audit_kind, req,
                  { session_id = sid, metadata = scrub(audit_meta(user, ctx)) })
        end

        respond(res, user, sid)
    end
end

--- Build the matching on_logout(req, res) callback. Destroys the
-- current session (from cookie) and clears it client-side.
function session.logout_handler(cookie_mod, opts)
    if type(cookie_mod) ~= "table" or type(cookie_mod.parse) ~= "function" then
        error("session.logout_handler: cookie module required")
    end
    opts = opts or {}
    local name        = opts.name        or DEFAULT_LOGIN_HANDLER_OPTS.name
    local cookie_opts = opts.cookie_opts or DEFAULT_LOGIN_HANDLER_OPTS.cookie_opts
    local respond = opts.respond or function(res) res:json({ ok = true }) end

    return function(req, res)
        local sid
        if req.ctx and req.ctx.session_id then
            sid = req.ctx.session_id
        else
            local cookies = cookie_mod.parse(req.headers.cookie or "")
            sid = cookies[name]
        end
        if sid then session.destroy(sid) end
        res:header("Set-Cookie", cookie_mod.clear(name, cookie_opts))
        respond(res)
    end
end

return session
