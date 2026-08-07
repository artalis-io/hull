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
 * `hull jobs worker`), and the ops surface (jobs.stats / dead / retry / cancel / cleanup). v1.1 adds durable cron (jobs.cron / uncron) and intra-process concurrency (run_worker concurrency=N), jobs.get(id), jobs.heartbeat for long jobs, fleet-wide rate limiting (jobs.limit), queue pause/resume/purge, and workflows (depends_on + result passing). v1.5 adds the standalone result backend (jobs.result / jobs.await), multi-queue draining (opts.queues, strict list or weighted map), bulk enqueue (jobs.enqueueMany), in-process lifecycle hooks (jobs.on completed/retried/dead), and polish (absolute at, jobs.progress, throttle window, fixed-offset tz cron). Durable workflows (jobs.workflow / jobs.start / ctx.step / ctx.sleep / ctx.wait_signal / jobs.signal, Phase 1) add crash-safe workflow-as-code: step memoization, durable timers, external signals, and saga compensation, plus deterministic-replay primitives (ctx.now / ctx.random / ctx.uuid, memoized) - a workflow instance is a job that rides the same claim/reaper/retry/result machinery (docs/jobs_durable_execution_design.md). Observability: jobs.metrics() (DB-derived per-queue gauges + backlog age) and W3C trace-context propagation (enqueue { trace } -> job.trace). Opt-in attempt history (jobs.init { history=true }) records a per-attempt timeline (jobs.history) and adds latency percentiles + throughput to jobs.metrics.
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
    history:           false, // attempt-history recording: true | { queues:[...] } | false
    historyRetention:  null,  // seconds to keep attempt rows (null = jobs.cleanup default)
    events:            false, // durable fleet-wide event log (jobs.subscribe / jobs.events)
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

// Registered durable workflows: name -> fn(ctx). jobs.workflow registers a
// reserved-type handler that runs the workflow through a memoizing ctx.
const _workflows = Object.create(null);

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
    if (o.history !== undefined) _cfg.history = o.history;
    if (o.historyRetention !== undefined) _cfg.historyRetention = o.historyRetention;
    if (o.events !== undefined) _cfg.events = o.events;
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
        "progress     INTEGER      NOT NULL DEFAULT 0," +
        "trace_context VARCHAR(255)," +
        // Per-key concurrency (jobs.enqueue concurrencyKey/concurrency): at most
        // `concurrency_limit` jobs sharing `concurrency_key` run at once. `strict`
        // (1) opts into the counter-backed hard cap; NULL/0 is the soft default.
        "concurrency_key    VARCHAR(255)," +
        "concurrency_limit  INTEGER," +
        "concurrency_strict INTEGER)");

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

    // Throttle scan path: recent jobs of a (queue, type) within the window
    // (enqueue opts.throttle). Without this the throttle probe scans the queue.
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_jobs_throttle " +
        "ON _hull_jobs(queue, type, created_at)");

    // Concurrency reconcile path: running jobs of a key, ranked by claim age.
    // The claim-then-reconcile rank probe (concApply) scans this.
    db.exec(
        "CREATE INDEX IF NOT EXISTS idx_hull_jobs_conc " +
        "ON _hull_jobs(concurrency_key, status, claimed_at, id)");

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

    // Additive migrations for DBs created before v1.5: add the column only when
    // absent (checked via the portable db.tableColumns, mirroring session.js),
    // rather than catching a duplicate-column error - a caught ALTER would abort
    // a surrounding transaction on Postgres if jobs.init ran inside a db.batch.
    const ensureColumn = (tbl, col, coldef) => {
        if (!(db.tableColumns(tbl) || []).includes(col))
            db.exec(`ALTER TABLE ${tbl} ADD COLUMN ${coldef}`);
    };
    ensureColumn("_hull_jobs", "progress", "progress INTEGER NOT NULL DEFAULT 0");
    ensureColumn("_hull_jobs", "trace_context", "trace_context VARCHAR(255)");
    ensureColumn("_hull_jobs", "concurrency_key", "concurrency_key VARCHAR(255)");
    ensureColumn("_hull_jobs", "concurrency_limit", "concurrency_limit INTEGER");
    ensureColumn("_hull_jobs", "concurrency_strict", "concurrency_strict INTEGER");
    ensureColumn("_hull_cron", "tz_offset", "tz_offset INTEGER NOT NULL DEFAULT 0");

    // Fleet-wide rate-limit counters (jobs.limit). One row per limited queue;
    // `name` (not the reserved word `key`), a window start, and the count.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_ratelimit (" +
        "name         VARCHAR(255) NOT NULL PRIMARY KEY," +
        "window_start INTEGER      NOT NULL," +
        "n            INTEGER      NOT NULL)");
    _rlLock = db.backendName === "sqlite" ? "" : " FOR UPDATE";

    // Strict per-key concurrency counter (jobs.enqueue concurrencyStrict). One
    // row per strict key: `n` running slots reserved. A claim atomically reserves
    // (n < limit -> n+1), work() releases on any exit from running, and the reaper
    // reconciles `n` to the true running count so a crashed worker can't leak a
    // slot. `name` (not the reserved word `key`) mirrors _hull_ratelimit.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_concurrency (" +
        "name VARCHAR(255) NOT NULL PRIMARY KEY," +
        "n    INTEGER      NOT NULL DEFAULT 0)");

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

    // Durable-execution step memo (jobs.workflow / ctx.step). One row per
    // completed step of a workflow instance; a re-run of the workflow (crash or
    // retry) reads these to skip already-done steps. Composite PK = idempotent.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_workflow_steps (" +
        "workflow_id INTEGER      NOT NULL," +
        "step_key    VARCHAR(255) NOT NULL," +
        "result      TEXT," +
        "status      VARCHAR(16)  NOT NULL DEFAULT 'done'," +
        "created_at  INTEGER      NOT NULL," +
        "PRIMARY KEY (workflow_id, step_key))");

    // Durable-execution signals (jobs.signal / ctx.waitSignal). One row per
    // (workflow, signal name); the payload is consumed once. Composite PK makes a
    // duplicate delivery a no-op (first wins).
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_workflow_signals (" +
        "workflow_id INTEGER      NOT NULL," +
        "name        VARCHAR(255) NOT NULL," +
        "payload     TEXT," +
        "created_at  INTEGER      NOT NULL," +
        "consumed_at INTEGER," +
        "PRIMARY KEY (workflow_id, name))");

    // Opt-in attempt history (jobs.init { history:true }). One row per attempt,
    // written by jobs.work at each outcome when history is enabled for the queue.
    // The durable timeline behind jobs.history + the latency/throughput metrics.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_attempts (" +
        "attempt_id  " + db.autoincrementIdDdl + ", " +
        "job_id      INTEGER      NOT NULL," +
        "queue       VARCHAR(255) NOT NULL," +
        "type        VARCHAR(255) NOT NULL," +
        "attempt_no  INTEGER      NOT NULL," +
        "wait_ms     BIGINT," +
        "started_ms  BIGINT       NOT NULL," +
        "finished_ms BIGINT       NOT NULL," +
        "duration_ms BIGINT       NOT NULL," +
        "outcome     VARCHAR(16)  NOT NULL," +
        "error       TEXT," +
        "trace_id    VARCHAR(255))");
    db.exec("CREATE INDEX IF NOT EXISTS idx_hull_job_attempts_job ON _hull_job_attempts(job_id, attempt_no)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_hull_job_attempts_recent ON _hull_job_attempts(finished_ms)");

    // Durable fleet-wide event log (jobs.init{events:true}). Append-only lifecycle
    // events, written in the SAME transaction as the state change that produced
    // them (transactional coupling: an event exists iff the transition committed).
    // The `id` is the total order + the future subscription cursor space. `data`
    // is small JSON metadata (error/attempt/trace), never the full result.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_events (" +
        "id       " + db.autoincrementIdDdl + ", " +
        "ts       INTEGER      NOT NULL," +
        "type     VARCHAR(32)  NOT NULL," +
        "job_id   INTEGER," +
        "job_type VARCHAR(255)," +
        "queue    VARCHAR(255)," +
        "data     TEXT)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_hull_job_events_type ON _hull_job_events(type, id)");
    db.exec("CREATE INDEX IF NOT EXISTS idx_hull_job_events_ts ON _hull_job_events(ts)");

    // Durable event subscriptions (jobs.subscribe). One row per named consumer:
    // `cursor` = last delivered event id, `types` = optional comma-list filter,
    // `lease_token`/`lease_until` = the drain lease (one worker drains at a time;
    // a crashed drainer's lease expires and another resumes from the cursor).
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_subscriptions (" +
        "name        VARCHAR(255) NOT NULL PRIMARY KEY," +
        "cursor_id   INTEGER      NOT NULL DEFAULT 0," +
        "types       VARCHAR(255)," +
        "lease_token VARCHAR(255)," +
        "lease_until INTEGER," +
        "failures    INTEGER      NOT NULL DEFAULT 0," +
        "max_failures INTEGER," +   // skip a poison event after N failures (NULL = never)
        "updated_at  INTEGER      NOT NULL)");

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

// Append a durable lifecycle event (opt-in via jobs.init{events:true}). Called
// INSIDE the transition's transaction, so the event commits iff the state change
// does (transactional coupling: no lost, no phantom events). `job` supplies
// id/type/queue/trace; `info` is small metadata (error/attempt) - never the full
// result (a consumer joins _hull_job_results for that).
// Latency-only wakeup: nudge idle workers / subscription drainers parked on
// conn.waitNotify so a freshly appended event or enqueued job wakes them
// immediately instead of at the next poll. Postgres LISTEN/NOTIFY; a no-op on
// backends without it (SQLite/MySQL keep polling). The channel is the fixed
// literal every emitter and waiter shares; the payload is unused. Issued on the
// same connection as the preceding INSERT, so inside a caller's db.batch it
// rides the same transaction and PG delivers (and coalesces) it on commit.
function wakeWorkers() {
    if (db.dialect.supportsNotify) {
        db.exec("SELECT pg_notify(?, ?)", ["hull_jobs", ""]);
    }
}

function emitDurable(event, job, info) {
    if (!_cfg.events) return;
    info = info || {};
    let data = null;
    if (info.error !== undefined || info.attempt !== undefined ||
        (job.trace !== undefined && job.trace !== null)) {
        data = json.encode({ error: info.error, attempt: info.attempt, trace: job.trace });
    }
    db.exec(
        "INSERT INTO _hull_job_events (ts, type, job_id, job_type, queue, data) " +
        "VALUES (?, ?, ?, ?, ?, ?)",
        [time.now(), event, job.id, job.type, job.queue, data]);
    wakeWorkers();   // wake drainers on the new event (no-op off Postgres)
}

// Apply a terminal transition and its durable event ATOMICALLY (one txn), then
// fire the in-process listeners. `transition()` runs the markX state change and
// returns the effective outcome ("done"|"dead"|"retried"). Wrapping in db.batch
// both couples the event to the state change and collapses markDone's several
// statements into one commit (fewer fsyncs, even when events are off).
const EVENT_OF = { done: "completed", dead: "dead", retried: "retried" };
function finish(job, info, transition) {
    let outcome;
    db.batch(() => {
        outcome = transition();
        emitDurable(EVENT_OF[outcome] || outcome, job, info);
    });
    emit(EVENT_OF[outcome] || outcome, job, info);
    return outcome;
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
 * @param {object} [opts]   { queue, priority, delay, runAt, maxAttempts, dedupKey, dependsOn, onDepFailure, concurrencyKey, concurrency }
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
    // Optional W3C trace context, carried through to the handler as job.trace.
    if (o.trace !== undefined && o.trace !== null) { cols.push("trace_context"); vals.push(o.trace); }
    // Optional per-key concurrency: at most `concurrency` jobs sharing
    // `concurrencyKey` run at once. Soft (default) reconciles at claim (may
    // briefly overshoot); `concurrencyStrict: true` opts into the counter-backed
    // hard cap. All jobs of a key should agree on the strict flag.
    if (o.concurrencyKey !== undefined && o.concurrencyKey !== null &&
        o.concurrency && o.concurrency > 0) {
        cols.push("concurrency_key");   vals.push(o.concurrencyKey);
        cols.push("concurrency_limit"); vals.push(o.concurrency);
        if (o.concurrencyStrict) { cols.push("concurrency_strict"); vals.push(1); }
    }
    let id;
    if (o.dedupKey !== undefined && o.dedupKey !== null) {
        const n = db.insertIfAbsent("_hull_jobs", ["queue", "dedup_key"], cols, vals);
        if (!(n && n > 0)) return null;   // an un-run job with this (queue, dedupKey) exists
        // Portable id fetch: the unique (queue, dedup_key) identifies the new row.
        // (db.lastId is unsupported on Postgres, which has no last-insert-rowid.)
        const rows = db.query("SELECT id FROM _hull_jobs WHERE queue=? AND dedup_key=?",
            [o.queue || "default", o.dedupKey]);
        id = rows.length ? rows[0].id : null;
    } else {
        const ph = cols.map(() => "?");
        const sql = "INSERT INTO _hull_jobs (" + cols.join(", ") + ") VALUES (" + ph.join(", ") + ")";
        if (db.dialect.supportsReturning) {
            // SQLite + Postgres: read the id back from the INSERT itself.
            const rows = db.query(sql + " RETURNING id", vals);
            id = rows.length ? rows[0].id : null;
        } else {
            db.exec(sql, vals);   // MySQL: LAST_INSERT_ID via db.lastId
            id = db.lastId();
        }
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
    // Durable "enqueued" event (opt-in). Emitted here so it joins the caller's
    // db.batch when enqueue is called inside one (transactional coupling).
    emitDurable("enqueued",
        { id, type: jobType, queue: o.queue || "default", trace: o.trace }, {});
    // Low-latency job pickup: wake an idle worker on the new job. When events
    // are on, emitDurable already issued the NOTIFY (coalesced within the
    // transaction), so only nudge directly when it did not.
    if (!_cfg.events) wakeWorkers();
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
             attempts: row.attempts, maxAttempts: row.max_attempts,
             trace: row.trace_context,   // trace-context propagation (null if unset)
             queue: row.queue, createdAt: row.created_at };   // for attempt history
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

// Atomically reserve one STRICT-concurrency slot for `key`: bump n iff n < limit.
// Returns true when reserved. Mirrors rlReserve - blocking FOR UPDATE serializes
// reservers on PG/MySQL, SQLite's write txn serializes - so the hard cap holds
// with no overshoot even under concurrent claimers.
function concReserve(key, limit) {
    db.insertIfAbsent("_hull_job_concurrency", ["name"], ["name", "n"], [key, 0]);
    let ok = false;
    db.batch(() => {
        const sel = db.query("SELECT n FROM _hull_job_concurrency WHERE name=?" + _rlLock, [key]);
        const n = (sel[0] && sel[0].n) || 0;
        if (n < limit) {
            ok = true;
            db.exec("UPDATE _hull_job_concurrency SET n=n+1 WHERE name=?", [key]);
        }
    });
    return ok;
}

// Release one strict-concurrency slot (floored at 0). Called when a strict-keyed
// job leaves 'running' (any work() outcome). The reaper reconciles the counter to
// the true running count, so a slot lost to a crashed worker self-heals.
function concRelease(key) {
    db.exec("UPDATE _hull_job_concurrency SET n = CASE WHEN n > 0 THEN n - 1 ELSE 0 END " +
        "WHERE name=?", [key]);
}

// Per-key concurrency reconcile (claim-then-reconcile, mirroring rlApply). Soft
// (default): a job with `concurrency_limit` running peers ahead is released to
// pending (attempt undone); correct under load, may briefly overshoot under
// same-instant races. Strict (`concurrencyStrict`): an atomic counter reserve -
// a hard cap with no overshoot; a failed reserve releases the claim. `now` is the
// claim time (== claimed_at of every job in `out`).
function concApply(out, now) {
    if (out.length === 0) return out;
    const drop = new Set();
    for (const j of out) {
        const lim = j._concLimit;
        if (j._concKey !== undefined && j._concKey !== null && lim && lim > 0) {
            if (j._concStrict === 1) {
                // Strict: atomic counter reserve; a full key releases the claim.
                if (!concReserve(j._concKey, lim)) drop.add(j.id);
            } else {
                // Soft: rank = running peers strictly ahead in (claimed_at, id)
                // order. Fleet-wide deterministic, so no two workers evict the
                // same slot: each job keeps iff its rank is under the limit.
                const r = db.query(
                    "SELECT COUNT(*) AS n FROM _hull_jobs " +
                    "WHERE concurrency_key=? AND status='running' " +
                    "AND (claimed_at < ? OR (claimed_at = ? AND id < ?))",
                    [j._concKey, now, now, j.id]);
                const rank = (r[0] && r[0].n) || 0;
                if (rank >= lim) drop.add(j.id);
            }
        }
    }
    if (drop.size === 0) return out;
    const params = [now];
    const ph = [];
    for (const id of drop) { ph.push("?"); params.push(id); }
    db.exec(
        "UPDATE _hull_jobs SET status='pending', claim_token=NULL, claimed_at=NULL, " +
        "attempts=attempts-1, updated_at=? WHERE id IN (" + ph.join(",") + ")",
        params);
    return out.filter((j) => !drop.has(j.id));
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
            "RETURNING id, queue, type, payload, priority, attempts, max_attempts, trace_context, created_at, concurrency_key, concurrency_limit, concurrency_strict",
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
            "SELECT id, queue, type, payload, priority, attempts, max_attempts, trace_context, created_at, concurrency_key, concurrency_limit, concurrency_strict FROM _hull_jobs WHERE claim_token=?",
            [token]);
    } else {
        rows = db.query(
            "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, " +
            "attempts=attempts+1, updated_at=? WHERE id IN (" +
            "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? " +
            "ORDER BY priority DESC, id LIMIT ?) " +
            "RETURNING id, queue, type, payload, priority, attempts, max_attempts, trace_context, created_at, concurrency_key, concurrency_limit, concurrency_strict",
            [token, now, now, queue, now, batch]);
    }

    // Priority-correct order within the batch: RETURNING / the token read-back
    // don't preserve the subquery's ORDER BY, so sort by priority DESC, id ASC.
    const out = (rows || [])
        .sort((x, y) => (y.priority || 0) - (x.priority || 0) || (x.id - y.id))
        .map((r) => {
            const j = shape(r);
            j.claimToken = token;   // handle for jobs.heartbeat on long-running work
            j._concKey    = r.concurrency_key;     // internal: concApply reconcile
            j._concLimit  = r.concurrency_limit;
            j._concStrict = r.concurrency_strict;  // 1 = counter-backed hard cap
            return j;
        });
    // Reconcile the claim: fleet-wide rate limit, then soft per-key concurrency.
    // Both requeue the excess (claim-then-reconcile, mirroring each other).
    return concApply(rlApply(queue, out), now);
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
    // Wake durable-workflow signal waits whose timeout (run_at > 0) has passed, so
    // they re-run and return null from ctx.waitSignal. run_at = 0 means "no
    // timeout" (wait forever) and is left alone - only jobs.signal wakes it.
    db.exec(
        "UPDATE _hull_jobs SET status='pending', updated_at=? " +
        "WHERE status='waiting' AND run_at > 0 AND run_at <= ?",
        [now, now]);
    const reclaimed = db.exec(
        "UPDATE _hull_jobs SET status='pending', claim_token=NULL, updated_at=? " +
        "WHERE status='running' AND claimed_at <= ?",
        [now, now - vt]) || 0;
    // Reconcile strict-concurrency counters to the true running count. This frees
    // a slot leaked by a crashed worker (its job was just reclaimed) and returns a
    // parked workflow's slot - the self-healing backstop for concReserve/release.
    // The subquery reads _hull_jobs (a different table), so it is valid on MySQL.
    db.exec(
        "UPDATE _hull_job_concurrency SET n = (" +
        "SELECT COUNT(*) FROM _hull_jobs j " +
        "WHERE j.concurrency_key = _hull_job_concurrency.name " +
        "AND j.concurrency_strict = 1 AND j.status = 'running')");
    return reclaimed;
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

// Is attempt-history recording on for this queue? true = all queues; an object
// with a `queues` list = only those.
function historyEnabled(queue) {
    const h = _cfg.history;
    if (h === true) return true;
    if (h && typeof h === "object" && Array.isArray(h.queues)) return h.queues.includes(queue);
    return false;
}

// Record one attempt into the (opt-in) history timeline. `startedMs` is captured
// before the handler; `outcome` is "done" | "retried" | "dead". waitMs is the
// queue latency (start - enqueue), from the job's createdAt.
function recordAttempt(job, startedMs, finishedMs, outcome, err) {
    const waitMs = job.createdAt != null ? startedMs - job.createdAt * 1000 : null;
    db.exec(
        "INSERT INTO _hull_job_attempts (job_id, queue, type, attempt_no, wait_ms, " +
        "started_ms, finished_ms, duration_ms, outcome, error, trace_id) " +
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [job.id, job.queue || "default", job.type, job.attempts, waitMs,
         startedMs, finishedMs, finishedMs - startedMs, outcome, err || null, job.trace || null]);
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
    // Drain durable event subscriptions (throttled; the per-subscription lease
    // makes this fleet-safe). Only workers that registered subscribers do work.
    const subNames = Object.keys(_subscribers);
    if (subNames.length && now - _lastEdrain >= 1) {
        for (const name of subNames) eventsDrain(name, { now });
        _lastEdrain = now;
    }
    const batch = claim(o);
    for (const job of batch) {
        job.deps = loadDeps(job.id);   // workflow: dependency results, in order
        const h = _handlers[job.type] || _default;
        const startedMs = time.nowMs();   // attempt timing (used only if history on)
        let outcome, errStr;              // undefined outcome = a yield (not recorded)
        if (!h) {
            errStr = `no handler for job type '${job.type}'`;
            outcome = finish(job, { error: errStr }, () => { markDead(job.id, errStr); return "dead"; });
        } else {
        try {
            const result = await h(job);
            if (result && typeof result === "object" && result.__hullWfYield) {
                // Durable workflow yielded - no terminal outcome, no event. A
                // yield is not a failed attempt, so undo the claim's attempt
                // increment (a workflow may sleep / wait many times without
                // exhausting maxAttempts).
                const now = time.now();
                if (result.waiting) {
                    // ctx.waitSignal: park in the non-terminal 'waiting' status
                    // (excluded by the claim query). run_at carries the optional
                    // timeout deadline (0 = none); the reaper wakes a timed-out
                    // wait, jobs.signal wakes a delivered one.
                    db.exec("UPDATE _hull_jobs SET status='waiting', run_at=?, " +
                        "attempts=attempts-1, claim_token=NULL, updated_at=? WHERE id=?",
                        [result.deadline || 0, now, job.id]);
                    // Close the deliver-before-park race: a signal delivered in the
                    // check->park window couldn't re-activate us (we were 'running'),
                    // so re-check now that we are 'waiting'.
                    if (result.signalName) {
                        const sig = db.query("SELECT 1 AS x FROM _hull_workflow_signals " +
                            "WHERE workflow_id=? AND name=? AND consumed_at IS NULL",
                            [job.id, result.signalName]);
                        if (sig.length) {
                            db.exec("UPDATE _hull_jobs SET status='pending', run_at=?, " +
                                "updated_at=? WHERE id=? AND status='waiting'", [now, now, job.id]);
                        }
                    }
                } else {
                    // ctx.sleep: future-dated pending job.
                    db.exec("UPDATE _hull_jobs SET status='pending', run_at=?, " +
                        "attempts=attempts-1, claim_token=NULL, updated_at=? WHERE id=?",
                        [result.wakeAt || now, now, job.id]);
                }
            } else if (result === DEAD) {
                errStr = "handler returned jobs.DEAD";
                outcome = finish(job, { error: errStr }, () => { markDead(job.id, errStr); return "dead"; });
            } else if (result === RETRY) {
                errStr = "handler requested retry";
                outcome = finish(job, { error: errStr, attempt: job.attempts },
                    () => markRetry(job, errStr));
            } else {
                // undefined / true / DISCARD -> done, no result; any other return
                // value is stored as the job's result (for dependents).
                const res = (result !== undefined && result !== true && result !== DISCARD)
                    ? result : undefined;
                outcome = finish(job, { result: res }, () => { markDone(job.id, res); return "done"; });
            }
        } catch (e) {
            errStr = String((e && e.message) || e);
            outcome = finish(job, { error: errStr, attempt: job.attempts },
                () => markRetry(job, errStr));
        }
        }
        // Opt-in attempt history (a yield leaves outcome undefined -> not recorded).
        if (outcome && historyEnabled(job.queue)) {
            recordAttempt(job, startedMs, time.nowMs(), outcome, errStr);
        }
        // Release the strict-concurrency slot reserved at claim: any exit from
        // 'running' (done / dead / retry / workflow yield) frees it. A yielded
        // workflow re-reserves on its next claim.
        if (job._concStrict === 1 && job._concKey !== undefined && job._concKey !== null) {
            concRelease(job._concKey);
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
// Unix ts of the last subscription drain (throttled to >=1s in work()).
let _lastEdrain = 0;
// Durable event subscribers registered on THIS worker (jobs.subscribe):
// name -> { handler(event) }. The subscription row (cursor/lease) is durable in
// _hull_job_subscriptions; the handler is per-worker, like a job handler.
const _subscribers = Object.create(null);
// Lease duration (seconds) for a subscription drain (a crashed drainer's lease
// expires after this and another worker resumes from the unadvanced cursor).
const _EDRAIN_LEASE = 30;

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

    // Idle wait between empty polls. On a NOTIFY-capable backend (Postgres) a
    // single-loop worker parks on conn.waitNotify, so a freshly enqueued job or
    // appended event wakes it immediately instead of after pollMs; the timeout
    // is still pollMs, so a missed/absent NOTIFY just falls back to a poll
    // (latency only, never correctness). Gated on concurrency === 1: with N
    // in-process loops all idle we would occupy N worker-pool threads on the
    // wait, so the multi-loop case keeps plain sleep (the fleet still gets the
    // win via separate single-loop worker processes).
    //
    // Three wait outcomes:
    //   * true  -> work is available; return at once (the whole point).
    //   * false at ~pollMs -> a real timeout; loop and poll.
    //   * false well before pollMs -> a dropped-connection wait (worker_db
    //     already invalidated it), NOT a real timeout, so sleep the remainder;
    //     otherwise a flapping connection would spin the loop.
    // A throw (no thread pool / event loop, or a transient DB error mid-reopen)
    // sleeps this tick and retries next tick, so a recovered database restores
    // the fast path without a worker restart.
    const useNotify = db.dialect.supportsNotify && concurrency === 1;
    const idleWait = async () => {
        if (!useNotify) { await hull.sleep(pollMs); return; }
        const t0 = time.nowMs();
        let got;
        try {
            got = await db.waitNotify("hull_jobs", pollMs);
        } catch (_e) {
            await hull.sleep(pollMs);
            return;
        }
        if (got === false) {
            const elapsed = time.nowMs() - t0;
            if (elapsed < pollMs) await hull.sleep(pollMs - elapsed);
        }
    };

    // One independent claim-loop; returns its own processed count.
    const loop = async () => {
        let processed = 0, empty = 0;
        while (_running) {
            const n = await work(o);
            processed += n;
            if (n === 0) {
                empty++;
                if (maxEmpty > 0 && empty >= maxEmpty) break;
                await idleWait();
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
// p50/p95/p99 of a numeric list (computed host-side; portable SQL has no
// PERCENTILE). Nearest-rank on the sorted sample.
function percentiles(vals) {
    if (vals.length === 0) return { p50: 0, p95: 0, p99: 0 };
    vals.sort((a, b) => a - b);
    const pick = (p) => vals[Math.min(vals.length - 1, Math.max(0, Math.ceil(p * vals.length) - 1))];
    return { p50: pick(0.50), p95: pick(0.95), p99: pick(0.99) };
}

function stats(opts) {
    const o = opts || {};
    const rows = o.queue
        ? db.query("SELECT status, COUNT(*) AS c FROM _hull_jobs WHERE queue=? GROUP BY status", [o.queue])
        : db.query("SELECT status, COUNT(*) AS c FROM _hull_jobs GROUP BY status");
    const s = { pending: 0, running: 0, done: 0, dead: 0, blocked: 0 };
    for (const r of rows) s[r.status] = r.c;
    return s;
}

/**
 * A DB-derived metrics snapshot for dashboards / a /metrics route. Pull-based (no
 * exporter, no process counters), so it is correct across a whole fleet of
 * workers. Returns `{ queues: { <queue>: { pending, running, waiting, blocked,
 * dead, oldestPendingAge }, ... }, totals: { ... } }`. `oldestPendingAge` is
 * `now - createdAt` of the oldest READY (run_at<=now) pending job in the queue -
 * the actionable backlog age. `opts.queue` scopes to one queue. Latency
 * percentiles and throughput are added by the attempt-history pillar (opt-in).
 * @param {object} [opts]  { queue }
 * @returns {object}
 */
function metrics(opts) {
    const o = opts || {};
    const now = time.now();
    const LIVE = { pending: 1, running: 1, waiting: 1, blocked: 1, dead: 1 };
    const newBucket = () => ({ pending: 0, running: 0, waiting: 0, blocked: 0, dead: 0 });
    const queues = {}, totals = newBucket();
    const rows = o.queue
        ? db.query("SELECT queue, status, COUNT(*) AS n FROM _hull_jobs WHERE queue=? GROUP BY queue, status", [o.queue])
        : db.query("SELECT queue, status, COUNT(*) AS n FROM _hull_jobs GROUP BY queue, status");
    for (const r of rows) {
        const q = queues[r.queue] || (queues[r.queue] = newBucket());
        if (LIVE[r.status]) { q[r.status] = (q[r.status] || 0) + r.n; totals[r.status] += r.n; }
    }
    // Oldest ready-to-run pending job per queue -> backlog age.
    const ages = o.queue
        ? db.query("SELECT queue, MIN(created_at) AS oldest FROM _hull_jobs WHERE status='pending' AND run_at<=? AND queue=? GROUP BY queue", [now, o.queue])
        : db.query("SELECT queue, MIN(created_at) AS oldest FROM _hull_jobs WHERE status='pending' AND run_at<=? GROUP BY queue", [now]);
    for (const r of ages) {
        const q = queues[r.queue];
        if (q && r.oldest != null) q.oldestPendingAge = now - r.oldest;
    }
    const out = { queues, totals };
    // Latency + throughput from the attempt history over the last `window`
    // seconds (present only when attempt recording is on, i.e. there is data).
    const window = o.window || 300;
    const sinceMs = (now - window) * 1000;
    const sample = o.queue
        ? db.query("SELECT wait_ms, duration_ms, outcome FROM _hull_job_attempts WHERE finished_ms >= ? AND queue=? ORDER BY finished_ms DESC LIMIT 5000", [sinceMs, o.queue])
        : db.query("SELECT wait_ms, duration_ms, outcome FROM _hull_job_attempts WHERE finished_ms >= ? ORDER BY finished_ms DESC LIMIT 5000", [sinceMs]);
    if (sample.length) {
        const waits = [], runs = []; let doneN = 0, deadN = 0;
        for (const r of sample) {
            if (r.wait_ms != null) waits.push(r.wait_ms);
            runs.push(r.duration_ms);
            if (r.outcome === "done") doneN++; else if (r.outcome === "dead") deadN++;
        }
        out.latency = { waitMs: percentiles(waits), runMs: percentiles(runs) };
        out.throughput = { donePerSec: doneN / window, deadPerSec: deadN / window };
    }
    // Durable-event log health (Phase 3): total log depth + per-subscription lag
    // (positions behind the log head = max id - cursor) and failure count.
    {
        const depth = db.query("SELECT COUNT(*) AS n, MAX(id) AS mx FROM _hull_job_events");
        const logDepth = (depth[0] && depth[0].n) || 0;
        if (logDepth > 0) {
            const maxId = (depth[0] && depth[0].mx) || 0;
            const subs = db.query("SELECT name, cursor_id, failures FROM _hull_job_subscriptions ORDER BY name");
            out.events = {
                logDepth,
                subscriptions: subs.map((s) => ({
                    name: s.name, lag: maxId - (s.cursor_id || 0), failures: s.failures || 0,
                })),
            };
        }
    }
    return out;
}

/**
 * The attempt timeline for a job (opt-in history; empty when off / none). Each
 * entry is `{ attemptNo, startedMs, finishedMs, durationMs, waitMs, outcome,
 * error }`, oldest first - a queryable "what happened to this job".
 * @param {number} id
 * @returns {object[]}
 */
function history(id) {
    const rows = db.query(
        "SELECT attempt_no, started_ms, finished_ms, duration_ms, wait_ms, outcome, error " +
        "FROM _hull_job_attempts WHERE job_id=? ORDER BY attempt_no, started_ms", [id]);
    return rows.map((r) => ({
        attemptNo: r.attempt_no, startedMs: r.started_ms, finishedMs: r.finished_ms,
        durationMs: r.duration_ms, waitMs: r.wait_ms, outcome: r.outcome, error: r.error,
    }));
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
        // Fail soft on a corrupted result row (we wrote valid JSON, but don't
        // throw on external corruption): fall back to the raw string.
        if (rr.length && rr[0].result != null) {
            try { out.result = JSON.parse(rr[0].result); }
            catch (e) { out.result = rr[0].result; }
        }
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
    const interval = Math.max(opts.interval || 100, 10);   // floor: never a tight loop
    const deadline = opts.timeout != null ? time.clock() + opts.timeout : null;
    for (;;) {
        const r = result(id);
        if (r === null) return null;                            // unknown / purged
        if (r.status === "done" || r.status === "dead") return r;
        if (deadline !== null && time.clock() >= deadline) return r;
        await hull.sleep(interval);
    }
}

// ── Durable execution: workflow-as-code (Phase 1a) ──────────────────────────
// A durable workflow is a function fn(ctx) run as a job of the reserved type
// "__wf:<name>". Each `await ctx.step(name, fn)` memoizes its result in
// _hull_workflow_steps; if the workflow re-runs (crash or retry re-enters it from
// the top), completed steps return their stored result instead of re-executing,
// so the body resumes where it left off. Design:
// docs/jobs_durable_execution_design.md.
const WF_PREFIX = "__wf:";

// Run (or replay) one memoized step. Returns fn()'s value; on a re-run of the
// workflow the value comes from the store and fn is NOT called again.
async function runStep(workflowId, stepKey, fn) {
    if (typeof stepKey !== "string" || stepKey === "")
        throw new Error("ctx.step: name must be a non-empty string");
    if (typeof fn !== "function") throw new Error("ctx.step: fn must be a function");
    const rows = db.query(
        "SELECT result FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
        [workflowId, stepKey]);
    if (rows.length) {
        if (rows[0].result == null) return undefined;
        try { return JSON.parse(rows[0].result); } catch (e) { return rows[0].result; }
    }
    // First execution: run, then persist. At-least-once - a crash between the
    // side effect and this INSERT re-runs the step on resume (steps must be
    // idempotent, same contract as a job handler).
    const res = await fn();
    const enc = res !== undefined ? json.encode(res) : null;
    db.exec(
        "INSERT INTO _hull_workflow_steps (workflow_id, step_key, result, status, created_at) " +
        "VALUES (?, ?, ?, 'done', ?)",
        [workflowId, stepKey, enc, time.now()]);
    return res;
}

// Durable timer. `n` is the ordinal of this sleep in the workflow body (stable
// across re-runs). On first encounter it records the wake time and throws the
// yield sentinel (the runner reschedules the job to run_at = wake_at); on a
// resume the recorded wake time is read, and once it has passed the call returns
// so the body continues past it.
function runSleep(workflowId, n, seconds) {
    const key = "__sleep:" + n;
    const rows = db.query(
        "SELECT result FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
        [workflowId, key]);
    let wakeAt;
    if (rows.length) {
        wakeAt = Number(rows[0].result);
    } else {
        wakeAt = time.now() + (Number(seconds) || 0);
        db.exec(
            "INSERT INTO _hull_workflow_steps (workflow_id, step_key, result, status, created_at) " +
            "VALUES (?, ?, ?, 'sleeping', ?)",
            [workflowId, key, String(wakeAt), time.now()]);
    }
    if (time.now() >= wakeAt) return;   // elapsed: continue
    const e = new Error("__hull_yield");   // caught by the runner (Error carries the marker)
    e.__hullYield = true;
    e.wakeAt = wakeAt;
    throw e;
}

// Wait for an external signal (jobs.signal). If the named signal is present it is
// consumed and its payload returned; otherwise the workflow yields to the
// non-terminal 'waiting' status (re-activated by jobs.signal). With opts.timeout
// the wait also arms a deadline (stored once, stable across resumes) so the
// reaper wakes it if no signal arrives; a timed-out wait returns null.
function runWaitSignal(workflowId, name, opts) {
    if (typeof name !== "string" || name === "")
        throw new Error("ctx.waitSignal: name must be a non-empty string");
    const rows = db.query(
        "SELECT payload FROM _hull_workflow_signals WHERE workflow_id=? AND name=? AND consumed_at IS NULL",
        [workflowId, name]);
    if (rows.length) {
        db.exec("UPDATE _hull_workflow_signals SET consumed_at=? WHERE workflow_id=? AND name=?",
            [time.now(), workflowId, name]);
        if (rows[0].payload == null) return null;
        try { return JSON.parse(rows[0].payload); } catch (e) { return rows[0].payload; }
    }
    let deadline = 0;
    if (opts && opts.timeout) {
        const key = "__waitdl:" + name;
        const drows = db.query(
            "SELECT result FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
            [workflowId, key]);
        if (drows.length) {
            deadline = Number(drows[0].result) || 0;
        } else {
            deadline = time.now() + opts.timeout;
            db.exec(
                "INSERT INTO _hull_workflow_steps (workflow_id, step_key, result, status, created_at) " +
                "VALUES (?, ?, ?, 'waiting', ?)",
                [workflowId, key, String(deadline), time.now()]);
        }
        if (time.now() >= deadline) return null;   // timed out, no signal
    }
    const e = new Error("__hull_yield");
    e.__hullYield = true;
    e.waiting = true;
    e.signalName = name;
    e.deadline = deadline;
    throw e;
}

// Run the compensations of completed steps in reverse order (saga rollback). Each
// runs at most once (marked 'compensated'), so a crash mid-rollback resumes.
async function runCompensations(comps, workflowId) {
    for (let i = comps.length - 1; i >= 0; i--) {
        const c = comps[i];
        const done = db.query(
            "SELECT status FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
            [workflowId, c.key]);
        if (!(done.length && done[0].status === "compensated")) {
            try { await c.fn(); } catch (e) { /* at-least-once; must be idempotent */ }
            db.exec("UPDATE _hull_workflow_steps SET status='compensated' WHERE workflow_id=? AND step_key=?",
                [workflowId, c.key]);
        }
    }
}

// Build the durable ctx handed to a workflow function. `sleepN` counts sleeps;
// `comps` collects saga compensations (registered by every ctx.step call that has
// one, so on the terminal-failure run the list covers all completed steps).
function makeCtx(job, name) {
    let sleepN = 0, detN = 0;
    const comps = [];
    // Workflow versioning (Temporal-style patching). `frontier` = how many
    // step-family rows this instance had recorded when THIS run began; `stepPos`
    // counts the step-family ops executed so far this run. Sleep / wait-deadline
    // rows are excluded from both (control rows, not body positions), so the two
    // stay aligned regardless of how a workflow interleaves sleeps. ctx.patched
    // compares them to decide old-vs-new (see below).
    let stepPos = 0;
    const frontier = (() => {
        // Exclude the sleep / wait-deadline control rows with a prefix compare, not
        // LIKE: '__sleep:' / '__waitdl:' contain '_' (a LIKE wildcard) and no
        // portable ESCAPE clause exists - MySQL processes backslash escapes at the
        // string-literal level, and "ESCAPE '\'" there is a mangled quote. substr
        // prefix compare is wildcard-free and identical on SQLite / PG / MySQL.
        const r = db.query(
            "SELECT COUNT(*) AS n FROM _hull_workflow_steps WHERE workflow_id=? " +
            "AND substr(step_key, 1, 8) <> '__sleep:' " +
            "AND substr(step_key, 1, 9) <> '__waitdl:'",
            [job.id]);
        return (r[0] && r[0].n) || 0;
    })();
    return {
        id: job.id,
        name,
        input: job.data,
        trace: job.trace,   // trace-context propagation (observability design)
        _comps: comps,
        // Deterministic primitives (Phase 2): a workflow body re-runs from the top
        // on every resume, so reading the clock / RNG directly would differ each
        // replay and break memo-key matching. These memoize via the step store, so
        // now / random / uuid return the SAME value on every replay - the supported
        // way to be non-deterministic in a workflow. Hidden from steps_done.
        now: () => { detN += 1; stepPos += 1; return runStep(job.id, "__now:" + detN, () => time.now()); },
        random: () => { detN += 1; stepPos += 1; return runStep(job.id, "__rand:" + detN, () => Math.random()); },
        uuid: () => { detN += 1; stepPos += 1; return runStep(job.id, "__uuid:" + detN, () => crypto.base64urlEncode(crypto.random(16))); },
        step: async (stepKey, fn, opts) => {
            stepPos += 1;
            const result = await runStep(job.id, stepKey, fn);
            if (opts && typeof opts.compensate === "function") comps.push({ key: stepKey, fn: opts.compensate });
            return result;
        },
        sleep: (seconds) => { sleepN += 1; return runSleep(job.id, sleepN, seconds); },
        waitSignal: (signalName, opts) => runWaitSignal(job.id, signalName, opts),
        // Workflow versioning: ctx.patched(patchId) lets a changed workflow branch
        // old-vs-new so in-flight instances finish on the definition they started.
        // Call it ONCE per patchId (no await - returns a boolean). Semantics:
        //   * marker already recorded for this instance -> true (memoized).
        //   * undecided AND inside the already-recorded prefix (stepPos < frontier)
        //     -> ran past here under the OLD definition: keep old branch, return
        //     false, record nothing.
        //   * undecided AND at/after the frontier -> reached fresh: adopt the
        //     patch, record it true so every later resume returns true.
        patched: (patchId) => {
            if (typeof patchId !== "string" || patchId === "")
                throw new Error("ctx.patched: patchId must be a non-empty string");
            const key = "__patch:" + patchId;
            const rows = db.query(
                "SELECT status FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
                [job.id, key]);
            if (rows.length) { stepPos += 1; return true; }   // marker: only ever true
            if (stepPos < frontier) return false;             // replaying old prefix
            stepPos += 1;
            db.exec(
                "INSERT INTO _hull_workflow_steps (workflow_id, step_key, result, status, created_at) " +
                "VALUES (?, ?, ?, 'done', ?)",
                [job.id, key, json.encode(true), time.now()]);
            return true;
        },
    };
}

/**
 * Register a durable workflow. `fn(ctx)` is the workflow body; its return value
 * becomes the workflow's result (fetch via jobs.await / jobs.result). Inside it,
 * `await ctx.step(name, fn)` runs `fn` once and memoizes the result, so a crashed
 * or retried workflow resumes past completed steps. `ctx.input` is the payload
 * from jobs.start, `ctx.id` the workflow id. A workflow instance IS a job
 * (reserved type "__wf:<name>"), so it inherits claim / reaper / retry / worker /
 * result.
 * @param {string} name
 * @param {Function} fn   (ctx) => result   (may be async)
 * @returns {object} the jobs module (for chaining)
 */
function workflow(name, fn) {
    if (typeof name !== "string" || name === "")
        throw new Error("jobs.workflow: name must be a non-empty string");
    if (typeof fn !== "function") throw new Error("jobs.workflow: fn must be a function");
    _workflows[name] = fn;
    // A ctx.sleep / ctx.waitSignal throws the yield sentinel; catch it and hand
    // work() the yield marker (sleep -> reschedule pending; signal -> 'waiting').
    // A real error re-throws for the retry path - but on the LAST attempt (about
    // to dead-letter) or an explicit jobs.DEAD, run the saga compensations first.
    handler(WF_PREFIX + name, async (job) => {
        const ctx = makeCtx(job, name);
        let res;
        try { res = await fn(ctx); }
        catch (e) {
            if (e && e.__hullYield) {
                return { __hullWfYield: true, wakeAt: e.wakeAt,
                         waiting: e.waiting, signalName: e.signalName, deadline: e.deadline };
            }
            const max = (job.maxAttempts != null) ? job.maxAttempts : _cfg.maxAttempts;
            if ((job.attempts || 0) >= max) await runCompensations(ctx._comps, job.id);
            throw e;
        }
        if (res === DEAD) await runCompensations(ctx._comps, job.id);
        return res;
    });
    return jobs;
}

/**
 * Start a durable workflow instance. Returns the workflow id (a job id); the
 * input is the workflow's `ctx.input`. Accepts the usual enqueue opts (queue,
 * priority, dedupKey for an idempotent start, delay/at). Poll jobs.workflowStatus
 * (id) or jobs.await(id) for the outcome.
 * @param {string} name
 * @param {*} [input]
 * @param {object} [opts]   enqueue options
 * @returns {number|null}   the workflow id, or null if a dedupKey collapsed it
 */
function start(name, input, opts) {
    if (!_workflows[name])
        throw new Error(`jobs.start: unknown workflow '${name}' (register it with jobs.workflow)`);
    return enqueue(WF_PREFIX + name, input, opts);
}

/**
 * Deliver a signal to a durable workflow (see ctx.waitSignal). Records the named
 * payload (first delivery per name wins) and re-activates the workflow if it is
 * parked waiting for it. Safe to call before the workflow reaches the wait (the
 * signal is stored and consumed when it gets there) - no lost-signal race.
 * @param {number} id       the workflow id
 * @param {string} name     the signal name
 * @param {*} [payload]
 * @returns {boolean}
 */
function signal(id, name, payload) {
    if (typeof name !== "string" || name === "")
        throw new Error("jobs.signal: name must be a non-empty string");
    const enc = payload !== undefined && payload !== null ? json.encode(payload) : null;
    const now = time.now();
    db.insertIfAbsent("_hull_workflow_signals", ["workflow_id", "name"],
        ["workflow_id", "name", "payload", "created_at"], [id, name, enc, now]);
    // Re-activate a parked wait (waiting -> pending). A 'running' or unrelated
    // 'pending' workflow is left alone: it finds the stored signal on its own.
    db.exec("UPDATE _hull_jobs SET status='pending', run_at=?, updated_at=? " +
        "WHERE id=? AND status='waiting'", [now, now, id]);
    return true;
}

/**
 * Query a workflow instance's state (DB-derived, so it works even when no worker
 * is currently running it). Returns null for an unknown id, else
 * { status, name, stepsDone, startedAt, updatedAt, result?, error? }.
 * @param {number} id
 * @returns {object|null}
 */
function workflowStatus(id) {
    const job = get(id);
    if (!job) return null;
    const rows = db.query(
        "SELECT step_key FROM _hull_workflow_steps WHERE workflow_id=? ORDER BY created_at, step_key",
        [id]);
    const out = {
        status: job.status,
        name: String(job.type).startsWith(WF_PREFIX) ? String(job.type).slice(WF_PREFIX.length) : job.type,
        // Hide internal keys (durable-sleep markers "__sleep:N"); only user steps.
        stepsDone: rows.map((r) => r.step_key).filter((k) => !String(k).startsWith("__")),
        startedAt: job.createdAt,
        updatedAt: job.updatedAt,
    };
    // What the workflow is parked on: a signal ('waiting') or a durable timer
    // (pending with a future run_at).
    if (job.status === "waiting") {
        out.waitingFor = "signal";
    } else if (job.status === "pending" && job.runAt && job.runAt > time.now()) {
        out.waitingFor = "sleep:" + job.runAt;
    }
    if (job.status === "done") {
        const r = result(id);
        if (r) out.result = r.result;
    } else if (job.status === "dead") {
        out.error = job.lastError;
    }
    return out;
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
    if (!_cfg.events) {
        return (db.exec("DELETE FROM _hull_jobs WHERE id=? AND status='pending'", [id]) || 0) > 0;
    }
    // events on: capture type/queue for the "cancelled" event, delete + emit in
    // one txn (BEGIN IMMEDIATE serializes, so the SELECT->DELETE stays consistent).
    let cancelled = false;
    db.batch(() => {
        const r = db.query("SELECT type, queue FROM _hull_jobs WHERE id=? AND status='pending'", [id]);
        if (r[0]) {
            db.exec("DELETE FROM _hull_jobs WHERE id=? AND status='pending'", [id]);
            emitDurable("cancelled", { id, type: r[0].type, queue: r[0].queue }, {});
            cancelled = true;
        }
    });
    return cancelled;
}

/**
 * Read-only tail of the durable event log (jobs.init{events:true}), newest first
 * - for a dashboard or ad-hoc inspection. Phase 2 adds durable subscriptions
 * (jobs.subscribe) with at-least-once delivery + cursors.
 * @param {object} [opts] { since: <event id>, types: ["dead", ...], limit: 100 }
 * @returns {Array} of { id, ts, type, job_id, job_type, queue, data }
 */
function events(opts) {
    const o = opts || {};
    const where = ["1=1"], params = [];
    if (o.since !== undefined) { where.push("id > ?"); params.push(o.since); }
    if (Array.isArray(o.types) && o.types.length) {
        where.push("type IN (" + o.types.map(() => "?").join(",") + ")");
        for (const t of o.types) params.push(t);
    }
    params.push(Math.min(Number(o.limit) || 100, 1000));
    const rows = db.query(
        "SELECT id, ts, type, job_id, job_type, queue, data FROM _hull_job_events " +
        "WHERE " + where.join(" AND ") + " ORDER BY id DESC LIMIT ?", params);
    for (const r of rows) {
        if (r.data !== undefined && r.data !== null && r.data !== "") {
            try { r.data = json.decode(r.data); } catch (_e) { /* leave raw */ }
        }
    }
    return rows;
}

/**
 * Register a durable named subscription over the event log (Phase 2). `handler`
 * is called once per event, in id order, AT-LEAST-ONCE (a crash between a handler
 * succeeding and the cursor advancing re-delivers it) - so handlers must be
 * idempotent. The cursor is durable (survives restarts); the handler is
 * per-worker, so call jobs.subscribe on every worker (like jobs.handler). Drained
 * by jobs.work / jobs.run_worker.
 * @param {string} name
 * @param {Function} handler   (event) => void
 * @param {object} [opts] { types: ["dead", ...], from: "now" | "beginning" }
 */
function subscribe(name, handler, opts) {
    if (typeof name !== "string" || name === "")
        throw new Error("jobs.subscribe: name must be a non-empty string");
    if (typeof handler !== "function")
        throw new Error("jobs.subscribe: handler must be a function");
    const o = opts || {};
    const typesCsv = (Array.isArray(o.types) && o.types.length) ? o.types.join(",") : null;
    const maxF = (typeof o.maxFailures === "number" && o.maxFailures > 0) ? o.maxFailures : null;
    // Start cursor: "now" (default) skips existing history; "beginning" replays all.
    let start = 0;
    if (o.from !== "beginning") {
        const m = db.query("SELECT MAX(id) AS m FROM _hull_job_events");
        start = (m[0] && m[0].m) || 0;
    }
    const now = time.now();
    const n = db.insertIfAbsent("_hull_job_subscriptions", ["name"],
        ["name", "cursor_id", "types", "failures", "max_failures", "updated_at"],
        [name, start, typesCsv, 0, maxF, now]);
    if (!(n && n > 0)) {
        db.exec("UPDATE _hull_job_subscriptions SET types=?, max_failures=?, updated_at=? WHERE name=?",
            [typesCsv, maxF, now, name]);
    }
    _subscribers[name] = { handler };
    return jobs;
}

/** Remove a subscription: this worker's handler and the durable row (cursor). */
function unsubscribe(name) {
    delete _subscribers[name];
    db.exec("DELETE FROM _hull_job_subscriptions WHERE name=?", [name]);
    return jobs;
}

// Synchronous drain seam (the Phase 2 testability keystone): lease the
// subscription, deliver new events in id order, advance the cursor, release the
// lease. No timers/sleeps - work() drives it. Returns { delivered, cursor,
// leased }. Test seams: opts.now (clock), opts.batch, opts.commitCursor (false =
// deliver but don't advance -> models a crash before the cursor write),
// opts.releaseLease (false = hold the lease -> models a worker death mid-drain).
function eventsDrain(name, opts) {
    const o = opts || {};
    const sub = _subscribers[name];
    if (!sub) return { delivered: 0, leased: false };
    const now = o.now !== undefined ? o.now : time.now();
    const token = crypto.base64urlEncode(crypto.random(8));
    // Acquire the lease (CAS): detect acquisition via RETURNING on PG/SQLite
    // (db.exec does not surface an affected-row count on Postgres); MySQL has no
    // RETURNING but returns the count.
    const leaseSql = "UPDATE _hull_job_subscriptions SET lease_token=?, lease_until=?, updated_at=? " +
        "WHERE name=? AND (lease_until IS NULL OR lease_until <= ?)";
    const leaseParams = [token, now + _EDRAIN_LEASE, now, name, now];
    const got = db.dialect.supportsReturning
        ? db.query(leaseSql + " RETURNING name", leaseParams).length
        : (db.exec(leaseSql, leaseParams) || 0);
    if (got === 0) return { delivered: 0, leased: false };
    const row = db.query("SELECT cursor_id, types, failures, max_failures FROM _hull_job_subscriptions WHERE name=?", [name]);
    const cursor = row[0].cursor_id;
    const failures = row[0].failures || 0;
    const maxFailures = row[0].max_failures;
    const where = ["id > ?"], params = [cursor];
    if (row[0].types) {
        const parts = String(row[0].types).split(",").filter((s) => s.length);
        if (parts.length) {
            where.push("type IN (" + parts.map(() => "?").join(",") + ")");
            for (const t of parts) params.push(t);
        }
    }
    params.push(o.batch || 100);
    const evs = db.query(
        "SELECT id, ts, type, job_id, job_type, queue, data FROM _hull_job_events " +
        "WHERE " + where.join(" AND ") + " ORDER BY id LIMIT ?", params);
    // Deliver in id order. On a handler error, stop at that event (the poison);
    // the successful prefix is committed and the poison retried on the next drain.
    let lastOk = cursor, delivered = 0, poison = null, poisonErr = null;
    for (const e of evs) {
        if (e.data !== undefined && e.data !== null && e.data !== "") {
            try { e.data = json.decode(e.data); } catch (_e) { /* leave raw */ }
        }
        try { sub.handler(e); lastOk = e.id; delivered += 1; }
        catch (err) { poison = e; poisonErr = err; break; }
    }
    // Failure accounting + poison skip (Phase 3). `failures` counts consecutive
    // drains stalled on the SAME frontier event; making progress resets to a fresh
    // 1. Once it reaches maxFailures (opt-in), skip the poison: advance past it and
    // record a durable `subscription_skipped` event so it can't wedge the
    // subscription (or retention) forever.
    let newFailures = 0, newCursor = lastOk;
    if (poison) {
        newFailures = (delivered > 0) ? 1 : failures + 1;
        if (maxFailures && newFailures >= maxFailures) {
            if (poison.type !== "subscription_skipped") {   // don't chain skip-of-skip
                emitDurable("subscription_skipped",
                    { id: poison.job_id, type: poison.job_type, queue: poison.queue },
                    { error: `subscription '${name}' skipped event ${poison.id} after ` +
                        `${newFailures} failures: ${(poisonErr && poisonErr.message) || poisonErr}` });
            }
            newCursor = poison.id;   // advance PAST the poison
            newFailures = 0;
        }
    }
    if (o.commitCursor === false) newCursor = cursor;
    const release = o.releaseLease !== false;
    db.exec(
        "UPDATE _hull_job_subscriptions SET cursor_id=?, failures=?, " +
        (release ? "lease_token=NULL, lease_until=NULL, " : "") +
        "updated_at=? WHERE name=? AND lease_token=?",
        [newCursor, newFailures, now, name, token]);
    return { delivered, cursor: newCursor, leased: true,
             skipped: (poison !== null && newCursor === poison.id) };
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
    db.exec("DELETE FROM _hull_workflow_steps WHERE workflow_id NOT IN (SELECT id FROM _hull_jobs)");
    db.exec("DELETE FROM _hull_workflow_signals WHERE workflow_id NOT IN (SELECT id FROM _hull_jobs)");
    // Attempt history is retained by AGE (it outlives the job on purpose, for
    // post-hoc metrics), not by orphan-ness.
    const hr = _cfg.historyRetention || o.olderThan || 604800;
    db.exec("DELETE FROM _hull_job_attempts WHERE finished_ms < ?", [(time.now() - hr) * 1000]);
    // Durable event log: retained by age, but NEVER past an unconsumed event -
    // min(cursor) across subscriptions is the safe watermark. No subscriptions ->
    // age only (the Phase 1 behavior).
    const mc = db.query("SELECT MIN(cursor_id) AS m FROM _hull_job_subscriptions");
    const watermark = mc[0] ? mc[0].m : null;
    if (watermark === null || watermark === undefined) {
        db.exec("DELETE FROM _hull_job_events WHERE ts < ?", [cutoff]);
    } else {
        db.exec("DELETE FROM _hull_job_events WHERE ts < ? AND id <= ?", [cutoff, watermark]);
    }
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
    stats, runWorker, stop, get, result, await: await_, progress, heartbeat, limit, metrics, history,
    pause, resume, purge,
    workflow, start, signal, workflowStatus,
    dead, retry, cancel, cleanup, cron, uncron, events, subscribe, unsubscribe,
    RETRY, DEAD, DISCARD, _config: _cfg, _cronNext, _tick, _eventsDrain: eventsDrain,
};
export { init, enqueue, enqueueMany, claim, handler, on, work, reap, stats,
         runWorker, stop, get, result, progress, heartbeat, limit, metrics, history, pause, resume,
         purge, workflow, start, signal, workflowStatus,
         dead, retry, cancel, cleanup, cron, uncron, events, subscribe, unsubscribe };
export default jobs;
