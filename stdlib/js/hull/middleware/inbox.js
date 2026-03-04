/*
 * hull:middleware:inbox -- Inbox deduplication for incoming events
 *
 * Prevents duplicate processing of incoming events (webhook receipts,
 * queue messages, etc.) by tracking processed message IDs in SQLite.
 *
 * Usage:
 *   import { inbox } from "hull:middleware:inbox";
 *   inbox.init();
 *
 *   app.post("/webhooks/receive", (req, res) => {
 *       const eventId = req.header("x-webhook-event-id");
 *       if (inbox.isDuplicate(eventId, "upstream")) {
 *           return res.json({ received: true, duplicate: true });
 *       }
 *       // Process event...
 *       inbox.markProcessed(eventId, "upstream");
 *       res.json({ received: true });
 *   });
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";
import { time } from "hull:time";

let inboxTtl = 604800; // default 7 days

/**
 * Initialize the inbox table.
 * @param {Object} opts - Options: ttl (seconds, default 604800 = 7 days)
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
 * Check if a message has already been processed.
 * @param {string} messageId - Unique message identifier
 * @param {string} source - Source identifier (default "default")
 * @returns {boolean} true if already processed and not expired
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
 * Mark a message as processed.
 * @param {string} messageId - Unique message identifier
 * @param {string} source - Source identifier (default "default")
 * @param {Object} opts - Options: ttl (override module TTL)
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
 * Check and mark in a single call. Returns true if duplicate (already
 * processed), false if new (and marks it as processed).
 */
function checkAndMark(messageId, source, opts) {
    if (isDuplicate(messageId, source))
        return true;
    markProcessed(messageId, source, opts);
    return false;
}

/**
 * Delete expired inbox records. Returns number of deleted rows.
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
