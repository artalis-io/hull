-- hull.web.middleware.audit-log — append-only sign-in / auth
-- event log. Composable with hull/web/auth-flows (emit events
-- automatically when opts.sign_in_log is set), with the session
-- module (group sessions by device fingerprint), or standalone
-- (apps record their own kinds: api_token_issued,
-- admin_impersonate, etc.).
--
-- Schema: one table `_hull_audit_log`. No PII beyond what the
-- request already carries (IP + UA); for stricter privacy
-- regimes call `audit_log.record(..., { ip = nil, user_agent = nil })`
-- or override `audit_log.fingerprint` via `opts.fingerprint`.
--
-- API:
--   audit_log.init(opts?)                  -- opts.retain_days = 365
--   audit_log.record(user_id, kind, req, opts?)
--                                          -- opts.session_id, opts.metadata,
--                                          -- opts.fingerprint (override),
--                                          -- opts.ip / opts.user_agent (override)
--   audit_log.list(user_id, opts?)         -- opts.limit (50), opts.kinds (filter)
--   audit_log.list_devices(user_id, opts?) -- opts.window_days (90)
--   audit_log.is_new_device(user_id, req, opts?) -> bool
--                                          -- opts.window_days (30)
--   audit_log.fingerprint(req)             -- exposed for app-side reuse
--   audit_log.cleanup()                    -- delete events older than retain_days

local crypto = require("hull.crypto")
local db     = require("hull.db")
local time   = require("hull.time")
local json   = require("hull.json")

local audit_log = {}

local _state = {
    retain_days  = 365,
    _initialized = false,
}

--- Initialize the audit-log table. Call once at app startup
-- (typically right after session.init()).
-- @tparam[opt] table opts  `{ retain_days = integer }` (default 365).
function audit_log.init(opts)
    opts = opts or {}
    if opts.retain_days ~= nil then
        _state.retain_days = opts.retain_days
    end
    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_audit_log (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id     TEXT    NOT NULL,
            event_at    INTEGER NOT NULL,
            kind        TEXT    NOT NULL,
            ip          TEXT,
            user_agent  TEXT,
            fingerprint TEXT,
            session_id  TEXT,
            metadata    TEXT
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS _hull_audit_log_user_at
            ON _hull_audit_log(user_id, event_at DESC)
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS _hull_audit_log_user_fp
            ON _hull_audit_log(user_id, fingerprint)
    ]])
    _state._initialized = true
end

-- ── Fingerprint helpers ────────────────────────────────────────────
--
-- v1 fingerprint = sha256(normalize_ua || "|" || ip_prefix), first
-- 16 hex chars. Coarse on purpose: a Chrome version bump shouldn't
-- read as "new device", and an ISP rebalancing the user's IP within
-- the same /24 shouldn't either. Stronger fingerprinting (long-lived
-- "trusted browser" cookie, à la Stripe) is a documented v2 path.

local function normalize_ua(ua)
    if type(ua) ~= "string" then return "unknown|unknown" end
    local s = ua:lower()
    local os = "other"
    if s:find("android",  1, true) then os = "android"
    elseif s:find("iphone", 1, true) or s:find("ipad", 1, true) then os = "ios"
    elseif s:find("mac os", 1, true) then os = "macos"
    elseif s:find("windows", 1, true) then os = "windows"
    elseif s:find("linux",  1, true) then os = "linux"
    end
    local family = "other"
    -- Order matters: Edge contains "Chrome", Chrome contains "Safari"
    -- in their UA strings. Check most-specific first.
    if s:find("firefox", 1, true) then family = "firefox"
    elseif s:find("edg/",   1, true) then family = "edge"
    elseif s:find("chrome", 1, true) then family = "chrome"
    elseif s:find("safari", 1, true) then family = "safari"
    elseif s:find("curl",   1, true) then family = "curl"
    end
    return os .. "|" .. family
end

local function ip_prefix(ip)
    if type(ip) ~= "string" or ip == "" then return "0.0.0.0/24" end
    -- Strip whitespace + take first IP from x-forwarded-for chain.
    ip = (ip:match("^([^,]+)") or ip):gsub("^%s+", ""):gsub("%s+$", "")
    local a, b, c = ip:match("^(%d+)%.(%d+)%.(%d+)%.")
    if a then return a .. "." .. b .. "." .. c .. ".0/24" end
    -- IPv6 — first 4 groups as /64.
    if ip:find(":", 1, true) then
        local parts = {}
        for part in ip:gmatch("[^:]+") do
            parts[#parts + 1] = part
            if #parts == 4 then break end
        end
        return table.concat(parts, ":") .. "::/64"
    end
    return ip
end

local function extract_ip(req)
    if not (req and req.headers) then return nil end
    local xff = req.headers["x-forwarded-for"]
    if xff then
        local first = xff:match("^([^,]+)")
        if first then return first:gsub("^%s+", ""):gsub("%s+$", "") end
    end
    return req.remote_addr
end

local function extract_ua(req)
    return req and req.headers and req.headers["user-agent"] or nil
end

--- Compute the (coarse) device fingerprint for a request.
-- Hex SHA-256(normalized_ua || "|" || ip_prefix), truncated to 16
-- chars (64 bits). See module header for the trade-offs.
function audit_log.fingerprint(req)
    local ua  = extract_ua(req)
    local ip  = extract_ip(req)
    local key = normalize_ua(ua) .. "|" .. ip_prefix(ip)
    return crypto.hex_encode(crypto.sha256(key)):sub(1, 16)
end

--- Record an event. user_id and kind are required; everything
-- else is derived from req or supplied via opts.
function audit_log.record(user_id, kind, req, opts)
    if type(user_id) ~= "string" or user_id == ""
       or type(kind) ~= "string" or kind == "" then return end
    opts = opts or {}
    local ip = opts.ip ~= nil and opts.ip or extract_ip(req)
    local ua = opts.user_agent ~= nil and opts.user_agent or extract_ua(req)
    local fp = opts.fingerprint or audit_log.fingerprint(req)
    local meta = nil
    if opts.metadata ~= nil then
        meta = json.encode(opts.metadata)
    end
    db.exec(
        "INSERT INTO _hull_audit_log "
        .. "(user_id, event_at, kind, ip, user_agent, fingerprint, "
        .. " session_id, metadata) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        { user_id, time.now(), kind, ip, ua, fp,
          opts.session_id, meta })
end

--- List recent events for a user, newest first.
-- opts.limit (default 50), opts.kinds (filter — array of kinds).
function audit_log.list(user_id, opts)
    if type(user_id) ~= "string" or user_id == "" then return {} end
    opts = opts or {}
    local limit = opts.limit or 50
    local rows
    if opts.kinds and #opts.kinds > 0 then
        local placeholders = {}
        local params = { user_id }
        for _, k in ipairs(opts.kinds) do
            placeholders[#placeholders + 1] = "?"
            params[#params + 1] = k
        end
        params[#params + 1] = limit
        rows = db.query(
            "SELECT id, event_at, kind, ip, user_agent, fingerprint, "
            .. " session_id, metadata FROM _hull_audit_log "
            .. "WHERE user_id = ? AND kind IN ("
            .. table.concat(placeholders, ",")
            .. ") ORDER BY event_at DESC LIMIT ?",
            params)
    else
        rows = db.query(
            "SELECT id, event_at, kind, ip, user_agent, fingerprint, "
            .. " session_id, metadata FROM _hull_audit_log "
            .. "WHERE user_id = ? ORDER BY event_at DESC LIMIT ?",
            { user_id, limit })
    end
    for _, r in ipairs(rows or {}) do
        if r.metadata then
            local ok, decoded = pcall(json.decode, r.metadata)
            r.metadata = ok and decoded or nil
        end
    end
    return rows or {}
end

--- Group recent events by fingerprint into a per-device summary.
-- Each row: { fingerprint, first_seen, last_seen, count, ip, user_agent }.
-- opts.window_days (default 90) — older events excluded.
function audit_log.list_devices(user_id, opts)
    if type(user_id) ~= "string" or user_id == "" then return {} end
    opts = opts or {}
    local cutoff = time.now() - ((opts.window_days or 90) * 86400)
    local rows = db.query(
        "SELECT fingerprint, "
        .. "  MIN(event_at) AS first_seen, "
        .. "  MAX(event_at) AS last_seen, "
        .. "  COUNT(*)      AS count, "
        .. "  MAX(ip)         AS ip, "
        .. "  MAX(user_agent) AS user_agent "
        .. "FROM _hull_audit_log "
        .. "WHERE user_id = ? AND event_at >= ? "
        .. "  AND fingerprint IS NOT NULL "
        .. "GROUP BY fingerprint ORDER BY last_seen DESC",
        { user_id, cutoff })
    return rows or {}
end

--- Is this request from a device this user hasn't been seen on
-- recently? Returns true if the fingerprint has NO events in the
-- last `opts.window_days` (default 30) for `user_id`.
function audit_log.is_new_device(user_id, req, opts)
    if type(user_id) ~= "string" or user_id == "" then return false end
    opts = opts or {}
    local cutoff = time.now() - ((opts.window_days or 30) * 86400)
    local fp = audit_log.fingerprint(req)
    local rows = db.query(
        "SELECT 1 FROM _hull_audit_log "
        .. "WHERE user_id = ? AND fingerprint = ? AND event_at >= ? "
        .. "LIMIT 1",
        { user_id, fp, cutoff })
    return rows == nil or #rows == 0
end

--- Delete events older than retain_days. Returns rowcount.
-- Typically scheduled via `app.daily("03:00", audit_log.cleanup)`.
function audit_log.cleanup()
    local cutoff = time.now() - (_state.retain_days * 86400)
    return db.exec("DELETE FROM _hull_audit_log WHERE event_at < ?",
                    { cutoff }) or 0
end

-- Exposed for tests (mocking time, inspecting normalize behavior).
audit_log._test = {
    normalize_ua = normalize_ua,
    ip_prefix    = ip_prefix,
    reset = function() _state.retain_days = 365; _state._initialized = false end,
}

return audit_log
