/*
 * hull:middleware:rbac -- Role-based access control
 *
 * rbac.init(opts?)                       - creates tables
 * rbac.defineRole(name, permissions?)    - insert role + optionally grant permissions
 * rbac.definePermission(name)            - insert permission
 * rbac.assign(userId, role)              - grant role to user
 * rbac.revoke(userId, role)              - remove role from user
 * rbac.grant(role, permission)           - add permission to role
 * rbac.ungrant(role, permission)         - remove permission from role
 * rbac.roles(userId)                     - array of role names
 * rbac.permissions(userId)               - array of permission names (via join)
 * rbac.hasRole(userId, role)             - boolean
 * rbac.hasPermission(userId, perm)       - boolean
 * rbac.hasAnyRole(userId, roles)         - boolean
 * rbac.hasAnyPermission(userId, perms)   - boolean
 * rbac.requireRole(roleOrRoles)          - returns middleware fn (403 if missing)
 * rbac.requirePermission(permOrPerms)    - returns middleware fn (403 if missing)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";

function init(opts) {
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_roles (" +
        "  name TEXT PRIMARY KEY" +
        ")"
    );
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_permissions (" +
        "  name TEXT PRIMARY KEY" +
        ")"
    );
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_role_permissions (" +
        "  role TEXT NOT NULL REFERENCES _hull_roles(name) ON DELETE CASCADE," +
        "  permission TEXT NOT NULL REFERENCES _hull_permissions(name) ON DELETE CASCADE," +
        "  PRIMARY KEY (role, permission)" +
        ")"
    );
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_user_roles (" +
        "  user_id TEXT NOT NULL," +
        "  role TEXT NOT NULL REFERENCES _hull_roles(name) ON DELETE CASCADE," +
        "  PRIMARY KEY (user_id, role)" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx__hull_user_roles_user " +
        "ON _hull_user_roles(user_id)"
    );
}

// M-8: reject null/undefined/empty-string names upfront. Otherwise the
// SQL layer would either insert "null"/"undefined" literals (via String
// coercion in the binding) or fail with a NOT NULL constraint error
// that obscures the API misuse.
function requireName(value, what) {
    if (typeof value !== "string" || value.length === 0)
        throw new Error("rbac: " + what + " is required");
    return value;
}

function defineRole(name, permissions) {
    const roleName = requireName(name, "role name");
    db.exec("INSERT OR IGNORE INTO _hull_roles (name) VALUES (?)", [roleName]);

    if (permissions) {
        for (let i = 0; i < permissions.length; i++) {
            const permName = requireName(permissions[i], "permission name");
            db.exec("INSERT OR IGNORE INTO _hull_permissions (name) VALUES (?)", [permName]);
            db.exec(
                "INSERT OR IGNORE INTO _hull_role_permissions (role, permission) VALUES (?, ?)",
                [roleName, permName]
            );
        }
    }
}

function definePermission(name) {
    const permName = requireName(name, "permission name");
    db.exec("INSERT OR IGNORE INTO _hull_permissions (name) VALUES (?)", [permName]);
}

// String() on null/undefined yields the literal "null"/"undefined", which
// would silently insert a real-looking user_id row. Reject explicitly.
function normalizeUserId(userId) {
    if (userId === null || userId === undefined)
        throw new Error("rbac: userId is required");
    return String(userId);
}

function assign(userId, role) {
    db.exec(
        "INSERT OR IGNORE INTO _hull_user_roles (user_id, role) VALUES (?, ?)",
        [normalizeUserId(userId), requireName(role, "role name")]
    );
}

function revoke(userId, role) {
    db.exec(
        "DELETE FROM _hull_user_roles WHERE user_id = ? AND role = ?",
        [normalizeUserId(userId), requireName(role, "role name")]
    );
}

function grant(role, permission) {
    db.exec(
        "INSERT OR IGNORE INTO _hull_role_permissions (role, permission) VALUES (?, ?)",
        [requireName(role, "role name"), requireName(permission, "permission name")]
    );
}

function ungrant(role, permission) {
    db.exec(
        "DELETE FROM _hull_role_permissions WHERE role = ? AND permission = ?",
        [requireName(role, "role name"), requireName(permission, "permission name")]
    );
}

function roles(userId) {
    const rows = db.query(
        "SELECT role FROM _hull_user_roles WHERE user_id = ? ORDER BY role",
        [normalizeUserId(userId)]
    );
    const result = [];
    for (let i = 0; i < rows.length; i++)
        result.push(rows[i].role);
    return result;
}

function permissions(userId) {
    const rows = db.query(
        "SELECT DISTINCT rp.permission FROM _hull_user_roles ur " +
        "JOIN _hull_role_permissions rp ON ur.role = rp.role " +
        "WHERE ur.user_id = ? ORDER BY rp.permission",
        [normalizeUserId(userId)]
    );
    const result = [];
    for (let i = 0; i < rows.length; i++)
        result.push(rows[i].permission);
    return result;
}

function hasRole(userId, role) {
    const rows = db.query(
        "SELECT 1 FROM _hull_user_roles WHERE user_id = ? AND role = ?",
        [normalizeUserId(userId), role]
    );
    return rows.length > 0;
}

function hasPermission(userId, permission) {
    const rows = db.query(
        "SELECT 1 FROM _hull_user_roles ur " +
        "JOIN _hull_role_permissions rp ON ur.role = rp.role " +
        "WHERE ur.user_id = ? AND rp.permission = ?",
        [normalizeUserId(userId), permission]
    );
    return rows.length > 0;
}

function hasAnyRole(userId, roleList) {
    const uid = normalizeUserId(userId);
    for (let i = 0; i < roleList.length; i++) {
        if (hasRole(uid, roleList[i]))
            return true;
    }
    return false;
}

function hasAnyPermission(userId, permList) {
    const uid = normalizeUserId(userId);
    for (let i = 0; i < permList.length; i++) {
        if (hasPermission(uid, permList[i]))
            return true;
    }
    return false;
}

function requireRole(roleOrRoles, opts) {
    const isList = Array.isArray(roleOrRoles);
    const getUserId = (opts && opts.getUserId) || null;

    return function requireRoleMw(req, res) {
        const userId = getUserId
            ? getUserId(req)
            : (req.ctx && req.ctx.session && req.ctx.session.user_id);
        if (!userId) {
            res.status(401);
            res.json({ error: "authentication required" });
            return 1;
        }
        const allowed = isList
            ? hasAnyRole(userId, roleOrRoles)
            : hasRole(userId, roleOrRoles);

        if (!allowed) {
            res.status(403);
            res.json({ error: "forbidden" });
            return 1;
        }

        return 0;
    };
}

function requirePermission(permOrPerms, opts) {
    const isList = Array.isArray(permOrPerms);
    const getUserId = (opts && opts.getUserId) || null;

    return function requirePermissionMw(req, res) {
        const userId = getUserId
            ? getUserId(req)
            : (req.ctx && req.ctx.session && req.ctx.session.user_id);
        if (!userId) {
            res.status(401);
            res.json({ error: "authentication required" });
            return 1;
        }
        const allowed = isList
            ? hasAnyPermission(userId, permOrPerms)
            : hasPermission(userId, permOrPerms);

        if (!allowed) {
            res.status(403);
            res.json({ error: "forbidden" });
            return 1;
        }

        return 0;
    };
}

const rbac = { init, defineRole, definePermission, assign, revoke, grant, ungrant,
               roles, permissions, hasRole, hasPermission, hasAnyRole, hasAnyPermission,
               requireRole, requirePermission };
export { rbac };
