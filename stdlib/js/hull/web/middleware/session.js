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

import { db } from "hull:db";
import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";

let sessionTtl = 86400;
// Set by init(). loginHandler / logoutHandler check this so a
// missing session.init() before authflows.init() throws at wire
// time, not at first request.
let initialized = false;

/**
 * Initialize the sessions table. Call once at startup.
 *
 * @param {Object} [opts]
 * @param {number} [opts.ttl=86400]  Session lifetime in seconds.
 */
function init(opts) {
    const o = opts || {};
    sessionTtl = o.ttl !== undefined ? o.ttl : 86400;

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_sessions (" +
        "  id TEXT PRIMARY KEY," +
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
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN user_id TEXT");
    if (!existing.ip)
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN ip TEXT");
    if (!existing.user_agent)
        db.exec("ALTER TABLE _hull_sessions ADD COLUMN user_agent TEXT");
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx__hull_sessions_user_id " +
        "ON _hull_sessions(user_id)");
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
        const xff = h["x-forwarded-for"];
        if (xff) ip = (xff.split(",")[0] || xff).trim();
        else ip = opts.req.remote_addr || null;
        ua = h["user-agent"] || null;
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
        "SELECT data, expires_at FROM _hull_sessions WHERE id = ?",
        [sessionId]
    );

    if (!rows || rows.length === 0)
        return null;

    // Check expiry
    if (rows[0].expires_at <= now) {
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
    name:       "session",
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
 *   - name        cookie name (default "session")
 *   - cookieOpts  forwarded to cookie.serialize
 *   - extractData(user) -> obj for session.create
 *   - respond(res, user, sid) -> write response body
 *   - rotate      bool, rotate any prior session (default true)
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

    return function (req, res, user) {
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
