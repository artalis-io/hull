--- Role-based access control backed by SQLite.
--
-- Stores `(role, permission, user_role)` triples in four `_hull_*` tables
-- and exposes both a query API (`has_role`, `has_permission`, ...) and
-- middleware factories (`require_role`, `require_permission`) that
-- short-circuit unauthorized requests with `401`/`403`.
--
-- @par Tables
--   `_hull_roles`, `_hull_permissions`, `_hull_role_permissions`,
--   `_hull_user_roles`. Schemas are created by `rbac.init()`.
--
-- @module hull.web.middleware.rbac
-- @license AGPL-3.0-or-later
-- @usage
--   local rbac = require("hull.web.middleware.rbac")
--   rbac.init()
--   rbac.define_role("admin", { "users.write", "users.delete" })
--   rbac.assign(user_id, "admin")
--   app.use_post("*", "/admin/*", rbac.require_role("admin"))

local db = require("hull.db")

local rbac = {}

--- Create the four `_hull_*` RBAC tables.
--
-- Idempotent — safe to call on every boot.
--
-- @function rbac.init
-- @tparam[opt] table _opts  Reserved for future configuration.
function rbac.init(_opts)
    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_roles (
            name TEXT PRIMARY KEY
        )
    ]])
    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_permissions (
            name TEXT PRIMARY KEY
        )
    ]])
    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_role_permissions (
            role TEXT NOT NULL REFERENCES _hull_roles(name) ON DELETE CASCADE,
            permission TEXT NOT NULL REFERENCES _hull_permissions(name) ON DELETE CASCADE,
            PRIMARY KEY (role, permission)
        )
    ]])
    db.exec([[
        CREATE TABLE IF NOT EXISTS _hull_user_roles (
            user_id TEXT NOT NULL,
            role TEXT NOT NULL REFERENCES _hull_roles(name) ON DELETE CASCADE,
            PRIMARY KEY (user_id, role)
        )
    ]])
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx__hull_user_roles_user
        ON _hull_user_roles(user_id)
    ]])
end

--- Define a role and optionally grant it permissions. Idempotent.
--
-- @function rbac.define_role
-- @tparam string name         Role name (e.g. `"admin"`, `"editor"`).
-- @tparam[opt] table permissions  Array of permission names to grant.
function rbac.define_role(name, permissions)
    db.insert_if_absent("_hull_roles", { "name" }, { "name" }, { name })
    if permissions then
        for _, perm in ipairs(permissions) do
            db.insert_if_absent("_hull_permissions", { "name" },
                { "name" }, { perm })
            db.insert_if_absent("_hull_role_permissions",
                { "role", "permission" },
                { "role", "permission" }, { name, perm })
        end
    end
end

--- Define a permission. Idempotent.
-- @function rbac.define_permission
-- @tparam string name
function rbac.define_permission(name)
    db.insert_if_absent("_hull_permissions", { "name" }, { "name" }, { name })
end

--- Assign a role to a user. Idempotent.
-- @function rbac.assign
-- @tparam string user_id
-- @tparam string role
function rbac.assign(user_id, role)
    db.insert_if_absent("_hull_user_roles",
        { "user_id", "role" },
        { "user_id", "role" }, { user_id, role })
end

--- Revoke a role from a user.
-- @function rbac.revoke
-- @tparam string user_id
-- @tparam string role
function rbac.revoke(user_id, role)
    db.exec(
        "DELETE FROM _hull_user_roles WHERE user_id = ? AND role = ?",
        { user_id, role }
    )
end

--- Grant a permission to a role. Idempotent.
-- @function rbac.grant
-- @tparam string role
-- @tparam string permission
function rbac.grant(role, permission)
    db.insert_if_absent("_hull_role_permissions",
        { "role", "permission" },
        { "role", "permission" }, { role, permission })
end

--- Remove a permission from a role.
-- @function rbac.ungrant
-- @tparam string role
-- @tparam string permission
function rbac.ungrant(role, permission)
    db.exec(
        "DELETE FROM _hull_role_permissions WHERE role = ? AND permission = ?",
        { role, permission }
    )
end

--- List all roles assigned to a user.
-- @function rbac.roles
-- @tparam string user_id
-- @treturn {string,...}  Array of role names (possibly empty).
function rbac.roles(user_id)
    local rows = db.query(
        "SELECT role FROM _hull_user_roles WHERE user_id = ?",
        { user_id }
    )
    local result = {}
    for i, row in ipairs(rows) do
        result[i] = row.role
    end
    return result
end

--- List distinct permissions a user holds (via any role).
-- @function rbac.permissions
-- @tparam string user_id
-- @treturn {string,...}
function rbac.permissions(user_id)
    local rows = db.query(
        [[SELECT DISTINCT rp.permission FROM _hull_user_roles ur
          JOIN _hull_role_permissions rp ON ur.role = rp.role
          WHERE ur.user_id = ?]],
        { user_id }
    )
    local result = {}
    for i, row in ipairs(rows) do
        result[i] = row.permission
    end
    return result
end

--- Does the user have this role?
-- @function rbac.has_role
-- @tparam string user_id
-- @tparam string role
-- @treturn boolean
function rbac.has_role(user_id, role)
    local rows = db.query(
        "SELECT 1 FROM _hull_user_roles WHERE user_id = ? AND role = ? LIMIT 1",
        { user_id, role }
    )
    return #rows > 0
end

--- Does the user have this permission (via any role)?
-- @function rbac.has_permission
-- @tparam string user_id
-- @tparam string permission
-- @treturn boolean
function rbac.has_permission(user_id, permission)
    local rows = db.query(
        [[SELECT 1 FROM _hull_user_roles ur
          JOIN _hull_role_permissions rp ON ur.role = rp.role
          WHERE ur.user_id = ? AND rp.permission = ? LIMIT 1]],
        { user_id, permission }
    )
    return #rows > 0
end

--- Does the user hold any role from the list?
-- @function rbac.has_any_role
-- @tparam string user_id
-- @tparam {string,...} roles_list
-- @treturn boolean
function rbac.has_any_role(user_id, roles_list)
    for _, role in ipairs(roles_list) do
        if rbac.has_role(user_id, role) then
            return true
        end
    end
    return false
end

--- Does the user hold any permission from the list?
-- @function rbac.has_any_permission
-- @tparam string user_id
-- @tparam {string,...} perms_list
-- @treturn boolean
function rbac.has_any_permission(user_id, perms_list)
    for _, perm in ipairs(perms_list) do
        if rbac.has_permission(user_id, perm) then
            return true
        end
    end
    return false
end

--- Middleware: require the user to hold a role (or any from a list).
--
-- - No `user_id` → `401`.
-- - User has none of the required roles → `403`.
-- - Otherwise → `0` (continue).
--
-- @function rbac.require_role
-- @tparam string|{string,...} role_or_roles  Single role or any-of array.
-- @tparam[opt] table opts
-- @tparam[opt] function(req)->string opts.get_user_id
--   Default resolves `req.ctx.session.user_id`.
-- @treturn function(req, res) -> 0|1
function rbac.require_role(role_or_roles, opts)
    opts = opts or {}
    local get_user_id = opts.get_user_id
    local roles_list
    if type(role_or_roles) == "string" then
        roles_list = { role_or_roles }
    else
        roles_list = role_or_roles
    end

    return function(req, res)
        local user_id
        if get_user_id then
            user_id = get_user_id(req)
        else
            local session = req.ctx and req.ctx.session
            user_id = session and session.user_id
        end
        if not user_id then
            res:status(401):json({ error = "authentication required" })
            return 1
        end

        for _, role in ipairs(roles_list) do
            if rbac.has_role(user_id, role) then
                return 0
            end
        end

        res:status(403):json({ error = "forbidden" })
        return 1
    end
end

--- Middleware: require the user to hold a permission (or any from a list).
--
-- Returns `401` for unauthenticated requests and `403` for authorized
-- users who lack every requested permission.
--
-- @function rbac.require_permission
-- @tparam string|{string,...} perm_or_perms
-- @tparam[opt] table opts  Same as `rbac.require_role`.
-- @treturn function(req, res) -> 0|1
function rbac.require_permission(perm_or_perms, opts)
    opts = opts or {}
    local get_user_id = opts.get_user_id
    local perms_list
    if type(perm_or_perms) == "string" then
        perms_list = { perm_or_perms }
    else
        perms_list = perm_or_perms
    end

    return function(req, res)
        local user_id
        if get_user_id then
            user_id = get_user_id(req)
        else
            local session = req.ctx and req.ctx.session
            user_id = session and session.user_id
        end
        if not user_id then
            res:status(401):json({ error = "authentication required" })
            return 1
        end

        for _, perm in ipairs(perms_list) do
            if rbac.has_permission(user_id, perm) then
                return 0
            end
        end

        res:status(403):json({ error = "forbidden" })
        return 1
    end
end

return rbac
