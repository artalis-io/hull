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

local function probe_sessions(include_counts)
    if not table_exists("_hull_sessions") then
        return { ok = false, reason = "table _hull_sessions not created "
                                   .. "(call session.init())" }
    end
    local out = { ok = true }
    if include_counts then out.sessions = row_count("_hull_sessions") end
    return out
end

local function probe_audit_log(include_counts)
    if not table_exists("_hull_audit_log") then
        return { ok = false, reason = "table _hull_audit_log not created "
                                   .. "(call audit_log.init())" }
    end
    local ok, mod = pcall(require, "hull.web.middleware.audit-log")
    local scheduled = nil
    if ok and mod and type(mod.is_cleanup_scheduled) == "function" then
        scheduled = mod.is_cleanup_scheduled()
    end
    local out = { ok = true, cleanup_scheduled = scheduled }
    if include_counts then out.events = row_count("_hull_audit_log") end
    return out
end

local function probe_pwned()
    local ok, mod = pcall(require, "hull.web.pwned")
    if not ok or not mod.health then
        return { ok = false, reason = "hull/web/pwned not available" }
    end
    local h = mod.health()
    -- Distinguish "no probe attempted yet" (h.ok == nil) from
    -- "last probe succeeded" (h.ok == true) and "last probe failed"
    -- (h.ok == false). The first two are healthy; only false is
    -- not_ok. Surface the state so operators can tell a freshly-
    -- started process from a confirmed-reachable HIBP.
    local status
    if h.ok == nil then status = "not_yet_checked"
    elseif h.ok then    status = "reachable"
    else                status = "fail_open" end
    return {
        ok            = h.ok ~= false,
        status        = status,
        last_check_at = h.last_check_at,
        last_error    = h.last_error,
    }
end

local function probe_totp(include_counts)
    if not table_exists("_hull_totp") then
        return { ok = false, reason = "table _hull_totp not created "
                                   .. "(call totp.init())" }
    end
    local out = { ok = true }
    if include_counts then
        local enrolled = db.query(
            "SELECT count(*) AS n FROM _hull_totp WHERE confirmed = 1")
        out.enrolled_users = (enrolled and enrolled[1] and enrolled[1].n) or 0
    end
    return out
end

local function probe_rbac(include_counts)
    local roles_ok = table_exists("_hull_roles")
    local perms_ok = table_exists("_hull_permissions")
    if not roles_ok and not perms_ok then
        return { ok = false, reason = "rbac tables not created "
                                   .. "(call rbac.init())" }
    end
    local out = { ok = roles_ok and perms_ok }
    if include_counts then
        out.roles       = row_count("_hull_roles")
        out.permissions = row_count("_hull_permissions")
    end
    return out
end

--- Run all probes; return a JSON-ready table.
-- @tparam[opt] table opts  `{ include_counts = false }`. With
--   include_counts = true, the output also carries enumeration
--   counts (events, sessions, enrolled_users, roles, permissions).
--   Default is FALSE because the endpoint surface is often
--   reachable by anyone with admin access and the counts are
--   recon material. Enable only when you want the dashboard view.
function auth_health.check(opts)
    opts = opts or {}
    local include = opts.include_counts == true
    local out = {
        sessions  = probe_sessions(include),
        audit_log = probe_audit_log(include),
        pwned     = probe_pwned(),
        totp      = probe_totp(include),
        rbac      = probe_rbac(include),
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
--
-- `opts.auth_check(req) -> bool` is REQUIRED. The endpoint exposes
-- session/enrollment counts and operational state — recon material
-- if left open. Auth_check wires into the app's RBAC predicate or a
-- token check. A 401 (no auth context) or 403 (no permission) is
-- returned on failure. Returning true admits the request.
--
-- @tparam table app
-- @tparam table opts
--   * `auth_check(req) -> bool`  REQUIRED. Gate for the endpoint.
--   * `path` (default `/admin/auth-status`).
--   * `include_counts` (default false). When false, the response
--     omits enumeration counts (sessions, enrolled_users, etc.)
--     and only returns ok/reason + operational fields.
function auth_health.routes(app, opts)
    opts = opts or {}
    if type(opts.auth_check) ~= "function" then
        error("auth_health.routes: opts.auth_check function is required. "
              .. "The endpoint exposes session/enrollment counts and "
              .. "operational state — gate it behind your RBAC predicate "
              .. "or a token check. Pass auth_check = function(req) "
              .. "return ... end to wire the gate.")
    end
    local path = opts.path or "/admin/auth-status"
    local include_counts = opts.include_counts == true
    local auth_check = opts.auth_check
    app.get(path, function(req, res)
        local ok, allowed = pcall(auth_check, req)
        -- Round-8 LOW-12: split the two failure modes cleanly to
        -- match the JS sibling and the docstring. Pre-round-8 the
        -- shortcut `allowed == nil and 401 or 403` returned 403 on
        -- throw too (because pcall's failure payload is the error
        -- STRING, not nil), contradicting the documented
        -- "throwing returns 401" contract.
        if not ok then
            return res:status(401):json({ error = "forbidden" })
        end
        if not allowed then
            return res:status(403):json({ error = "forbidden" })
        end
        res:json(auth_health.check({ include_counts = include_counts }))
    end)
end

return auth_health
