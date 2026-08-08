/**
 * @file hull:web:middleware:session
 * @module hull:web:middleware:session
 * @description Server-side sessions backed by SQLite. Lua parity:
 *   `hull.web.middleware.session`.
 *
 * Storage: `_hull_sessions` table with sliding TTL (refreshed on every
 * {@link load} hit). Session ids are 64-char hex from `crypto.random`.
 *
 * @license AGPL-3.0-or-later
 */

import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";
import { app } from "hull:app";
import { log } from "hull:log";
import { _request } from "hull:web:_request";

let sessionTtl = 86400;
// Round-8 MEDIUM-8: absolute (hard) TTL cap from created_at.
// Sliding sessionTtl extends expires_at on every hit; absolute caps
// the session at created_at + absoluteTtl regardless of activity.
// Default 86400 = 24h (OWASP-stricter); opt-out via
// absoluteTtl = false. Reads created_at, which exists on every row.
let absoluteTtl = 86400;
let cleanupCatchupDone = false;
let cleanupScheduled = false;
// Set by init(). loginHandler / logoutHandler check this so a
// missing session.init() before authflows.init() throws at wire
// time, not at first request.
let initialized = false;
// When false (default), the recorded client IP is req.remote_addr (the
// un-spoofable socket peer). Set trustProxy: true only behind a proxy that
// sets X-Forwarded-For, so a directly-exposed client can't forge its IP to
// evade the device-list / new-device audit trail.
let trustProxy = false;

/**
 * Initialize the sessions table. Call once at startup.
 *
 * @param {Object} [opts]
 * @param {number}  [opts.ttl=86400]   SLIDING lifetime in seconds.
 * @param {number|false} [opts.absoluteTtl=86400]  HARD upper bound
 *   from created_at. Sessions older than absoluteTtl fail to load
 *   even if sliding TTL would have kept them alive. Pass false to
 *   disable (apps that intentionally want forever-sessions stay
 *   loud about it). Round-8 MEDIUM-8.
 * @param {boolean} [opts.cleanup=true]  When true, schedules a daily
 *   timer via app.daily that calls session.cleanup. Set false to
 *   drive cleanup from cron or your own wiring. Mirrors the auto-
 *   schedule pattern in auditLog.init.
 * @param {string}  [opts.cleanupAt="03:00"]  UTC wall-clock time
 *   for the daily cleanup.
 */
function init(opts) {
    const o = opts || {};
    // Round-10 MEDIUM-7: only update when explicitly given. Pre-fix
    // every init({}) clobbered prior sessionTtl back to 86400, while
    // Lua kept the prior value on re-init. Apps that call init twice
    // (first with custom ttl, second with just absoluteTtl) lost
    // their ttl on JS. Now matches Lua semantics.
    if (o.ttl !== undefined) sessionTtl = o.ttl;
    if (o.trustProxy !== undefined) trustProxy = o.trustProxy === true;
    if (o.absoluteTtl !== undefined) {
        if (o.absoluteTtl === false) {
            absoluteTtl = null;
        } else if (typeof o.absoluteTtl === "number"
                   && (!Number.isFinite(o.absoluteTtl)
                       || o.absoluteTtl <= 0)) {
            // Round-10 MEDIUM-8: NaN and Infinity slipped through.
            // NaN <= 0 is false → stored as NaN → cap silently
            // disabled. Infinity makes the comparison always false
            // too. Reject non-finite explicitly. Same shape on Lua.
            log.warn("session.init: absoluteTtl must be finite > 0; "
                + "use `false` for the explicit opt-out so the intent "
                + "is loud.");
            absoluteTtl = null;
        } else {
            absoluteTtl = o.absoluteTtl;
        }
    }

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_sessions (" +
        "  id VARCHAR(255) PRIMARY KEY," +
        "  data TEXT NOT NULL," +
        "  created_at INTEGER NOT NULL," +
        "  last_accessed INTEGER NOT NULL," +
        "  expires_at INTEGER NOT NULL" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx__hull_sessions_expires " +
        "ON _hull_sessions(expires_at)"
    );
    // Additive migration for the device-management helpers
    // (listForUser, destroyOthers, destroyAll). Both SQLite and
    // Postgres expose the column set via the backend's vtable
    // so we don't bake a dialect-specific PRAGMA query in here.
    // Old rows keep working with NULL in new columns.
    const existing = {};
    const cols = db.tableColumns("_hull_sessions") || [];
    for (let i = 0; i < cols.length; i++) existing[cols[i]] = true;
    if (!existing.user_id)
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN user_id VARCHAR(255)");
    if (!existing.ip)
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN ip TEXT");
    if (!existing.user_agent)
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN user_agent TEXT");
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx__hull_sessions_user_id " +
        "ON _hull_sessions(user_id)");

    // Lazy catchup + auto-schedule daily cleanup. Mirrors the
    // auditLog pattern: bound _hull_sessions growth even when the
    // app forgets to wire a cleanup timer.
    if (o.cleanup !== false && !cleanupCatchupDone) {
        try { cleanup(); }
        catch (e) {
            log.warn("session: init-time cleanup failed: " + (e && e.message || e));
        }
        cleanupCatchupDone = true;
    }
    if (o.cleanup !== false && !cleanupScheduled) {
        const at = o.cleanupAt || "03:00";
        if (app && typeof app.daily === "function") {
            app.daily(at, () => cleanup());
            cleanupScheduled = true;
        } else {
            log.warn("session: app.daily not available "
                + "(CLI flavor or hull/timers not admitted) "
                + "— auto-cleanup runs only at init(). "
                + "Wire your own cron/worker for steady-state.");
        }
    }

    initialized = true;
}

function generateId() {
    const bytes = new Uint8Array(crypto.random(32));
    let id = "";
    for (let i = 0; i < bytes.length; i++)
        id += bytes[i].toString(16).padStart(2, "0");
    return id;
}

/**
 * Create a new session with the given data.
 * opts.ttl: override module-level TTL for this session (optional)
 */
/**
 * Create a new session.
 *
 * @param {Object} [data={}]  Initial session payload (JSON-encoded).
 * @param {Object} [opts]
 * @param {number} [opts.ttl]  Override module-level TTL for this session.
 * @returns {string}  Session id (64-char hex).
 */
function create(data, opts) {
    const id = generateId();
    const now = time.now();
    const ttl = (opts && opts.ttl !== undefined) ? opts.ttl : sessionTtl;
    const encoded = json.encode(data || {});

    // Capture device columns for audit-log + listForUser. user_id
    // comes from the data blob (standard auth-flows pattern); ip
    // + ua come from opts.req if supplied.
    const userId = (data && typeof data === "object" && data.user_id) || null;
    let ip = null, ua = null;
    if (opts && opts.req) {
        const h = opts.req.headers || {};
        ip = _request.clientIp(opts.req, trustProxy);
        ua = h["user-agent"] || null;
        // Real UAs top out around 500 chars; bots and scanners can
        // send 100KB UAs. Cap to bound the row size — the value is
        // only used for the /devices listing display.
        if (typeof ua === "string" && ua.length > 512) {
            ua = ua.substring(0, 512);
        }
        // Round-9 MEDIUM-7: cap IP. See Lua sibling for rationale.
        if (typeof ip === "string" && ip.length > 64) {
            ip = ip.substring(0, 64);
        }
    }

    db.exec(
        "INSERT INTO _hull_sessions "
        + "(id, data, created_at, last_accessed, expires_at, "
        + " user_id, ip, user_agent) "
        + "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        [id, encoded, now, now, now + ttl, userId, ip, ua]
    );

    return id;
}

/**
 * Load a session by ID.
 * opts.ttl: override module-level TTL for expiry extension (optional)
 */
/**
 * Load a session by id; refresh expiry on hit (sliding TTL).
 *
 * Validates the 64-char hex shape before hitting the DB. Expired
 * sessions return `null` (and are NOT deleted — see {@link cleanup}).
 *
 * @param {string} sessionId
 * @param {Object} [opts]  `{ ttl }` for the extended expiry.
 * @returns {Object|null}  Decoded session data, or `null`.
 */
function load(sessionId, opts) {
    if (!sessionId || typeof sessionId !== "string")
        return null;
    // Validate format: must be 64-char hex (from create)
    if (sessionId.length !== 64 || !/^[0-9a-f]+$/.test(sessionId))
        return null;

    const now = time.now();
    const rows = db.query(
        "SELECT data, expires_at, created_at FROM _hull_sessions "
        + "WHERE id = ?",
        [sessionId]
    );

    if (!rows || rows.length === 0)
        return null;

    // Check expiry (sliding)
    if (rows[0].expires_at <= now) {
        db.exec("DELETE FROM _hull_sessions WHERE id = ?", [sessionId]);
        return null;
    }
    // Round-8 MEDIUM-8: hard absolute-TTL cap. Refuse to extend past
    // created_at + absoluteTtl regardless of sliding state. created_at
    // may be null on legacy migrated rows; skip the cap in that case.
    if (absoluteTtl && rows[0].created_at != null
        && (rows[0].created_at + absoluteTtl) <= now) {
        db.exec("DELETE FROM _hull_sessions WHERE id = ?", [sessionId]);
        return null;
    }

    // Touch: update last_accessed and extend expiration
    const ttl = (opts && opts.ttl !== undefined) ? opts.ttl : sessionTtl;
    db.exec(
        "UPDATE _hull_sessions SET last_accessed = ?, expires_at = ? WHERE id = ?",
        [now, now + ttl, sessionId]
    );

    let decoded;
    try { decoded = json.decode(rows[0].data); }
    catch (_e) { decoded = null; }
    if (decoded == null || typeof decoded !== "object") {
        // Corrupted session data — destroy and return null
        db.exec("DELETE FROM _hull_sessions WHERE id = ?", [sessionId]);
        return null;
    }
    return decoded;
}

/**
 * Replace session data for an existing session.
 * opts.ttl: override module-level TTL for expiry extension (optional)
 */
/**
 * Replace session data and refresh expiry.
 *
 * @param {string} sessionId
 * @param {Object} data  Replacement payload.
 * @param {Object} [opts]  `{ ttl }`.
 */
function update(sessionId, data, opts) {
    if (!sessionId || typeof sessionId !== "string")
        return false;

    const now = time.now();
    const ttl = (opts && opts.ttl !== undefined) ? opts.ttl : sessionTtl;
    const encoded = json.encode(data || {});

    const affected = db.exec(
        "UPDATE _hull_sessions SET data = ?, last_accessed = ?, expires_at = ? WHERE id = ? AND expires_at > ?",
        [encoded, now, now + ttl, sessionId, now]
    );

    return affected > 0;
}

/**
 * Delete a session by id. Empty / null is a no-op.
 *
 * @param {string} sessionId
 */
function destroy(sessionId) {
    if (!sessionId || typeof sessionId !== "string")
        return false;

    const affected = db.exec(
        "DELETE FROM _hull_sessions WHERE id = ?",
        [sessionId]
    );

    return affected > 0;
}

/**
 * Delete all expired sessions. Run periodically.
 *
 * @returns {number}  Rows deleted.
 *
 * @example
 * app.every(3600 * 1000, session.cleanup);
 */
function cleanup() {
    const now = time.now();
    return db.exec(
        "DELETE FROM _hull_sessions WHERE expires_at <= ?",
        [now]
    );
}

/**
 * List currently active sessions for a user.
 * Returns rows {id, created_at, last_accessed, ip, user_agent}, newest
 * first. Excludes expired rows.
 */
function listForUser(userId) {
    if (typeof userId !== "string" || userId === "") return [];
    const now = time.now();
    // Round-10 MEDIUM-9: filter past-absolute-ttl rows so list+load
    // stay in sync. See Lua sibling.
    if (absoluteTtl) {
        // Round-11 HIGH-2: tolerate NULL created_at to match load().
        // See Lua sibling.
        return db.query(
            "SELECT id, created_at, last_accessed, ip, user_agent "
            + "FROM _hull_sessions "
            + "WHERE user_id = ? AND expires_at > ? "
            + "  AND (created_at IS NULL OR created_at + ? > ?) "
            + "ORDER BY last_accessed DESC",
            [userId, now, absoluteTtl, now]) || [];
    }
    return db.query(
        "SELECT id, created_at, last_accessed, ip, user_agent "
        + "FROM _hull_sessions "
        + "WHERE user_id = ? AND expires_at > ? "
        + "ORDER BY last_accessed DESC",
        [userId, now]) || [];
}

/**
 * Destroy every session for `userId` EXCEPT `currentSid`. Returns
 * the number of rows removed.
 */
function destroyOthers(currentSid, userId) {
    if (typeof userId !== "string" || userId === "") return 0;
    return db.exec(
        "DELETE FROM _hull_sessions WHERE user_id = ? AND id != ?",
        [userId, currentSid || ""]) || 0;
}

/**
 * Destroy every session for `userId`. Used by auth-flows on
 * successful password reset when revokeSessionsOnPasswordReset is
 * true (default).
 */
function destroyAll(userId) {
    if (typeof userId !== "string" || userId === "") return 0;
    return db.exec("DELETE FROM _hull_sessions WHERE user_id = ?",
                   [userId]) || 0;
}

// ── Session-fixation defense + on_login factories ────────────────

/**
 * Rotate the session id. Destroys oldSid (if non-empty) and
 * creates a new session with the supplied data. Use during login
 * to defend against session-fixation attacks.
 */
function rotate(oldSid, data, opts) {
    if (oldSid) destroy(oldSid);
    return create(data, opts);
}

const DEFAULT_LOGIN_HANDLER_OPTS = {
    name:       "hull_session",
    cookieOpts: { path: "/", httpOnly: true, sameSite: "Lax" },
};

/**
 * Build a standard onLogin(req, res, user) callback for
 * hull/web/auth-flows or hull/web/middleware/oauth. Eliminates
 * the per-app boilerplate of "create session -> serialize cookie
 * -> set Set-Cookie -> respond". Session-fixation defense
 * (session.rotate) is on by default.
 *
 * @param {Object} cookieMod  hull:web:cookie module reference.
 * @param {Object} [opts]
 *   - name        cookie name (default "hull_session", matching
 *                 hull:web:middleware:auth's sessionMiddleware)
 *   - cookieOpts  forwarded to cookie.serialize
 *   - extractData(user) -> obj for session.create
 *   - respond(res, user, sid) -> write response body
 *   - rotate      bool, rotate any prior session (default true)
 *
 * Audit + new-device opts (shared across all login sources):
 *   - auditLog          module ref (e.g. `import * as auditLog from
 *                       "hull:web:middleware:audit-log"`). When set,
 *                       records a login event after the session is set.
 *   - auditKind         event kind string (default "login").
 *   - auditMetadata(user, ctx) -> object emitted as event metadata.
 *                       Default derives `{ factors: ctx.factors }` (auth-flows)
 *                       or `{ factors: "oauth:" + ctx.provider }` (oauth)
 *                       or `{ factors: "unknown" }` (no ctx).
 *   - onNewDevice(req, res, user) — called before auditLog.record when
 *                       auditLog.isNewDevice(userId, req) returns true.
 *                       Requires auditLog. Wrapped in try/catch so callback
 *                       bugs cannot fail the login.
 */
function loginHandler(cookieMod, opts) {
    if (!initialized) {
        throw new Error("session.loginHandler: call session.init() first");
    }
    if (!cookieMod || typeof cookieMod.serialize !== "function") {
        throw new Error("session.loginHandler: cookie module required");
    }
    opts = opts || {};
    const name       = opts.name       || DEFAULT_LOGIN_HANDLER_OPTS.name;
    const cookieOpts = opts.cookieOpts || DEFAULT_LOGIN_HANDLER_OPTS.cookieOpts;
    const rotateOn   = opts.rotate !== false;
    const extractData = opts.extractData || (user =>
        ({ user_id: user.id, email: user.email }));
    const respond = opts.respond || ((res, user) =>
        res.json({ ok: true, user_id: user.id, email: user.email }));
    const auditLog    = opts.auditLog || null;
    const auditKind   = opts.auditKind || "login";
    const onNewDev    = opts.onNewDevice || null;
    const auditMeta   = opts.auditMetadata || ((_user, ctx) => {
        if (ctx && typeof ctx === "object") {
            if (typeof ctx.factors === "string") return { factors: ctx.factors };
            if (typeof ctx.provider === "string")
                return { factors: "oauth:" + ctx.provider };
        }
        return { factors: "unknown" };
    });
    // Defense in depth: scrub keys that must never reach the audit
    // store, no matter what the app's auditMetadata returned. OAuth
    // ctx (claims, tokens) is passed straight through to apps via
    // onLogin's 4th arg, which makes `auditMetadata: (_,c) => c` a
    // one-keystroke leak of access_token / refresh_token / raw
    // ID-token into _hull_audit_log.metadata (where it persists for
    // opts.retainDays — default 365). The list mirrors the Lua half
    // and covers OAuth, password, and TOTP secret surfaces. If you
    // need to log claim details, pull them out by name in a custom
    // auditMetadata — never pass the raw ctx through.
    const SCRUB_KEYS = new Set([
        "tokens", "token", "access_token", "refresh_token",
        "id_token", "claims",
        "password", "password_hash", "pwhash", "secret",
    ]);
    const scrub = (meta) => {
        if (!meta || typeof meta !== "object") return meta;
        const out = {};
        for (const k in meta) {
            if (!SCRUB_KEYS.has(k)) out[k] = meta[k];
        }
        return out;
    };

    return function (req, res, user, ctx) {
        if (!user || typeof user !== "object")
            throw new Error("session.loginHandler: user.id is required");
        // Accept integer 0 as a valid id (mirrors Lua's nil/"" check);
        // `!user.id` would reject 0 and break apps with INTEGER PRIMARY
        // KEY user_ids seeded at 0.
        const id = user.id;
        if (id === undefined || id === null || id === "")
            throw new Error("session.loginHandler: user.id is required");
        const data = extractData(user) || {};
        let sid;
        if (rotateOn) {
            let existing = null;
            if (req.ctx && req.ctx.session_id) {
                existing = req.ctx.session_id;
            } else {
                const cookies = cookieMod.parse(req.headers.cookie || "");
                existing = cookies[name] || null;
            }
            sid = rotate(existing, data, { req });
        } else {
            sid = create(data, { req });
        }
        res.header("Set-Cookie", cookieMod.serialize(name, sid, cookieOpts));

        // New-device + audit emission happen AFTER the session row exists
        // so the new-device check sees prior history (not the row we just
        // created), and the audit row carries the canonical session_id.
        if (auditLog) {
            if (onNewDev) {
                try {
                    if (auditLog.isNewDevice(user.id, req)) {
                        try { onNewDev(req, res, user); }
                        catch (e) {
                            // Round-11 MEDIUM-5: surface the swallowed
                            // error. The request still succeeds (the
                            // notification is best-effort), but a
                            // silent catch left new-device alerts
                            // broken for weeks in some deploys.
                            log.warn("session.loginHandler: onNewDevice "
                                + "callback threw: " + (e && e.message || e));
                        }
                    }
                } catch (e) {
                    log.warn("session.loginHandler: auditLog.isNewDevice "
                        + "threw: " + (e && e.message || e));
                }
            }
            try {
                auditLog.record(user.id, auditKind, req,
                    { session_id: sid, metadata: scrub(auditMeta(user, ctx)) });
            } catch (e) {
                log.warn("session.loginHandler: auditLog.record threw: "
                    + (e && e.message || e));
            }
        }

        respond(res, user, sid);
    };
}

/**
 * Build the matching onLogout(req, res) callback. Destroys the
 * current session (from cookie) and clears it client-side.
 */
function logoutHandler(cookieMod, opts) {
    if (!cookieMod || typeof cookieMod.parse !== "function") {
        throw new Error("session.logoutHandler: cookie module required");
    }
    opts = opts || {};
    const name       = opts.name       || DEFAULT_LOGIN_HANDLER_OPTS.name;
    const cookieOpts = opts.cookieOpts || DEFAULT_LOGIN_HANDLER_OPTS.cookieOpts;
    const respond = opts.respond || (res => res.json({ ok: true }));

    return function (req, res) {
        let sid = null;
        if (req.ctx && req.ctx.session_id) {
            sid = req.ctx.session_id;
        } else {
            const cookies = cookieMod.parse(req.headers.cookie || "");
            sid = cookies[name];
        }
        if (sid) destroy(sid);
        res.header("Set-Cookie", cookieMod.clear(name, cookieOpts));
        respond(res);
    };
}

const session = { init, create, load, update, destroy, cleanup,
                  listForUser, destroyOthers, destroyAll,
                  rotate, loginHandler, logoutHandler };
export { session };
