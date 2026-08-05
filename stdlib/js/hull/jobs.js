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
 * Full surface: schema + jobs.init, enqueue, the atomic claim, per-type +
 * catch-all handlers, the work loop (retry-with-backoff, dead-letter, and the
 * visibility-timeout reaper), the dedicated worker (jobs.runWorker +
 * `hull jobs worker`), and the ops surface (jobs.stats / dead / retry / cancel / cleanup).
 *
 * @license AGPL-3.0-or-later
 */

import { db as dbModule } from "hull:db";
import { time } from "hull:time";
import { json } from "hull:json";
import { crypto } from "hull:crypto";
const db = dbModule.default();

// Outcome sentinels a handler returns. Frozen objects so
// a handler can't collide with them by returning ordinary data.
export const RETRY   = Object.freeze({ _hullJobsOutcome: "retry" });
export const DEAD    = Object.freeze({ _hullJobsOutcome: "dead" });
export const DISCARD = Object.freeze({ _hullJobsOutcome: "discard" });

// Module config; defaults overridable via init(opts).
const _cfg = {
    maxAttempts:       25,    // dead-letter threshold
    visibilityTimeout: 300,   // seconds before an orphaned `running` job is reclaimed
    reapInterval:      30,    // min seconds between reaper sweeps (0 = every work() call)
    backoff: defaultBackoff,
};

// Exponential backoff: 2^attempt * 10s, capped at 1h (shared with outbox math).
function defaultBackoff(attempt) {
    const d = Math.pow(2, attempt) * 10;
    return d > 3600 ? 3600 : d;
}

// Registered handlers: type -> fn. `_default` is the optional catch-all.
// Prototype-free so a job whose `type` matches an Object.prototype name
// ("toString", "constructor", ...) resolves to undefined (no handler -> default
// / dead-letter), not an inherited function. Matters when the app enqueues a
// user-controlled type; mirrors Lua tables (which have no prototype chain).
const _handlers = Object.create(null);
let _default = null;

/**
 * Create the `_hull_jobs` table and its indexes. Idempotent - safe on every
 * boot. Uses the connection's portable identity DDL + IF-NOT-EXISTS index
 * form, so the same call runs unchanged on SQLite, PostgreSQL, and MySQL.
 *
 * @param {object} [opts]
 * @param {number} [opts.maxAttempts=25]        dead-letter threshold
 * @param {number} [opts.visibilityTimeout=300] seconds before reclaim
 * @param {number} [opts.reapInterval=30]       min seconds between reaper sweeps
 * @param {function} [opts.backoff]             attempt -> delay seconds
 */
function init(opts) {
    const o = opts || {};
    if (o.maxAttempts !== undefined) _cfg.maxAttempts = o.maxAttempts;
    if (o.visibilityTimeout !== undefined) _cfg.visibilityTimeout = o.visibilityTimeout;
    if (o.reapInterval !== undefined) _cfg.reapInterval = o.reapInterval;
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
 * one caller. Low-level: `jobs.work` wraps this.
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
 * (`app.every(1000, () => jobs.work())`) or a dedicated worker (runWorker).
 * Idempotent handlers required (a job may run more than once). Returns the
 * number of jobs processed.
 * @param {object} [opts]  { queue, batch, visibilityTimeout }
 * @returns {Promise<number>}
 */
async function work(opts) {
    const o = opts || {};
    // Throttle the reaper: a no-op sweep still takes the write lock, so under a
    // fast poller x N workers reaping every tick adds needless contention.
    const interval = o.reapInterval !== undefined ? o.reapInterval : _cfg.reapInterval;
    const now = time.now();
    if (now - _lastReap >= interval) { reap(o); _lastReap = now; }
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

// Worker loop control: stop() flips this so a running runWorker returns after
// its current iteration. A fresh runWorker resets it to true.
let _running = false;

// Unix ts of the last reaper sweep (throttled in work() via _cfg.reapInterval).
let _lastReap = 0;

/**
 * Request the running runWorker loop to stop after the current iteration.
 * No-op if no worker is running. Safe to call from a handler.
 */
function stop() {
    _running = false;
}

/**
 * Blocking claim loop: the dedicated-worker execution model. Call it from
 * `app.main` (`app.main(async () => { await jobs.runWorker(); })`) and run the
 * app as its own process (`hull app.js`, or `hull jobs worker app.js`); run K
 * copies for horizontal scale - each claims disjoint jobs via the atomic claim.
 * Each iteration runs `work` (claim a batch, dispatch, reap); when a claim comes
 * back empty it sleeps `pollMs` (yielding to the event loop, so async handlers
 * and timers keep running). Returns the total jobs processed when the loop exits.
 *
 * Exit conditions: `jobs.stop()` (graceful), or - for bounded / batch-drain
 * runs - `opts.drain` / `opts.maxEmptyPolls`. With neither, it runs until
 * stopped or the process is signalled (an in-flight job is then reclaimed by
 * the visibility-timeout reaper, since handlers are at-least-once).
 * @param {object} [opts]  { queue, batch, pollMs, visibilityTimeout, drain,
 *                           maxEmptyPolls }
 * @returns {Promise<number>}  total jobs processed
 */
async function runWorker(opts) {
    const o = opts || {};
    const pollMs = o.pollMs !== undefined ? o.pollMs : 1000;
    const maxEmpty = o.maxEmptyPolls !== undefined ? o.maxEmptyPolls : (o.drain ? 1 : 0);
    _running = true;
    let processed = 0, empty = 0;
    while (_running) {
        const n = await work(o);
        processed += n;
        if (n === 0) {
            empty++;
            if (maxEmpty > 0 && empty >= maxEmpty) break;
            await hull.sleep(pollMs);
        } else {
            empty = 0;
        }
    }
    return processed;
}

/**
 * Count jobs by status (optionally scoped to a queue). The ops overview.
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

// Decode an ops row into an inspection view: the handler-facing shape plus the
// bookkeeping columns an operator needs (queue, lastError, timestamps).
function shapeOps(r) {
    const j = shape(r);
    j.queue = r.queue;
    j.attempts = r.attempts;
    j.lastError = r.last_error;
    j.createdAt = r.created_at;
    j.updatedAt = r.updated_at;
    return j;
}

/**
 * List dead-lettered jobs (status='dead'), newest first. The ops entry point
 * for inspecting failures before requeuing (retry) or purging (cleanup).
 * @param {object} [opts]  { queue, limit = 100, offset = 0 }
 * @returns {Array}  { id, queue, type, data, attempts, maxAttempts, lastError,
 *                     createdAt, updatedAt }
 */
function dead(opts) {
    const o = opts || {};
    const limit = o.limit !== undefined ? o.limit : 100;
    const offset = o.offset !== undefined ? o.offset : 0;
    const rows = o.queue
        ? db.query("SELECT * FROM _hull_jobs WHERE status='dead' AND queue=? " +
                   "ORDER BY updated_at DESC, id DESC LIMIT ? OFFSET ?", [o.queue, limit, offset])
        : db.query("SELECT * FROM _hull_jobs WHERE status='dead' " +
                   "ORDER BY updated_at DESC, id DESC LIMIT ? OFFSET ?", [limit, offset]);
    return rows.map(shapeOps);
}

/**
 * Requeue a dead-lettered job for another run. Resets it to pending with a
 * fresh attempt budget (attempts=0) and clears the last error. No-op unless the
 * job exists and is currently dead (so it can't double-requeue a live job).
 * @param {number} id
 * @returns {boolean}  true if a dead job was requeued
 */
function retry(id) {
    const now = time.now();
    const n = db.exec(
        "UPDATE _hull_jobs SET status='pending', run_at=?, attempts=0, " +
        "claim_token=NULL, claimed_at=NULL, last_error=NULL, updated_at=? " +
        "WHERE id=? AND status='dead'",
        [now, now, id]);
    return (n || 0) > 0;
}

/**
 * Cancel a not-yet-started job by id: delete it if it is still `pending`
 * (covers delayed / scheduled jobs). A `running` job is mid-flight and is NOT
 * cancelled (let it finish or dead-letter). Returns whether a row was removed.
 * @param {number} id
 * @returns {boolean}
 */
function cancel(id) {
    return (db.exec("DELETE FROM _hull_jobs WHERE id=? AND status='pending'", [id]) || 0) > 0;
}

/**
 * Purge terminal jobs (done + dead by default) whose last update is older than
 * a retention age. Run from app.daily to bound table growth. Only touches
 * terminal rows, so it never races a pending / running job.
 * @param {object} [opts]  { queue, olderThan = 604800, before, statuses = ["done","dead"] }
 * @returns {number}  rows deleted
 */
function cleanup(opts) {
    const o = opts || {};
    const statuses = o.statuses || ["done", "dead"];
    const cutoff = o.before !== undefined ? o.before : (time.now() - (o.olderThan !== undefined ? o.olderThan : 604800));
    const placeholders = statuses.map(() => "?");
    const params = statuses.slice();
    params.push(cutoff);
    let sql = "DELETE FROM _hull_jobs WHERE status IN (" +
        placeholders.join(",") + ") AND updated_at < ?";
    if (o.queue) { sql += " AND queue=?"; params.push(o.queue); }
    return db.exec(sql, params) || 0;
}

export const jobs = {
    init, enqueue, claim, handler, default: setDefault, work, reap, stats,
    runWorker, stop, dead, retry, cancel, cleanup, RETRY, DEAD, DISCARD, _config: _cfg,
};
export { init, enqueue, claim, handler, work, reap, stats, runWorker, stop,
         dead, retry, cancel, cleanup };
export default jobs;
