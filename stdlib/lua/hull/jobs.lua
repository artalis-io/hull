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
-- `hull jobs worker`), and the ops surface (jobs.stats / dead / retry / cancel / cleanup).
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

-- Unix ts of the last reaper sweep (throttled in jobs.work via _cfg.reap_interval).
local _last_reap = 0

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
        .. "updated_at   INTEGER      NOT NULL)")

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

    return jobs
end

--- Enqueue a job. A plain INSERT, so calling it inside a `db.batch()` commits
-- the job atomically with the business row (transactional coupling - the reason
-- jobs is DB-backed, not an external broker).
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
    local run_at = opts.run_at or (now + (opts.delay or 0))
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
    if opts.dedup_key ~= nil then
        -- INSERT ... ON CONFLICT(queue,dedup_key) DO NOTHING / INSERT OR IGNORE.
        local n = db.insert_if_absent("_hull_jobs", { "queue", "dedup_key" }, cols, vals)
        if n and n > 0 then return db.last_id() end
        return nil   -- an un-run job with this (queue, dedup_key) already exists
    end
    db.exec(
        "INSERT INTO _hull_jobs (queue, type, payload, priority, max_attempts, "
        .. "run_at, dedup_key, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        vals)
    return db.last_id()
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

--- Atomically claim up to `batch` ready jobs from a queue, marking them
-- `running`. Concurrency-safe across workers and processes: SKIP LOCKED on
-- Postgres/MySQL, serialized on SQLite (single-writer + WAL busy-wait). A
-- `claim_token` nonce disambiguates the claimant on backends without RETURNING.
-- Each ready job is claimed by exactly one caller. Low-level: `jobs.work`
-- wraps this; a custom worker can call it directly.
--
-- @tparam[opt] table opts  { queue = "default", batch = 10 }
-- @treturn table  array of claimed jobs { id, type, data, attempts, max_attempts }
function jobs.claim(opts)
    opts = opts or {}
    local queue = opts.queue or "default"
    local batch = opts.batch or 10
    local now   = time.now()
    local token = crypto.base64url_encode(crypto.random(16))
    local d = db.dialect

    local rows
    if d.supports_skip_locked and d.supports_returning then
        -- Postgres: one atomic statement, skips rows other workers hold.
        rows = db.query(
            "UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?, "
            .. "attempts=attempts+1, updated_at=? WHERE id IN ("
            .. "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? "
            .. "ORDER BY priority DESC, id LIMIT ? FOR UPDATE SKIP LOCKED) "
            .. "RETURNING id, type, payload, priority, attempts, max_attempts",
            { token, now, now, queue, now, batch })
    elseif d.supports_skip_locked then
        -- MySQL: no RETURNING. Lock + mark in one txn, then read back by token.
        db.batch(function()
            local sel = db.query(
                "SELECT id FROM _hull_jobs WHERE queue=? AND status='pending' AND run_at<=? "
                .. "ORDER BY priority DESC, id LIMIT ? FOR UPDATE SKIP LOCKED",
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
    for _, r in ipairs(rows) do out[#out + 1] = shape(r) end
    return out
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

local function mark_done(id)
    db.exec("UPDATE _hull_jobs SET status='done', claim_token=NULL, updated_at=? WHERE id=?",
        { time.now(), id })
end

local function mark_dead(id, err)
    db.exec("UPDATE _hull_jobs SET status='dead', last_error=?, claim_token=NULL, updated_at=? WHERE id=?",
        { err, time.now(), id })
end

-- Reschedule with backoff, or dead-letter once attempts are exhausted. `attempts`
-- was already incremented by the claim, so it is the count of the attempt that
-- just ran.
local function mark_retry(job, err)
    local attempts = job.attempts or 0
    local max = job.max_attempts or _cfg.max_attempts
    if attempts >= max then
        mark_dead(job.id, err)
        return
    end
    local now = time.now()
    db.exec(
        "UPDATE _hull_jobs SET status='pending', run_at=?, last_error=?, claim_token=NULL, "
        .. "updated_at=? WHERE id=?",
        { now + _cfg.backoff(attempts), err, now, job.id })
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
    return db.exec(
        "UPDATE _hull_jobs SET status='pending', claim_token=NULL, updated_at=? "
        .. "WHERE status='running' AND claimed_at <= ?",
        { now, now - vt }) or 0
end

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
    local batch = jobs.claim(opts)
    for _, job in ipairs(batch) do
        local h = _handlers[job.type] or _default
        if not h then
            mark_dead(job.id, "no handler for job type '" .. tostring(job.type) .. "'")
        else
            local ok, result = pcall(h, job)
            if not ok then
                mark_retry(job, tostring(result))
            elseif result == jobs.DEAD then
                mark_dead(job.id, "handler returned jobs.DEAD")
            elseif result == jobs.RETRY then
                mark_retry(job, "handler requested retry")
            else
                -- nil / true / jobs.DISCARD / any other value -> done.
                mark_done(job.id)
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
-- Exit conditions: `jobs.stop()` (graceful), or - for bounded / batch-drain
-- runs - `opts.drain` / `opts.max_empty_polls`. With neither, it runs until
-- stopped or the process is signalled (an in-flight job is then reclaimed by
-- the visibility-timeout reaper, since handlers are at-least-once).
-- @tparam[opt] table opts  { queue, batch, poll_ms, visibility_timeout,
--                            drain, max_empty_polls }
-- @treturn number  total jobs processed
function jobs.run_worker(opts)
    opts = opts or {}
    local poll_ms = opts.poll_ms or 1000
    -- drain = "exit as soon as the queue is empty" (== max_empty_polls 1).
    local max_empty = opts.max_empty_polls or (opts.drain and 1 or 0)
    _running = true
    local processed, empty = 0, 0
    while _running do
        local n = jobs.work(opts)
        processed = processed + n
        if n == 0 then
            empty = empty + 1
            if max_empty > 0 and empty >= max_empty then break end
            hull.sleep(poll_ms)
        else
            empty = 0
        end
    end
    return processed
end

--- Count jobs by status (optionally scoped to a queue). The ops overview.
-- @tparam[opt] table opts  { queue }
-- @treturn table  { pending, running, done, failed, dead }
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
    local s = { pending = 0, running = 0, done = 0, failed = 0, dead = 0 }
    for _, r in ipairs(rows) do s[r.status] = r.c end
    return s
end

-- Decode an ops row into an inspection view: the handler-facing shape plus the
-- bookkeeping columns an operator needs (queue, last_error, timestamps).
local function shape_ops(r)
    local j = shape(r)
    j.queue      = r.queue
    j.attempts   = r.attempts
    j.last_error = r.last_error
    j.created_at = r.created_at
    j.updated_at = r.updated_at
    return j
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
    return db.exec(sql, params) or 0
end

return jobs
