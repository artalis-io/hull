/*
 * hull:session -- Server-side sessions backed by SQLite
 *
 * session.init(opts)          - creates hull_sessions table, opts.ttl default 86400
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
        "CREATE TABLE IF NOT EXISTS hull_sessions (" +
        "  id TEXT PRIMARY KEY," +
        "  data TEXT NOT NULL," +
        "  created_at INTEGER NOT NULL," +
        "  expires_at INTEGER NOT NULL" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_sessions_expires " +
        "ON hull_sessions(expires_at)"
    );
}

function generateId() {
    const bytes = new Uint8Array(crypto.random(32));
    let id = "";
    for (let i = 0; i < bytes.length; i++)
        id += bytes[i].toString(16).padStart(2, "0");
    return id;
}

function create(data) {
    const id = generateId();
    const now = time.now();
    const expiresAt = now + sessionTtl;
    const encoded = json.encode(data || {});

    db.exec(
        "INSERT INTO hull_sessions (id, data, created_at, expires_at) VALUES (?, ?, ?, ?)",
        [id, encoded, now, expiresAt]
    );

    return id;
}

function load(sessionId) {
    if (!sessionId || typeof sessionId !== "string")
        return null;

    const now = time.now();
    const rows = db.query(
        "SELECT data FROM hull_sessions WHERE id = ? AND expires_at > ?",
        [sessionId, now]
    );

    if (!rows || rows.length === 0)
        return null;

    // Touch: extend expiration on access
    db.exec(
        "UPDATE hull_sessions SET expires_at = ? WHERE id = ?",
        [now + sessionTtl, sessionId]
    );

    return json.decode(rows[0].data);
}

function update(sessionId, data) {
    if (!sessionId || typeof sessionId !== "string")
        return false;

    const now = time.now();
    const encoded = json.encode(data || {});

    const affected = db.exec(
        "UPDATE hull_sessions SET data = ?, expires_at = ? WHERE id = ? AND expires_at > ?",
        [encoded, now + sessionTtl, sessionId, now]
    );

    return affected > 0;
}

function destroy(sessionId) {
    if (!sessionId || typeof sessionId !== "string")
        return false;

    const affected = db.exec(
        "DELETE FROM hull_sessions WHERE id = ?",
        [sessionId]
    );

    return affected > 0;
}

function cleanup() {
    const now = time.now();
    return db.exec(
        "DELETE FROM hull_sessions WHERE expires_at <= ?",
        [now]
    );
}

const session = { init, create, load, update, destroy, cleanup };
export { session };
