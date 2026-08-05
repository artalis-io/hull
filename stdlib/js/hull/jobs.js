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
 * `hull jobs worker`), and the ops surface (jobs.stats / dead / retry / cancel / cleanup). v1.1 adds durable cron (jobs.cron / uncron) and intra-process concurrency (run_worker concurrency=N), jobs.get(id), jobs.heartbeat for long jobs, fleet-wide rate limiting (jobs.limit), queue pause/resume/purge, and workflows (depends_on + result passing).
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

// In-process lifecycle listeners (jobs.on). Fired synchronously by work() in the
// worker that processed the job, for jobs THAT worker ran. Not fleet-wide - a
// durable cross-process event stream is the LISTEN/NOTIFY epic (#235).
const _listeners = { completed: [], retried: [], dead: [] };

// Whether the server parses SKIP LOCKED (probed in init(); null = not probed).
let _skipLocked = null;
// Per-queue rate limits { [queue]: { rate, per } }, in-memory (re-registered on
// boot); the shared window COUNTER lives in _hull_ratelimit (fleet-wide).
const _limits = Object.create(null);
// Row-lock clause for the rate counter: blocking FOR UPDATE on PG/MySQL, empty
// on SQLite (its write txn already serializes). Set in init().
let _rlLock = "";
// Paused-queue cache: set of paused queue names + when it was last loaded.
// Refreshed at most every 1s so pausing never adds a DB read to every claim.
let _paused = Object.create(null);
let _pausedAt = 0;

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
        "updated_at   INTEGER      NOT NULL," +
        "progress     INTEGER      NOT NULL DEFAULT 0)");

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
        "updated_at   INTEGER      NOT NULL," +
        "tz_offset    INTEGER      NOT NULL DEFAULT 0)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_hull_cron_due ON _hull_cron(next_run_at)");

    // Additive migrations for DBs created before v1.5 (idempotent: a duplicate-
    // column error on an already-migrated DB is expected and swallowed).
    try { db.exec("ALTER TABLE _hull_jobs ADD COLUMN progress INTEGER NOT NULL DEFAULT 0"); } catch (e) { /* exists */ }
    try { db.exec("ALTER TABLE _hull_cron ADD COLUMN tz_offset INTEGER NOT NULL DEFAULT 0"); } catch (e) { /* exists */ }

    // Fleet-wide rate-limit counters (jobs.limit). One row per limited queue;
    // `name` (not the reserved word `key`), a window start, and the count.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_ratelimit (" +
        "name         VARCHAR(255) NOT NULL PRIMARY KEY," +
        "window_start INTEGER      NOT NULL," +
        "n            INTEGER      NOT NULL)");
    _rlLock = db.backendName === "sqlite" ? "" : " FOR UPDATE";

    // Durable per-queue pause state (jobs.pause / resume). A paused queue is not
    // claimed (workers skip it); fleet-wide (in the DB) and restart-durable.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_queue (" +
        "name   VARCHAR(255) NOT NULL PRIMARY KEY," +
        "paused INTEGER      NOT NULL DEFAULT 0)");
    _paused = Object.create(null);
    _pausedAt = 0;

    // Workflow dependency edges (jobs.enqueue dependsOn). A dependent starts
    // 'blocked'; each edge is marked satisfied when its dependency completes, and
    // the dependent unblocks once its last edge is satisfied. The edge survives
    // until the dependent runs, so its deps' results can be injected. failMode
    // 'run' = "run even if this dep failed" (else cascade).
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_deps (" +
        "dependent_id INTEGER NOT NULL," +
        "dep_id       INTEGER NOT NULL," +
        "ord          INTEGER NOT NULL," +
        "fail_mode    VARCHAR(8)," +
        "satisfied    INTEGER NOT NULL DEFAULT 0," +
        "PRIMARY KEY (dependent_id, dep_id))");
    db.exec("CREATE INDEX IF NOT EXISTS idx_hull_job_deps_dep ON _hull_job_deps(dep_id)");

    // A job's result (its handler's return value), for a dependent to consume.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_results (" +
        "job_id INTEGER NOT NULL PRIMARY KEY," +
        "result TEXT)");

    // Verify the server actually parses SKIP LOCKED (the compile-time dialect
    // flag says "this backend supports it" but a MySQL<8 / MariaDB<10.6 server
    // does not). A 0-row probe against the real table detects it once; the claim
    // falls back to plain FOR UPDATE otherwise.
    _skipLocked = db.dialect.supportsSkipLocked;
    if (_skipLocked) {
        try { db.query("SELECT id FROM _hull_jobs WHERE 1=0 FOR UPDATE SKIP LOCKED"); }
        catch (_e) { _skipLocked = false; }
    }

    return jobs;
}

// ── Workflow dependencies (jobs.enqueue dependsOn) ──────────────────────────

// Apply one dependency's terminal outcome to one edge. Idempotent. Returns
// "failed" when it cascade-fails the dependent (so callers recurse).
function resolveEdge(dependentId, depId, depOk, failMode) {
    if (depOk || failMode === "run") {
        const n = db.exec(
            "UPDATE _hull_job_deps SET satisfied=1 WHERE dependent_id=? AND dep_id=? AND satisfied=0",
            [dependentId, depId]);
        if ((n || 0) === 0) return null;   // already satisfied / gone
        const rows = db.query(
            "SELECT COUNT(*) AS c FROM _hull_job_deps WHERE dependent_id=? AND satisfied=0",
            [dependentId]);
        if ((rows[0] ? rows[0].c : 0) === 0) {
            db.exec("UPDATE _hull_jobs SET status='pending', updated_at=? WHERE id=? AND status='blocked'",
                [time.now(), dependentId]);
        }
        return null;
    }
    // cascade-fail: this dependency died and the dependent didn't opt to run.
    const n = db.exec(
        "UPDATE _hull_jobs SET status='dead', last_error=?, updated_at=? WHERE id=? AND status='blocked'",
        [`dependency ${depId} failed`, time.now(), dependentId]);
    if ((n || 0) > 0) {
        db.exec("DELETE FROM _hull_job_deps WHERE dependent_id=?", [dependentId]);
        return "failed";
    }
    return null;
}

// A job reached a terminal state; propagate to its dependents transitively.
function resolveDeps(id, ok) {
    const work = [{ id, ok }];
    let guard = 0;
    while (work.length > 0 && guard < 100000) {
        guard++;
        const cur = work.pop();
        const edges = db.query(
            "SELECT dependent_id, fail_mode FROM _hull_job_deps WHERE dep_id=?", [cur.id]);
        for (const e of edges) {
            if (resolveEdge(e.dependent_id, cur.id, cur.ok, e.fail_mode) === "failed") {
                work.push({ id: e.dependent_id, ok: false });
            }
        }
    }
}

// Gather a dependent's dependency results (declaration order) for job.deps.
// Returns null when the job has no dependencies (a PK-indexed 0-row probe).
function loadDeps(dependentId) {
    const edges = db.query(
        "SELECT dep_id FROM _hull_job_deps WHERE dependent_id=? ORDER BY ord", [dependentId]);
    if (edges.length === 0) return null;
    const out = [];
    edges.forEach((e, i) => {
        const r = db.query("SELECT result FROM _hull_job_results WHERE job_id=?", [e.dep_id]);
        if (r[0] && r[0].result !== undefined && r[0].result !== null) {
            try { out[i] = json.decode(r[0].result); } catch (_e) { out[i] = undefined; }
        } else {
            out[i] = undefined;
        }
    });
    return out;
}

/**
 * Enqueue a job. A plain INSERT, so calling it inside a `db.batch()` commits
 * the job atomically with the business row (transactional coupling).
 *
 * With `opts.dependsOn` (a list of job ids) the job starts `blocked` and only
 * becomes claimable once all those jobs complete; each dependency's result is
 * injected into the handler as `job.deps` (in order). If a dependency
 * dead-letters, the dependent cascade-fails too, unless `opts.onDepFailure === "run"`.
 *
 * @param {string} jobType  handler dispatch key (non-empty)
 * @param {*} [data]        JSON-encodable payload (reaches the handler as job.data)
 * @param {object} [opts]   { queue, priority, delay, runAt, maxAttempts, dedupKey, dependsOn, onDepFailure }
 * @returns {number|null}   the new job id, or null when a dedupKey collapsed it
 */
function enqueue(jobType, data, opts) {
    if (typeof jobType !== "string" || jobType === "")
        throw new Error("jobs.enqueue: type must be a non-empty string");
    const o = opts || {};
    const now = time.now();
    // Schedule: absolute `at` (unix ts) wins, else `runAt`, else now + `delay`.
    const runAt = o.at !== undefined ? o.at
        : (o.runAt !== undefined ? o.runAt : now + (o.delay || 0));
    // Windowed throttle: skip (return null) if a job of this (queue, type) was
    // created within the last `throttle` seconds. Best-effort - keyed by
    // (queue, type), no unique constraint (use dedupKey for exact-once).
    if (o.throttle && o.throttle > 0) {
        const hit = db.query(
            "SELECT 1 AS x FROM _hull_jobs WHERE queue=? AND type=? AND created_at > ? LIMIT 1",
            [o.queue || "default", jobType, now - o.throttle]);
        if (hit.length) return null;
    }
    const deps = o.dependsOn;
    const hasDeps = Array.isArray(deps) && deps.length > 0;
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
    if (hasDeps) { cols.push("status"); vals.push("blocked"); }
    let id;
    if (o.dedupKey !== undefined && o.dedupKey !== null) {
        const n = db.insertIfAbsent("_hull_jobs", ["queue", "dedup_key"], cols, vals);
        if (!(n && n > 0)) return null;   // an un-run job with this (queue, dedupKey) exists
        id = db.lastId();
    } else {
        const ph = cols.map(() => "?");
        db.exec("INSERT INTO _hull_jobs (" + cols.join(", ") +
            ") VALUES (" + ph.join(", ") + ")", vals);
        id = db.lastId();
    }
    if (hasDeps) {
        // Record edges, then re-check each dep: closes the enqueue/complete race
        // (whichever side runs second is a no-op - both idempotent).
        const failMode = o.onDepFailure === "run" ? "run" : null;
        deps.forEach((dep, i) => {
            db.exec("INSERT INTO _hull_job_deps (dependent_id, dep_id, ord, fail_mode) " +
                "VALUES (?, ?, ?, ?)", [id, dep, i + 1, failMode]);
        });
        for (const dep of deps) {
            const g = get(dep);
            if (g === null || g.status === "done") resolveEdge(id, dep, true, failMode);
            else if (g.status === "dead") resolveEdge(id, dep, false, failMode);
        }
    }
    return id;
}

/**
 * Enqueue many jobs in one transaction. `items` is an array of
 * `{ type, data?, opts? }`, each accepting the same `opts` as `jobs.enqueue`
 * EXCEPT `dependsOn` (bulk is for independent jobs - build graphs with
 * `jobs.enqueue`, which throws here if seen). All rows commit together (one
 * `db.batch`), so the batch is atomic AND cheap: a single commit/fsync instead
 * of one per job. Returns an array of ids in input order, with `null` for any
 * item whose `dedupKey` collided with an existing un-run job.
 * @param {Array<{type:string,data?:*,opts?:object}>} items
 * @returns {Array<number|null>}  ids in input order (null per deduped item)
 */
function enqueueMany(items) {
    if (!Array.isArray(items)) throw new Error("jobs.enqueueMany: items must be an array");
    if (items.length === 0) return [];
    items.forEach((it, i) => {
        if (!it || typeof it.type !== "string" || it.type === "")
            throw new Error(`jobs.enqueueMany: item ${i} needs a non-empty string type`);
        if (it.opts && it.opts.dependsOn)
            throw new Error("jobs.enqueueMany: dependsOn is not supported in bulk; use jobs.enqueue for graph nodes");
    });
    const ids = [];
    db.batch(() => {
        for (let i = 0; i < items.length; i++) {
            ids[i] = enqueue(items[i].type, items[i].data, items[i].opts);
        }
    });
    return ids;
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
 * Set (or clear) a fleet-wide rate limit for a queue: at most `rate` jobs are
 * dispatched per `per`-second window across ALL workers (a shared DB counter, so
 * K processes still total `rate`). Register at startup on every worker (the
 * limit lives in code; only the counter is shared).
 * @param {string} queue  the queue to limit
 * @param {object} [opts] { rate, per = 1 } - null/false removes it
 * @returns {object} the jobs module (for chaining)
 */
function limit(queue, opts) {
    if (opts === null || opts === undefined || opts === false) { delete _limits[queue]; return jobs; }
    if (typeof opts.rate !== "number" || opts.rate < 1)
        throw new Error("jobs.limit: opts.rate must be a positive number");
    _limits[queue] = { rate: opts.rate, per: opts.per || 1 };
    return jobs;
}

// Atomically reserve up to `want` slots in the current window for `key`, rolling
// the window if stale. Returns how many were granted (0..want).
function rlReserve(key, want, rate, per) {
    const now = time.now();
    let granted = 0;
    db.insertIfAbsent("_hull_ratelimit", ["name"],
        ["name", "window_start", "n"], [key, now, 0]);
    db.batch(() => {
        const sel = db.query(
            "SELECT window_start, n FROM _hull_ratelimit WHERE name=?" + _rlLock, [key]);
        let ws = sel[0].window_start, cnt = sel[0].n;
        if (now - ws >= per) { ws = now; cnt = 0; }
        granted = Math.min(want, rate - cnt);
        if (granted < 0) granted = 0;
        db.exec("UPDATE _hull_ratelimit SET window_start=?, n=? WHERE name=?",
            [ws, cnt + granted, key]);
    });
    return granted;
}

// Enforce a queue's rate limit on a just-claimed batch: keep the highest-priority
// `granted`, requeue the excess (undoing the claim's attempts+1). Returns kept.
function rlApply(queue, out) {
    const lim = _limits[queue];
    if (!lim || out.length === 0) return out;
    const granted = rlReserve(queue, out.length, lim.rate, lim.per);
    if (granted >= out.length) return out;
    const excess = out.slice(granted);
    const params = [time.now()];
    const ph = excess.map((j) => { params.push(j.id); return "?"; });
    db.exec(
        "UPDATE _hull_jobs SET status='pending', claim_token=NULL, claimed_at=NULL, " +
        "attempts=attempts-1, updated_at=? WHERE id IN (" + ph.join(",") + ")",
        params);
    return out.slice(0, granted);   // keep the top `granted`
}

// Is `queue` paused? Reads the durable state at most once/second (cached), so
// pausing never adds a DB read to the steady claim path.
function isPaused(queue) {
    const now = time.now();
    if (now - _pausedAt >= 1) {
        _paused = Object.create(null);
        for (const r of db.query("SELECT name FROM _hull_queue WHERE paused=1")) _paused[r.name] = true;
        _pausedAt = now;
    }
    return _paused[queue] === true;
}

function setPaused(queue, v) {
    const n = db.exec("UPDATE _hull_queue SET paused=? WHERE name=?", [v, queue]);
    if ((n || 0) === 0) db.exec("INSERT INTO _hull_queue (name, paused) VALUES (?, ?)", [queue, v]);
    _pausedAt = 0;   // invalidate this process's cache so the change is seen now
    return jobs;
}

/**
 * Pause a queue: workers stop claiming from it (in-flight jobs finish; enqueue
 * still works). Durable + fleet-wide. Takes effect within ~1s on other workers.
 * @param {string} queue
 * @returns {object} the jobs module (for chaining)
 */
function pause(queue) { return setPaused(queue, 1); }

/**
 * Resume a paused queue.
 * @param {string} queue
 * @returns {object} the jobs module (for chaining)
 */
function resume(queue) { return setPaused(queue, 0); }

/**
 * Delete jobs from a queue (the "clear the backlog" op). Defaults to `pending`
 * only, leaving in-flight and terminal rows; pass opts.statuses to widen.
 * Returns the number deleted.
 * @param {string} queue
 * @param {object} [opts]  { statuses = ["pending"] }
 * @returns {number}
 */
function purge(queue, opts) {
    const o = opts || {};
    const statuses = o.statuses || ["pending"];
    const params = [queue].concat(statuses);
    const ph = statuses.map(() => "?");
    return db.exec(
        "DELETE FROM _hull_jobs WHERE queue=? AND status IN (" + ph.join(",") + ")",
        params) || 0;
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
// Resolve opts.queue / opts.queues to an ordered list of queues to try. A single
// `queue` (or the default) is a one-element order. `queues` may be a LIST
// ["critical","default","low"] (strict priority - try each in order, claim from
// the first with ready work) or a MAP {critical:3, default:2, low:1} (weighted
// fairness: a weighted-random shuffle with every queue still present as a
// fallback, so no claim cycle is wasted and no queue starves; a homogeneous
// fleet gives fleet-wide fairness). Zero/negative/non-number weights are dropped.
function resolveQueueOrder(opts) {
    const qs = opts.queues;
    if (qs == null) return [opts.queue || "default"];
    if (Array.isArray(qs)) return qs.slice();                 // list (strict priority)
    const names = [], weights = {};                           // map (weighted)
    let total = 0;
    for (const name of Object.keys(qs)) {
        const w = qs[name];
        if (typeof w === "number" && w > 0) { names.push(name); weights[name] = w; total += w; }
    }
    const order = [];
    while (names.length) {
        const pick = Math.random() * total;
        let acc = 0, idx = names.length - 1;
        for (let i = 0; i < names.length; i++) {
            acc += weights[names[i]];
            if (pick <= acc) { idx = i; break; }
        }
        const chosen = names[idx];
        order.push(chosen);
        total -= weights[chosen];
        names.splice(idx, 1);
    }
    return order;
}

// Claim up to `batch` ready jobs from ONE queue (the per-backend atomic claim).
function claimOne(queue, batch) {
    if (isPaused(queue)) return [];   // paused: don't dispatch
    const now = time.now();
    const token = crypto.base64urlEncode(crypto.random(16));
    const d = db.dialect;
    // SKIP LOCKED needs PG 9.5+ / MySQL 8+ / MariaDB 10.6+. init() probes the
    // server and clears _skipLocked on older ones, where we fall back to plain
    // FOR UPDATE (correct - one job, one worker - but claimants block instead of
    // skipping). null = not probed yet -> assume supported.
    const lock = _skipLocked === false ? "FOR UPDATE" : "FOR UPDATE SKIP LOCKED";

    let rows;
    if (d.supportsSkipLocked && d.supportsReturning) {
        rows = db.query(
            "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, " +
            "attempts=attempts+1, updated_at=? WHERE id IN (" +
            "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? " +
            "ORDER BY priority DESC, id LIMIT ? " + lock + ") " +
            "RETURNING id, type, payload, priority, attempts, max_attempts",
            [token, now, now, queue, now, batch]);
    } else if (d.supportsSkipLocked) {
        db.batch(() => {
            const sel = db.query(
                "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? " +
                "ORDER BY priority DESC, id LIMIT ? " + lock,
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
    const out = (rows || [])
        .sort((x, y) => (y.priority || 0) - (x.priority || 0) || (x.id - y.id))
        .map((r) => {
            const j = shape(r);
            j.claimToken = token;   // handle for jobs.heartbeat on long-running work
            return j;
        });
    // Enforce a fleet-wide rate limit (claim-then-reconcile; requeues the excess).
    return rlApply(queue, out);
}

/**
 * Atomically claim up to `batch` ready jobs, marking them `running`.
 * Concurrency-safe (SKIP LOCKED on Postgres/MySQL, serialized on SQLite). Draws
 * from a single queue (`opts.queue`, default "default") or across several via
 * `opts.queues` - a LIST for strict priority or a MAP for weighted fairness (see
 * resolveQueueOrder). Multi-queue claims from the first queue in the resolved
 * order with ready work; a worker loop drains the rest on later calls.
 * @param {object} [opts]  { queue | queues, batch }
 * @returns {object[]}
 */
function claim(opts) {
    const o = opts || {};
    const batch = o.batch || 10;
    for (const q of resolveQueueOrder(o)) {
        const got = claimOne(q, batch);
        if (got.length) return got;
    }
    return [];
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

/**
 * Register an in-process lifecycle listener. `event` is one of "completed",
 * "retried" (a failed attempt that will be retried with backoff), or "dead"
 * (dead-lettered: retries exhausted, handler returned jobs.DEAD, or no handler).
 * The listener is called `fn(job, info)` synchronously by work(), in the worker
 * that processed the job, right after the outcome is applied:
 *   - completed -> info = { result }   (the handler's return value, or undefined)
 *   - retried   -> info = { error, attempt }   (attempt = the one that just failed)
 *   - dead      -> info = { error }
 * Multiple listeners per event fire in registration order; each is isolated (a
 * throwing listener can't derail the loop or the others). Keep hooks fast and
 * synchronous - they run inline on the event-loop thread and their return
 * (incl. a promise) is not awaited. Per-worker, for observability/metrics/side-
 * effects; a durable fleet-wide event stream is the LISTEN/NOTIFY epic (#235).
 * Workflow cascade-failures don't emit here (the dependency's own `dead` fires).
 * @param {string} event  "completed" | "retried" | "dead"
 * @param {Function} fn
 * @returns {object} the jobs module (for chaining)
 */
function on(event, fn) {
    if (!_listeners[event])
        throw new Error(`jobs.on: unknown event '${event}' (completed|retried|dead)`);
    if (typeof fn !== "function") throw new Error("jobs.on: listener must be a function");
    _listeners[event].push(fn);
    return jobs;
}

function markDone(id, result) {
    db.exec("UPDATE _hull_jobs SET status='done', claim_token=NULL, updated_at=? WHERE id=?",
        [time.now(), id]);
    if (result !== undefined) {   // persist the handler's return value for dependents
        const enc = json.encode(result);
        const n = db.exec("UPDATE _hull_job_results SET result=? WHERE job_id=?", [enc, id]);
        if ((n || 0) === 0) db.exec("INSERT INTO _hull_job_results (job_id, result) VALUES (?, ?)", [id, enc]);
    }
    resolveDeps(id, true);                                   // unblock dependents
    db.exec("DELETE FROM _hull_job_deps WHERE dependent_id=?", [id]);   // consumed its own deps
}

function markDead(id, err) {
    db.exec("UPDATE _hull_jobs SET status='dead', last_error=?, claim_token=NULL, updated_at=? WHERE id=?",
        [err, time.now(), id]);
    resolveDeps(id, false);                                  // cascade to dependents
    db.exec("DELETE FROM _hull_job_deps WHERE dependent_id=?", [id]);
}

// Reschedule with backoff, or dead-letter once attempts are exhausted. attempts
// was already incremented by the claim (the attempt that just ran).
// Returns the disposition it applied: "dead" (retries exhausted -> dead-letter)
// or "retried" (rescheduled with backoff), so work() can emit the right event.
function markRetry(job, err) {
    const attempts = job.attempts || 0;
    const max = job.maxAttempts !== undefined && job.maxAttempts !== null
        ? job.maxAttempts : _cfg.maxAttempts;
    if (attempts >= max) { markDead(job.id, err); return "dead"; }
    const now = time.now();
    db.exec(
        "UPDATE _hull_jobs SET status='pending', run_at=?, last_error=?, claim_token=NULL, " +
        "updated_at=? WHERE id=?",
        [now + _cfg.backoff(attempts), err, now, job.id]);
    return "retried";
}

// Fire in-process listeners for a lifecycle event. Each listener is isolated: a
// throwing hook can't derail the work loop or the other listeners. Listener
// return values (incl. promises) are ignored - hooks are fire-and-forget.
function emit(event, job, info) {
    const L = _listeners[event];
    for (let i = 0; i < L.length; i++) {
        try { L[i](job, info); } catch (e) { /* isolated: hooks must not break work */ }
    }
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
// Resolve a cron `tz` option to a FIXED offset in seconds east of UTC. Accepts a
// number (minutes east of UTC) or a string "+HH:MM" / "-HH:MM" / "+HHMM" / "Z".
// IANA names ("Europe/Budapest") are rejected: a DST-correct named zone needs a
// tz database Hull can't read inside the sandbox. Fixed-offset only - no DST.
function parseTzOffset(tz) {
    if (tz == null) return 0;
    if (typeof tz === "number") return Math.floor(tz * 60);
    if (typeof tz === "string") {
        if (tz === "Z" || tz === "UTC" || tz === "utc") return 0;
        const m = tz.match(/^([+-])(\d\d):?(\d\d)$/);
        if (m) {
            const off = Number(m[2]) * 3600 + Number(m[3]) * 60;
            return m[1] === "-" ? -off : off;
        }
        throw new Error(`jobs.cron: tz must be a fixed offset ("+02:00", "-0530", or minutes ` +
            `east of UTC); IANA zone names like '${tz}' need a tz database unavailable in the sandbox`);
    }
    throw new Error("jobs.cron: tz must be a string offset or a number of minutes");
}

// Next matching instant at or after fromTs, as a UTC unix timestamp. `offset`
// (seconds east of UTC, default 0) shifts only the calendar decode, so the spec
// matches wall-clock fields in that fixed-offset zone while the result stays UTC.
function cronNext(c, fromTs, offset) {
    const off = offset || 0;
    let t = Math.floor(fromTs / 60) * 60 + 60;
    for (let i = 0; i < 200000; i++) {
        const { month, day, hour, minute, dow } = decodeTs(t + off);
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
        "SELECT name, spec, type, payload, queue, priority, max_attempts, next_run_at, tz_offset " +
        "FROM _hull_cron WHERE next_run_at <= ?", [now]);
    for (const c of due) {
        const parsed = parseCron(c.spec);
        const nxt = parsed ? cronNext(parsed, now, c.tz_offset) : (now + 60);
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
    const tzOffset = parseTzOffset(o.tz);
    const nxt = cronNext(parsed, now, tzOffset);
    if (nxt === null) throw new Error("jobs.cron: spec has no upcoming occurrence");
    const jobType = o.type || name;
    const payload = data !== undefined && data !== null ? json.encode(data) : null;
    const queue = o.queue || "default";
    const priority = o.priority || 0;
    const ma = o.maxAttempts !== undefined ? o.maxAttempts : null;
    const n = db.exec(
        "UPDATE _hull_cron SET spec=?, type=?, payload=?, queue=?, priority=?, " +
        "max_attempts=?, next_run_at=?, tz_offset=?, updated_at=? WHERE name=?",
        [spec, jobType, payload, queue, priority, ma, nxt, tzOffset, now, name]);
    if ((n || 0) === 0) {
        db.exec(
            "INSERT INTO _hull_cron (name, spec, type, payload, queue, priority, " +
            "max_attempts, next_run_at, tz_offset, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [name, spec, jobType, payload, queue, priority, ma, nxt, tzOffset, now]);
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
        job.deps = loadDeps(job.id);   // workflow: dependency results, in order
        const h = _handlers[job.type] || _default;
        if (!h) {
            const err = `no handler for job type '${job.type}'`;
            markDead(job.id, err);
            emit("dead", job, { error: err });
            continue;
        }
        try {
            const result = await h(job);
            if (result === DEAD) {
                markDead(job.id, "handler returned jobs.DEAD");
                emit("dead", job, { error: "handler returned jobs.DEAD" });
            } else if (result === RETRY) {
                emit(markRetry(job, "handler requested retry"), job,
                     { error: "handler requested retry", attempt: job.attempts });
            } else {
                // undefined / true / DISCARD -> done, no result; any other return
                // value is stored as the job's result (for dependents).
                const res = (result !== undefined && result !== true && result !== DISCARD)
                    ? result : undefined;
                markDone(job.id, res);
                emit("completed", job, { result: res });
            }
        } catch (e) {
            const err = String((e && e.message) || e);
            emit(markRetry(job, err), job, { error: err, attempt: job.attempts });
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
    const s = { pending: 0, running: 0, done: 0, dead: 0, blocked: 0 };
    for (const r of rows) s[r.status] = r.c;
    return s;
}

// Decode an ops row into an inspection view: the handler-facing shape plus the
// bookkeeping columns an operator needs (status, queue, lastError, timestamps).
function shapeOps(r) {
    const j = shape(r);
    j.status = r.status;
    j.queue = r.queue;
    j.priority = r.priority;
    j.attempts = r.attempts;
    j.runAt = r.run_at;
    j.lastError = r.last_error;
    j.createdAt = r.created_at;
    j.updatedAt = r.updated_at;
    j.progress = r.progress;
    return j;
}

/**
 * Set a running job's progress percent (0-100), surfaced by
 * `jobs.get(id).progress`. Advisory - a UI/ops hint for long jobs, orthogonal to
 * the claim/heartbeat. Clamped to [0,100] and rounded. Call from a handler with
 * `job.id`. Returns the clamped value stored.
 * @param {number} id
 * @param {number} pct  0..100
 * @returns {number}  the clamped value written
 */
function progress(id, pct) {
    let p = Number(pct) || 0;
    if (p < 0) p = 0; else if (p > 100) p = 100;
    p = Math.round(p);
    db.exec("UPDATE _hull_jobs SET progress=?, updated_at=? WHERE id=?", [p, time.now(), id]);
    return p;
}

/**
 * Fetch a single job by id (its full status view), or null if it doesn't exist.
 * The only way to inspect an individual job from app code: `_hull_jobs` is in the
 * protected namespace, so a direct query is blocked. Poll this for a terminal
 * status, or use `jobs.result` / `jobs.await` to fetch a finished job's return
 * value (a handler's non-null return is persisted as the job's result).
 * @param {number} id
 * @returns {object|null}
 */
function get(id) {
    const rows = db.query("SELECT * FROM _hull_jobs WHERE id=?", [id]);
    return rows.length ? shapeOps(rows[0]) : null;
}

/**
 * Fetch a job's terminal result without the full status view. Returns null when
 * the job is unknown (never enqueued, or already purged by `jobs.cleanup`);
 * otherwise `{ status, result?, error? }`:
 *   - a `done` job carries `result` = the handler's decoded return value (absent
 *     when the handler returned null/undefined/true/jobs.DISCARD),
 *   - a `dead` job carries `error` = its last error string,
 *   - a still-running (`pending`/`running`/`blocked`) job carries neither.
 * The standalone counterpart to a workflow dependent's injected
 * `job.deps[i].result`: any job's return value is readable here, not only a
 * dependency's. The result lives as long as the job row (governed by
 * `jobs.cleanup`), so read it before the terminal row is purged.
 * @param {number} id
 * @returns {object|null}
 */
function result(id) {
    const rows = db.query("SELECT status, last_error FROM _hull_jobs WHERE id=?", [id]);
    if (!rows.length) return null;
    const status = rows[0].status;
    const out = { status };
    if (status === "done") {
        const rr = db.query("SELECT result FROM _hull_job_results WHERE job_id=?", [id]);
        if (rr.length && rr[0].result != null) out.result = JSON.parse(rr[0].result);
    } else if (status === "dead") {
        out.error = rows[0].last_error;
    }
    return out;
}

/**
 * Block (yielding to the event loop) until a job reaches a terminal status, then
 * return its `result`. For the "enqueue then await the value" pattern; it polls
 * `result` every `opts.interval` ms (default 100), `await hull.sleep`-ing between
 * polls so other work keeps running. With `opts.timeout` (ms) it gives up after
 * that budget and returns the last (non-terminal) result view instead of the
 * terminal one; without a timeout it waits indefinitely. Returns null immediately
 * if the job is unknown. Async: `await jobs.await(id)`.
 * @param {number} id
 * @param {object} [opts]  { timeout: <ms>, interval: <ms> }
 * @returns {Promise<object|null>}
 */
async function await_(id, opts) {
    opts = opts || {};
    const interval = opts.interval || 100;
    const deadline = opts.timeout != null ? time.clock() + opts.timeout : null;
    for (;;) {
        const r = result(id);
        if (r === null) return null;                            // unknown / purged
        if (r.status === "done" || r.status === "dead") return r;
        if (deadline !== null && time.clock() >= deadline) return r;
        await hull.sleep(interval);
    }
}

/**
 * Extend the claim on a job the current handler is processing (heartbeat). A
 * handler whose work may exceed `visibilityTimeout` should call this
 * periodically (at least every visibilityTimeout/2 s) so the reaper doesn't
 * presume it orphaned and re-run it elsewhere. Bumps `claimed_at` only while
 * THIS worker still owns the claim (guarded by the job's claimToken): returns
 * false once the claim has been lost (already reaped / re-claimed / finished),
 * which is the handler's signal to stop and let the other runner win.
 * @param {object} job  the job object passed to the handler
 * @returns {boolean}  true if the claim was extended, false if no longer held
 */
function heartbeat(job) {
    if (!job || job.id === undefined || job.claimToken === undefined) return false;
    const now = time.now();
    const n = db.exec(
        "UPDATE _hull_jobs SET claimed_at=?, updated_at=? " +
        "WHERE id=? AND claim_token=? AND status='running'",
        [now, now, job.id, job.claimToken]);
    return (n || 0) > 0;
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
    const deleted = db.exec(sql, params) || 0;
    // Drop results whose producing job is gone (workflow result store hygiene).
    db.exec("DELETE FROM _hull_job_results WHERE job_id NOT IN (SELECT id FROM _hull_jobs)");
    return deleted;
}

// Internal seams for tests / introspection; not part of the app-facing contract.
function _cronNext(spec, from, offset) {
    const p = parseCron(spec);
    return p ? cronNext(p, from !== undefined ? from : time.now(), offset) : null;
}
function _tick(now) { processCron(now !== undefined ? now : time.now()); }

export const jobs = {
    init, enqueue, enqueueMany, claim, handler, default: setDefault, on, work, reap,
    stats, runWorker, stop, get, result, await: await_, progress, heartbeat, limit,
    pause, resume, purge,
    dead, retry, cancel, cleanup, cron, uncron,
    RETRY, DEAD, DISCARD, _config: _cfg, _cronNext, _tick,
};
export { init, enqueue, enqueueMany, claim, handler, on, work, reap, stats,
         runWorker, stop, get, result, progress, heartbeat, limit, pause, resume,
         purge, dead, retry, cancel, cleanup, cron, uncron };
export default jobs;
