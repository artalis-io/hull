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
 * `hull jobs worker`), and the ops surface (jobs.stats / dead / retry / cancel / cleanup). v1.1 adds durable cron (jobs.cron / uncron) and intra-process concurrency (run_worker concurrency=N).
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

    // Durable cron schedules (jobs.cron). A worker atomically advances a due
    // row's next_run_at (compare-and-set) and enqueues, so exactly one worker
    // fires each tick across the fleet.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_cron (" +
        "name         VARCHAR(255) NOT NULL PRIMARY KEY," +
        "spec         VARCHAR(255) NOT NULL," +
        "type         VARCHAR(255) NOT NULL," +
        "payload      TEXT," +
        "queue        VARCHAR(255) NOT NULL DEFAULT 'default'," +
        "priority     INTEGER      NOT NULL DEFAULT 0," +
        "max_attempts INTEGER," +
        "next_run_at  INTEGER      NOT NULL," +
        "last_run_at  INTEGER," +
        "updated_at   INTEGER      NOT NULL)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_hull_cron_due ON _hull_cron(next_run_at)");

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

// ── Cron (durable recurring schedules) ──────────────────────────────────────

// Broken-down UTC calendar from a unix ts (civil algorithm; no Date/gmtime
// dependency, so it's identical across backends and platforms).
function civilFromDays(z) {
    z += 719468;
    const era = Math.floor((z >= 0 ? z : z - 146096) / 146097);
    const doe = z - era * 146097;
    const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524)
                - Math.floor(doe / 146096)) / 365);
    const y = yoe + era * 400;
    const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100));
    const mp = Math.floor((5 * doy + 2) / 153);
    const d = doy - Math.floor((153 * mp + 2) / 5) + 1;
    const m = mp < 10 ? mp + 3 : mp - 9;
    return { year: y + (m <= 2 ? 1 : 0), month: m, day: d };
}

function decodeTs(ts) {
    const days = Math.floor(ts / 86400);
    const secs = ts - days * 86400;
    const c = civilFromDays(days);
    return { month: c.month, day: c.day, hour: Math.floor(secs / 3600),
             minute: Math.floor((secs % 3600) / 60), dow: ((days % 7) + 4) % 7 };
}

// Parse one cron field into a Set over [lo,hi]. Supports *, n, a-b, */s, a-b/s,
// and comma lists. Returns null on any malformed / out-of-range part.
function parseField(f, lo, hi) {
    const set = new Set();
    for (const part of f.split(",")) {
        const m = part.match(/^([^/]+)\/(\d+)$/);
        const step = m ? parseInt(m[2], 10) : 1;
        const range = m ? m[1] : part;
        let a, b;
        if (range === "*") { a = lo; b = hi; }
        else {
            const r = range.match(/^(\d+)-(\d+)$/);
            if (r) { a = parseInt(r[1], 10); b = parseInt(r[2], 10); }
            else { a = parseInt(range, 10); b = a; }
        }
        if (!Number.isFinite(a) || !Number.isFinite(b) || step < 1 || a < lo || b > hi || a > b) return null;
        for (let v = a; v <= b; v += step) set.add(v);
    }
    return set;
}

// Parse a 5-field cron spec (min hour dom month dow; dow 0/7=Sunday).
function parseCron(spec) {
    if (typeof spec !== "string") return null;
    const f = spec.trim().split(/\s+/);
    if (f.length !== 5) return null;
    const min = parseField(f[0], 0, 59), hour = parseField(f[1], 0, 23);
    const dom = parseField(f[2], 1, 31), month = parseField(f[3], 1, 12);
    const dow = parseField(f[4], 0, 7);
    if (!min || !hour || !dom || !month || !dow) return null;
    if (dow.has(7)) { dow.add(0); dow.delete(7); }
    return { min, hour, dom, month, dow, domStar: f[2] === "*", dowStar: f[4] === "*" };
}

// Smallest unix ts strictly after `fromTs` whose UTC time matches `c`.
function cronNext(c, fromTs) {
    let t = Math.floor(fromTs / 60) * 60 + 60;
    for (let i = 0; i < 200000; i++) {
        const { month, day, hour, minute, dow } = decodeTs(t);
        if (!c.month.has(month)) { t = (Math.floor(t / 86400) + 1) * 86400; continue; }
        let dayOk;
        if (c.domStar && c.dowStar) dayOk = true;
        else if (c.domStar) dayOk = c.dow.has(dow);
        else if (c.dowStar) dayOk = c.dom.has(day);
        else dayOk = c.dom.has(day) || c.dow.has(dow);
        if (!dayOk) { t = (Math.floor(t / 86400) + 1) * 86400; continue; }
        if (!c.hour.has(hour)) { t = (Math.floor(t / 3600) + 1) * 3600; continue; }
        if (!c.min.has(minute)) { t += 60; continue; }
        return t;
    }
    return null;
}

// Fire due schedules: compare-and-set next_run_at (multi-worker-safe on every
// backend), then enqueue. Missed ticks advance to the next future occurrence
// (fire-once, no backfill). Called (throttled) from work().
function processCron(now) {
    const due = db.query(
        "SELECT name, spec, type, payload, queue, priority, max_attempts, next_run_at " +
        "FROM _hull_cron WHERE next_run_at <= ?", [now]);
    for (const c of due) {
        const parsed = parseCron(c.spec);
        const nxt = parsed ? cronNext(parsed, now) : (now + 60);
        const won = db.exec(
            "UPDATE _hull_cron SET next_run_at=?, last_run_at=?, updated_at=? " +
            "WHERE name=? AND next_run_at=?",
            [nxt, now, now, c.name, c.next_run_at]);
        if ((won || 0) > 0) {
            let data = null;
            if (c.payload) { try { data = json.decode(c.payload); } catch (_e) { data = null; } }
            enqueue(c.type, data, {
                queue: c.queue, priority: c.priority,
                // NULL (no override) -> undefined so enqueue applies its default,
                // not a NULL bind into the NOT-NULL max_attempts column.
                maxAttempts: c.max_attempts === null ? undefined : c.max_attempts,
            });
        }
    }
}

/**
 * Register (or update) a durable recurring schedule. On each matching minute,
 * exactly one worker enqueues a job (compare-and-set on the schedule row).
 * Schedules live in the DB, so they survive restarts and coordinate across the
 * fleet; a worker (poller or runWorker) must be running to fire them.
 * @param {string} name  unique schedule id; also the enqueued job type by default
 * @param {string} spec  5-field cron: "min hour dom month dow" (UTC; dow 0/7=Sun)
 * @param {*} [data]      payload for the enqueued job
 * @param {object} [opts] { type, queue, priority, maxAttempts }
 * @returns {object} the jobs module (for chaining)
 */
function cron(name, spec, data, opts) {
    if (typeof name !== "string" || name === "")
        throw new Error("jobs.cron: name must be a non-empty string");
    const parsed = parseCron(spec);
    if (!parsed) throw new Error("jobs.cron: invalid cron spec");
    const o = opts || {};
    const now = time.now();
    const nxt = cronNext(parsed, now);
    if (nxt === null) throw new Error("jobs.cron: spec has no upcoming occurrence");
    const jobType = o.type || name;
    const payload = data !== undefined && data !== null ? json.encode(data) : null;
    const queue = o.queue || "default";
    const priority = o.priority || 0;
    const ma = o.maxAttempts !== undefined ? o.maxAttempts : null;
    const n = db.exec(
        "UPDATE _hull_cron SET spec=?, type=?, payload=?, queue=?, priority=?, " +
        "max_attempts=?, next_run_at=?, updated_at=? WHERE name=?",
        [spec, jobType, payload, queue, priority, ma, nxt, now, name]);
    if ((n || 0) === 0) {
        db.exec(
            "INSERT INTO _hull_cron (name, spec, type, payload, queue, priority, " +
            "max_attempts, next_run_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [name, spec, jobType, payload, queue, priority, ma, nxt, now]);
    }
    return jobs;
}

/**
 * Remove a cron schedule by name. Returns whether a schedule was removed.
 * @param {string} name
 * @returns {boolean}
 */
function uncron(name) {
    return (db.exec("DELETE FROM _hull_cron WHERE name=?", [name]) || 0) > 0;
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
    // Fire due cron schedules (throttled; cron is minute-grained).
    if (now - _lastCron >= 1) { processCron(now); _lastCron = now; }
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
// Unix ts of the last cron due-check (throttled to >=1s; cron is minute-grained).
let _lastCron = 0;

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
 * `concurrency` (default 1) runs N independent claim-loops in this one process,
 * so up to N handlers are in flight at once - real intra-process parallelism for
 * I/O-bound handlers (each loop's `work` claims disjoint jobs via the atomic
 * claim). Orthogonal to running K processes; they multiply.
 *
 * Exit conditions: `jobs.stop()` (graceful), or - for bounded / batch-drain
 * runs - `opts.drain` / `opts.maxEmptyPolls`. With neither, it runs until
 * stopped or the process is signalled (an in-flight job is then reclaimed by
 * the visibility-timeout reaper, since handlers are at-least-once).
 * @param {object} [opts]  { queue, batch, concurrency, pollMs,
 *                           visibilityTimeout, drain, maxEmptyPolls }
 * @returns {Promise<number>}  total jobs processed
 */
async function runWorker(opts) {
    const o = opts || {};
    const pollMs = o.pollMs !== undefined ? o.pollMs : 1000;
    const maxEmpty = o.maxEmptyPolls !== undefined ? o.maxEmptyPolls : (o.drain ? 1 : 0);
    const concurrency = (o.concurrency && o.concurrency > 1) ? o.concurrency : 1;
    _running = true;

    // One independent claim-loop; returns its own processed count.
    const loop = async () => {
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
    };

    if (concurrency === 1) return loop();
    // N loops in flight; Promise.all joins them (all exit on stop / drain).
    const counts = await Promise.all(Array.from({ length: concurrency }, loop));
    return counts.reduce((a, b) => a + b, 0);
}

/**
 * Count jobs by status (optionally scoped to a queue). The ops overview.
 * @param {object} [opts]  { queue }
 * @returns {{pending:number, running:number, done:number, dead:number}}
 */
function stats(opts) {
    const o = opts || {};
    const rows = o.queue
        ? db.query("SELECT status, COUNT(*) AS c FROM _hull_jobs WHERE queue=? GROUP BY status", [o.queue])
        : db.query("SELECT status, COUNT(*) AS c FROM _hull_jobs GROUP BY status");
    const s = { pending: 0, running: 0, done: 0, dead: 0 };
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

// Internal seams for tests / introspection; not part of the app-facing contract.
function _cronNext(spec, from) {
    const p = parseCron(spec);
    return p ? cronNext(p, from !== undefined ? from : time.now()) : null;
}
function _tick(now) { processCron(now !== undefined ? now : time.now()); }

export const jobs = {
    init, enqueue, claim, handler, default: setDefault, work, reap, stats,
    runWorker, stop, dead, retry, cancel, cleanup, cron, uncron,
    RETRY, DEAD, DISCARD, _config: _cfg, _cronNext, _tick,
};
export { init, enqueue, claim, handler, work, reap, stats, runWorker, stop,
         dead, retry, cancel, cleanup, cron, uncron };
export default jobs;
