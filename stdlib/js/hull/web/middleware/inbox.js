/**
 * @file hull:web:middleware:inbox
 * @module hull:web:middleware:inbox
 * @description Inbox deduplication for incoming events. Lua parity:
 *   `hull.web.middleware.inbox`.
 *
 * Prevents duplicate processing of incoming events (webhook receipts,
 * queue messages, retries) by tracking processed `(source, message_id)`
 * pairs in SQLite.
 *
 * Counterpart to `hull:web:middleware:outbox`: outbox provides at-least-once
 * delivery, inbox lets receivers deduplicate.
 *
 * @license AGPL-3.0-or-later
 */

import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { time } from "hull:time";
import { _test_hooks } from "hull:_test_hooks";

let inboxTtl = 604800; // default 7 days

// `messageId` / `source` form the PK, both VARCHAR(255). Reject an over-length
// id rather than let MySQL silently truncate it, which would collapse two
// distinct messages sharing a 255-char prefix into one (dropping the second as
// a false duplicate). Uniform across every backend.
const MAX_ID_LEN = 255;
// A message id as stored: webhook ids are often numeric. An integer is its
// decimal string (a number has no .length, so the length cap never applied);
// any other non-string is a caller error, said plainly.
function normId(messageId) {
    if (typeof messageId === "number" && Number.isSafeInteger(messageId))
        return String(messageId);
    if (messageId !== null && messageId !== undefined && typeof messageId !== "string")
        throw new Error("inbox: messageId must be a string or an integer");
    return messageId;
}

// A source as stored (audit 10; see the Lua sibling): unset or "" is
// "default", an integer its decimal string, any other non-string an error. A
// number had no .length (the cap never applied) and reached the VARCHAR
// column as a number, which MySQL compared numerically.
function normSource(source) {
    if (source === undefined || source === null || source === "") return "default";
    if (typeof source === "number" && Number.isSafeInteger(source)) return String(source);
    if (typeof source !== "string")
        throw new Error("inbox: source must be a string or an integer");
    return source;
}

function checkIdLen(messageId, source) {
    if (messageId && messageId.length > MAX_ID_LEN)
        throw new Error("inbox: messageId too long (max " + MAX_ID_LEN + ")");
    if (source && source.length > MAX_ID_LEN)
        throw new Error("inbox: source too long (max " + MAX_ID_LEN + ")");
}

// A ttl is a positive, finite NUMBER of seconds (audit 15): a string one
// ("86400") concatenated onto the expiry (now + "86400"). Same rule as the
// Lua twin and auth.login; anything else throws.
//
// It is also a whole number of seconds no larger than MAX_TTL (366 days;
// audit 26): an unbounded one made `now + ttl` overflow a 32-bit INTEGER
// expiry column on PostgreSQL / MySQL (and run past 2038).
const MAX_TTL = 31622400;
function checkTtl(who, v) {
    if (!(typeof v === "number" && Number.isInteger(v) && v > 0 && v <= MAX_TTL))
        throw new TypeError(who + " must be a whole number of seconds in [1, "
            + MAX_TTL + "]");
    return v;
}

/**
 * Initialize the `_hull_inbox_processed` SQLite table. Idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.ttl=604800]  Record lifetime in seconds (default = 7 days).
 */
function init(opts) {
    const o = opts || {};
    if (o.ttl !== undefined) inboxTtl = checkTtl("inbox.init: ttl", o.ttl);

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_inbox_processed (" +
        "  message_id   VARCHAR(255) NOT NULL," +
        "  source       VARCHAR(255) NOT NULL," +
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
    messageId = normId(messageId);
    if (messageId === null || messageId === undefined || messageId === "")
        return false;
    source = normSource(source);
    checkIdLen(messageId, source);

    const now = time.now();
    const rows = db.query(
        "SELECT expires_at FROM _hull_inbox_processed WHERE source = ? AND message_id = ?",
        [source, messageId]
    );

    if (!rows || rows.length === 0)
        return false;

    if (rows[0].expires_at <= now) {
        // Only while it is still expired: a concurrent mark since the SELECT
        // is a fresh row, and the unguarded DELETE removed it, so the
        // duplicate it recorded was processed again.
        db.exec(
            "DELETE FROM _hull_inbox_processed WHERE source = ? AND message_id = ? " +
            "AND expires_at <= ?",
            [source, messageId, now]
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
    messageId = normId(messageId);
    if (messageId === null || messageId === undefined || messageId === "")
        return;
    source = normSource(source);
    checkIdLen(messageId, source);
    const o = opts || {};

    const now = time.now();
    const ttl = o.ttl !== undefined ? checkTtl("inbox.markProcessed: ttl", o.ttl) : inboxTtl;

    db.upsert(
        "_hull_inbox_processed",
        ["source", "message_id"],
        ["message_id", "source", "processed_at", "expires_at"],
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
    messageId = normId(messageId);
    if (messageId === null || messageId === undefined || messageId === "")
        return false;
    source = normSource(source);
    checkIdLen(messageId, source);
    const o = opts || {};
    const now = time.now();
    const ttl = o.ttl !== undefined ? checkTtl("inbox.checkAndMark: ttl", o.ttl) : inboxTtl;

    // A mark that has expired no longer counts: drop it, so the message is
    // new again.
    db.exec(
        "DELETE FROM _hull_inbox_processed "
        + "WHERE source = ? AND message_id = ? AND expires_at <= ?",
        [source, messageId, now]);
    // The insert IS the check. It used to be a SELECT, then an upsert: two
    // instances handling the same redelivery at once (Postgres at READ
    // COMMITTED, MySQL) both saw no row, both wrote one, and both ran the side
    // effect. The primary key lets exactly one insert in; whoever's did not
    // land has a duplicate.
    const inserted = db.insertIfAbsent(
        "_hull_inbox_processed",
        ["source", "message_id"],
        ["message_id", "source", "processed_at", "expires_at"],
        [messageId, source, now, now + ttl]);
    return inserted === 0;
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

// Test-only: mark a message's record expired. A ttl must be positive (audit
// 15), so a test can no longer plant an already-expired mark with ttl: -1.
// Inside the module so the _hull_* namespace check admits the write.
const _test = {
    expire(messageId, source) {
        db.exec("UPDATE _hull_inbox_processed SET expires_at = 0 "
                + "WHERE source = ? AND message_id = ?",
                [normSource(source), normId(messageId)]);
    },
};
// Registered with the stdlib-internal hull:_test_hooks rather than exported
// (audit 16): app code could call them.
_test_hooks.register("hull:web:middleware:inbox", _test);

const inbox = { init, isDuplicate, markProcessed, checkAndMark, cleanup };
export { inbox };
