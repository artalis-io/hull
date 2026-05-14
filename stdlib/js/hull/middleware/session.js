/*
 * hull:session -- Server-side sessions backed by SQLite
 *
 * session.init(opts)          - creates _hull_sessions table, opts.ttl default 86400
 * session.create(data)        - returns session_id (64-char hex)
 * session.load(sessionId)     - returns data object or null
 * session.update(sessionId, data) - boolean
 * session.destroy(sessionId)  - boolean
 * session.cleanup()           - count of expired sessions deleted
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";
import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";

let sessionTtl = 86400;

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
function create(data, opts) {
    const id = generateId();
    const now = time.now();
    const ttl = (opts && opts.ttl !== undefined) ? opts.ttl : sessionTtl;
    const encoded = json.encode(data || {});

    db.exec(
        "INSERT INTO _hull_sessions (id, data, created_at, last_accessed, expires_at) VALUES (?, ?, ?, ?, ?)",
        [id, encoded, now, now, now + ttl]
    );

    return id;
}

/**
 * Load a session by ID.
 * opts.ttl: override module-level TTL for expiry extension (optional)
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

function destroy(sessionId) {
    if (!sessionId || typeof sessionId !== "string")
        return false;

    const affected = db.exec(
        "DELETE FROM _hull_sessions WHERE id = ?",
        [sessionId]
    );

    return affected > 0;
}

function cleanup() {
    const now = time.now();
    return db.exec(
        "DELETE FROM _hull_sessions WHERE expires_at <= ?",
        [now]
    );
}

const session = { init, create, load, update, destroy, cleanup };
export { session };
