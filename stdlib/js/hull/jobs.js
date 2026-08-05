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
 * Implemented: schema + jobs.init, enqueue, the atomic claim, per-type +
 * catch-all handlers, and the work loop (retry-with-backoff, dead-letter, and
 * the visibility-timeout reaper). The dedicated `hull jobs worker` process
 * lands in a later phase.
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

// Registered handlers: type -> fn. `_default` is the optional catch-all.
const _handlers = {};
let _default = null;

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

// ── Handlers + the work loop ────────────────────────────────────────────

/**
 * Register a handler for a job type. `jobs.work` dispatches each claimed job to
 * its type's handler (the handler may be sync or async). Return nil/true (or
 * any data) to complete; throw to retry with backoff; return `jobs.RETRY` /
 * `jobs.DEAD` / `jobs.DISCARD` for that outcome explicitly.
 * @param {string} jobType
 * @param {function} fn  (job) => void | Promise
 */
function handler(jobType, fn) {
    if (typeof jobType !== "string" || jobType === "")
        throw new Error("jobs.handler: type must be a non-empty string");
    if (typeof fn !== "function") throw new Error("jobs.handler: handler must be a function");
    _handlers[jobType] = fn;
    return jobs;
}

/**
 * Optional catch-all for job types with no registered handler. Without it, an
 * unhandled type is dead-lettered.
 * @param {function} fn
 */
function setDefault(fn) {
    if (typeof fn !== "function") throw new Error("jobs.default: handler must be a function");
    _default = fn;
    return jobs;
}

function markDone(id) {
    db.exec("UPDATE _hull_jobs SET status='done', claim_token=NULL, updated_at=? WHERE id=?",
        [time.now(), id]);
}

function markDead(id, err) {
    db.exec("UPDATE _hull_jobs SET status='dead', last_error=?, claim_token=NULL, updated_at=? WHERE id=?",
        [err, time.now(), id]);
}

// Reschedule with backoff, or dead-letter once attempts are exhausted. attempts
// was already incremented by the claim (the attempt that just ran).
function markRetry(job, err) {
    const attempts = job.attempts || 0;
    const max = job.maxAttempts !== undefined && job.maxAttempts !== null
        ? job.maxAttempts : _cfg.maxAttempts;
    if (attempts >= max) { markDead(job.id, err); return; }
    const now = time.now();
    db.exec(
        "UPDATE _hull_jobs SET status='pending', run_at=?, last_error=?, claim_token=NULL, " +
        "updated_at=? WHERE id=?",
        [now + _cfg.backoff(attempts), err, now, job.id]);
}

/**
 * Reclaim jobs stuck in `running` past the visibility timeout (a worker died
 * mid-job). Reset to `pending`; their incremented attempts persist. `jobs.work`
 * runs this each call.
 * @param {object} [opts]  { visibilityTimeout }
 */
function reap(opts) {
    const o = opts || {};
    const vt = o.visibilityTimeout !== undefined ? o.visibilityTimeout : _cfg.visibilityTimeout;
    const now = time.now();
    return db.exec(
        "UPDATE _hull_jobs SET status='pending', claim_token=NULL, updated_at=? " +
        "WHERE status='running' AND claimed_at <= ?",
        [now, now - vt]) || 0;
}

/**
 * Claim a batch and run each job's handler (awaited, so sync and async handlers
 * both work), applying the outcome. Runs the reaper first. Drive from a timer
 * (`app.every(1000, () => jobs.work())`) or a dedicated worker (Phase 4).
 * Idempotent handlers required (a job may run more than once). Returns the
 * number of jobs processed.
 * @param {object} [opts]  { queue, batch, visibilityTimeout }
 * @returns {Promise<number>}
 */
async function work(opts) {
    const o = opts || {};
    reap(o);
    const batch = claim(o);
    for (const job of batch) {
        const h = _handlers[job.type] || _default;
        if (!h) { markDead(job.id, `no handler for job type '${job.type}'`); continue; }
        try {
            const result = await h(job);
            if (result === DEAD) markDead(job.id, "handler returned jobs.DEAD");
            else if (result === RETRY) markRetry(job, "handler requested retry");
            else markDone(job.id);   // undefined / true / DISCARD / any value -> done
        } catch (e) {
            markRetry(job, String((e && e.message) || e));
        }
    }
    return batch.length;
}

/**
 * Count jobs by status (optionally scoped to a queue). A minimal ops view.
 * @param {object} [opts]  { queue }
 * @returns {{pending:number, running:number, done:number, failed:number, dead:number}}
 */
function stats(opts) {
    const o = opts || {};
    const rows = o.queue
        ? db.query("SELECT status, COUNT(*) AS c FROM _hull_jobs WHERE queue=? GROUP BY status", [o.queue])
        : db.query("SELECT status, COUNT(*) AS c FROM _hull_jobs GROUP BY status");
    const s = { pending: 0, running: 0, done: 0, failed: 0, dead: 0 };
    for (const r of rows) s[r.status] = r.c;
    return s;
}

export const jobs = {
    init, enqueue, claim, handler, default: setDefault, work, reap, stats,
    RETRY, DEAD, DISCARD, _config: _cfg,
};
export { init, enqueue, claim, handler, work, reap, stats };
export default jobs;
