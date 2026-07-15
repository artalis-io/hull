/**
 * @file hull:web:middleware:outbox
 * @module hull:web:middleware:outbox
 * @description Transactional outbox for reliable side-effect delivery. Lua
 *   parity: `hull.web.middleware.outbox`.
 *
 * Decouples external side effects (webhooks, HTTP, SMTP) from the request
 * transaction. The handler enqueues an outbox row inside the same
 * `db.batch` that commits the state change; the outbox flusher then
 * delivers the row after commit with exponential backoff retries.
 *
 * Delivery is **at-least-once** — design receivers to be idempotent
 * (e.g. with `hull:web:middleware:inbox`).
 *
 * **Backoff schedule:** `2^attempt * 10s`, capped at 1 hour. After
 * `maxAttempts` (default 5) the row is marked `failed`.
 *
 * @license AGPL-3.0-or-later
 */

import { db as dbModule } from "hull:db";
const db = dbModule.default();
import { httpClient } from "hull:http-client";
import { time } from "hull:time";
import { json } from "hull:json";

let maxAttempts = 5;

/**
 * Initialize the `_hull_outbox` SQLite table. Idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.maxAttempts=5]
 *   Max delivery attempts before the row is marked `failed`.
 */
function init(opts) {
    const o = opts || {};
    if (o.maxAttempts !== undefined) maxAttempts = o.maxAttempts;

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_outbox (" +
        "  id              " + db.autoincrementIdDdl + "," +
        "  kind            TEXT NOT NULL," +
        "  destination     TEXT NOT NULL," +
        "  payload         TEXT NOT NULL," +
        "  headers         TEXT," +
        "  idempotency_key VARCHAR(255)," +
        "  attempts        INTEGER NOT NULL DEFAULT 0," +
        "  max_attempts    INTEGER NOT NULL DEFAULT 5," +
        "  next_attempt_at INTEGER NOT NULL," +
        "  state           VARCHAR(32) NOT NULL DEFAULT 'pending'," +
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
 *
 * Must be called inside a `db.batch` to be atomic with the state change.
 *
 * @param {Object} opts
 * @param {string} opts.kind            `"webhook"`, `"http"`, `"smtp"`, ...
 * @param {string} opts.destination     Target URL or address.
 * @param {string} opts.payload         Body bytes.
 * @param {string} [opts.headers]       JSON-encoded headers.
 * @param {string} [opts.idempotencyKey]  Dedup key (recommended).
 * @param {number} [opts.maxAttempts]   Override module default.
 * @param {number} [opts.delay=0]       Seconds to delay first attempt.
 * @returns {number}  New `_hull_outbox.id`.
 * @throws If `kind`, `destination`, or `payload` is missing.
 *
 * @example
 * outbox.enqueue({
 *     kind: "webhook",
 *     destination: webhook.url,
 *     payload: JSON.stringify(data),
 *     headers: JSON.stringify({ "Content-Type": "application/json" }),
 *     idempotencyKey: `evt-${eventId}-wh-${wh.id}`,
 * });
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
async function deliverItem(item) {
    if (item.kind === "webhook" || item.kind === "http") {
        let reqHeaders = {};
        if (item.headers) {
            let decoded;
            try { decoded = json.decode(item.headers); }
            catch (_e) { decoded = null; }
            if (decoded && typeof decoded === "object") reqHeaders = decoded;
        }

        try {
            const result = await httpClient.async.post(item.destination, item.payload, {
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
    // L-4: `1 << attempt` overflows / sign-flips at attempt >= 31. Default
    // maxAttempts is 5, but a user-set higher cap should still produce
    // sane backoff. Math.pow + cap on the exponent first.
    // Phase 6 audit L-4: defensive typeof guard — non-numeric `attempt`
    // (e.g. undefined from a malformed row) coerces to 0 via `| 0` which
    // is fine but worth making the intent explicit.
    if (typeof attempt !== "number" || !isFinite(attempt)) attempt = 0;
    const exp = Math.min(Math.max(0, attempt | 0), 30);
    return Math.min(Math.pow(2, exp) * 10, 3600);
}

/**
 * Flush pending outbox items where `next_attempt_at <= now`.
 *
 * Successful deliveries are marked `delivered`. Failures bump the attempt
 * counter and reschedule; after `maxAttempts` the row is marked `failed`.
 *
 * Delivery is at-least-once — design receivers to be idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.limit=50]  Max items to process per call.
 * @returns {Promise<{ delivered: number, failed: number, retried: number }>}
 */
async function flush(opts) {
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
        const [ok, err] = await deliverItem(item);

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
 * Build a post-body middleware that marks the request for auto-flush.
 *
 * Sets `req.ctx._outbox_flush = true`. Call `outbox.flushIfNeeded(req)`
 * from a post-handler hook to trigger the actual flush.
 *
 * @returns {(req, res) => number}
 */
function middleware() {
    return function(req, _res) {
        if (!req.ctx) req.ctx = {};
        req.ctx._outbox_flush = true;
        return 0;
    };
}

/**
 * Flush if `outbox.middleware()` marked the request.
 *
 * Call from a post-handler hook (or explicitly from the handler).
 *
 * @param {Object} req
 */
async function flushIfNeeded(req) {
    if (req.ctx && req.ctx._outbox_flush)
        await flush();
}

/**
 * Count outbox rows per state.
 *
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
 *
 * L-5: NOTE — by default this only deletes `state = 'delivered'` rows.
 * Failed deliveries (`state = 'failed'`, i.e. exceeded maxAttempts) are
 * retained indefinitely as an audit trail. Pass `opts.alsoFailed = true`
 * to delete old failed rows too (when, e.g., you've drained them via an
 * out-of-band investigation).
 *
 * @param {number} maxAge - seconds (default 7 days)
 * @param {Object} [opts] - { alsoFailed?: boolean }
 * @returns {number} deleted count
 */
function cleanup(maxAge, opts) {
    if (maxAge === undefined) maxAge = 86400 * 7;
    const cutoff = time.now() - maxAge;
    const alsoFailed = !!(opts && opts.alsoFailed);
    if (alsoFailed) {
        // Failed rows have delivered_at = NULL (only the success
        // path at line ~190 sets it). The pre-fix predicate
        // `delivered_at <= ?` evaluates NULL for those rows in
        // SQLite — NULL is falsy in WHERE, so failed rows were
        // never deleted. Branch the SQL: gate `delivered` on
        // delivered_at; gate `failed` on created_at (which is
        // always set).
        return db.exec(
            "DELETE FROM _hull_outbox WHERE "
            + "(state = 'delivered' AND delivered_at <= ?) "
            + "OR (state = 'failed' AND created_at <= ?)",
            [cutoff, cutoff]
        );
    }
    return db.exec(
        "DELETE FROM _hull_outbox WHERE state = 'delivered' AND delivered_at <= ?",
        [cutoff]
    );
}

const outbox = { init, enqueue, flush, middleware, flushIfNeeded, stats, cleanup };
export { outbox };
