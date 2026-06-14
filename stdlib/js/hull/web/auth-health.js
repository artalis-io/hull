/**
 * @file hull:web:auth-health
 * @module hull:web:auth-health
 * @description Runtime health probes for the auth stack.
 *
 * Mirror of stdlib/lua/hull/web/auth-health.lua. See the Lua module
 * header for the design — what's probed, what's deliberately NOT
 * probed (external reachability), and the wiring patterns.
 *
 * @license AGPL-3.0-or-later
 */

import { db }        from "hull:db";
import { auditLog }  from "hull:web:middleware:audit-log";
import { pwned }     from "hull:web:pwned";

function tableExists(name) {
    const rows = db.query(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?",
        [name]);
    return rows && rows.length > 0;
}

function rowCount(name) {
    if (!tableExists(name)) return 0;
    const rows = db.query("SELECT count(*) AS n FROM " + name);
    return (rows && rows[0] && rows[0].n) || 0;
}

function probeSessions() {
    if (!tableExists("_hull_sessions")) {
        return { ok: false, reason: "table _hull_sessions not created "
                                 + "(call session.init())" };
    }
    return { ok: true, sessions: rowCount("_hull_sessions") };
}

function probeAuditLog() {
    if (!tableExists("_hull_audit_log")) {
        return { ok: false, reason: "table _hull_audit_log not created "
                                 + "(call auditLog.init())" };
    }
    const scheduled = typeof auditLog.isCleanupScheduled === "function"
        ? auditLog.isCleanupScheduled() : null;
    return {
        ok: true,
        events: rowCount("_hull_audit_log"),
        cleanup_scheduled: scheduled,
    };
}

function probePwned() {
    if (!pwned || typeof pwned.health !== "function") {
        return { ok: false, reason: "hull/web/pwned not available" };
    }
    const h = pwned.health();
    return {
        // ok=true when either the last attempt succeeded OR no attempt
        // has happened yet (process just started). Only the explicit
        // false (fail-open) is a "not ok".
        ok:            h.ok !== false,
        last_check_at: h.last_check_at,
        last_error:    h.last_error,
    };
}

function probeTotp() {
    if (!tableExists("_hull_totp")) {
        return { ok: false, reason: "table _hull_totp not created "
                                 + "(call totp.init())" };
    }
    const r = db.query(
        "SELECT count(*) AS n FROM _hull_totp WHERE confirmed = 1");
    return {
        ok: true,
        enrolled_users: (r && r[0] && r[0].n) || 0,
    };
}

function probeRbac() {
    const rolesOk = tableExists("_hull_roles");
    const permsOk = tableExists("_hull_permissions");
    if (!rolesOk && !permsOk) {
        return { ok: false, reason: "rbac tables not created "
                                 + "(call rbac.init())" };
    }
    return {
        ok:          rolesOk && permsOk,
        roles:       rowCount("_hull_roles"),
        permissions: rowCount("_hull_permissions"),
    };
}

function check() {
    const out = {
        sessions:  probeSessions(),
        audit_log: probeAuditLog(),
        pwned:     probePwned(),
        totp:      probeTotp(),
        rbac:      probeRbac(),
    };
    let allOk = true;
    for (const k in out) {
        if (!out[k].ok) { allOk = false; break; }
    }
    out.all_ok = allOk;
    return out;
}

function routes(app, opts) {
    const o = opts || {};
    const path = o.path || "/admin/auth-status";
    app.get(path, (_req, res) => res.json(check()));
}

const authHealth = { check, routes };
export { authHealth };
