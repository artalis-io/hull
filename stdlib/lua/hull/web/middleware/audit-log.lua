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
local log    = require("hull.log")

local audit_log = {}

local _state = {
    retain_days       = 365,
    fingerprint_salt  = nil,  -- required at init time
    _initialized      = false,
}

--- Initialize the audit-log table. Call once at app startup
-- (typically right after session.init()).
-- @tparam table opts
--   * `fingerprint_salt` (REQUIRED, string ≥ 8 bytes). Deployment-
--     private salt prepended to the sha256 input that produces the
--     device fingerprint. Without a salt, the fingerprint algorithm
--     is universal — an attacker with read access to _hull_audit_log
--     could rainbow-table common (UA, IP) combinations against the
--     published normalization to dehash entries. The salt makes the
--     output deployment-private.
--     BREAKING: this opt is mandatory as of the round-6 hardening.
--     Existing rows have unsalted fingerprints; they will not match
--     new ones, so is_new_device would fire for every active user
--     once on next sign-in (mass "new device login" emails).
--     Mitigation: run `audit_log.recompute_fingerprints()` ONCE
--     during deploy, before traffic resumes on the new salt. The
--     helper recomputes every row's fingerprint from its stored
--     ip + user_agent under the current salt.
--   * `retain_days` (default 365). Rows older than this are deleted
--     by the scheduled cleanup.
--   * `cleanup` (default true). When true, schedules a daily timer
--     via app.daily that calls audit_log.cleanup(). Set false if you
--     prefer to drive cleanup from a CRON job, a separate worker,
--     or your own app.daily wiring.
--   * `cleanup_at` (default "03:00", UTC). Wall-clock time for the
--     daily cleanup. Off-peak by default.
function audit_log.init(opts)
    opts = opts or {}
    if type(opts.fingerprint_salt) ~= "string"
       or #opts.fingerprint_salt < 8 then
        error("audit_log.init: fingerprint_salt (string >= 8 bytes) "
              .. "is required. Pick a deployment-private value; "
              .. "rotating it invalidates existing fingerprints.")
    end
    _state.fingerprint_salt = opts.fingerprint_salt
    if opts.retain_days ~= nil then
        _state.retain_days = opts.retain_days
    end
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_audit_log ("
        .. "id          " .. db.autoincrement_id_ddl .. ", "
        .. "user_id     TEXT    NOT NULL,"
        .. "event_at    INTEGER NOT NULL,"
        .. "kind        TEXT    NOT NULL,"
        .. "ip          TEXT,"
        .. "user_agent  TEXT,"
        .. "fingerprint TEXT,"
        .. "session_id  TEXT,"
        .. "metadata    TEXT)")
    db.exec([[
        CREATE INDEX IF NOT EXISTS _hull_audit_log_user_at
            ON _hull_audit_log(user_id, event_at DESC)
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS _hull_audit_log_user_fp
            ON _hull_audit_log(user_id, fingerprint)
    ]])
    -- Lazy catchup: run cleanup once at init so apps that
    -- restart between scheduled fires (deploys, crashes) and
    -- apps in CLI flavor (where the daily timer never fires
    -- because app.main exits before 03:00) still bound their
    -- _hull_audit_log growth. Cheap — a single DELETE WHERE
    -- event_at < cutoff over an indexed range. Guarded against
    -- repeated init() calls in the same process (test fixtures,
    -- hot reload) so the DB scan only runs once per startup,
    -- not per re-init. Skipped when opts.cleanup = false.
    if opts.cleanup ~= false and not _state._catchup_done then
        local ok, err = pcall(audit_log.cleanup)
        if not ok then
            log.warn("audit-log: init-time cleanup failed: " .. tostring(err))
        end
        _state._catchup_done = true
    end

    -- Auto-schedule daily cleanup unless opted out. Prevents
    -- _hull_audit_log from growing unboundedly for apps that
    -- forget to wire a cleanup timer themselves. Guarded so a
    -- second init() (test fixtures, hot reload) doesn't stack
    -- timers — schedule once per process. app.daily comes from
    -- hull/timers, declared as a hard dep in module_registry.
    --
    -- CLI flavor (HL_ENABLE_HTTP_SERVER=0) has no serve loop, so
    -- the timer would never fire. The lazy-catchup above is the
    -- primary protection in that mode; warn once so operators
    -- know to wire cron / a separate worker for steady-state
    -- cleanup if the process lives more than a day.
    if opts.cleanup ~= false and not _state._cleanup_scheduled then
        local at = opts.cleanup_at or "03:00"
        if type(app) == "table" and type(app.daily) == "function" then
            app.daily(at, function() audit_log.cleanup() end)
            _state._cleanup_scheduled = true
        else
            log.warn("audit-log: app.daily not available "
                  .. "(CLI flavor or hull/timers not admitted) "
                  .. "— auto-cleanup runs only at init(). "
                  .. "Wire your own cron/worker for steady-state.")
        end
    end
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
    local ip
    local xff = req.headers["x-forwarded-for"]
    if xff then
        local first = xff:match("^([^,]+)")
        if first then
            ip = first:gsub("^%s+", ""):gsub("%s+$", "")
        end
    end
    ip = ip or req.remote_addr
    -- Round-9 MEDIUM-7: cap IP length. Same rationale as the
    -- session.create cap — an attacker submitting a 64 KiB XFF
    -- header would land the whole string in the indexed _hull_
    -- audit_log.ip column. 64 chars covers IPv6 with headroom.
    if type(ip) == "string" and #ip > 64 then
        ip = ip:sub(1, 64)
    end
    return ip
end

local function extract_ua(req)
    return req and req.headers and req.headers["user-agent"] or nil
end

--- Compute the (coarse) device fingerprint for a request.
-- Hex SHA-256(salt || "|" || normalized_ua || "|" || ip_prefix),
-- truncated to 16 chars (64 bits). The salt is deployment-private
-- (from init's fingerprint_salt opt) so the fingerprint output is
-- only meaningful within this deployment — an attacker with read
-- access to _hull_audit_log can't rainbow-table common (UA, IP)
-- combinations back to identifying data without also breaching
-- the app's config.
function audit_log.fingerprint(req)
    local ua  = extract_ua(req)
    local ip  = extract_ip(req)
    local salt = _state.fingerprint_salt or ""
    local key = salt .. "|" .. normalize_ua(ua) .. "|" .. ip_prefix(ip)
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
    -- Bound the user_agent column the same way session.create does.
    -- Real UAs top out around 500 chars; bots and scanners can send
    -- 100 KB UAs. 512 covers all real traffic.
    if type(ua) == "string" and #ua > 512 then ua = ua:sub(1, 512) end
    -- App-supplied fingerprint overrides are accepted but capped at
    -- 64 chars so apps that wire a header-derived fingerprint can't
    -- land arbitrary-length values in the column. The auto-derived
    -- fingerprint is always 16 chars (sha256 prefix).
    local fp = opts.fingerprint or audit_log.fingerprint(req)
    if type(fp) == "string" and #fp > 64 then fp = fp:sub(1, 64) end
    -- Metadata column cap. Apps that accidentally put request bodies
    -- or debug dumps into metadata otherwise grow the column without
    -- bound (default 365-day retention compounds the damage). 4 KB
    -- is generous for the canonical { factors = ..., sub = ... }
    -- shape and small enough that 1M rows stay under 4 GB. Drop
    -- with a log.warn on overflow rather than truncating, since a
    -- truncated JSON tail is unparseable.
    local META_MAX = 4096
    local meta = nil
    if opts.metadata ~= nil then
        meta = json.encode(opts.metadata)
        if #meta > META_MAX then
            log.warn(string.format(
                "audit-log: metadata for kind '%s' is %d bytes (max %d); dropping",
                kind, #meta, META_MAX))
            meta = nil
        end
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

--- Read-only view: is the auto-daily cleanup timer scheduled in
-- this process? Used by hull/web/auth-health to surface "audit log
-- will be pruned" in the health JSON.
-- @treturn boolean
function audit_log.is_cleanup_scheduled()
    return _state._catchup_done == true and _state._cleanup_scheduled == true
end

--- Migration helper: recompute every row's `fingerprint` column
-- from its stored `ip` + `user_agent` under the current
-- `fingerprint_salt`. Use ONCE after switching salts (or after
-- the round-6 mandatory-salt upgrade) to avoid a mass
-- `is_new_device` storm — without this, every active user's next
-- sign-in would trigger the new-device path because the new
-- salted fingerprint doesn't match the old unsalted one.
--
-- Idempotent: rows whose stored fingerprint already matches the
-- recomputed value are skipped. Returns `{scanned, updated}`.
-- Run during deploy, before traffic resumes on the new salt.
--
-- The `ip` and `user_agent` columns are stored alongside the
-- fingerprint, so no external data is needed.
-- Round-8 MEDIUM-7: page through the table so the runtime doesn't
-- hold the entire _hull_audit_log in memory and so a partial run
-- (DB lock, crash, OOM) leaves a coherent batch boundary instead of
-- thousands of auto-committed singleton UPDATEs. Page size is a
-- compromise: small enough that a runaway run can be aborted with
-- visible progress (returns counters even if you `kill -INT`), big
-- enough that 365-day retention on a busy site finishes in minutes
-- not hours. Each page runs inside db.batch so the per-page UPDATEs
-- share one transaction.
local RECOMPUTE_PAGE = 5000

-- Round-9 MEDIUM-9: in-process mutex. The helper is publicly
-- exported for ops to call ONCE at deploy time, but a misconfigured
-- app could accidentally route it (`/admin/recompute`) or two
-- operators could kick it off concurrently. Two concurrent callers
-- would double-scan + double-write the same rows, blocking
-- audit_log.record (which sits on every login) for minutes.
--
-- Round-10 LOW-12: store the start time instead of a bool so a
-- runtime panic that doesn't unwind the pcall doesn't strand the
-- flag at `true` forever. After RECOMPUTE_STALE_AFTER seconds the
-- flag is treated as stale and a fresh call proceeds. The actual
-- work uses pcall so normal errors clear the flag; this is the
-- escape hatch for C-level aborts.
local _recompute_started_at = nil
local RECOMPUTE_STALE_AFTER = 3600  -- 1 hour

function audit_log.recompute_fingerprints()
    local now = time.now()
    if _recompute_started_at
       and (now - _recompute_started_at) < RECOMPUTE_STALE_AFTER then
        return { scanned = 0, updated = 0, error = "already_running" }
    end
    _recompute_started_at = now
    local ok, result = pcall(function()
        local salt = _state.fingerprint_salt or ""
        -- Round-10 HIGH-2: snapshot the highest id at start. Round-9
        -- MEDIUM-10 broke only on empty page, but under sustained
        -- INSERT load (audit_log.record fires per login) the loop
        -- never terminates — every SELECT finds new rows past
        -- last_id. The single live caller would hold the writer
        -- transaction periodically and grow page_updates without
        -- bound. Bounding by start_max_id terminates correctly while
        -- still covering everything that existed at call time.
        -- Rows inserted DURING recompute are written with the
        -- current salt already (audit_log.record uses the same salt)
        -- so they don't need re-fingerprinting.
        local max_rows = db.query(
            "SELECT MAX(id) AS max_id FROM _hull_audit_log")
        local start_max_id = max_rows and max_rows[1]
                             and max_rows[1].max_id or 0
        local scanned, updated = 0, 0
        local last_id = -1
        while last_id < start_max_id do
            local rows = db.query(
                "SELECT id, ip, user_agent, fingerprint "
                .. "FROM _hull_audit_log "
                .. "WHERE id > ? AND id <= ? "
                .. "ORDER BY id LIMIT ?",
                { last_id, start_max_id, RECOMPUTE_PAGE })
            if not rows or #rows == 0 then break end
            local page_updates = {}
            for _, r in ipairs(rows) do
                scanned = scanned + 1
                last_id = r.id
                local key = salt .. "|" .. normalize_ua(r.user_agent)
                               .. "|" .. ip_prefix(r.ip)
                local new_fp = crypto.hex_encode(crypto.sha256(key)):sub(1, 16)
                if new_fp ~= r.fingerprint then
                    page_updates[#page_updates + 1] = { new_fp, r.id }
                end
            end
            if #page_updates > 0 then
                db.batch(function()
                    for _, u in ipairs(page_updates) do
                        db.exec(
                            "UPDATE _hull_audit_log SET fingerprint = ? "
                            .. "WHERE id = ?", u)
                    end
                end)
                updated = updated + #page_updates
            end
        end
        return { scanned = scanned, updated = updated }
    end)
    _recompute_started_at = nil
    if not ok then error(result) end
    return result
end

-- Exposed for tests (mocking time, inspecting normalize behavior).
audit_log._test = {
    normalize_ua = normalize_ua,
    ip_prefix    = ip_prefix,
    reset = function()
        _state.retain_days       = 365
        _state.fingerprint_salt  = nil
        _state._catchup_done     = false
        _state._cleanup_scheduled = false
        _state._initialized      = false
    end,
}

return audit_log
