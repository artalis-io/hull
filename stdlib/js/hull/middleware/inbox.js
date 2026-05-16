/**
 * @file hull:middleware:inbox
 * @module hull:middleware:inbox
 * @description Inbox deduplication for incoming events. Lua parity:
 *   `hull.middleware.inbox`.
 *
 * Prevents duplicate processing of incoming events (webhook receipts,
 * queue messages, retries) by tracking processed `(source, message_id)`
 * pairs in SQLite.
 *
 * Counterpart to `hull:middleware:outbox`: outbox provides at-least-once
 * delivery, inbox lets receivers deduplicate.
 *
 * @license AGPL-3.0-or-later
 */

import { db } from "hull:db";
import { time } from "hull:time";

let inboxTtl = 604800; // default 7 days

/**
 * Initialize the `_hull_inbox_processed` SQLite table. Idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.ttl=604800]  Record lifetime in seconds (default = 7 days).
 */
function init(opts) {
    const o = opts || {};
    if (o.ttl !== undefined) inboxTtl = o.ttl;

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_inbox_processed (" +
        "  message_id   TEXT NOT NULL," +
        "  source       TEXT NOT NULL," +
        "  processed_at INTEGER NOT NULL," +
        "  expires_at   INTEGER NOT NULL," +
        "  PRIMARY KEY (source, message_id)" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_inbox_expires " +
        "ON _hull_inbox_processed(expires_at)"
    );
}

/**
 * Has `(source, messageId)` been processed within its TTL?
 *
 * Expired rows are deleted lazily and treated as not-yet-processed.
 *
 * @param {string} messageId
 * @param {string} [source="default"]  Source namespace.
 * @returns {boolean}
 */
function isDuplicate(messageId, source) {
    if (!messageId || messageId === "")
        return false;
    if (!source) source = "default";

    const now = time.now();
    const rows = db.query(
        "SELECT expires_at FROM _hull_inbox_processed WHERE source = ? AND message_id = ?",
        [source, messageId]
    );

    if (!rows || rows.length === 0)
        return false;

    if (rows[0].expires_at <= now) {
        db.exec(
            "DELETE FROM _hull_inbox_processed WHERE source = ? AND message_id = ?",
            [source, messageId]
        );
        return false;
    }

    return true;
}

/**
 * Record `(source, messageId)` as processed.
 *
 * Uses `INSERT OR REPLACE` so repeated calls slide the expiry forward.
 *
 * @param {string} messageId
 * @param {string} [source="default"]
 * @param {Object} [opts]
 * @param {number} [opts.ttl]  Override module-level TTL for this message.
 */
function markProcessed(messageId, source, opts) {
    if (!messageId || messageId === "")
        return;
    if (!source) source = "default";
    const o = opts || {};

    const now = time.now();
    const ttl = o.ttl !== undefined ? o.ttl : inboxTtl;

    db.exec(
        "INSERT OR REPLACE INTO _hull_inbox_processed " +
        "(message_id, source, processed_at, expires_at) VALUES (?, ?, ?, ?)",
        [messageId, source, now, now + ttl]
    );
}

/**
 * Atomic check-and-mark.
 *
 * Returns `true` if `(source, messageId)` was already processed.
 * Otherwise marks it as processed and returns `false`.
 *
 * @param {string} messageId
 * @param {string} [source="default"]
 * @param {Object} [opts]  Same as `markProcessed`.
 * @returns {boolean}  `true` if duplicate, `false` if new (and marked).
 *
 * @example
 * if (inbox.checkAndMark(eventId, "stripe")) {
 *     return res.json({ received: true, duplicate: true });
 * }
 */
function checkAndMark(messageId, source, opts) {
    let isDup = false;
    db.batch(function() {
        if (isDuplicate(messageId, source)) {
            isDup = true;
            return;
        }
        markProcessed(messageId, source, opts);
    });
    return isDup;
}

/**
 * Delete expired inbox records.
 *
 * @returns {number}  Count of deleted rows.
 */
function cleanup() {
    const now = time.now();
    return db.exec(
        "DELETE FROM _hull_inbox_processed WHERE expires_at <= ?",
        [now]
    );
}

const inbox = { init, isDuplicate, markProcessed, checkAndMark, cleanup };
export { inbox };
