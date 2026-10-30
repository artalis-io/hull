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
 * Delivery is **at-least-once** - design receivers to be idempotent
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
import { _test_hooks } from "hull:_test_hooks";
import { _refusal } from "hull:_refusal";

let maxAttempts = 5;

// The delivery call. Tests swap it through hull:_test_hooks: the httpClient
// export is frozen (audit 18), so it can no longer be stubbed by assignment -
// and an app could not stub it either.
const realPost = (url, body, opts) => httpClient.async.post(url, body, opts);
let post = realPost;

// Whether `headers` already names `want`, in any case: the app's own
// "idempotency-key" was sent beside the row's "Idempotency-Key" as two
// headers (audit 23).
function hasHeader(headers, want) {
    want = want.toLowerCase();
    const keys = Object.keys(headers);
    for (let i = 0; i < keys.length; i++)
        if (keys[i].toLowerCase() === want) return true;
    return false;
}

// maxAttempts lands in an INTEGER column and is compared with the attempt
// count: a whole number, 0 (no retries) up to int4's maximum. A string or a
// fraction was bound as given (audit 25).
const MAX_ATTEMPTS_MAX = 2147483647;
function checkMaxAttempts(v, where) {
    if (!Number.isInteger(v) || v < 0 || v > MAX_ATTEMPTS_MAX)
        throw new TypeError(where + ": maxAttempts must be a whole number >= 0");
    return v;
}

// The first attempt's delay: a finite number of seconds or a numeric string,
// as whole seconds (next_attempt_at is an INTEGER column). `now + "60"`
// concatenated to "170000000060" (audit 25).
const DELAY_MAX = 2 ** 52;
function delaySecs(v) {
    if (v === undefined || v === null) return 0;
    let n = v;
    if (typeof n === "string" && n.trim() !== "") n = Number(n);
    if (typeof n !== "number" || !Number.isFinite(n))
        throw new TypeError("outbox.enqueue: delay must be a finite number of seconds");
    if (n > DELAY_MAX) n = DELAY_MAX;
    if (n < -DELAY_MAX) n = -DELAY_MAX;
    return Math.ceil(n);
}

// The id column's DDL. DuckDB has no inline identity column: its
// identityColumn template names a sequence (`nextval('%s')`) that
// identitySequence creates, and the raw template referenced a sequence
// literally named %s. The sequence is Hull's (`_hull_...`), so app SQL
// cannot reach it (audit 37). Every other backend's DDL is inline.
function idColumnDdl(seq) {
    const d = db.dialect;
    if (d && typeof d.identitySequence === "string") {
        db.exec(d.identitySequence.replace("%s", seq));
        return d.identityColumn.replace("%s", seq);
    }
    return db.autoincrementIdDdl;
}

/**
 * Initialize the `_hull_outbox` SQLite table. Idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.maxAttempts=5]
 *   Max delivery attempts before the row is marked `failed`.
 */
function init(opts) {
    const o = opts || {};
    if (o.maxAttempts !== undefined) maxAttempts = checkMaxAttempts(o.maxAttempts, "outbox.init");

    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_outbox (" +
        "  id              " + idColumnDdl("_hull_outbox_id_seq") + "," +
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
    const max = opts.maxAttempts !== undefined
        ? checkMaxAttempts(opts.maxAttempts, "outbox.enqueue") : maxAttempts;
    const delay = delaySecs(opts.delay);

    const sql = "INSERT INTO _hull_outbox " +
        "(kind, destination, payload, headers, idempotency_key, max_attempts, next_attempt_at, state, created_at) " +
        "VALUES (?, ?, ?, ?, ?, ?, ?, 'pending', ?)";
    const vals = [opts.kind, opts.destination, opts.payload, opts.headers || null,
                  opts.idempotencyKey || null, max, now + delay, now];
    // The new id from the INSERT itself where the backend can (SQLite,
    // Postgres): lastId has no Postgres implementation, so enqueue returned
    // nothing there. MySQL has no RETURNING; its lastId is per connection.
    if (db.dialect && db.dialect.supportsReturning) {
        const rows = db.query(sql + " RETURNING id", vals);
        return rows && rows[0] ? rows[0].id : undefined;
    }
    db.exec(sql, vals);
    return db.lastId();
}

// How long a flush's claim on a row lasts before another flush may take it
// (seconds): longer than any delivery can run - http.fetch's ceiling
// (HL_HTTP_FETCH_MAX_TIMEOUT_MS, 10 min) plus a minute. It was 300 s, under
// the 10 min a manifest `http.timeoutMs` may give a delivery, so another
// flush took the row and sent it again while the first was still waiting
// (audit 24). The app's own timeout still bounds each delivery.
const CLAIM_LEASE = 660;

/**
 * Attempt to deliver a single outbox item.
 * @returns {[boolean, string|null, *]} [success, errorMessage, refusal]:
 *   refusal is the runtime's refusal of the delivery (an Error whose `code`
 *   is "busy" or "wait_refused", hull:_refusal), if any. A refusal says where
 *   or when flush ran, not how the destination answered, so it is never
 *   recorded as a failed attempt (audit 18; audit 35: the fetch cap's
 *   refusal was, since the stdlib matched only the older messages - it tests
 *   the code now).
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
        // Delivery is at-least-once, so give the receiver what it needs to
        // drop a repeat: the row's idempotency key, unless the app set the
        // header itself.
        if (typeof item.idempotency_key === "string" && item.idempotency_key !== ""
            && !hasHeader(reqHeaders, "Idempotency-Key"))
            reqHeaders["Idempotency-Key"] = item.idempotency_key;

        try {
            const result = await post(item.destination, item.payload, {
                headers: reqHeaders
            });
            if (result && result.status >= 200 && result.status < 300)
                return [true, null];
            return [false, "HTTP " + result.status];
        } catch (e) {
            if (_refusal.code(e)) return [false, String(e), e];
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
    // Phase 6 audit L-4: defensive typeof guard - non-numeric `attempt`
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
 * Delivery is at-least-once - design receivers to be idempotent.
 *
 * @param {Object} [opts]
 * @param {number} [opts.limit=50]  Max items to process per call.
 * @returns {Promise<{ delivered: number, failed: number, retried: number, busy?: true }>}
 *   `busy: true` when the runtime refused a delivery as busy (the VM's fetch
 *   cap): the flush stopped there, the rest stay pending, unattempted.
 */

// Lua's tonumber(v) for a number or a string: a decimal numeral (optional
// sign, fraction, exponent) or a 0x hex integer, surrounded by optional
// whitespace; anything else is undefined. Number(v) also read "Infinity",
// "0b11" and "0o7", which Lua's tonumber refuses (audit 27).
function luaToNumber(v) {
    if (typeof v === "number") return v;
    if (typeof v !== "string") return undefined;
    const t = v.trim();
    if (/^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(t)) return Number(t);
    const m = /^([+-]?)0[xX]([0-9a-fA-F]+)$/.exec(t);
    if (m) { const h = parseInt(m[2], 16); return m[1] === "-" ? -h : h; }
    return undefined;
}

async function flush(opts) {
    // Delivery waits on http.fetch, which the runtime refuses inside a
    // transaction; refused here, before any row is claimed (audit 18).
    if (db.inTransaction()) {
        throw new Error("outbox.flush cannot run inside db.batch or an open " +
            "transaction: delivery waits on the network. Call it after the commit");
    }
    const o = opts || {};
    // 1..1000 (default 50): a number or numeric string counts (Lua's
    // tonumber), anything else and NaN take the default, and 0 clamps to 1
    // as in Lua; a negative LIMIT is "no limit" on SQLite (audit 24).
    let limit = luaToNumber(o.limit);
    if (limit === undefined || Number.isNaN(limit)) limit = 50;
    limit = Math.floor(Math.max(1, Math.min(limit, 1000)));
    const now = time.now();

    const items = db.query(
        "SELECT id, kind, destination, payload, headers, idempotency_key, attempts, max_attempts " +
        "FROM _hull_outbox WHERE state = 'pending' AND next_attempt_at <= ? ORDER BY id LIMIT ?",
        [now, limit]
    );

    let delivered = 0, failed = 0, retried = 0;

    for (let i = 0; i < items.length; i++) {
        const item = items[i];
        // Claim the row before sending it. Delivery yields to the event loop,
        // so another request's flush could select the same pending rows and
        // send them again. The claim pushes next_attempt_at out by a lease;
        // only the flush whose UPDATE changed the row delivers it. If this
        // process dies mid-send the lease runs out and the row is retried.
        // The lease starts at the claim, not at the flush: items are claimed
        // one at a time, each before its own awaited delivery, so with slow
        // deliveries ahead of it a later item got a lease that had already
        // run out - another flush reclaimed it at once and sent it again.
        const claimNow = time.now();
        const lease = claimNow + CLAIM_LEASE;
        const claimed = db.exec(
            "UPDATE _hull_outbox SET next_attempt_at = ? WHERE id = ? " +
            "AND state = 'pending' AND next_attempt_at <= ?",
            [lease, item.id, claimNow]);
        if (claimed !== 1) continue;
        const [ok, err, refusal] = await deliverItem(item);
        if (refusal !== undefined) {
            // Give the claim back (the row is due again at once): nothing was
            // sent, so no attempt is counted. A busy refusal (the VM's fetch
            // cap, its off-heap copy total) is transient and would refuse
            // every row after this one too, so the flush stops and reports it
            // (`busy: true`); the rows stay pending for the next flush. A
            // refusal to wait is the caller's bug: rethrown.
            db.exec(
                "UPDATE _hull_outbox SET next_attempt_at = ? WHERE id = ? " +
                "AND state = 'pending' AND next_attempt_at = ?",
                [claimNow, item.id, lease]);
            if (_refusal.isBusy(refusal))
                return { delivered, failed, retried, busy: true };
            throw refusal;
        }

        // Every outcome is written only while the row still holds THIS flush's
        // lease: a delivery that outran it was reclaimed and sent again by
        // another flush, and a late write here overwrote that flush's outcome.
        const mine = " AND state = 'pending' AND next_attempt_at = ?";
        if (ok) {
            const n = db.exec(
                "UPDATE _hull_outbox SET state = 'delivered', delivered_at = ?, attempts = attempts + 1 WHERE id = ?" + mine,
                [time.now(), item.id, lease]
            );
            if (n === 1) delivered++;
        } else {
            const newAttempts = item.attempts + 1;
            if (newAttempts >= item.max_attempts) {
                const n = db.exec(
                    "UPDATE _hull_outbox SET state = 'failed', attempts = ?, last_error = ? WHERE id = ?" + mine,
                    [newAttempts, err, item.id, lease]
                );
                if (n === 1) failed++;
            } else {
                const nextAt = time.now() + backoffDelay(newAttempts);
                const n = db.exec(
                    "UPDATE _hull_outbox SET attempts = ?, next_attempt_at = ?, last_error = ? WHERE id = ?" + mine,
                    [newAttempts, nextAt, err, item.id, lease]
                );
                if (n === 1) retried++;
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
 * L-5: NOTE - by default this only deletes `state = 'delivered'` rows.
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
        // SQLite - NULL is falsy in WHERE, so failed rows were
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

_test_hooks.register("hull:web:middleware:outbox", {
    setPost(fn) { post = fn || realPost; },
    claimLease: CLAIM_LEASE,
    // The suite reads its rows back from inside the module, where the
    // _hull_* namespace check admits the query.
    rows(id) {
        return db.query("SELECT state, kind, destination, idempotency_key, " +
                        "next_attempt_at, created_at FROM _hull_outbox WHERE id = ?", [id]);
    },
    clear() { db.exec("DELETE FROM _hull_outbox"); },
});
