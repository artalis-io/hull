/**
 * @file hull:jobs
 * @module hull:jobs
 * @description Durable, DB-backed background job queue. Lua parity:
 *   `hull.jobs`.
 *
 * Enqueue a unit of work, process it later with retries, backoff, and a
 * dead-letter path. DB-backend agnostic (SQLite / PostgreSQL / MySQL via the
 * hull/db capability surface), orthogonal (deps: hull/db + hull/time +
 * hull/json only), and transactionally coupled (enqueue is a plain INSERT, so
 * it joins the caller's db.batch and commits with the business row).
 *
 * Full design: docs/jobs_design.md.
 *
 * Phase 1 (this file): schema + jobs.init. enqueue / handlers / the atomic
 * claim / the work loop / the dedicated worker land in later phases.
 *
 * @license AGPL-3.0-or-later
 */

import { db as dbModule } from "hull:db";
import { time } from "hull:time";
import { json } from "hull:json";
import { crypto } from "hull:crypto";
const db = dbModule.default();

// Outcome sentinels a handler returns (consumed in Phase 3). Frozen objects so
// a handler can't collide with them by returning ordinary data.
export const RETRY   = Object.freeze({ _hullJobsOutcome: "retry" });
export const DEAD    = Object.freeze({ _hullJobsOutcome: "dead" });
export const DISCARD = Object.freeze({ _hullJobsOutcome: "discard" });

// Module config; defaults overridable via init(opts). Read by later phases.
const _cfg = {
    maxAttempts:       25,    // dead-letter threshold
    visibilityTimeout: 300,   // seconds before an orphaned `running` job is reclaimed
    backoff: defaultBackoff,
};

// Exponential backoff: 2^attempt * 10s, capped at 1h (shared with outbox math).
function defaultBackoff(attempt) {
    const d = Math.pow(2, attempt) * 10;
    return d > 3600 ? 3600 : d;
}

/**
 * Create the `_hull_jobs` table and its indexes. Idempotent - safe on every
 * boot. Uses the connection's portable identity DDL + IF-NOT-EXISTS index
 * form, so the same call runs unchanged on SQLite, PostgreSQL, and MySQL.
 *
 * @param {object} [opts]
 * @param {number} [opts.maxAttempts=25]        dead-letter threshold
 * @param {number} [opts.visibilityTimeout=300] seconds before reclaim
 * @param {function} [opts.backoff]             attempt -> delay seconds
 */
function init(opts) {
    const o = opts || {};
    if (o.maxAttempts !== undefined) _cfg.maxAttempts = o.maxAttempts;
    if (o.visibilityTimeout !== undefined) _cfg.visibilityTimeout = o.visibilityTimeout;
    if (o.backoff !== undefined) _cfg.backoff = o.backoff;

    // Keyed/indexed text columns are VARCHAR(255) so MySQL can index them;
    // data-only columns (payload, last_error) stay TEXT.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_jobs (" +
        "id           " + db.autoincrementIdDdl + ", " +
        "queue        VARCHAR(255) NOT NULL DEFAULT 'default'," +
        "type         VARCHAR(255) NOT NULL," +
        "payload      TEXT," +
        "status       VARCHAR(32)  NOT NULL DEFAULT 'pending'," +
        "priority     INTEGER      NOT NULL DEFAULT 0," +
        "attempts     INTEGER      NOT NULL DEFAULT 0," +
        "max_attempts INTEGER      NOT NULL DEFAULT 25," +
        "run_at       INTEGER      NOT NULL DEFAULT 0," +
        "claim_token  VARCHAR(255)," +
        "claimed_at   INTEGER," +
        "dedup_key    VARCHAR(255)," +
        "last_error   TEXT," +
        "created_at   INTEGER      NOT NULL," +
        "updated_at   INTEGER      NOT NULL)");

    // Claim scan path: ready-to-run pending jobs in a queue, by priority then id.
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_jobs_claim " +
        "ON _hull_jobs(queue, status, run_at, priority, id)");

    // Reaper scan path: running jobs by claim age (visibility-timeout reclaim).
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_jobs_reap " +
        "ON _hull_jobs(status, claimed_at)");

    // Idempotent enqueue: a non-null dedup_key is unique per queue. NULLs are
    // distinct on every backend, so un-deduped jobs never collide.
    db.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_hull_jobs_dedup " +
        "ON _hull_jobs(queue, dedup_key)");

    return jobs;
}

/**
 * Enqueue a job. A plain INSERT, so calling it inside a `db.batch()` commits
 * the job atomically with the business row (transactional coupling).
 *
 * @param {string} jobType  handler dispatch key (non-empty)
 * @param {*} [data]        JSON-encodable payload (reaches the handler as job.data)
 * @param {object} [opts]   { queue, priority, delay, runAt, maxAttempts, dedupKey }
 * @returns {number|null}   the new job id, or null when a dedupKey collapsed it
 */
function enqueue(jobType, data, opts) {
    if (typeof jobType !== "string" || jobType === "")
        throw new Error("jobs.enqueue: type must be a non-empty string");
    const o = opts || {};
    const now = time.now();
    const runAt = o.runAt !== undefined ? o.runAt : now + (o.delay || 0);
    const vals = [
        o.queue || "default",
        jobType,
        data !== undefined && data !== null ? json.encode(data) : null,
        o.priority || 0,
        o.maxAttempts !== undefined ? o.maxAttempts : _cfg.maxAttempts,
        runAt,
        o.dedupKey !== undefined ? o.dedupKey : null,
        now, now,
    ];
    const cols = ["queue", "type", "payload", "priority", "max_attempts",
                  "run_at", "dedup_key", "created_at", "updated_at"];
    if (o.dedupKey !== undefined && o.dedupKey !== null) {
        const n = db.insertIfAbsent("_hull_jobs", ["queue", "dedup_key"], cols, vals);
        if (n && n > 0) return db.lastId();
        return null;   // an un-run job with this (queue, dedupKey) already exists
    }
    db.exec(
        "INSERT INTO _hull_jobs (queue, type, payload, priority, max_attempts, " +
        "run_at, dedup_key, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        vals);
    return db.lastId();
}

// Decode a claimed DB row into a handler-facing job (payload -> data).
function shape(row) {
    let data = null;
    if (row.payload !== undefined && row.payload !== null && row.payload !== "") {
        try { data = json.decode(row.payload); } catch (_e) { data = null; }
    }
    return { id: row.id, type: row.type, data,
             attempts: row.attempts, maxAttempts: row.max_attempts };
}

/**
 * Atomically claim up to `batch` ready jobs from a queue, marking them
 * `running`. Concurrency-safe across workers and processes (SKIP LOCKED on
 * Postgres/MySQL, serialized on SQLite). Each ready job is claimed by exactly
 * one caller. Low-level: `jobs.work` (Phase 3) wraps this.
 *
 * @param {object} [opts]  { queue = "default", batch = 10 }
 * @returns {Array}  claimed jobs { id, type, data, attempts, maxAttempts }
 */
function claim(opts) {
    const o = opts || {};
    const queue = o.queue || "default";
    const batch = o.batch || 10;
    const now = time.now();
    const token = crypto.base64urlEncode(crypto.random(16));
    const d = db.dialect;

    let rows;
    if (d.supportsSkipLocked && d.supportsReturning) {
        rows = db.query(
            "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, " +
            "attempts=attempts+1, updated_at=? WHERE id IN (" +
            "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? " +
            "ORDER BY priority DESC, id LIMIT ? FOR UPDATE SKIP LOCKED) " +
            "RETURNING id, type, payload, priority, attempts, max_attempts",
            [token, now, now, queue, now, batch]);
    } else if (d.supportsSkipLocked) {
        db.batch(() => {
            const sel = db.query(
                "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? " +
                "ORDER BY priority DESC, id LIMIT ? FOR UPDATE SKIP LOCKED",
                [queue, now, batch]);
            if (sel.length === 0) return;
            const ph = [], params = [token, now, now];
            for (const r of sel) { ph.push("?"); params.push(r.id); }
            db.exec(
                "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, " +
                "attempts=attempts+1, updated_at=? WHERE id IN (" + ph.join(",") + ")",
                params);
        });
        rows = db.query(
            "SELECT id, type, payload, priority, attempts, max_attempts FROM _hull_jobs WHERE claim_token=?",
            [token]);
    } else {
        rows = db.query(
            "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, " +
            "attempts=attempts+1, updated_at=? WHERE id IN (" +
            "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? " +
            "ORDER BY priority DESC, id LIMIT ?) " +
            "RETURNING id, type, payload, priority, attempts, max_attempts",
            [token, now, now, queue, now, batch]);
    }

    // Priority-correct order within the batch: RETURNING / the token read-back
    // don't preserve the subquery's ORDER BY, so sort by priority DESC, id ASC.
    return (rows || [])
        .sort((x, y) => (y.priority || 0) - (x.priority || 0) || (x.id - y.id))
        .map(shape);
}

export const jobs = { init, enqueue, claim, RETRY, DEAD, DISCARD, _config: _cfg };
export { init, enqueue, claim };
export default jobs;
