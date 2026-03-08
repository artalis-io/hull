--
-- hull.rbac -- Role-based access control middleware
--
-- DB-backed RBAC with role/permission management and middleware factories.
-- Uses the global `db` object for storage.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local rbac = {}

--- Initialize RBAC tables.
-- opts: reserved for future configuration
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

--- Define a role. Idempotent.
-- name: role name
-- permissions: optional array of permission names to grant
function rbac.define_role(name, permissions)
    db.exec("INSERT OR IGNORE INTO _hull_roles (name) VALUES (?)", { name })
    if permissions then
        for _, perm in ipairs(permissions) do
            db.exec("INSERT OR IGNORE INTO _hull_permissions (name) VALUES (?)", { perm })
            db.exec(
                "INSERT OR IGNORE INTO _hull_role_permissions (role, permission) VALUES (?, ?)",
                { name, perm }
            )
        end
    end
end

--- Define a permission. Idempotent.
function rbac.define_permission(name)
    db.exec("INSERT OR IGNORE INTO _hull_permissions (name) VALUES (?)", { name })
end

--- Assign a role to a user. Idempotent.
function rbac.assign(user_id, role)
    db.exec(
        "INSERT OR IGNORE INTO _hull_user_roles (user_id, role) VALUES (?, ?)",
        { user_id, role }
    )
end

--- Revoke a role from a user.
function rbac.revoke(user_id, role)
    db.exec(
        "DELETE FROM _hull_user_roles WHERE user_id = ? AND role = ?",
        { user_id, role }
    )
end

--- Grant a permission to a role. Idempotent.
function rbac.grant(role, permission)
    db.exec(
        "INSERT OR IGNORE INTO _hull_role_permissions (role, permission) VALUES (?, ?)",
        { role, permission }
    )
end

--- Remove a permission from a role.
function rbac.ungrant(role, permission)
    db.exec(
        "DELETE FROM _hull_role_permissions WHERE role = ? AND permission = ?",
        { role, permission }
    )
end

--- Get all roles for a user.
-- Returns an array of role name strings.
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

--- Get all permissions for a user (via role membership).
-- Returns an array of distinct permission name strings.
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

--- Check if a user has a specific role.
function rbac.has_role(user_id, role)
    local rows = db.query(
        "SELECT 1 FROM _hull_user_roles WHERE user_id = ? AND role = ? LIMIT 1",
        { user_id, role }
    )
    return #rows > 0
end

--- Check if a user has a specific permission (via any of their roles).
function rbac.has_permission(user_id, permission)
    local rows = db.query(
        [[SELECT 1 FROM _hull_user_roles ur
          JOIN _hull_role_permissions rp ON ur.role = rp.role
          WHERE ur.user_id = ? AND rp.permission = ? LIMIT 1]],
        { user_id, permission }
    )
    return #rows > 0
end

--- Check if a user has at least one of the given roles.
-- roles_list: array of role name strings
function rbac.has_any_role(user_id, roles_list)
    for _, role in ipairs(roles_list) do
        if rbac.has_role(user_id, role) then
            return true
        end
    end
    return false
end

--- Check if a user has at least one of the given permissions.
-- perms_list: array of permission name strings
function rbac.has_any_permission(user_id, perms_list)
    for _, perm in ipairs(perms_list) do
        if rbac.has_permission(user_id, perm) then
            return true
        end
    end
    return false
end

--- Middleware factory: require the user to have a role (or any of several roles).
-- role_or_roles: a string (single role) or table (array of roles, user needs at least one)
-- Returns a middleware function.
function rbac.require_role(role_or_roles)
    local roles_list
    if type(role_or_roles) == "string" then
        roles_list = { role_or_roles }
    else
        roles_list = role_or_roles
    end

    return function(req, res)
        local session = req.ctx.session
        if not session or not session.user_id then
            res:status(403):json({ error = "forbidden" })
            return 1
        end

        local user_id = session.user_id

        for _, role in ipairs(roles_list) do
            if rbac.has_role(user_id, role) then
                return 0
            end
        end

        res:status(403):json({ error = "forbidden" })
        return 1
    end
end

--- Middleware factory: require the user to have a permission (or any of several).
-- perm_or_perms: a string (single permission) or table (array, user needs at least one)
-- Returns a middleware function.
function rbac.require_permission(perm_or_perms)
    local perms_list
    if type(perm_or_perms) == "string" then
        perms_list = { perm_or_perms }
    else
        perms_list = perm_or_perms
    end

    return function(req, res)
        local session = req.ctx.session
        if not session or not session.user_id then
            res:status(403):json({ error = "forbidden" })
            return 1
        end

        local user_id = session.user_id

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
