/**
 * @file hull:web:middleware:rbac
 * @module hull:web:middleware:rbac
 * @description Role-based access control backed by SQLite. Lua parity:
 *   `hull.web.middleware.rbac`.
 *
 * Stores `(role, permission, user_role)` triples in four `_hull_*` tables
 * and exposes both a query API (`hasRole`, `hasPermission`, ...) and
 * middleware factories (`requireRole`, `requirePermission`) that
 * short-circuit unauthorized requests with `401`/`403`.
 *
 * Tables: `_hull_roles`, `_hull_permissions`, `_hull_role_permissions`,
 * `_hull_user_roles`. Schemas are created by `rbac.init()`.
 *
 * @license AGPL-3.0-or-later
 */

import { db as dbModule } from "hull:db";
const db = dbModule.default();
/**
 * Create the four `_hull_*` RBAC tables. Idempotent.
 *
 * @param {Object} [opts]  Reserved for future configuration.
 */
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

/**
 * Define a role and optionally grant it permissions. Idempotent.
 *
 * @param {string} name             Role name (e.g. `"admin"`).
 * @param {string[]} [permissions]  Array of permission names to grant.
 */
function defineRole(name, permissions) {
    const roleName = requireName(name, "role name");
    db.insertIfAbsent("_hull_roles", ["name"], ["name"], [roleName]);

    if (permissions) {
        for (let i = 0; i < permissions.length; i++) {
            const permName = requireName(permissions[i], "permission name");
            db.insertIfAbsent("_hull_permissions", ["name"], ["name"], [permName]);
            db.insertIfAbsent(
                "_hull_role_permissions",
                ["role", "permission"],
                ["role", "permission"],
                [roleName, permName]
            );
        }
    }
}

/**
 * Define a permission. Idempotent.
 * @param {string} name
 */
function definePermission(name) {
    const permName = requireName(name, "permission name");
    db.insertIfAbsent("_hull_permissions", ["name"], ["name"], [permName]);
}

// String() on null/undefined yields the literal "null"/"undefined", which
// would silently insert a real-looking user_id row. Reject explicitly.
function normalizeUserId(userId) {
    if (userId === null || userId === undefined)
        throw new Error("rbac: userId is required");
    return String(userId);
}

/**
 * Assign a role to a user. Idempotent.
 * @param {string} userId
 * @param {string} role
 */
function assign(userId, role) {
    db.insertIfAbsent(
        "_hull_user_roles",
        ["user_id", "role"],
        ["user_id", "role"],
        [normalizeUserId(userId), requireName(role, "role name")]
    );
}

/**
 * Revoke a role from a user.
 * @param {string} userId
 * @param {string} role
 */
function revoke(userId, role) {
    db.exec(
        "DELETE FROM _hull_user_roles WHERE user_id = ? AND role = ?",
        [normalizeUserId(userId), requireName(role, "role name")]
    );
}

/**
 * Grant a permission to a role. Idempotent.
 * @param {string} role
 * @param {string} permission
 */
function grant(role, permission) {
    db.insertIfAbsent(
        "_hull_role_permissions",
        ["role", "permission"],
        ["role", "permission"],
        [requireName(role, "role name"), requireName(permission, "permission name")]
    );
}

/**
 * Remove a permission from a role.
 * @param {string} role
 * @param {string} permission
 */
function ungrant(role, permission) {
    db.exec(
        "DELETE FROM _hull_role_permissions WHERE role = ? AND permission = ?",
        [requireName(role, "role name"), requireName(permission, "permission name")]
    );
}

/**
 * List all roles assigned to a user.
 * @param {string} userId
 * @returns {string[]}
 */
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

/**
 * List distinct permissions the user holds (via any role).
 * @param {string} userId
 * @returns {string[]}
 */
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

/**
 * Does the user have this role?
 * @param {string} userId
 * @param {string} role
 * @returns {boolean}
 */
function hasRole(userId, role) {
    const rows = db.query(
        "SELECT 1 FROM _hull_user_roles WHERE user_id = ? AND role = ?",
        [normalizeUserId(userId), role]
    );
    return rows.length > 0;
}

/**
 * Does the user have this permission (via any role)?
 * @param {string} userId
 * @param {string} permission
 * @returns {boolean}
 */
function hasPermission(userId, permission) {
    const rows = db.query(
        "SELECT 1 FROM _hull_user_roles ur " +
        "JOIN _hull_role_permissions rp ON ur.role = rp.role " +
        "WHERE ur.user_id = ? AND rp.permission = ?",
        [normalizeUserId(userId), permission]
    );
    return rows.length > 0;
}

/**
 * Does the user hold any role in the list?
 * @param {string} userId
 * @param {string[]} roleList
 * @returns {boolean}
 */
function hasAnyRole(userId, roleList) {
    const uid = normalizeUserId(userId);
    for (let i = 0; i < roleList.length; i++) {
        if (hasRole(uid, roleList[i]))
            return true;
    }
    return false;
}

/**
 * Does the user hold any permission in the list?
 * @param {string} userId
 * @param {string[]} permList
 * @returns {boolean}
 */
function hasAnyPermission(userId, permList) {
    const uid = normalizeUserId(userId);
    for (let i = 0; i < permList.length; i++) {
        if (hasPermission(uid, permList[i]))
            return true;
    }
    return false;
}

/**
 * Middleware: require the user to hold a role (or any from a list).
 *
 * - No `userId` → `401`.
 * - User has none of the required roles → `403`.
 * - Otherwise → `0` (continue).
 *
 * @param {string|string[]} roleOrRoles  Single role or any-of array.
 * @param {Object} [opts]
 * @param {(req) => string} [opts.getUserId]
 *   Default resolves `req.ctx.session.user_id`.
 * @returns {(req, res) => number}
 *
 * @example
 * app.usePost("*", "/admin/*", rbac.requireRole("admin"));
 */
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

/**
 * Middleware: require the user to hold a permission (or any from a list).
 *
 * Returns `401` for unauthenticated requests and `403` for authorized
 * users who lack every requested permission.
 *
 * @param {string|string[]} permOrPerms
 * @param {Object} [opts]  Same as `requireRole`.
 * @returns {(req, res) => number}
 */
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
