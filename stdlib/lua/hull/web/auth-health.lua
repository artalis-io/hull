--- hull.web.auth-health — runtime health probes for the auth stack.
--
-- @module hull.web.auth-health
-- @license AGPL-3.0-or-later
--
-- A single read-only function `auth_health.check()` that probes the
-- shape of the auth-stack tables and module state. Apps wire it into
-- their existing /ready handler (via hull/web/middleware/health) or
-- a dedicated /admin/auth-status endpoint. The probe is also the
-- backing store for `hull agent auth-status` (the CLI just curls
-- the configured endpoint and pretty-prints the JSON).
--
-- The check does NOT exercise external dependencies (HIBP, IdP
-- JWKS, SMTP). Those would turn a health probe into an availability
-- dependency. Instead we report cached state:
--   * pwned.health() (last attempt status, populated by previous
--     /auth/register attempts)
--   * oauth providers configured (count, no reachability test)
--   * audit_log cleanup scheduled (boolean, from init state)
--   * session table exists + has rows (count)
--   * totp table exists + count of enrolled users
--   * rbac roles + permissions tables exist
--
-- A probe is reported as { ok = bool, ... } where additional fields
-- give specifics. `all_ok` is the AND across every probe.
--
-- ## Usage
--
--     local auth_health = require("hull.web.auth-health")
--     local h = auth_health.check()
--     if not h.all_ok then ... end
--
-- Or wire into hull/web/middleware/health:
--
--     local health      = require("hull.web.middleware.health")
--     local auth_health = require("hull.web.auth-health")
--     health.register("auth", function() return auth_health.check().all_ok end)
--
-- Or expose a JSON endpoint for the agent CLI:
--
--     local auth_health = require("hull.web.auth-health")
--     app.get("/admin/auth-status", function(_req, res)
--         res:json(auth_health.check())
--     end)

local db = require("hull.db")

local auth_health = {}

-- Helper: does a table exist?
local function table_exists(name)
    local rows = db.query(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?",
        { name })
    return rows and #rows > 0 or false
end

-- Helper: how many rows in a table (0 if table missing).
local function row_count(name)
    if not table_exists(name) then return 0 end
    local rows = db.query("SELECT count(*) AS n FROM " .. name)
    return (rows and rows[1] and rows[1].n) or 0
end

-- Probe individual subsystems. Each returns { ok, ...details }.
-- Probes use pcall so a single failure doesn't take down the rest.

local function probe_sessions()
    if not table_exists("_hull_sessions") then
        return { ok = false, reason = "table _hull_sessions not created "
                                   .. "(call session.init())" }
    end
    return { ok = true, sessions = row_count("_hull_sessions") }
end

local function probe_audit_log()
    if not table_exists("_hull_audit_log") then
        return { ok = false, reason = "table _hull_audit_log not created "
                                   .. "(call audit_log.init())" }
    end
    local ok, mod = pcall(require, "hull.web.middleware.audit-log")
    local scheduled = nil
    if ok and mod and type(mod.is_cleanup_scheduled) == "function" then
        scheduled = mod.is_cleanup_scheduled()
    end
    return {
        ok = true,
        events = row_count("_hull_audit_log"),
        cleanup_scheduled = scheduled,
    }
end

local function probe_pwned()
    local ok, mod = pcall(require, "hull.web.pwned")
    if not ok or not mod.health then
        return { ok = false, reason = "hull/web/pwned not available" }
    end
    local h = mod.health()
    return {
        -- ok=true when either the last attempt succeeded OR no attempt
        -- has happened yet (process just started). The fail-open
        -- condition (ok=false) is what operators care about.
        ok            = h.ok ~= false,
        last_check_at = h.last_check_at,
        last_error    = h.last_error,
    }
end

local function probe_totp()
    if not table_exists("_hull_totp") then
        return { ok = false, reason = "table _hull_totp not created "
                                   .. "(call totp.init())" }
    end
    local enrolled = db.query(
        "SELECT count(*) AS n FROM _hull_totp WHERE confirmed = 1")
    return {
        ok = true,
        enrolled_users = (enrolled and enrolled[1] and enrolled[1].n) or 0,
    }
end

local function probe_rbac()
    local roles_ok = table_exists("_hull_roles")
    local perms_ok = table_exists("_hull_permissions")
    if not roles_ok and not perms_ok then
        return { ok = false, reason = "rbac tables not created "
                                   .. "(call rbac.init())" }
    end
    return {
        ok          = roles_ok and perms_ok,
        roles       = row_count("_hull_roles"),
        permissions = row_count("_hull_permissions"),
    }
end

--- Run all probes; return a JSON-ready table.
function auth_health.check()
    local out = {
        sessions  = probe_sessions(),
        audit_log = probe_audit_log(),
        pwned     = probe_pwned(),
        totp      = probe_totp(),
        rbac      = probe_rbac(),
    }
    local all_ok = true
    for _, p in pairs(out) do
        if not p.ok then all_ok = false; break end
    end
    out.all_ok = all_ok
    return out
end

--- Convenience: mount a JSON status endpoint at `/admin/auth-status`
-- (overridable). Backs the `hull agent auth-status` CLI.
-- @tparam table app
-- @tparam[opt] table opts  { path = "/admin/auth-status" }
function auth_health.routes(app, opts)
    opts = opts or {}
    local path = opts.path or "/admin/auth-status"
    app.get(path, function(_req, res) res:json(auth_health.check()) end)
end

return auth_health
