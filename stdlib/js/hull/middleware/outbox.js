/*
 * hull:middleware:outbox -- Transactional outbox for reliable side effects
 *
 * Decouples external side effects (HTTP, SMTP, webhook delivery) from the
 * request transaction. Side effects are written as outbox rows inside the
 * same transaction as the state change, then delivered after commit.
 *
 * Usage:
 *   import { outbox } from "hull:middleware:outbox";
 *   outbox.init({ flushAfterRequest: true });
 *
 *   // Inside a handler (within a transaction):
 *   outbox.enqueue({
 *       kind: "webhook",
 *       destination: webhook.url,
 *       payload: JSON.stringify(data),
 *       headers: JSON.stringify({ "Content-Type": "application/json" }),
 *       idempotencyKey: `evt-${eventId}-wh-${wh.id}`,
 *   });
 *
 *   // After handler returns (or explicitly):
 *   outbox.flush();
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";
import { time } from "hull:time";
import { json } from "hull:json";

let maxAttempts = 5;

/**
 * Initialize the outbox table.
 * @param {Object} opts - Options: maxAttempts (default 5)
 */
function init(opts) {
    const o = opts || {};
    if (o.maxAttempts !== undefined) maxAttempts = o.maxAttempts;

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_outbox (" +
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT," +
        "  kind            TEXT NOT NULL," +
        "  destination     TEXT NOT NULL," +
        "  payload         TEXT NOT NULL," +
        "  headers         TEXT," +
        "  idempotency_key TEXT," +
        "  attempts        INTEGER NOT NULL DEFAULT 0," +
        "  max_attempts    INTEGER NOT NULL DEFAULT 5," +
        "  next_attempt_at INTEGER NOT NULL," +
        "  state           TEXT NOT NULL DEFAULT 'pending'," +
        "  created_at      INTEGER NOT NULL," +
        "  delivered_at    INTEGER," +
        "  last_error      TEXT" +
        ")"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_outbox_pending " +
        "ON _hull_outbox(state, next_attempt_at)"
    );
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_outbox_idem " +
        "ON _hull_outbox(idempotency_key)"
    );
}

/**
 * Enqueue a side effect for delivery after commit.
 * Must be called inside a transaction (db.batch) to be atomic with state changes.
 *
 * @param {Object} opts
 * @param {string} opts.kind - "webhook", "http", "smtp"
 * @param {string} opts.destination - URL or address
 * @param {string} opts.payload - string body
 * @param {string} opts.headers - JSON-encoded headers (optional)
 * @param {string} opts.idempotencyKey - unique key for dedup (optional)
 * @param {number} opts.maxAttempts - override default (optional)
 * @param {number} opts.delay - seconds to delay first attempt (optional)
 * @returns {number} outbox item ID
 */
function enqueue(opts) {
    if (!opts || !opts.kind)
        throw new Error("outbox.enqueue requires opts.kind");
    if (!opts.destination)
        throw new Error("outbox.enqueue requires opts.destination");
    if (!opts.payload)
        throw new Error("outbox.enqueue requires opts.payload");

    const now = time.now();
    const max = opts.maxAttempts !== undefined ? opts.maxAttempts : maxAttempts;
    const delay = opts.delay || 0;

    db.exec(
        "INSERT INTO _hull_outbox " +
        "(kind, destination, payload, headers, idempotency_key, max_attempts, next_attempt_at, state, created_at) " +
        "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', ?)",
        [opts.kind, opts.destination, opts.payload, opts.headers || null,
         opts.idempotencyKey || null, max, now + delay, now]
    );

    return db.lastId();
}

/**
 * Attempt to deliver a single outbox item.
 * @returns {[boolean, string|null]} [success, errorMessage]
 */
function deliverItem(item) {
    if (item.kind === "webhook" || item.kind === "http") {
        let reqHeaders = {};
        if (item.headers) {
            const decoded = json.decode(item.headers);
            if (decoded) reqHeaders = decoded;
        }

        try {
            const result = http.post(item.destination, item.payload, {
                headers: reqHeaders
            });
            if (result && result.status >= 200 && result.status < 300)
                return [true, null];
            return [false, "HTTP " + result.status];
        } catch (e) {
            return [false, String(e)];
        }
    }

    return [false, "unsupported outbox kind: " + item.kind];
}

/**
 * Compute exponential backoff delay (seconds) for attempt N.
 * 2^attempt * 10 seconds, capped at 1 hour.
 */
function backoffDelay(attempt) {
    let delay = Math.pow(2, attempt) * 10;
    if (delay > 3600) delay = 3600;
    return delay;
}

/**
 * Flush pending outbox items. Delivers items where next_attempt_at <= now.
 * @param {Object} opts - Options: limit (max items, default 50)
 * @returns {{ delivered: number, failed: number, retried: number }}
 */
function flush(opts) {
    const o = opts || {};
    const limit = o.limit || 50;
    const now = time.now();

    const items = db.query(
        "SELECT id, kind, destination, payload, headers, idempotency_key, attempts, max_attempts " +
        "FROM _hull_outbox WHERE state = 'pending' AND next_attempt_at <= ? ORDER BY id LIMIT ?",
        [now, limit]
    );

    let delivered = 0, failed = 0, retried = 0;

    for (let i = 0; i < items.length; i++) {
        const item = items[i];
        const [ok, err] = deliverItem(item);

        if (ok) {
            db.exec(
                "UPDATE _hull_outbox SET state = 'delivered', delivered_at = ?, attempts = attempts + 1 WHERE id = ?",
                [time.now(), item.id]
            );
            delivered++;
        } else {
            const newAttempts = item.attempts + 1;
            if (newAttempts >= item.max_attempts) {
                db.exec(
                    "UPDATE _hull_outbox SET state = 'failed', attempts = ?, last_error = ? WHERE id = ?",
                    [newAttempts, err, item.id]
                );
                failed++;
            } else {
                const nextAt = time.now() + backoffDelay(newAttempts);
                db.exec(
                    "UPDATE _hull_outbox SET attempts = ?, next_attempt_at = ?, last_error = ? WHERE id = ?",
                    [newAttempts, nextAt, err, item.id]
                );
                retried++;
            }
        }
    }

    return { delivered, failed, retried };
}

/**
 * Create a post-body middleware that marks requests for outbox flush.
 */
function middleware() {
    return function(req, _res) {
        if (!req.ctx) req.ctx = {};
        req.ctx._outbox_flush = true;
        return 0;
    };
}

/**
 * Flush if the request was marked for it.
 */
function flushIfNeeded(req) {
    if (req.ctx && req.ctx._outbox_flush)
        flush();
}

/**
 * Get counts of items by state.
 * @returns {{ pending: number, delivered: number, failed: number }}
 */
function stats() {
    const rows = db.query(
        "SELECT state, COUNT(*) as count FROM _hull_outbox GROUP BY state"
    );
    const result = { pending: 0, delivered: 0, failed: 0 };
    for (let i = 0; i < rows.length; i++)
        result[rows[i].state] = rows[i].count;
    return result;
}

/**
 * Delete delivered items older than maxAge seconds.
 * @param {number} maxAge - seconds (default 7 days)
 * @returns {number} deleted count
 */
function cleanup(maxAge) {
    if (maxAge === undefined) maxAge = 86400 * 7;
    const cutoff = time.now() - maxAge;
    return db.exec(
        "DELETE FROM _hull_outbox WHERE state = 'delivered' AND delivered_at <= ?",
        [cutoff]
    );
}

const outbox = { init, enqueue, flush, middleware, flushIfNeeded, stats, cleanup };
export { outbox };
