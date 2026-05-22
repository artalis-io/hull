--- Liveness + readiness HTTP endpoints.
--
-- `health.middleware()` intercepts `GET /health` and `GET /ready`:
--
--   - `/health` (liveness) — always returns `200 {status="ok", uptime}`.
--     Verifies the process is alive; never fails.
--   - `/ready`  (readiness) — runs the DB ping (if enabled) and every
--     check registered via `health.register()`. Returns `200` when all
--     pass, otherwise `503` with a per-check breakdown.
--
-- Custom check contract:
--   * `true` or no return value  → ok
--   * `false, "msg"`              → fail (optional message)
--   * raised error                → fail (uses error string)
--
-- @module hull.middleware.health
-- @license AGPL-3.0-or-later
-- @usage
--   local health = require("hull.middleware.health")
--   health.register("redis", function()
--       return redis.ping() == "PONG"
--   end)
--   app.use("GET", "/*", health.middleware())

local db = require("hull.db")
local server = require("hull.http-server")
local time = require("hull.time")

local health = {}

local _checks = {}
local _start_time = nil

--- Register a custom readiness check.
--
-- Replaces any prior check with the same name.
--
-- @function health.register
-- @tparam string name  Identifier (appears in `/ready` JSON output).
-- @tparam function fn  `function() -> ok[, msg]` — see contract above.
function health.register(name, fn)
    _checks[name] = fn
end

--- Remove a previously registered check.
-- @function health.unregister
-- @tparam string name
function health.unregister(name)
    _checks[name] = nil
end

--- Run the DB ping (if enabled) + every registered check.
--
-- @function health.run_checks
-- @tparam[opt] table opts
-- @tparam[opt=true] boolean opts.db_check  Include the DB ping.
-- @treturn table  `{ checks = { name = {status, latency_ms, error?}, ... },
--                  all_ok = boolean }`
function health.run_checks(opts)
    opts = opts or {}
    local results = {}
    local all_ok = true

    -- DB check
    if opts.db_check ~= false and db then
        local t0 = time.clock()
        local ok, err = pcall(db.query, "SELECT 1")
        local latency = math.floor((time.clock() - t0) * 10 + 0.5) / 10
        if ok then
            results.db = { status = "ok", latency_ms = latency }
        else
            results.db = { status = "fail", error = tostring(err), latency_ms = latency }
            all_ok = false
        end
    end

    -- Custom checks. Contract: fn() returns
    --   true               → ok
    --   (no return / nil)  → ok (back-compat — common Lua idiom for
    --                        success when an assert-style check passes)
    --   false[, "msg"]     → fail (optional message)
    --   true, "info"       → ok with extra info (ignored)
    --   other (number/etc) → fail to surface bugs
    --
    -- Phase 6 audit M-1: the original Phase 6 patch tightened the
    -- contract to "only `ret == true` is ok", which silently flipped
    -- every existing `function() end` check to fail. Operationally
    -- dangerous on upgrade (readiness probes go to 503). Restored
    -- back-compat: nil-on-success and bare-true are both ok.
    for name, fn in pairs(_checks) do
        local t0 = time.clock()
        local ok, ret, err_msg = pcall(fn)
        local latency = math.floor((time.clock() - t0) * 10 + 0.5) / 10
        if not ok then
            -- Check threw — `ret` is the error message.
            results[name] = { status = "fail",
                              error = type(ret) == "string" and ret or "check threw",
                              latency_ms = latency }
            all_ok = false
        elseif ret == nil or ret == true then
            -- Success: explicit `true` or implicit (no return value).
            results[name] = { status = "ok", latency_ms = latency }
        else
            -- ret is false, a string, a number, etc. — treat as failure.
            local msg = "check failed"
            if type(err_msg) == "string" then msg = err_msg
            elseif type(ret) == "string" then msg = ret end
            results[name] = { status = "fail", error = msg, latency_ms = latency }
            all_ok = false
        end
    end

    return { checks = results, all_ok = all_ok }
end

--- Build the `/health` + `/ready` middleware.
--
-- Returns `1` (short-circuit) when the request hits one of the health
-- paths and `0` (continue) otherwise. Records the start time on first
-- invocation so subsequent `/health` responses include uptime.
--
-- @function health.middleware
-- @tparam[opt] table opts
-- @tparam[opt="/health"] string opts.path_health  Liveness path.
-- @tparam[opt="/ready"]  string opts.path_ready   Readiness path.
-- @tparam[opt=true]      boolean opts.db_check    Include DB ping in `/ready`.
-- @treturn function(req, res) -> 0|1
function health.middleware(opts)
    opts = opts or {}
    local path_health = opts.path_health or "/health"
    local path_ready  = opts.path_ready or "/ready"
    local do_db_check = opts.db_check ~= false

    if not _start_time then
        _start_time = time.now()
    end

    return function(req, res)
        -- Only handle GET/HEAD
        if req.method ~= "GET" and req.method ~= "HEAD" then
            return 0
        end

        local uptime = math.floor(time.now() - _start_time)

        if req.path == path_health then
            res:json({ status = "ok", uptime = uptime })
            return 1
        end

        if req.path == path_ready then
            local result = health.run_checks({ db_check = do_db_check })

            local body = {
                status = result.all_ok and "ok" or "fail",
                checks = result.checks,
                uptime = uptime,
            }

            -- Include server stats if available
            if server and server.stats then
                body.stats = server.stats()
            end

            if result.all_ok then
                res:json(body)
            else
                res:status(503):json(body)
            end
            return 1
        end

        return 0
    end
end

return health
