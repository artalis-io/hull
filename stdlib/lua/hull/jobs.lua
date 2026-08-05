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
-- `hull jobs worker`), and the ops surface (jobs.stats / dead / retry / cancel / cleanup). v1.1 adds durable cron (jobs.cron / uncron) and intra-process concurrency (run_worker concurrency=N), jobs.get(id), and jobs.heartbeat for long jobs.
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
-- Unix ts of the last cron due-check (throttled to >=1s; cron is minute-grained).
local _last_cron = 0
-- Whether the server parses SKIP LOCKED (probed in jobs.init; nil = not probed).
local _skip_locked = nil

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
        .. "updated_at   INTEGER      NOT NULL)")
    db.exec([[
        CREATE INDEX IF NOT EXISTS idx_hull_cron_due ON _hull_cron(next_run_at)
    ]])

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
local function cron_next(c, from_ts)
    local t = math.floor(from_ts / 60) * 60 + 60
    for _ = 1, 200000 do
        local month, day, hour, minute, dow = decode_ts(t)
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
        "SELECT name, spec, type, payload, queue, priority, max_attempts, next_run_at "
        .. "FROM _hull_cron WHERE next_run_at <= ?", { now })
    for _, c in ipairs(due) do
        local parsed = parse_cron(c.spec)
        local nxt = parsed and cron_next(parsed, now) or (now + 60)
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
    local nxt = cron_next(parsed, now)
    if not nxt then error("jobs.cron: spec has no upcoming occurrence") end
    local job_type = opts.type or name
    local payload  = data ~= nil and json.encode(data) or nil
    local queue    = opts.queue or "default"
    local priority = opts.priority or 0
    local n = db.exec(
        "UPDATE _hull_cron SET spec=?, type=?, payload=?, queue=?, priority=?, "
        .. "max_attempts=?, next_run_at=?, updated_at=? WHERE name=?",
        { spec, job_type, payload, queue, priority, opts.max_attempts, nxt, now, name })
    if (n or 0) == 0 then
        db.exec(
            "INSERT INTO _hull_cron (name, spec, type, payload, queue, priority, "
            .. "max_attempts, next_run_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            { name, spec, job_type, payload, queue, priority, opts.max_attempts, nxt, now })
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
jobs._cron_next = function(spec, from)
    local p = parse_cron(spec)
    return p and cron_next(p, from or time.now()) or nil
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
    local s = { pending = 0, running = 0, done = 0, dead = 0 }
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
    return j
end

--- Fetch a single job by id (its full status view), or nil if it doesn't exist.
-- The only way to inspect an individual job from app code: `_hull_jobs` is in the
-- protected namespace, so a direct query is blocked. Poll this for a terminal
-- status (jobs are fire-and-forget; there is no result backend - a handler that
-- produces output must persist it itself).
-- @tparam number id
-- @treturn table|nil  { id, type, data, status, queue, priority, attempts,
--                       max_attempts, run_at, last_error, created_at, updated_at }
function jobs.get(id)
    local rows = db.query("SELECT * FROM _hull_jobs WHERE id=?", { id })
    if not rows or #rows == 0 then return nil end
    return shape_ops(rows[1])
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
    return db.exec(sql, params) or 0
end

return jobs
