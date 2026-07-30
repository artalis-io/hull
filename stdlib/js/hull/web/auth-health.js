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

import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { auditLog }  from "hull:web:middleware:audit-log";
import { pwned }     from "hull:web:pwned";

// Does a table exist? Uses the dialect-aware column probe (SQLite
// PRAGMA table_info / Postgres information_schema) so the health check
// works on either backend. An empty column set means the table is absent.
function tableExists(name) {
    const cols = db.tableColumns(name);
    return cols && cols.length > 0;
}

function rowCount(name) {
    // All callers pass hardcoded `_hull_*` literals, but this interpolates the
    // name into SQL (SQLite can't parameterize identifiers), so fail closed on
    // anything that isn't a plain identifier -- a future variable table name can
    // never become an injection vector (mirrors auth-health.lua).
    if (typeof name !== "string" || !/^[A-Za-z0-9_]+$/.test(name))
        throw new Error("auth-health.rowCount: table name must be a plain identifier");
    if (!tableExists(name)) return 0;
    const rows = db.query("SELECT count(*) AS n FROM " + name);
    return (rows && rows[0] && rows[0].n) || 0;
}

function probeSessions(includeCounts) {
    if (!tableExists("_hull_sessions")) {
        return { ok: false, reason: "table _hull_sessions not created "
                                 + "(call session.init())" };
    }
    const out = { ok: true };
    if (includeCounts) out.sessions = rowCount("_hull_sessions");
    return out;
}

function probeAuditLog(includeCounts) {
    if (!tableExists("_hull_audit_log")) {
        return { ok: false, reason: "table _hull_audit_log not created "
                                 + "(call auditLog.init())" };
    }
    // Round-11 MEDIUM-7: tri-state cleanup status. See Lua sibling.
    const status = typeof auditLog.cleanupStatus === "function"
        ? auditLog.cleanupStatus() : null;
    const scheduled = typeof auditLog.isCleanupScheduled === "function"
        ? auditLog.isCleanupScheduled() : null;
    const out = { ok: true,
                  cleanup: status,
                  cleanup_scheduled: scheduled };
    if (includeCounts) out.events = rowCount("_hull_audit_log");
    return out;
}

function probePwned() {
    if (!pwned || typeof pwned.health !== "function") {
        return { ok: false, reason: "hull/web/pwned not available" };
    }
    const h = pwned.health();
    // Distinguish "no probe attempted yet" (h.ok == null) from
    // "last probe succeeded" (true) and "last probe failed" (false).
    let status;
    if (h.ok == null)   status = "not_yet_checked";
    else if (h.ok)      status = "reachable";
    else                status = "fail_open";
    return {
        ok:            h.ok !== false,
        status:        status,
        last_check_at: h.last_check_at,
        last_error:    h.last_error,
    };
}

function probeTotp(includeCounts) {
    if (!tableExists("_hull_totp")) {
        return { ok: false, reason: "table _hull_totp not created "
                                 + "(call totp.init())" };
    }
    const out = { ok: true };
    if (includeCounts) {
        const r = db.query(
            "SELECT count(*) AS n FROM _hull_totp WHERE confirmed = 1");
        out.enrolled_users = (r && r[0] && r[0].n) || 0;
    }
    return out;
}

function probeRbac(includeCounts) {
    const rolesOk = tableExists("_hull_roles");
    const permsOk = tableExists("_hull_permissions");
    if (!rolesOk && !permsOk) {
        return { ok: false, reason: "rbac tables not created "
                                 + "(call rbac.init())" };
    }
    const out = { ok: rolesOk && permsOk };
    if (includeCounts) {
        out.roles       = rowCount("_hull_roles");
        out.permissions = rowCount("_hull_permissions");
    }
    return out;
}

/**
 * Run all probes; return a JSON-ready object.
 * opts.includeCounts (default false) gates enumeration counts
 * (events, sessions, enrolled_users, roles, permissions). Default
 * is FALSE — the endpoint surface is often reachable by anyone with
 * admin access and the counts are recon material.
 */
function check(opts) {
    const o = opts || {};
    const include = o.includeCounts === true;
    const out = {
        sessions:  probeSessions(include),
        audit_log: probeAuditLog(include),
        pwned:     probePwned(),
        totp:      probeTotp(include),
        rbac:      probeRbac(include),
    };
    let allOk = true;
    for (const k in out) {
        if (!out[k].ok) { allOk = false; break; }
    }
    out.all_ok = allOk;
    return out;
}

/**
 * Mount /admin/auth-status (overridable). opts.authCheck(req) is
 * REQUIRED — the endpoint exposes session/enrollment counts and
 * operational state, recon material if left open. Must return the
 * boolean `true` to admit; any other value (including truthy ones
 * like `1` or `"yes"`) is rejected with 403; throwing returns 401;
 * returning a Promise (async authCheck) returns 500.
 * opts.includeCounts (default false) gates enumeration counts.
 */
function routes(app, opts) {
    const o = opts || {};
    if (typeof o.authCheck !== "function") {
        throw new Error("authHealth.routes: opts.authCheck function is "
            + "required. The endpoint exposes session/enrollment counts "
            + "and operational state — gate it behind your RBAC "
            + "predicate or a token check.");
    }
    const path = o.path || "/admin/auth-status";
    const includeCounts = o.includeCounts === true;
    const authCheck = o.authCheck;
    app.get(path, (req, res) => {
        let allowed;
        try { allowed = authCheck(req); }
        catch (_e) { return res.status(401).json({ error: "forbidden" }); }
        // Round-8 MEDIUM-5: strict-equality the boolean true. An
        // `async authCheck` returns a Promise, which is truthy under
        // `!allowed` — the gate silently fell open and the handler
        // shipped operational state to anonymous callers. Reject any
        // thenable explicitly so the gate-by-async-mistake fails
        // closed AT the gate, not at the wire.
        if (allowed && typeof allowed.then === "function") {
            return res.status(500).json({
                error: "authCheck must be synchronous; "
                     + "received a thenable (Promise)",
            });
        }
        if (allowed !== true) {
            return res.status(403).json({ error: "forbidden" });
        }
        res.json(check({ includeCounts: includeCounts }));
    });
}

const authHealth = { check, routes };
export { authHealth };
