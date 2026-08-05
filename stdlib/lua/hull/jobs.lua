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
-- Implemented: schema + jobs.init, enqueue, the atomic claim, per-type +
-- catch-all handlers, and the work loop (retry-with-backoff, dead-letter, and
-- the visibility-timeout reaper). The dedicated `hull jobs worker` process
-- lands in a later phase.
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

-- Outcome sentinels a handler returns (consumed in Phase 3). Tables (not
-- strings) so a handler can't collide with them by returning ordinary data.
jobs.RETRY   = setmetatable({}, { __tostring = function() return "jobs.RETRY" end })
jobs.DEAD    = setmetatable({}, { __tostring = function() return "jobs.DEAD" end })
jobs.DISCARD = setmetatable({}, { __tostring = function() return "jobs.DISCARD" end })

-- Module config; defaults overridable via jobs.init(opts). Read by later phases.
local _cfg = {
    max_attempts       = 25,    -- dead-letter threshold
    visibility_timeout = 300,   -- seconds before an orphaned `running` job is reclaimed
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

--- Create the `_hull_jobs` table and its indexes. Idempotent - safe to call on
-- every boot. Uses the connection's portable identity DDL + IF-NOT-EXISTS index
-- form, so the same call runs unchanged on SQLite, PostgreSQL, and MySQL.
--
-- @tparam[opt] table opts
-- @tparam[opt=25]  number opts.max_attempts        dead-letter threshold
-- @tparam[opt=300] number opts.visibility_timeout  seconds before reclaim
-- @tparam[opt]     function opts.backoff            attempt -> delay seconds
-- @treturn table the jobs module (for chaining)
function jobs.init(opts)
    opts = opts or {}
    if opts.max_attempts ~= nil then _cfg.max_attempts = opts.max_attempts end
    if opts.visibility_timeout ~= nil then _cfg.visibility_timeout = opts.visibility_timeout end
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
-- (Phase 3) wraps this; a custom worker can call it directly.
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
-- (Phase 4). Idempotent handlers required: a job may run more than once
-- (crash-then-reclaim). Returns the number of jobs processed this call.
-- @tparam[opt] table opts  { queue, batch, visibility_timeout }
-- @treturn number
function jobs.work(opts)
    opts = opts or {}
    jobs.reap(opts)
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

--- Count jobs by status (optionally scoped to a queue). A minimal ops view;
-- the fuller dead-letter / retry / cleanup surface lands in a later phase.
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

return jobs
