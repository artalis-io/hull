--- hull/jobs@1 - durable, DB-backed background job queue.
--
-- Enqueue a unit of work, process it later with retries, backoff, and a
-- dead-letter path. DB-backend agnostic (SQLite / PostgreSQL / MySQL via the
-- hull/db capability surface), orthogonal (deps: hull/db + hull/time +
-- hull/json only), and transactionally coupled (enqueue is a plain INSERT, so
-- it joins the caller's db.batch and commits with the business row).
--
-- Full design: docs/jobs_design.md.
--
-- Full surface: schema + jobs.init, enqueue, the atomic claim, per-type +
-- catch-all handlers, the work loop (retry-with-backoff, dead-letter, and the
-- visibility-timeout reaper), the dedicated worker (jobs.run_worker +
-- `hull jobs worker`), and the ops surface (jobs.stats / dead / retry / cancel / cleanup). v1.1 adds durable cron (jobs.cron / uncron) and intra-process concurrency (run_worker concurrency=N), jobs.get(id), jobs.heartbeat for long jobs, fleet-wide rate limiting (jobs.limit), queue pause/resume/purge, and workflows (depends_on + result passing). v1.5 adds the standalone result backend (jobs.result / jobs.await), multi-queue draining (opts.queues, strict list or weighted map), bulk enqueue (jobs.enqueue_many), in-process lifecycle hooks (jobs.on completed/retried/dead), and polish (absolute at, jobs.progress, throttle window, fixed-offset tz cron). Durable workflows (jobs.workflow / jobs.start / ctx.step / ctx.sleep / ctx.wait_signal / jobs.signal, Phase 1) add crash-safe workflow-as-code: step memoization, durable timers, external signals, and saga compensation - a workflow instance is a job that rides the same claim/reaper/retry/result machinery (docs/jobs_durable_execution_design.md).
--
-- Usage (target shape):
--   local jobs = require("hull.jobs")
--   jobs.init()
--   jobs.handler("send_email", function(job) email.send(job.data) end)
--   -- inside a request transaction:
--   jobs.enqueue("send_email", { to = "a@b.c" })
--   -- drive processing from a timer, or `hull jobs worker`:
--   app.every(1000, function() jobs.work() end)

local db     = require("hull.db").default()
local time   = require("hull.time")
local json   = require("hull.json")
local crypto = require("hull.crypto")

local jobs = {}

-- Outcome sentinels a handler returns. Tables (not
-- strings) so a handler can't collide with them by returning ordinary data.
jobs.RETRY   = setmetatable({}, { __tostring = function() return "jobs.RETRY" end })
jobs.DEAD    = setmetatable({}, { __tostring = function() return "jobs.DEAD" end })
jobs.DISCARD = setmetatable({}, { __tostring = function() return "jobs.DISCARD" end })

-- Module config; defaults overridable via jobs.init(opts).
local _cfg = {
    max_attempts       = 25,    -- dead-letter threshold
    visibility_timeout = 300,   -- seconds before an orphaned `running` job is reclaimed
    reap_interval      = 30,    -- min seconds between reaper sweeps (0 = every work() call)
}

-- Exponential backoff: 2^attempt * 10s, capped at 1h (shared with outbox math).
local function default_backoff(attempt)
    local d = (2 ^ attempt) * 10
    if d > 3600 then d = 3600 end
    return d
end
_cfg.backoff = default_backoff

-- Exposed for tests / introspection; not part of the app-facing contract.
jobs._config = _cfg

-- Registered handlers: type -> fn. `_default` is the optional catch-all.
local _handlers = {}
local _default = nil

-- In-process lifecycle listeners (jobs.on). Fired synchronously by jobs.work in
-- the worker that processed the job, for jobs THAT worker ran. Not fleet-wide -
-- a durable cross-process event stream is the LISTEN/NOTIFY epic (#235).
local _listeners = { completed = {}, retried = {}, dead = {} }

-- Registered durable workflows: name -> fn(ctx). jobs.workflow registers a
-- reserved-type handler that runs the workflow through a memoizing ctx.
local _workflows = {}

-- Unix ts of the last reaper sweep (throttled in jobs.work via _cfg.reap_interval).
local _last_reap = 0
-- Unix ts of the last cron due-check (throttled to >=1s; cron is minute-grained).
local _last_cron = 0
-- Whether the server parses SKIP LOCKED (probed in jobs.init; nil = not probed).
local _skip_locked = nil
-- Per-queue rate limits { [queue] = { rate, per } }, in-memory (re-registered on
-- boot); the shared window COUNTER lives in _hull_ratelimit (fleet-wide).
local _limits = {}
-- Row-lock clause for the rate counter: blocking FOR UPDATE on PG/MySQL (serialize
-- reservers), empty on SQLite (its write txn already serializes). Set in init.
local _rl_lock = ""
-- Paused-queue cache: a set of paused queue names + when it was last loaded.
-- Refreshed at most every 1s so a paused queue isn't a DB read on every claim.
local _paused = {}
local _paused_at = 0

--- Create the `_hull_jobs` table and its indexes. Idempotent - safe to call on
-- every boot. Uses the connection's portable identity DDL + IF-NOT-EXISTS index
-- form, so the same call runs unchanged on SQLite, PostgreSQL, and MySQL.
--
-- @tparam[opt] table opts
-- @tparam[opt=25]  number opts.max_attempts        dead-letter threshold
-- @tparam[opt=300] number opts.visibility_timeout  seconds before reclaim
-- @tparam[opt=30]  number opts.reap_interval        min seconds between reaper sweeps
-- @tparam[opt]     function opts.backoff            attempt -> delay seconds
-- @treturn table the jobs module (for chaining)
function jobs.init(opts)
    opts = opts or {}
    if opts.max_attempts ~= nil then _cfg.max_attempts = opts.max_attempts end
    if opts.visibility_timeout ~= nil then _cfg.visibility_timeout = opts.visibility_timeout end
    if opts.reap_interval ~= nil then _cfg.reap_interval = opts.reap_interval end
    if opts.backoff ~= nil then _cfg.backoff = opts.backoff end

    -- Keyed / indexed text columns are VARCHAR(255) so MySQL can index them;
    -- data-only columns (payload, last_error) stay TEXT. status/type/queue are
    -- short but indexed, so bounded VARCHARs.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_jobs ("
        .. "id           " .. db.autoincrement_id_ddl .. ", "
        .. "queue        VARCHAR(255) NOT NULL DEFAULT 'default',"
        .. "type         VARCHAR(255) NOT NULL,"
        .. "payload      TEXT,"
        .. "status       VARCHAR(32)  NOT NULL DEFAULT 'pending',"
        .. "priority     INTEGER      NOT NULL DEFAULT 0,"
        .. "attempts     INTEGER      NOT NULL DEFAULT 0,"
        .. "max_attempts INTEGER      NOT NULL DEFAULT 25,"
        .. "run_at       INTEGER      NOT NULL DEFAULT 0,"
        .. "claim_token  VARCHAR(255),"
        .. "claimed_at   INTEGER,"
        .. "dedup_key    VARCHAR(255),"
        .. "last_error   TEXT,"
        .. "created_at   INTEGER      NOT NULL,"
        .. "updated_at   INTEGER      NOT NULL,"
        .. "progress     INTEGER      NOT NULL DEFAULT 0)")

    -- Claim scan path: ready-to-run pending jobs in a queue, by priority then id.
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_jobs_claim
        ON _hull_jobs(queue, status, run_at, priority, id)
    ]])

    -- Reaper scan path: running jobs by claim age (visibility-timeout reclaim).
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_jobs_reap
        ON _hull_jobs(status, claimed_at)
    ]])

    -- Idempotent enqueue: a non-null dedup_key is unique per queue. NULLs are
    -- distinct on every backend, so un-deduped jobs never collide.
    db.exec([[
        CREATE UNIQUE INDEX IF NOT EXISTS idx_hull_jobs_dedup
        ON _hull_jobs(queue, dedup_key)
    ]])

    -- Throttle scan path: recent jobs of a (queue, type) within the window
    -- (enqueue opts.throttle). Without this the throttle probe scans the queue.
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_jobs_throttle
        ON _hull_jobs(queue, type, created_at)
    ]])

    -- Durable cron schedules (jobs.cron). A worker atomically advances a due
    -- row's next_run_at (compare-and-set) and enqueues, so exactly one worker
    -- fires each tick across the fleet.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_cron ("
        .. "name         VARCHAR(255) NOT NULL PRIMARY KEY,"
        .. "spec         VARCHAR(255) NOT NULL,"
        .. "type         VARCHAR(255) NOT NULL,"
        .. "payload      TEXT,"
        .. "queue        VARCHAR(255) NOT NULL DEFAULT 'default',"
        .. "priority     INTEGER      NOT NULL DEFAULT 0,"
        .. "max_attempts INTEGER,"
        .. "next_run_at  INTEGER      NOT NULL,"
        .. "last_run_at  INTEGER,"
        .. "updated_at   INTEGER      NOT NULL,"
        .. "tz_offset    INTEGER      NOT NULL DEFAULT 0)")
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_cron_due ON _hull_cron(next_run_at)
    ]])

    -- Additive migrations for DBs created before v1.5: add the column only when
    -- absent (checked via the portable db.table_columns, mirroring session.lua),
    -- rather than catching a duplicate-column error - a caught ALTER would abort
    -- a surrounding transaction on Postgres if jobs.init ran inside a db.batch.
    local function ensure_column(tbl, col, coldef)
        for _, name in ipairs(db.table_columns(tbl) or {}) do
            if name == col then return end
        end
        db.exec("ALTER TABLE " .. tbl .. " ADD COLUMN " .. coldef)
    end
    ensure_column("_hull_jobs", "progress", "progress INTEGER NOT NULL DEFAULT 0")
    ensure_column("_hull_cron", "tz_offset", "tz_offset INTEGER NOT NULL DEFAULT 0")

    -- Fleet-wide rate-limit counters (jobs.limit). One row per limited queue;
    -- `name` (not the reserved word `key`), a window start, and the count.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_ratelimit ("
        .. "name         VARCHAR(255) NOT NULL PRIMARY KEY,"
        .. "window_start INTEGER      NOT NULL,"
        .. "n            INTEGER      NOT NULL)")
    -- Blocking FOR UPDATE serializes reservers on PG/MySQL; SQLite's write txn
    -- already serializes (and rejects FOR UPDATE syntactically).
    _rl_lock = (db.backend_name == "sqlite") and "" or " FOR UPDATE"

    -- Durable per-queue pause state (jobs.pause / resume). A paused queue is not
    -- claimed (workers skip it); fleet-wide (in the DB) and restart-durable.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_queue ("
        .. "name   VARCHAR(255) NOT NULL PRIMARY KEY,"
        .. "paused INTEGER      NOT NULL DEFAULT 0)")
    _paused, _paused_at = {}, 0   -- force a reload after (re)init

    -- Workflow dependency edges (jobs.enqueue depends_on). A dependent starts
    -- 'blocked'; each edge is marked satisfied when its dependency completes, and
    -- the dependent unblocks (-> pending) once its last edge is satisfied. The
    -- edge survives until the dependent runs, so its deps' results can be
    -- injected. fail_mode 'run' = "run even if this dep failed" (else cascade).
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_deps ("
        .. "dependent_id INTEGER NOT NULL,"
        .. "dep_id       INTEGER NOT NULL,"
        .. "ord          INTEGER NOT NULL,"
        .. "fail_mode    VARCHAR(8),"
        .. "satisfied    INTEGER NOT NULL DEFAULT 0,"
        .. "PRIMARY KEY (dependent_id, dep_id))")
    db.exec([[ CREATE INDEX IF NOT EXISTS idx_hull_job_deps_dep ON _hull_job_deps(dep_id) ]])

    -- A job's result (its handler's return value), for a dependent to consume.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_job_results ("
        .. "job_id INTEGER NOT NULL PRIMARY KEY,"
        .. "result TEXT)")

    -- Durable-execution step memo (jobs.workflow / ctx.step). One row per
    -- completed step of a workflow instance; a re-run of the workflow (crash or
    -- retry) reads these to skip already-done steps. Composite PK = idempotent.
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_workflow_steps ("
        .. "workflow_id INTEGER      NOT NULL,"
        .. "step_key    VARCHAR(255) NOT NULL,"
        .. "result      TEXT,"
        .. "status      VARCHAR(16)  NOT NULL DEFAULT 'done',"
        .. "created_at  INTEGER      NOT NULL,"
        .. "PRIMARY KEY (workflow_id, step_key))")

    -- Durable-execution signals (jobs.signal / ctx.wait_signal). One row per
    -- (workflow, signal name); the payload is consumed once. Composite PK makes a
    -- duplicate delivery a no-op (first wins).
    db.exec(
        "CREATE TABLE IF NOT EXISTS _hull_workflow_signals ("
        .. "workflow_id INTEGER      NOT NULL,"
        .. "name        VARCHAR(255) NOT NULL,"
        .. "payload     TEXT,"
        .. "created_at  INTEGER      NOT NULL,"
        .. "consumed_at INTEGER,"
        .. "PRIMARY KEY (workflow_id, name))")

    -- Verify the server actually parses SKIP LOCKED (the compile-time dialect
    -- flag says "this backend supports it" but a MySQL<8 / MariaDB<10.6 server
    -- does not). A 0-row probe against the real table detects it once; the claim
    -- falls back to plain FOR UPDATE otherwise.
    _skip_locked = db.dialect.supports_skip_locked
    if _skip_locked then
        local ok = pcall(function()
            db.query("SELECT id FROM _hull_jobs WHERE 1=0 FOR UPDATE SKIP LOCKED")
        end)
        if not ok then _skip_locked = false end
    end

    return jobs
end

-- ── Workflow dependencies (jobs.enqueue depends_on) ─────────────────────────

-- Apply one dependency's terminal outcome to one edge. Idempotent (safe if the
-- edge was already resolved by a concurrent completion or the enqueue recheck).
-- Returns "failed" when it cascade-fails the dependent (so callers recurse).
local function resolve_edge(dependent_id, dep_id, dep_ok, fail_mode)
    if dep_ok or fail_mode == "run" then
        local n = db.exec(
            "UPDATE _hull_job_deps SET satisfied=1 WHERE dependent_id=? AND dep_id=? AND satisfied=0",
            { dependent_id, dep_id })
        if (n or 0) == 0 then return nil end   -- already satisfied / gone
        local rows = db.query(
            "SELECT COUNT(*) AS c FROM _hull_job_deps WHERE dependent_id=? AND satisfied=0",
            { dependent_id })
        if (rows[1] and rows[1].c or 0) == 0 then
            db.exec("UPDATE _hull_jobs SET status='pending', updated_at=? WHERE id=? AND status='blocked'",
                { time.now(), dependent_id })
        end
        return nil
    end
    -- cascade-fail: this dependency died and the dependent didn't opt to run.
    local n = db.exec(
        "UPDATE _hull_jobs SET status='dead', last_error=?, updated_at=? WHERE id=? AND status='blocked'",
        { "dependency " .. tostring(dep_id) .. " failed", time.now(), dependent_id })
    if (n or 0) > 0 then
        db.exec("DELETE FROM _hull_job_deps WHERE dependent_id=?", { dependent_id })
        return "failed"
    end
    return nil
end

-- A job reached a terminal state; propagate to its dependents - unblock on
-- success, cascade-fail on failure, transitively.
local function resolve_deps(id, ok)
    local work, guard = { { id = id, ok = ok } }, 0
    while #work > 0 and guard < 100000 do
        guard = guard + 1
        local cur = table.remove(work)
        local edges = db.query(
            "SELECT dependent_id, fail_mode FROM _hull_job_deps WHERE dep_id=?", { cur.id })
        for _, e in ipairs(edges) do
            if resolve_edge(e.dependent_id, cur.id, cur.ok, e.fail_mode) == "failed" then
                work[#work + 1] = { id = e.dependent_id, ok = false }
            end
        end
    end
end

-- Gather a dependent's dependency results (in declaration order) for injection
-- as job.deps. Returns nil when the job has no dependencies (the common case;
-- a PK-indexed 0-row probe). A slot is nil when that dependency had no result.
local function load_deps(dependent_id)
    local edges = db.query(
        "SELECT dep_id FROM _hull_job_deps WHERE dependent_id=? ORDER BY ord", { dependent_id })
    if #edges == 0 then return nil end
    local out = {}
    for i, e in ipairs(edges) do
        local r = db.query("SELECT result FROM _hull_job_results WHERE job_id=?", { e.dep_id })
        if r[1] and r[1].result ~= nil then
            local ok, decoded = pcall(json.decode, r[1].result)
            if ok then out[i] = decoded end
        end
    end
    return out
end

--- Enqueue a job. A plain INSERT, so calling it inside a `db.batch()` commits
-- the job atomically with the business row (transactional coupling - the reason
-- jobs is DB-backed, not an external broker).
--
-- With `opts.depends_on` (a list of job ids) the job starts `blocked` and only
-- becomes claimable once all those jobs complete; each dependency's result is
-- injected into the handler as `job.deps` (in declaration order). If a
-- dependency dead-letters, the dependent cascade-fails too, unless
-- `opts.on_dep_failure == "run"`.
--
-- @tparam string job_type  handler dispatch key (non-empty)
-- @tparam[opt] any data     JSON-encodable payload (reaches the handler as job.data)
-- @tparam[opt] table opts
-- @tparam[opt="default"] string opts.queue
-- @tparam[opt=0]  number opts.priority     higher runs first
-- @tparam[opt=0]  number opts.delay        seconds until claimable (run_at = now + delay)
-- @tparam[opt]    number opts.run_at        absolute unix ts (overrides delay)
-- @tparam[opt]    number opts.max_attempts  overrides the module default
-- @tparam[opt]    string opts.dedup_key     unique per queue; a duplicate enqueue is a no-op
-- @treturn number|nil  the new job id, or nil when a dedup_key collapsed it
function jobs.enqueue(job_type, data, opts)
    if type(job_type) ~= "string" or job_type == "" then
        error("jobs.enqueue: type must be a non-empty string")
    end
    opts = opts or {}
    local now    = time.now()
    -- Schedule: absolute `at` (unix ts) wins, else `run_at`, else now + `delay`.
    local run_at = opts.at or opts.run_at or (now + (opts.delay or 0))
    -- Windowed throttle: skip (return nil) if a job of this (queue, type) was
    -- created within the last `throttle` seconds. Best-effort - keyed by
    -- (queue, type), no unique constraint (use dedup_key for exact-once).
    if opts.throttle and opts.throttle > 0 then
        local hit = db.query(
            "SELECT 1 AS x FROM _hull_jobs WHERE queue=? AND type=? AND created_at > ? LIMIT 1",
            { opts.queue or "default", job_type, now - opts.throttle })
        if hit and #hit > 0 then return nil end
    end
    local deps = opts.depends_on
    local has_deps = type(deps) == "table" and #deps > 0
    local vals = {
        opts.queue or "default",
        job_type,
        data ~= nil and json.encode(data) or nil,
        opts.priority or 0,
        opts.max_attempts or _cfg.max_attempts,
        run_at,
        opts.dedup_key,
        now, now,
    }
    local cols = { "queue", "type", "payload", "priority", "max_attempts",
                   "run_at", "dedup_key", "created_at", "updated_at" }
    if has_deps then
        cols[#cols + 1] = "status"; vals[#vals + 1] = "blocked"
    end
    local id
    if opts.dedup_key ~= nil then
        -- INSERT ... ON CONFLICT(queue,dedup_key) DO NOTHING / INSERT OR IGNORE.
        local n = db.insert_if_absent("_hull_jobs", { "queue", "dedup_key" }, cols, vals)
        if not (n and n > 0) then
            return nil   -- an un-run job with this (queue, dedup_key) already exists
        end
        id = db.last_id()
    else
        local ph = {}
        for i = 1, #cols do ph[i] = "?" end
        db.exec("INSERT INTO _hull_jobs (" .. table.concat(cols, ", ")
            .. ") VALUES (" .. table.concat(ph, ", ") .. ")", vals)
        id = db.last_id()
    end
    if has_deps then
        -- Record edges first, then re-check each dependency's current state: this
        -- closes the enqueue/complete race (a dep that finished between the check
        -- and the insert is caught by whichever side runs second - both idempotent).
        local fail_mode = (opts.on_dep_failure == "run") and "run" or nil
        for i, dep in ipairs(deps) do
            db.exec("INSERT INTO _hull_job_deps (dependent_id, dep_id, ord, fail_mode) "
                .. "VALUES (?, ?, ?, ?)", { id, dep, i, fail_mode })
        end
        for _, dep in ipairs(deps) do
            local g = jobs.get(dep)
            if g == nil or g.status == "done" then
                resolve_edge(id, dep, true, fail_mode)   -- already satisfied
            elseif g.status == "dead" then
                resolve_edge(id, dep, false, fail_mode)  -- already failed
            end
        end
    end
    return id
end

--- Enqueue many jobs in one transaction. `items` is a list of
-- `{ type, data?, opts? }` tables, each accepting the same `opts` as
-- `jobs.enqueue` EXCEPT `depends_on` (bulk is for independent jobs - build graphs
-- with `jobs.enqueue`, which errors here if seen). All rows commit together (one
-- `db.batch`), so the whole batch is atomic AND cheap: a single commit/fsync
-- instead of one per job - the reason to prefer this over a loop of `enqueue`.
-- Returns an array of ids in input order, with `nil` for any item whose
-- `dedup_key` collided with an existing un-run job (same as `jobs.enqueue`).
-- @tparam table items  list of { type, data, opts }
-- @treturn table  array of ids (nil per deduped item), in input order
function jobs.enqueue_many(items)
    if type(items) ~= "table" then error("jobs.enqueue_many: items must be a list") end
    local n = #items
    local ids = {}
    if n == 0 then return ids end
    for i = 1, n do
        local it = items[i]
        if type(it) ~= "table" or type(it.type) ~= "string" or it.type == "" then
            error("jobs.enqueue_many: item " .. i .. " needs a non-empty string type")
        end
        if it.opts and it.opts.depends_on then
            error("jobs.enqueue_many: depends_on is not supported in bulk; use jobs.enqueue for graph nodes")
        end
    end
    db.batch(function()
        for i = 1, n do
            ids[i] = jobs.enqueue(items[i].type, items[i].data, items[i].opts)
        end
    end)
    return ids
end

-- Decode a claimed DB row into a handler-facing job (payload -> data).
local function shape(row)
    local data
    if row.payload ~= nil and row.payload ~= "" then
        local ok, decoded = pcall(json.decode, row.payload)
        data = ok and decoded or nil
    end
    return {
        id = row.id, type = row.type, data = data,
        attempts = row.attempts, max_attempts = row.max_attempts,
    }
end

--- Set (or clear) a fleet-wide rate limit for a queue: at most `rate` jobs are
-- dispatched per `per`-second window across ALL workers (a shared DB counter, so
-- K processes still total `rate`, unlike a per-process limiter). Use it to
-- respect a downstream's limit (an email/API quota). Register at startup on
-- every worker (the limit lives in code; only the counter is shared).
-- @tparam string queue  the queue to limit
-- @tparam[opt] table opts  { rate = <max jobs>, per = 1 } - nil/false removes it
-- @treturn table the jobs module (for chaining)
function jobs.limit(queue, opts)
    if opts == nil or opts == false then _limits[queue] = nil; return jobs end
    if type(opts.rate) ~= "number" or opts.rate < 1 then
        error("jobs.limit: opts.rate must be a positive number")
    end
    _limits[queue] = { rate = opts.rate, per = opts.per or 1 }
    return jobs
end

-- Atomically reserve up to `want` slots in the current window for `key`, rolling
-- the window if stale. Returns how many were granted (0..want). The row is
-- pre-created (idempotent), then read-modify-written under a row lock so K
-- workers share one counter.
local function rl_reserve(key, want, rate, per)
    local now = time.now()
    local granted = 0
    db.insert_if_absent("_hull_ratelimit", { "name" },
        { "name", "window_start", "n" }, { key, now, 0 })
    db.batch(function()
        local sel = db.query(
            "SELECT window_start, n FROM _hull_ratelimit WHERE name=?" .. _rl_lock, { key })
        local ws, cnt = sel[1].window_start, sel[1].n
        if now - ws >= per then ws, cnt = now, 0 end
        granted = math.min(want, rate - cnt)
        if granted < 0 then granted = 0 end
        db.exec("UPDATE _hull_ratelimit SET window_start=?, n=? WHERE name=?",
            { ws, cnt + granted, key })
    end)
    return granted
end

-- Enforce a queue's rate limit on a just-claimed batch: keep the highest-priority
-- `granted` jobs, requeue the excess (undoing the claim's attempts+1 so a
-- rate-deferred job keeps its retry budget). Claim-then-reconcile avoids burning
-- window budget on empty polls. Mutates + returns `out`.
local function rl_apply(queue, out)
    local lim = _limits[queue]
    if not lim or #out == 0 then return out end
    local granted = rl_reserve(queue, #out, lim.rate, lim.per)
    if granted >= #out then return out end
    local params = { time.now() }
    local ph = {}
    for i = granted + 1, #out do ph[#ph + 1] = "?"; params[#params + 1] = out[i].id end
    db.exec(
        "UPDATE _hull_jobs SET status='pending', claim_token=NULL, claimed_at=NULL, "
        .. "attempts=attempts-1, updated_at=? WHERE id IN (" .. table.concat(ph, ",") .. ")",
        params)
    for i = #out, granted + 1, -1 do out[i] = nil end   -- keep the top `granted`
    return out
end

-- Is `queue` currently paused? Reads the durable state at most once/second
-- (cached), so pausing never adds a DB read to the steady claim path.
local function is_paused(queue)
    local now = time.now()
    if now - _paused_at >= 1 then
        _paused = {}
        local rows = db.query("SELECT name FROM _hull_queue WHERE paused=1")
        for _, r in ipairs(rows) do _paused[r.name] = true end
        _paused_at = now
    end
    return _paused[queue] == true
end

local function set_paused(queue, v)
    local n = db.exec("UPDATE _hull_queue SET paused=? WHERE name=?", { v, queue })
    if (n or 0) == 0 then
        db.exec("INSERT INTO _hull_queue (name, paused) VALUES (?, ?)", { queue, v })
    end
    _paused_at = 0   -- invalidate this process's cache so the change is seen now
    return jobs
end

--- Pause a queue: workers stop claiming from it (in-flight jobs finish; enqueue
-- still works). Durable + fleet-wide. Takes effect within ~1s on other workers.
-- @tparam string queue
-- @treturn table the jobs module (for chaining)
function jobs.pause(queue) return set_paused(queue, 1) end

--- Resume a paused queue.
-- @tparam string queue
-- @treturn table the jobs module (for chaining)
function jobs.resume(queue) return set_paused(queue, 0) end

--- Delete jobs from a queue (the "clear the backlog" op). Defaults to `pending`
-- only, leaving in-flight and terminal rows; pass opts.statuses to widen (e.g.
-- { "pending", "running", "done", "dead" }). Returns the number deleted.
-- @tparam string queue
-- @tparam[opt] table opts  { statuses = { "pending" } }
-- @treturn number
function jobs.purge(queue, opts)
    opts = opts or {}
    local statuses = opts.statuses or { "pending" }
    local ph, params = {}, { queue }
    for _, s in ipairs(statuses) do ph[#ph + 1] = "?"; params[#params + 1] = s end
    return db.exec(
        "DELETE FROM _hull_jobs WHERE queue=? AND status IN (" .. table.concat(ph, ",") .. ")",
        params) or 0
end

-- Resolve opts.queue / opts.queues to an ordered list of queues to try. A single
-- `queue` (or the default) is a one-element order. `queues` may be:
--   * a LIST { "critical", "default", "low" } - strict priority: try each in
--     order, claim from the first with ready work (higher queues drain first).
--   * a MAP { critical=3, default=2, low=1 } - weighted fairness: the order is a
--     weighted-random shuffle (first-choice ~ weight over many calls), with every
--     queue still present as a fallback so no claim cycle is wasted and no queue
--     starves. A homogeneous fleet gives fleet-wide fairness (each worker draws
--     independently). Zero/negative weights and non-number values are dropped.
local function resolve_queue_order(opts)
    local qs = opts.queues
    if qs == nil then return { opts.queue or "default" } end
    if qs[1] ~= nil then                          -- list form (strict priority)
        local order = {}
        for i = 1, #qs do order[i] = qs[i] end
        return order
    end
    local names, weights, total = {}, {}, 0       -- map form (weighted)
    for name, w in pairs(qs) do
        if type(w) == "number" and w > 0 then
            names[#names + 1] = name; weights[name] = w; total = total + w
        end
    end
    local order = {}
    while #names > 0 do
        local pick, acc, idx = math.random() * total, 0, #names
        for i = 1, #names do
            acc = acc + weights[names[i]]
            if pick <= acc then idx = i; break end
        end
        local chosen = names[idx]
        order[#order + 1] = chosen
        total = total - weights[chosen]
        table.remove(names, idx)
    end
    return order
end

-- Claim up to `batch` ready jobs from ONE queue (the per-backend atomic claim).
local function claim_one(queue, batch)
    if is_paused(queue) then return {} end   -- paused: don't dispatch
    local now   = time.now()
    local token = crypto.base64url_encode(crypto.random(16))
    local d = db.dialect
    -- SKIP LOCKED needs PG 9.5+ / MySQL 8+ / MariaDB 10.6+. jobs.init probes the
    -- server and clears _skip_locked on older ones, where we fall back to plain
    -- FOR UPDATE (correct - one job, one worker - but claimants block instead of
    -- skipping). nil = not probed yet -> assume supported.
    local lock = (_skip_locked ~= false) and "FOR UPDATE SKIP LOCKED" or "FOR UPDATE"

    local rows
    if d.supports_skip_locked and d.supports_returning then
        -- Postgres: one atomic statement, skips rows other workers hold.
        rows = db.query(
            "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, "
            .. "attempts=attempts+1, updated_at=? WHERE id IN ("
            .. "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? "
            .. "ORDER BY priority DESC, id LIMIT ? " .. lock .. ") "
            .. "RETURNING id, type, payload, priority, attempts, max_attempts",
            { token, now, now, queue, now, batch })
    elseif d.supports_skip_locked then
        -- MySQL: no RETURNING. Lock + mark in one txn, then read back by token.
        db.batch(function()
            local sel = db.query(
                "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? "
                .. "ORDER BY priority DESC, id LIMIT ? " .. lock,
                { queue, now, batch })
            if #sel == 0 then return end
            local ph, params = {}, { token, now, now }
            for _, r in ipairs(sel) do ph[#ph + 1] = "?"; params[#params + 1] = r.id end
            db.exec(
                "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, "
                .. "attempts=attempts+1, updated_at=? WHERE id IN (" .. table.concat(ph, ",") .. ")",
                params)
        end)
        rows = db.query(
            "SELECT id, type, payload, priority, attempts, max_attempts FROM _hull_jobs WHERE claim_token=?",
            { token })
    else
        -- SQLite: single-writer serializes the claim, so the marked-running rows
        -- are invisible to the next claimant's subquery.
        rows = db.query(
            "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, "
            .. "attempts=attempts+1, updated_at=? WHERE id IN ("
            .. "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? "
            .. "ORDER BY priority DESC, id LIMIT ?) "
            .. "RETURNING id, type, payload, priority, attempts, max_attempts",
            { token, now, now, queue, now, batch })
    end

    -- Priority-correct order WITHIN the batch: RETURNING (PG/SQLite) and the
    -- token read-back (MySQL) don't preserve the subquery's ORDER BY, so sort
    -- the claimed rows by priority DESC, id ASC before handing them out.
    rows = rows or {}
    table.sort(rows, function(x, y)
        local px, py = x.priority or 0, y.priority or 0
        if px ~= py then return px > py end
        return x.id < y.id
    end)

    local out = {}
    for _, r in ipairs(rows) do
        local j = shape(r)
        j.claim_token = token   -- handle for jobs.heartbeat on long-running work
        out[#out + 1] = j
    end
    -- Enforce a fleet-wide rate limit (claim-then-reconcile; requeues the excess).
    return rl_apply(queue, out)
end

--- Atomically claim up to `batch` ready jobs, marking them `running`.
-- Concurrency-safe across workers and processes: SKIP LOCKED on Postgres/MySQL,
-- serialized on SQLite (single-writer + WAL busy-wait). A `claim_token` nonce
-- disambiguates the claimant on backends without RETURNING. Each ready job is
-- claimed by exactly one caller. Low-level: `jobs.work` wraps this.
--
-- Draws from a single queue (`opts.queue`, default "default") or across several
-- via `opts.queues` - a LIST for strict priority or a MAP for weighted fairness
-- (see `resolve_queue_order`). Multi-queue claims from the first queue in the
-- resolved order that has ready work, so a single call returns jobs from one
-- queue; a worker loop drains the rest on subsequent calls.
--
-- @tparam[opt] table opts  { queue = "default" | queues = {...}, batch = 10 }
-- @treturn table  array of claimed jobs { id, type, data, attempts, max_attempts }
function jobs.claim(opts)
    opts = opts or {}
    local batch = opts.batch or 10
    for _, q in ipairs(resolve_queue_order(opts)) do
        local got = claim_one(q, batch)
        if #got > 0 then return got end
    end
    return {}
end

-- ── Handlers + the work loop ────────────────────────────────────────────

--- Register a handler for a job type. `jobs.work` dispatches each claimed job
-- to its type's handler. A handler receives the job `{ id, type, data,
-- attempts, max_attempts }`; returning nil/true (or any data) completes it,
-- raising an error retries it with backoff, and returning `jobs.RETRY` /
-- `jobs.DEAD` / `jobs.DISCARD` requests that outcome explicitly.
-- @tparam string job_type
-- @tparam function fn
function jobs.handler(job_type, fn)
    if type(job_type) ~= "string" or job_type == "" then
        error("jobs.handler: type must be a non-empty string")
    end
    if type(fn) ~= "function" then error("jobs.handler: handler must be a function") end
    _handlers[job_type] = fn
    return jobs
end

--- Optional catch-all for job types with no registered handler. Without it, an
-- unhandled type is dead-lettered.
-- @tparam function fn
function jobs.default(fn)
    if type(fn) ~= "function" then error("jobs.default: handler must be a function") end
    _default = fn
    return jobs
end

--- Register an in-process lifecycle listener. `event` is one of "completed",
-- "retried" (a failed attempt that will be retried with backoff), or "dead"
-- (dead-lettered: retries exhausted, handler returned jobs.DEAD, or no handler).
-- The listener is called `fn(job, info)` synchronously by `jobs.work`, in the
-- worker that processed the job, right after the outcome is applied:
--   * completed -> info = { result = <handler return value or nil> }
--   * retried   -> info = { error, attempt }   (attempt = the one that just failed)
--   * dead      -> info = { error }
-- Multiple listeners per event fire in registration order; each is isolated (a
-- throwing listener can't derail the loop or the others). Keep hooks fast and
-- synchronous - they run inline on the event-loop thread. These are per-worker,
-- for observability/metrics/side-effects; a durable fleet-wide event stream is
-- the LISTEN/NOTIFY epic (#235). Workflow cascade-failures don't emit here (the
-- failing dependency's own `dead` event is the signal).
-- @tparam string event  "completed" | "retried" | "dead"
-- @tparam function fn
function jobs.on(event, fn)
    if not _listeners[event] then
        error("jobs.on: unknown event '" .. tostring(event) .. "' (completed|retried|dead)")
    end
    if type(fn) ~= "function" then error("jobs.on: listener must be a function") end
    local L = _listeners[event]
    L[#L + 1] = fn
    return jobs
end

local function mark_done(id, result)
    db.exec("UPDATE _hull_jobs SET status='done', claim_token=NULL, updated_at=? WHERE id=?",
        { time.now(), id })
    if result ~= nil then   -- persist the handler's return value for dependents
        local enc = json.encode(result)
        local n = db.exec("UPDATE _hull_job_results SET result=? WHERE job_id=?", { enc, id })
        if (n or 0) == 0 then
            db.exec("INSERT INTO _hull_job_results (job_id, result) VALUES (?, ?)", { id, enc })
        end
    end
    resolve_deps(id, true)                                   -- unblock dependents
    db.exec("DELETE FROM _hull_job_deps WHERE dependent_id=?", { id })   -- consumed its own deps
end

local function mark_dead(id, err)
    db.exec("UPDATE _hull_jobs SET status='dead', last_error=?, claim_token=NULL, updated_at=? WHERE id=?",
        { err, time.now(), id })
    resolve_deps(id, false)                                  -- cascade to dependents
    db.exec("DELETE FROM _hull_job_deps WHERE dependent_id=?", { id })
end

-- Reschedule with backoff, or dead-letter once attempts are exhausted. `attempts`
-- was already incremented by the claim, so it is the count of the attempt that
-- just ran.
-- Returns the disposition it applied: "dead" (retries exhausted -> dead-letter)
-- or "retried" (rescheduled with backoff), so jobs.work can emit the right event.
local function mark_retry(job, err)
    local attempts = job.attempts or 0
    local max = job.max_attempts or _cfg.max_attempts
    if attempts >= max then
        mark_dead(job.id, err)
        return "dead"
    end
    local now = time.now()
    db.exec(
        "UPDATE _hull_jobs SET status='pending', run_at=?, last_error=?, claim_token=NULL, "
        .. "updated_at=? WHERE id=?",
        { now + _cfg.backoff(attempts), err, now, job.id })
    return "retried"
end

-- Fire in-process listeners for a lifecycle event. Each listener is isolated:
-- a throwing hook can't derail the work loop or the other listeners.
local function emit(event, job, info)
    local L = _listeners[event]
    for i = 1, #L do pcall(L[i], job, info) end
end

--- Reclaim jobs stuck in `running` past the visibility timeout - a worker that
-- claimed them died before completing. Reset to `pending` for reclaim (their
-- incremented attempts persist, so a job that keeps killing its worker still
-- dead-letters). `jobs.work` runs this each call.
-- @tparam[opt] table opts  { visibility_timeout = <cfg default> }
function jobs.reap(opts)
    opts = opts or {}
    local vt = opts.visibility_timeout or _cfg.visibility_timeout
    local now = time.now()
    -- Wake durable-workflow signal waits whose timeout (run_at > 0) has passed, so
    -- they re-run and return nil from ctx.wait_signal. run_at = 0 means "no
    -- timeout" (wait forever) and is left alone - only jobs.signal wakes it.
    db.exec(
        "UPDATE _hull_jobs SET status='pending', updated_at=? "
        .. "WHERE status='waiting' AND run_at > 0 AND run_at <= ?",
        { now, now })
    return db.exec(
        "UPDATE _hull_jobs SET status='pending', claim_token=NULL, updated_at=? "
        .. "WHERE status='running' AND claimed_at <= ?",
        { now, now - vt }) or 0
end

-- ── Cron (durable recurring schedules) ─────────────────────────────────────

-- Broken-down UTC calendar from a unix ts (civil algorithm; no os/time-fields
-- dependency, so it's identical across backends and platforms).
local function civil_from_days(z)
    z = z + 719468
    local era = math.floor((z >= 0 and z or z - 146096) / 146097)
    local doe = z - era * 146097
    local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524)
                - math.floor(doe / 146096)) / 365)
    local y = yoe + era * 400
    local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
    local mp = math.floor((5 * doy + 2) / 153)
    local d = doy - math.floor((153 * mp + 2) / 5) + 1
    local m = mp < 10 and mp + 3 or mp - 9
    return y + (m <= 2 and 1 or 0), m, d
end

local function decode_ts(ts)
    local days = math.floor(ts / 86400)
    local secs = ts - days * 86400
    local _, month, day = civil_from_days(days)
    return month, day, math.floor(secs / 3600), math.floor((secs % 3600) / 60),
           (days % 7 + 4) % 7   -- dow: 0=Sunday
end

-- Parse one cron field into a set over [lo,hi]. Supports *, n, a-b, */s, a-b/s,
-- and comma lists of those. Returns nil on any malformed / out-of-range part.
local function parse_field(f, lo, hi)
    local set = {}
    for part in f:gmatch("[^,]+") do
        local range, step = part:match("^([^/]+)/(%d+)$")
        step = step and tonumber(step) or 1
        range = range or part
        local a, b
        if range == "*" then
            a, b = lo, hi
        else
            local x, y = range:match("^(%d+)%-(%d+)$")
            if x then a, b = tonumber(x), tonumber(y) else a = tonumber(range); b = a end
        end
        if not a or not b or step < 1 or a < lo or b > hi or a > b then return nil end
        local v = a
        while v <= b do set[v] = true; v = v + step end
    end
    return set
end

-- Parse a 5-field cron spec (min hour dom month dow; dow 0/7=Sunday).
local function parse_cron(spec)
    if type(spec) ~= "string" then return nil, "cron spec must be a string" end
    local f = {}
    for w in spec:gmatch("%S+") do f[#f + 1] = w end
    if #f ~= 5 then return nil, "cron spec must have 5 fields" end
    local min   = parse_field(f[1], 0, 59)
    local hour  = parse_field(f[2], 0, 23)
    local dom   = parse_field(f[3], 1, 31)
    local month = parse_field(f[4], 1, 12)
    local dow   = parse_field(f[5], 0, 7)
    if not (min and hour and dom and month and dow) then return nil, "invalid cron field" end
    if dow[7] then dow[0] = true; dow[7] = nil end
    return { min = min, hour = hour, dom = dom, month = month, dow = dow,
             dom_star = (f[3] == "*"), dow_star = (f[5] == "*") }
end

-- Smallest unix ts strictly after `from_ts` whose UTC time matches `c`.
-- Coarse-to-fine skips (whole day / hour / minute) keep it fast even for rare
-- specs like "0 0 29 2 *". Returns nil if none within the search bound.
-- Resolve a cron `tz` option to a FIXED offset in seconds east of UTC. Accepts a
-- number (minutes east of UTC) or a string "+HH:MM" / "-HH:MM" / "+HHMM" / "Z".
-- IANA names ("Europe/Budapest") are rejected: a DST-correct named zone needs a
-- tz database Hull can't read inside the sandbox. So this is fixed-offset only -
-- no daylight-saving transitions.
local function parse_tz_offset(tz)
    if tz == nil then return 0 end
    if type(tz) == "number" then return math.floor(tz * 60) end
    if type(tz) == "string" then
        if tz == "Z" or tz == "UTC" or tz == "utc" then return 0 end
        local sign, hh, mm = tz:match("^([+-])(%d%d):?(%d%d)$")
        if sign then
            local off = tonumber(hh) * 3600 + tonumber(mm) * 60
            return sign == "-" and -off or off
        end
        error("jobs.cron: tz must be a fixed offset (\"+02:00\", \"-0530\", or minutes "
            .. "east of UTC); IANA zone names like '" .. tz .. "' need a tz database "
            .. "unavailable in the sandbox")
    end
    error("jobs.cron: tz must be a string offset or a number of minutes")
end

-- Next matching instant AT OR AFTER from_ts, as a UTC unix timestamp. `offset`
-- (seconds east of UTC, default 0) shifts only the calendar decode, so the spec
-- matches wall-clock fields in that fixed-offset zone while the returned value
-- stays UTC.
local function cron_next(c, from_ts, offset)
    offset = offset or 0
    local t = math.floor(from_ts / 60) * 60 + 60
    for _ = 1, 200000 do
        local month, day, hour, minute, dow = decode_ts(t + offset)
        if not c.month[month] then
            t = (math.floor(t / 86400) + 1) * 86400
        else
            local day_ok
            if c.dom_star and c.dow_star then day_ok = true
            elseif c.dom_star then day_ok = c.dow[dow]
            elseif c.dow_star then day_ok = c.dom[day]
            else day_ok = c.dom[day] or c.dow[dow] end
            if not day_ok then
                t = (math.floor(t / 86400) + 1) * 86400
            elseif not c.hour[hour] then
                t = (math.floor(t / 3600) + 1) * 3600
            elseif not c.min[minute] then
                t = t + 60
            else
                return t
            end
        end
    end
    return nil
end

-- Fire due schedules: compare-and-set next_run_at (multi-worker-safe on every
-- backend), then enqueue. Missed ticks (worker down) advance to the next future
-- occurrence - fire-once, no backfill storm. Called (throttled) from jobs.work.
local function process_cron(now)
    local due = db.query(
        "SELECT name, spec, type, payload, queue, priority, max_attempts, next_run_at, tz_offset "
        .. "FROM _hull_cron WHERE next_run_at <= ?", { now })
    for _, c in ipairs(due) do
        local parsed = parse_cron(c.spec)
        local nxt = parsed and cron_next(parsed, now, c.tz_offset) or (now + 60)
        local won = db.exec(
            "UPDATE _hull_cron SET next_run_at=?, last_run_at=?, updated_at=? "
            .. "WHERE name=? AND next_run_at=?",
            { nxt, now, now, c.name, c.next_run_at })
        if (won or 0) > 0 then
            local data
            if c.payload and c.payload ~= "" then
                local ok, d = pcall(json.decode, c.payload)
                if ok then data = d end
            end
            jobs.enqueue(c.type, data,
                { queue = c.queue, priority = c.priority, max_attempts = c.max_attempts })
        end
    end
end

--- Register (or update) a durable recurring schedule. On each matching minute,
-- exactly one worker enqueues a job (compare-and-set on the schedule row).
-- Schedules live in the DB, so they survive restarts and coordinate across the
-- fleet; a worker (poller or `run_worker`) must be running to fire them.
-- @tparam string name  unique schedule id; also the enqueued job type by default
-- @tparam string spec  5-field cron: "min hour dom month dow" (UTC; dow 0/7=Sun)
-- @tparam[opt] any data  payload for the enqueued job
-- @tparam[opt] table opts  { type, queue, priority, max_attempts }
-- @treturn table the jobs module (for chaining)
function jobs.cron(name, spec, data, opts)
    if type(name) ~= "string" or name == "" then
        error("jobs.cron: name must be a non-empty string")
    end
    local parsed, err = parse_cron(spec)
    if not parsed then error("jobs.cron: " .. (err or "invalid cron spec")) end
    opts = opts or {}
    local now = time.now()
    local tz_offset = parse_tz_offset(opts.tz)
    local nxt = cron_next(parsed, now, tz_offset)
    if not nxt then error("jobs.cron: spec has no upcoming occurrence") end
    local job_type = opts.type or name
    local payload  = data ~= nil and json.encode(data) or nil
    local queue    = opts.queue or "default"
    local priority = opts.priority or 0
    local n = db.exec(
        "UPDATE _hull_cron SET spec=?, type=?, payload=?, queue=?, priority=?, "
        .. "max_attempts=?, next_run_at=?, tz_offset=?, updated_at=? WHERE name=?",
        { spec, job_type, payload, queue, priority, opts.max_attempts, nxt, tz_offset, now, name })
    if (n or 0) == 0 then
        db.exec(
            "INSERT INTO _hull_cron (name, spec, type, payload, queue, priority, "
            .. "max_attempts, next_run_at, tz_offset, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            { name, spec, job_type, payload, queue, priority, opts.max_attempts, nxt, tz_offset, now })
    end
    return jobs
end

--- Remove a cron schedule by name. Returns whether a schedule was removed.
-- @tparam string name
-- @treturn boolean
function jobs.uncron(name)
    return (db.exec("DELETE FROM _hull_cron WHERE name=?", { name }) or 0) > 0
end

-- Internal seams for tests / introspection; not part of the app-facing contract.
jobs._cron_next = function(spec, from, offset)
    local p = parse_cron(spec)
    return p and cron_next(p, from or time.now(), offset) or nil
end
jobs._tick = function(now) process_cron(now or time.now()) end

--- Claim a batch and run each job's handler, applying the outcome (done /
-- retry-with-backoff / dead-letter). Runs the reaper first. Drive it from a
-- timer (`app.every(1000, function() jobs.work() end)`) or a dedicated worker
-- (`jobs.run_worker`). Idempotent handlers required: a job may run more than
-- once (crash-then-reclaim). Returns the number of jobs processed this call.
-- @tparam[opt] table opts  { queue, batch, visibility_timeout }
-- @treturn number
function jobs.work(opts)
    opts = opts or {}
    -- Throttle the reaper: a no-op sweep still takes the write lock, so under a
    -- fast poller x N workers reaping every tick adds needless contention.
    local interval = opts.reap_interval or _cfg.reap_interval
    local now = time.now()
    if now - _last_reap >= interval then
        jobs.reap(opts)
        _last_reap = now
    end
    -- Fire due cron schedules (throttled; cron is minute-grained).
    if now - _last_cron >= 1 then
        process_cron(now)
        _last_cron = now
    end
    local batch = jobs.claim(opts)
    for _, job in ipairs(batch) do
        job.deps = load_deps(job.id)   -- workflow: dependency results, in order
        local h = _handlers[job.type] or _default
        if not h then
            local err = "no handler for job type '" .. tostring(job.type) .. "'"
            mark_dead(job.id, err)
            emit("dead", job, { error = err })
        else
            local ok, result = pcall(h, job)
            if ok and type(result) == "table" and result.__hull_wf_yield then
                -- Durable workflow yielded - no terminal outcome, no event. A
                -- yield is not a failed attempt, so undo the claim's attempt
                -- increment (a workflow may sleep / wait many times without
                -- exhausting max_attempts).
                local wf_now = time.now()
                if result.waiting then
                    -- ctx.wait_signal: park in the non-terminal 'waiting' status
                    -- (excluded by the claim query). run_at carries the optional
                    -- timeout deadline (0 = none); the reaper wakes a timed-out
                    -- wait, jobs.signal wakes a delivered one.
                    db.exec("UPDATE _hull_jobs SET status='waiting', run_at=?, "
                        .. "attempts=attempts-1, claim_token=NULL, updated_at=? WHERE id=?",
                        { result.deadline or 0, wf_now, job.id })
                    -- Close the deliver-before-park race: a signal delivered in the
                    -- check->park window couldn't re-activate us (we were 'running'),
                    -- so re-check now that we are 'waiting'.
                    if result.signal_name then
                        local sig = db.query("SELECT 1 AS x FROM _hull_workflow_signals "
                            .. "WHERE workflow_id=? AND name=? AND consumed_at IS NULL",
                            { job.id, result.signal_name })
                        if sig and #sig > 0 then
                            db.exec("UPDATE _hull_jobs SET status='pending', run_at=?, "
                                .. "updated_at=? WHERE id=? AND status='waiting'",
                                { wf_now, wf_now, job.id })
                        end
                    end
                else
                    -- ctx.sleep: future-dated pending job.
                    db.exec("UPDATE _hull_jobs SET status='pending', run_at=?, "
                        .. "attempts=attempts-1, claim_token=NULL, updated_at=? WHERE id=?",
                        { result.wake_at or wf_now, wf_now, job.id })
                end
            elseif not ok then
                local err = tostring(result)
                emit(mark_retry(job, err), job, { error = err, attempt = job.attempts })
            elseif result == jobs.DEAD then
                mark_dead(job.id, "handler returned jobs.DEAD")
                emit("dead", job, { error = "handler returned jobs.DEAD" })
            elseif result == jobs.RETRY then
                emit(mark_retry(job, "handler requested retry"), job,
                     { error = "handler requested retry", attempt = job.attempts })
            else
                -- nil / true / jobs.DISCARD -> done, no result; any other return
                -- value is stored as the job's result (for dependents).
                local res
                if result ~= nil and result ~= true and result ~= jobs.DISCARD then
                    res = result
                end
                mark_done(job.id, res)
                emit("completed", job, { result = res })
            end
        end
    end
    return #batch
end

-- Worker loop control: jobs.stop() flips this so a running jobs.run_worker
-- returns after its current iteration (graceful shutdown from a handler,
-- signal wrapper, or timer). A fresh run_worker resets it to true.
local _running = false

--- Request the running `jobs.run_worker` loop to stop after the current
-- iteration. No-op if no worker is running. Safe to call from a handler.
function jobs.stop()
    _running = false
end

--- Blocking claim loop: the dedicated-worker execution model. Call it from
-- `app.main` (`app.main(function() jobs.run_worker() end)`) and run the app as
-- its own process (`hull app.lua`, or `hull jobs worker app.lua`); run K copies
-- for horizontal scale - each claims disjoint jobs via the atomic claim. Each
-- iteration runs `jobs.work` (claim a batch, dispatch, reap); when a claim
-- comes back empty it sleeps `poll_ms` before polling again (yielding to the
-- event loop, so async handlers and timers keep running). Returns the total
-- number of jobs processed when the loop exits.
--
-- `concurrency` (default 1) runs N independent claim-loops in this one process
-- (via hull.async), so up to N handlers are in flight at once - real
-- intra-process parallelism for I/O-bound handlers (each loop's `jobs.work`
-- claims disjoint jobs via the atomic claim). Orthogonal to running K processes;
-- they multiply.
--
-- Exit conditions: `jobs.stop()` (graceful), or - for bounded / batch-drain
-- runs - `opts.drain` / `opts.max_empty_polls`. With neither, it runs until
-- stopped or the process is signalled (an in-flight job is then reclaimed by
-- the visibility-timeout reaper, since handlers are at-least-once).
-- @tparam[opt] table opts  { queue, batch, concurrency, poll_ms,
--                            visibility_timeout, drain, max_empty_polls }
-- @treturn number  total jobs processed
function jobs.run_worker(opts)
    opts = opts or {}
    local poll_ms = opts.poll_ms or 1000
    -- drain = "exit as soon as the queue is empty" (== max_empty_polls 1).
    local max_empty = opts.max_empty_polls or (opts.drain and 1 or 0)
    local concurrency = (opts.concurrency and opts.concurrency > 1) and opts.concurrency or 1
    _running = true

    -- Shared across loops (single event-loop thread, so no data race).
    local total, active = 0, concurrency
    local function loop()
        local empty = 0
        while _running do
            local n = jobs.work(opts)
            total = total + n
            if n == 0 then
                empty = empty + 1
                if max_empty > 0 and empty >= max_empty then break end
                hull.sleep(poll_ms)
            else
                empty = 0
            end
        end
        active = active - 1
    end

    if concurrency == 1 then
        loop()
    else
        -- N-1 detached loops (hull.async has no join) + 1 inline. Every loop
        -- exits on the same _running flag / drain, and `active` counts down as
        -- each finishes, so we can wait for all to settle before returning.
        for _ = 1, concurrency - 1 do hull.async(loop) end
        loop()
        while active > 0 do hull.sleep(5) end
    end
    return total
end

--- Count jobs by status (optionally scoped to a queue). The ops overview.
-- @tparam[opt] table opts  { queue }
-- @treturn table  { pending, running, done, dead }
function jobs.stats(opts)
    opts = opts or {}
    local rows
    if opts.queue then
        rows = db.query(
            "SELECT status, COUNT(*) AS c FROM _hull_jobs WHERE queue=? GROUP BY status",
            { opts.queue })
    else
        rows = db.query("SELECT status, COUNT(*) AS c FROM _hull_jobs GROUP BY status")
    end
    local s = { pending = 0, running = 0, done = 0, dead = 0, blocked = 0 }
    for _, r in ipairs(rows) do s[r.status] = r.c end
    return s
end

-- Decode an ops row into an inspection view: the handler-facing shape plus the
-- bookkeeping columns an operator needs (status, queue, last_error, timestamps).
local function shape_ops(r)
    local j = shape(r)
    j.status     = r.status
    j.queue      = r.queue
    j.priority   = r.priority
    j.attempts   = r.attempts
    j.run_at     = r.run_at
    j.last_error = r.last_error
    j.created_at = r.created_at
    j.updated_at = r.updated_at
    j.progress   = r.progress
    return j
end

--- Set a running job's progress percent (0-100), surfaced by
-- `jobs.get(id).progress`. Advisory - a UI/ops hint for long jobs, orthogonal
-- to the claim/heartbeat. The value is clamped to [0,100] and rounded. Call it
-- from a handler with `job.id`. Returns the clamped value stored.
-- @tparam number id
-- @tparam number pct  0..100
-- @treturn number  the clamped value written
function jobs.progress(id, pct)
    local p = tonumber(pct) or 0
    if p < 0 then p = 0 elseif p > 100 then p = 100 end
    p = math.floor(p + 0.5)
    db.exec("UPDATE _hull_jobs SET progress=?, updated_at=? WHERE id=?", { p, time.now(), id })
    return p
end

--- Fetch a single job by id (its full status view), or nil if it doesn't exist.
-- The only way to inspect an individual job from app code: `_hull_jobs` is in the
-- protected namespace, so a direct query is blocked. Poll this for a terminal
-- status, or use `jobs.result` / `jobs.await` to fetch a finished job's return
-- value (a handler's non-nil return is persisted as the job's result).
-- @tparam number id
-- @treturn table|nil  { id, type, data, status, queue, priority, attempts,
--                       max_attempts, run_at, last_error, created_at, updated_at }
function jobs.get(id)
    local rows = db.query("SELECT * FROM _hull_jobs WHERE id=?", { id })
    if not rows or #rows == 0 then return nil end
    return shape_ops(rows[1])
end

--- Fetch a job's terminal result without the full status view. Returns nil when
-- the job is unknown (never enqueued, or already purged by `jobs.cleanup`);
-- otherwise `{ status, result?, error? }`:
--   * a `done` job carries `result` = the handler's decoded return value (absent
--     when the handler returned nil / true / jobs.DISCARD),
--   * a `dead` job carries `error` = its last error string,
--   * a still-running (`pending`/`running`/`blocked`) job carries neither.
-- This is the standalone counterpart to a workflow dependent's injected
-- `job.deps[i].result`: any job's return value is readable here, not only a
-- dependency's. The result lives as long as the job row (governed by
-- `jobs.cleanup`), so read it before the terminal row is purged.
-- @tparam number id
-- @treturn table|nil
function jobs.result(id)
    local rows = db.query("SELECT status, last_error FROM _hull_jobs WHERE id=?", { id })
    if not rows or #rows == 0 then return nil end
    local status = rows[1].status
    local out = { status = status }
    if status == "done" then
        local rr = db.query("SELECT result FROM _hull_job_results WHERE job_id=?", { id })
        if rr and #rr > 0 and rr[1].result ~= nil then
            -- Fail soft on a corrupted result row (we wrote valid JSON, but don't
            -- raise on external corruption). Explicit if: a decoded false/nil is
            -- a valid result, so `ok and decoded or raw` would mis-map it.
            local ok, decoded = pcall(json.decode, rr[1].result)
            if ok then out.result = decoded else out.result = rr[1].result end
        end
    elseif status == "dead" then
        out.error = rows[1].last_error
    end
    return out
end

--- Block (yielding to the event loop) until a job reaches a terminal status,
-- then return its `jobs.result`. For the "enqueue then wait for the value"
-- pattern; it polls `jobs.result` every `opts.interval` ms (default 100),
-- sleeping via `hull.sleep` between polls so other work keeps running. With
-- `opts.timeout` (ms) it gives up after that budget and returns the last
-- (non-terminal) result view instead of the terminal one; without a timeout it
-- waits indefinitely. Returns nil immediately if the job is unknown. Requires a
-- yielding context (`app.main`, a handler, a timer) - a worker still processes
-- jobs while another coroutine awaits.
-- @tparam number id
-- @tparam[opt] table opts  { timeout = <ms>, interval = <ms> }
-- @treturn table|nil  the terminal `jobs.result`, a timed-out view, or nil
function jobs.await(id, opts)
    opts = opts or {}
    local interval = math.max(opts.interval or 100, 10)   -- floor: never a tight yield-loop
    local deadline = opts.timeout and (time.clock() + opts.timeout) or nil
    while true do
        local r = jobs.result(id)
        if r == nil then return nil end                     -- unknown / purged
        if r.status == "done" or r.status == "dead" then return r end
        if deadline and time.clock() >= deadline then return r end
        hull.sleep(interval)
    end
end

-- ── Durable execution: workflow-as-code (Phase 1a) ──────────────────────────
-- A durable workflow is a normal function fn(ctx) run as a job of the reserved
-- type "__wf:<name>". Each ctx.step(name, fn) memoizes its result in
-- _hull_workflow_steps; if the workflow re-runs (a crash or a retry re-enters it
-- from the top), completed steps return their stored result instead of
-- re-executing, so the body resumes where it left off. Design:
-- docs/jobs_durable_execution_design.md.
local WF_PREFIX = "__wf:"

-- Run (or replay) one memoized step. Returns fn()'s value; on a re-run of the
-- workflow the value comes from the store and fn is NOT called again.
local function run_step(workflow_id, step_key, fn)
    if type(step_key) ~= "string" or step_key == "" then
        error("ctx.step: name must be a non-empty string")
    end
    if type(fn) ~= "function" then error("ctx.step: fn must be a function") end
    local rows = db.query(
        "SELECT result FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
        { workflow_id, step_key })
    if rows and #rows > 0 then
        if rows[1].result == nil then return nil end
        local ok, decoded = pcall(json.decode, rows[1].result)
        if ok then return decoded else return rows[1].result end
    end
    -- First execution: run, then persist. At-least-once - a crash between the
    -- side effect and this INSERT re-runs the step on resume (steps must be
    -- idempotent, same contract as a job handler).
    local result = fn()
    local enc = result ~= nil and json.encode(result) or nil
    db.exec(
        "INSERT INTO _hull_workflow_steps (workflow_id, step_key, result, status, created_at) "
        .. "VALUES (?, ?, ?, 'done', ?)",
        { workflow_id, step_key, enc, time.now() })
    return result
end

-- Durable timer. `n` is the ordinal of this sleep in the workflow body (stable
-- across re-runs, since the step-call sequence is stable). On first encounter it
-- records the wake time and raises the yield sentinel (the runner reschedules the
-- job to run_at = wake_at); on a resume the recorded wake time is read, and once
-- it has passed the call returns and the body continues past it.
local function run_sleep(workflow_id, n, seconds)
    local key = "__sleep:" .. n
    local rows = db.query(
        "SELECT result FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
        { workflow_id, key })
    local wake_at
    if rows and #rows > 0 then
        wake_at = tonumber(rows[1].result)
    else
        wake_at = time.now() + (tonumber(seconds) or 0)
        db.exec(
            "INSERT INTO _hull_workflow_steps (workflow_id, step_key, result, status, created_at) "
            .. "VALUES (?, ?, ?, 'sleeping', ?)",
            { workflow_id, key, tostring(wake_at), time.now() })
    end
    if time.now() >= wake_at then return end   -- elapsed: continue
    error({ __hull_yield = true, wake_at = wake_at })   -- caught by the runner
end

-- Wait for an external signal (jobs.signal). If the named signal is present it is
-- consumed and its payload returned; otherwise the workflow yields to the
-- non-terminal 'waiting' status (re-activated by jobs.signal). With opts.timeout
-- the wait also arms a deadline (stored once, stable across resumes) so the
-- reaper wakes it if no signal arrives; a timed-out wait returns nil.
local function run_wait_signal(workflow_id, name, opts)
    if type(name) ~= "string" or name == "" then
        error("ctx.wait_signal: name must be a non-empty string")
    end
    local rows = db.query(
        "SELECT payload FROM _hull_workflow_signals WHERE workflow_id=? AND name=? AND consumed_at IS NULL",
        { workflow_id, name })
    if rows and #rows > 0 then
        db.exec("UPDATE _hull_workflow_signals SET consumed_at=? WHERE workflow_id=? AND name=?",
            { time.now(), workflow_id, name })
        if rows[1].payload == nil then return nil end
        local ok, decoded = pcall(json.decode, rows[1].payload)
        if ok then return decoded else return rows[1].payload end
    end
    local deadline = 0
    if opts and opts.timeout then
        local key = "__waitdl:" .. name
        local drows = db.query(
            "SELECT result FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
            { workflow_id, key })
        if drows and #drows > 0 then
            deadline = tonumber(drows[1].result) or 0
        else
            deadline = time.now() + opts.timeout
            db.exec(
                "INSERT INTO _hull_workflow_steps (workflow_id, step_key, result, status, created_at) "
                .. "VALUES (?, ?, ?, 'waiting', ?)",
                { workflow_id, key, tostring(deadline), time.now() })
        end
        if time.now() >= deadline then return nil end   -- timed out, no signal
    end
    error({ __hull_yield = true, waiting = true, signal_name = name, deadline = deadline })
end

-- Run the compensations of completed steps in reverse order (saga rollback). Each
-- runs at most once (marked 'compensated'), so a crash mid-rollback resumes.
local function run_compensations(comps, workflow_id)
    for i = #comps, 1, -1 do
        local c = comps[i]
        local done = db.query(
            "SELECT status FROM _hull_workflow_steps WHERE workflow_id=? AND step_key=?",
            { workflow_id, c.key })
        if not (done and #done > 0 and done[1].status == "compensated") then
            pcall(c.fn)   -- at-least-once; compensations must be idempotent
            db.exec("UPDATE _hull_workflow_steps SET status='compensated' WHERE workflow_id=? AND step_key=?",
                { workflow_id, c.key })
        end
    end
end

-- Build the durable ctx handed to a workflow function. `sleep_n` is a per-run
-- counter captured by ctx.sleep; `comps` collects saga compensations (registered
-- by every ctx.step call that has one, so on the terminal-failure run the list
-- covers all completed steps).
local function make_ctx(job, name)
    local sleep_n = 0
    local comps = {}
    local ctx = {
        id    = job.id,
        name  = name,
        input = job.data,
        trace = job.trace,   -- trace-context propagation (observability design)
        _comps = comps,
    }
    ctx.step = function(step_key, fn, opts)
        local result = run_step(job.id, step_key, fn)
        if opts and type(opts.compensate) == "function" then
            comps[#comps + 1] = { key = step_key, fn = opts.compensate }
        end
        return result
    end
    ctx.sleep = function(seconds)
        sleep_n = sleep_n + 1
        return run_sleep(job.id, sleep_n, seconds)
    end
    ctx.wait_signal = function(signal_name, opts) return run_wait_signal(job.id, signal_name, opts) end
    return ctx
end

--- Register a durable workflow. `fn(ctx)` is the workflow body; its return value
-- becomes the workflow's result (fetch via jobs.await / jobs.result). Inside it,
-- `ctx.step(name, fn)` runs `fn` once and memoizes the result, so a crashed or
-- retried workflow resumes past completed steps. `ctx.input` is the payload from
-- jobs.start, `ctx.id` the workflow id. A workflow instance IS a job (reserved
-- type "__wf:<name>"), so it inherits claim / reaper / retry / worker / result.
-- @tparam string name
-- @tparam function fn   function(ctx) -> result
function jobs.workflow(name, fn)
    if type(name) ~= "string" or name == "" then
        error("jobs.workflow: name must be a non-empty string")
    end
    if type(fn) ~= "function" then error("jobs.workflow: fn must be a function") end
    _workflows[name] = fn
    -- The workflow runs as a normal handler for its reserved type. A ctx.sleep /
    -- ctx.wait_signal raises the yield sentinel; catch it and hand work() the
    -- yield marker (sleep -> reschedule pending; signal -> 'waiting'). A real
    -- error re-raises for the normal retry path - but on the LAST attempt (about
    -- to dead-letter) or an explicit jobs.DEAD, run the saga compensations first.
    jobs.handler(WF_PREFIX .. name, function(job)
        local ctx = make_ctx(job, name)
        local ok, res = pcall(fn, ctx)
        if ok then
            if res == jobs.DEAD then run_compensations(ctx._comps, job.id) end
            return res
        end
        if type(res) == "table" and res.__hull_yield then
            return { __hull_wf_yield = true, wake_at = res.wake_at,
                     waiting = res.waiting, signal_name = res.signal_name,
                     deadline = res.deadline }
        end
        local max = job.max_attempts or _cfg.max_attempts
        if (job.attempts or 0) >= max then run_compensations(ctx._comps, job.id) end
        error(res, 0)
    end)
    return jobs
end

--- Start a durable workflow instance. Returns the workflow id (a job id); the
-- input is the workflow's `ctx.input`. Accepts the usual enqueue opts (queue,
-- priority, dedup_key for an idempotent start, delay/at). Poll jobs.workflow_
-- status(id) or jobs.await(id) for the outcome.
-- @tparam string name
-- @tparam[opt] table input
-- @tparam[opt] table opts   enqueue options
-- @treturn number|nil  the workflow id, or nil if a dedup_key collapsed it
function jobs.start(name, input, opts)
    if not _workflows[name] then
        error("jobs.start: unknown workflow '" .. tostring(name) .. "' (register it with jobs.workflow)")
    end
    return jobs.enqueue(WF_PREFIX .. name, input, opts)
end

--- Deliver a signal to a durable workflow (see ctx.wait_signal). Records the
-- named payload (first delivery per name wins) and re-activates the workflow if
-- it is parked waiting for it. Safe to call before the workflow reaches the wait
-- (the signal is stored and consumed when it gets there) - no lost-signal race.
-- Returns true.
-- @tparam number id       the workflow id
-- @tparam string name     the signal name
-- @tparam[opt] any payload
-- @treturn boolean
function jobs.signal(id, name, payload)
    if type(name) ~= "string" or name == "" then
        error("jobs.signal: name must be a non-empty string")
    end
    local enc = payload ~= nil and json.encode(payload) or nil
    local now = time.now()
    db.insert_if_absent("_hull_workflow_signals", { "workflow_id", "name" },
        { "workflow_id", "name", "payload", "created_at" }, { id, name, enc, now })
    -- Re-activate a parked wait (waiting -> pending). A 'running' or unrelated
    -- 'pending' workflow is left alone: it finds the stored signal on its own.
    db.exec("UPDATE _hull_jobs SET status='pending', run_at=?, updated_at=? "
        .. "WHERE id=? AND status='waiting'", { now, now, id })
    return true
end

--- Query a workflow instance's state (DB-derived, so it works even when no
-- worker is currently running it). Returns nil for an unknown id, else
-- { status, name, steps_done, started_at, updated_at, result?, error? }.
-- @tparam number id
-- @treturn table|nil
function jobs.workflow_status(id)
    local job = jobs.get(id)
    if not job then return nil end
    local rows = db.query(
        "SELECT step_key FROM _hull_workflow_steps WHERE workflow_id=? ORDER BY created_at, step_key",
        { id })
    local steps_done = {}
    for _, s in ipairs(rows or {}) do
        -- Hide internal keys (durable-sleep markers "__sleep:N"); only user steps.
        if not tostring(s.step_key):match("^__") then
            steps_done[#steps_done + 1] = s.step_key
        end
    end
    local out = {
        status     = job.status,
        name       = tostring(job.type):match("^" .. WF_PREFIX .. "(.+)$") or job.type,
        steps_done = steps_done,
        started_at = job.created_at,
        updated_at = job.updated_at,
    }
    -- What the workflow is parked on: a signal ('waiting') or a durable timer
    -- (pending with a future run_at).
    if job.status == "waiting" then
        out.waiting_for = "signal"
    elseif job.status == "pending" and job.run_at and job.run_at > time.now() then
        out.waiting_for = "sleep:" .. job.run_at
    end
    if job.status == "done" then
        local r = jobs.result(id)
        if r then out.result = r.result end
    elseif job.status == "dead" then
        out.error = job.last_error
    end
    return out
end

--- Extend the claim on a job the current handler is processing (heartbeat). A
-- handler whose work may exceed `visibility_timeout` should call this
-- periodically (at least every visibility_timeout/2 s) so the reaper doesn't
-- presume it orphaned and re-run it elsewhere. Bumps `claimed_at` only while
-- THIS worker still owns the claim (guarded by the job's claim_token): returns
-- false once the claim has been lost (already reaped / re-claimed / finished),
-- which is the handler's signal to stop and let the other runner win.
-- @tparam table job  the job object passed to the handler
-- @treturn boolean  true if the claim was extended, false if it is no longer held
function jobs.heartbeat(job)
    if type(job) ~= "table" or job.id == nil or job.claim_token == nil then
        return false
    end
    local now = time.now()
    local n = db.exec(
        "UPDATE _hull_jobs SET claimed_at=?, updated_at=? "
        .. "WHERE id=? AND claim_token=? AND status='running'",
        { now, now, job.id, job.claim_token })
    return (n or 0) > 0
end

--- List dead-lettered jobs (status='dead'), newest first. The ops entry point
-- for inspecting failures before requeuing (`jobs.retry`) or purging
-- (`jobs.cleanup`).
-- @tparam[opt] table opts  { queue, limit = 100, offset = 0 }
-- @treturn table  array of { id, queue, type, data, attempts, max_attempts,
--                            last_error, created_at, updated_at }
function jobs.dead(opts)
    opts = opts or {}
    local limit  = opts.limit or 100
    local offset = opts.offset or 0
    local rows
    if opts.queue then
        rows = db.query(
            "SELECT * FROM _hull_jobs WHERE status='dead' AND queue=? "
            .. "ORDER BY updated_at DESC, id DESC LIMIT ? OFFSET ?",
            { opts.queue, limit, offset })
    else
        rows = db.query(
            "SELECT * FROM _hull_jobs WHERE status='dead' "
            .. "ORDER BY updated_at DESC, id DESC LIMIT ? OFFSET ?",
            { limit, offset })
    end
    local out = {}
    for _, r in ipairs(rows) do out[#out + 1] = shape_ops(r) end
    return out
end

--- Requeue a dead-lettered job for another run. Resets it to `pending` with a
-- fresh attempt budget (attempts=0) and clears the last error. No-op unless the
-- job exists and is currently `dead` (so it can't double-requeue a live job).
-- @tparam number id
-- @treturn boolean  true if a dead job was requeued
function jobs.retry(id)
    local now = time.now()
    local n = db.exec(
        "UPDATE _hull_jobs SET status='pending', run_at=?, attempts=0, "
        .. "claim_token=NULL, claimed_at=NULL, last_error=NULL, updated_at=? "
        .. "WHERE id=? AND status='dead'",
        { now, now, id })
    return (n or 0) > 0
end

--- Cancel a not-yet-started job by id: delete it if it is still `pending`
-- (covers delayed / scheduled jobs). A `running` job is mid-flight and is NOT
-- cancelled (let it finish or dead-letter). Returns whether a row was removed.
-- @tparam number id
-- @treturn boolean
function jobs.cancel(id)
    return (db.exec("DELETE FROM _hull_jobs WHERE id=? AND status='pending'", { id }) or 0) > 0
end

--- Purge terminal jobs (done + dead by default) whose last update is older than
-- a retention age. Run it from `app.daily` to bound table growth. Only touches
-- terminal rows, so it never races a pending / running job.
-- @tparam[opt] table opts
-- @tparam[opt="default order"] string opts.queue        scope to one queue
-- @tparam[opt=604800] number opts.older_than             retention seconds (7 days)
-- @tparam[opt] number opts.before                        absolute cutoff ts (overrides older_than)
-- @tparam[opt={"done","dead"}] table opts.statuses       which terminal statuses to purge
-- @treturn number  rows deleted
function jobs.cleanup(opts)
    opts = opts or {}
    local statuses = opts.statuses or { "done", "dead" }
    local cutoff = opts.before or (time.now() - (opts.older_than or 604800))
    local placeholders, params = {}, {}
    for _, s in ipairs(statuses) do
        placeholders[#placeholders + 1] = "?"
        params[#params + 1] = s
    end
    params[#params + 1] = cutoff
    local sql = "DELETE FROM _hull_jobs WHERE status IN ("
        .. table.concat(placeholders, ",") .. ") AND updated_at < ?"
    if opts.queue then
        sql = sql .. " AND queue=?"
        params[#params + 1] = opts.queue
    end
    local deleted = db.exec(sql, params) or 0
    -- Drop results + workflow steps whose producing job is gone (store hygiene).
    db.exec("DELETE FROM _hull_job_results WHERE job_id NOT IN (SELECT id FROM _hull_jobs)")
    db.exec("DELETE FROM _hull_workflow_steps WHERE workflow_id NOT IN (SELECT id FROM _hull_jobs)")
    db.exec("DELETE FROM _hull_workflow_signals WHERE workflow_id NOT IN (SELECT id FROM _hull_jobs)")
    return deleted
end

return jobs
