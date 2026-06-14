/**
 * @file hull:web:middleware:audit-log
 * @module hull:web:middleware:audit-log
 * @description Append-only sign-in / auth event log.
 *
 * Mirror of stdlib/lua/hull/web/middleware/audit-log.lua. See the
 * Lua module header for the design rationale + fingerprint
 * algorithm.
 *
 *   auditLog.init(opts?)
 *   auditLog.record(userId, kind, req, opts?)
 *   auditLog.list(userId, opts?)
 *   auditLog.listDevices(userId, opts?)
 *   auditLog.isNewDevice(userId, req, opts?) -> bool
 *   auditLog.fingerprint(req)
 *   auditLog.cleanup()
 *
 * @license AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";
import { db }     from "hull:db";
import { time }   from "hull:time";
import { json }   from "hull:json";
import { app }    from "hull:app";

const _state = {
    retainDays: 365,
    initialized: false,
    cleanupScheduled: false,
};

function init(opts) {
    opts = opts || {};
    if (opts.retainDays !== undefined) _state.retainDays = opts.retainDays;
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_audit_log ("
        + "id          " + db.autoincrementIdDdl + ", "
        + "user_id     TEXT    NOT NULL,"
        + "event_at    INTEGER NOT NULL,"
        + "kind        TEXT    NOT NULL,"
        + "ip          TEXT,"
        + "user_agent  TEXT,"
        + "fingerprint TEXT,"
        + "session_id  TEXT,"
        + "metadata    TEXT)");
    db.exec(`CREATE INDEX IF NOT EXISTS _hull_audit_log_user_at
             ON _hull_audit_log(user_id, event_at DESC)`);
    db.exec(`CREATE INDEX IF NOT EXISTS _hull_audit_log_user_fp
             ON _hull_audit_log(user_id, fingerprint)`);
    // Auto-schedule daily cleanup unless opted out. Prevents
    // _hull_audit_log from growing unboundedly for apps that
    // forget to wire a cleanup timer themselves. Guarded so a
    // second init() (test fixtures, hot reload) doesn't stack
    // timers. app.daily comes from hull/timers, declared as a
    // hard dep of hull/web/middleware/audit-log.
    if (opts.cleanup !== false && !_state.cleanupScheduled) {
        const at = opts.cleanupAt || "03:00";
        if (app && typeof app.daily === "function") {
            app.daily(at, () => cleanup());
            _state.cleanupScheduled = true;
        }
    }
    _state.initialized = true;
}

function normalizeUa(ua) {
    if (typeof ua !== "string") return "unknown|unknown";
    const s = ua.toLowerCase();
    let os = "other";
    if (s.indexOf("android") >= 0) os = "android";
    else if (s.indexOf("iphone") >= 0 || s.indexOf("ipad") >= 0) os = "ios";
    else if (s.indexOf("mac os") >= 0) os = "macos";
    else if (s.indexOf("windows") >= 0) os = "windows";
    else if (s.indexOf("linux") >= 0)   os = "linux";
    // Order matters: Edge contains "Chrome", Chrome contains "Safari"
    // in their UA strings. Check most-specific first.
    let family = "other";
    if (s.indexOf("firefox") >= 0) family = "firefox";
    else if (s.indexOf("edg/") >= 0)  family = "edge";
    else if (s.indexOf("chrome") >= 0) family = "chrome";
    else if (s.indexOf("safari") >= 0) family = "safari";
    else if (s.indexOf("curl") >= 0)   family = "curl";
    return os + "|" + family;
}

function ipPrefix(ip) {
    if (typeof ip !== "string" || ip === "") return "0.0.0.0/24";
    const first = (ip.split(",")[0] || ip).trim();
    const m = first.match(/^(\d+)\.(\d+)\.(\d+)\./);
    if (m) return m[1] + "." + m[2] + "." + m[3] + ".0/24";
    if (first.indexOf(":") >= 0) {
        const parts = first.split(":").filter(p => p.length > 0).slice(0, 4);
        return parts.join(":") + "::/64";
    }
    return first;
}

function extractIp(req) {
    if (!req || !req.headers) return null;
    const xff = req.headers["x-forwarded-for"];
    if (xff) return (xff.split(",")[0] || xff).trim();
    return req.remote_addr || null;
}

function extractUa(req) {
    return req && req.headers && req.headers["user-agent"] || null;
}

// Binary-safe byte→hex. Local so audit-log doesn't depend on
// auth-flows. Mirrors the same workaround used in auth-flows and
// oauth: crypto.hexEncode for a string input goes through
// JS_ToCStringLen which UTF-8-inflates any byte >= 0x80, so a raw
// SHA-256 digest cannot be hexed via the cap helper without first
// running it through this mask. Without this, fingerprints are
// stable per-runtime but NOT byte-identical to Lua's, which means
// a Lua→JS migration of the same DB would mark every existing user
// as a new device on first request.
function bytesToHex(s) {
    let h = "";
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i) & 0xff;
        h += (c < 16 ? "0" : "") + c.toString(16);
    }
    return h;
}

function fingerprint(req) {
    const ua  = extractUa(req);
    const ip  = extractIp(req);
    const key = normalizeUa(ua) + "|" + ipPrefix(ip);
    return bytesToHex(crypto.sha256(key)).substring(0, 16);
}

function record(userId, kind, req, opts) {
    if (typeof userId !== "string" || userId === ""
        || typeof kind !== "string" || kind === "") return;
    opts = opts || {};
    const ip = opts.ip !== undefined ? opts.ip : extractIp(req);
    const ua = opts.user_agent !== undefined ? opts.user_agent : extractUa(req);
    const fp = opts.fingerprint || fingerprint(req);
    const meta = opts.metadata !== undefined ? json.encode(opts.metadata) : null;
    db.exec(
        "INSERT INTO _hull_audit_log "
        + "(user_id, event_at, kind, ip, user_agent, fingerprint, "
        + " session_id, metadata) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        [userId, time.now(), kind, ip, ua, fp,
         opts.session_id || null, meta]);
}

function list(userId, opts) {
    if (typeof userId !== "string" || userId === "") return [];
    opts = opts || {};
    const limit = opts.limit || 50;
    let rows;
    if (opts.kinds && opts.kinds.length > 0) {
        const placeholders = opts.kinds.map(() => "?").join(",");
        const params = [userId].concat(opts.kinds);
        params.push(limit);
        rows = db.query(
            "SELECT id, event_at, kind, ip, user_agent, fingerprint, "
            + " session_id, metadata FROM _hull_audit_log "
            + "WHERE user_id = ? AND kind IN (" + placeholders + ") "
            + "ORDER BY event_at DESC LIMIT ?", params);
    } else {
        rows = db.query(
            "SELECT id, event_at, kind, ip, user_agent, fingerprint, "
            + " session_id, metadata FROM _hull_audit_log "
            + "WHERE user_id = ? ORDER BY event_at DESC LIMIT ?",
            [userId, limit]);
    }
    rows = rows || [];
    for (let i = 0; i < rows.length; i++) {
        if (rows[i].metadata) {
            try { rows[i].metadata = json.decode(rows[i].metadata); }
            catch (_e) { rows[i].metadata = null; }
        }
    }
    return rows;
}

function listDevices(userId, opts) {
    if (typeof userId !== "string" || userId === "") return [];
    opts = opts || {};
    const cutoff = time.now() - ((opts.window_days || 90) * 86400);
    return db.query(
        "SELECT fingerprint, "
        + "  MIN(event_at) AS first_seen, "
        + "  MAX(event_at) AS last_seen, "
        + "  COUNT(*)      AS count, "
        + "  MAX(ip)         AS ip, "
        + "  MAX(user_agent) AS user_agent "
        + "FROM _hull_audit_log "
        + "WHERE user_id = ? AND event_at >= ? "
        + "  AND fingerprint IS NOT NULL "
        + "GROUP BY fingerprint ORDER BY last_seen DESC",
        [userId, cutoff]) || [];
}

function isNewDevice(userId, req, opts) {
    if (typeof userId !== "string" || userId === "") return false;
    opts = opts || {};
    const cutoff = time.now() - ((opts.window_days || 30) * 86400);
    const fp = fingerprint(req);
    const rows = db.query(
        "SELECT 1 FROM _hull_audit_log "
        + "WHERE user_id = ? AND fingerprint = ? AND event_at >= ? LIMIT 1",
        [userId, fp, cutoff]);
    return rows === null || rows === undefined || rows.length === 0;
}

function cleanup() {
    const cutoff = time.now() - (_state.retainDays * 86400);
    return db.exec("DELETE FROM _hull_audit_log WHERE event_at < ?",
                    [cutoff]) || 0;
}

const _test = {
    normalizeUa,
    ipPrefix,
    reset: () => { _state.retainDays = 365; _state.initialized = false; },
};

const auditLog = { init, record, list, listDevices, isNewDevice,
                    fingerprint, cleanup, _test };
export { auditLog };
