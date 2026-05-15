--
-- hull.middleware.health -- Health check endpoints
--
-- Provides /health (liveness) and /ready (readiness) endpoints with
-- built-in DB ping, custom check registration, server stats, and uptime.
--
-- Usage:
--   local health = require("hull.middleware.health")
--   health.register("cache", function() return true end)
--   app.use("GET", "/*", health.middleware())
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local health = {}

local _checks = {}
local _start_time = nil

--- Register a custom health check.
-- name: string identifier for the check
-- fn: function() -> true | false, err_string
function health.register(name, fn)
    _checks[name] = fn
end

--- Remove a registered health check.
function health.unregister(name)
    _checks[name] = nil
end

--- Run all checks and return results table.
-- Returns: { checks = { name = { status, latency_ms, error? } }, all_ok = bool }
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
    --   false, "msg"       → fail with msg
    --   true, "info"       → ok with extra info (ignored)
    --   nil / other        → ambiguous; treat as fail to surface bugs
    -- M-4: the previous `ok and err ~= false` treated a nil return as
    -- success and a true-with-msg as success, but missed
    -- `false`-without-msg (which lua's `pcall` would never produce
    -- anyway) and `nil` (which it would). Promote the function return
    -- value to the source of truth on pcall success.
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
        elseif ret == true then
            results[name] = { status = "ok", latency_ms = latency }
        else
            local msg = "check failed"
            if type(err_msg) == "string" then msg = err_msg
            elseif type(ret) == "string" then msg = ret end
            results[name] = { status = "fail", error = msg, latency_ms = latency }
            all_ok = false
        end
    end

    return { checks = results, all_ok = all_ok }
end

--- Create health check middleware.
-- opts.path_health: liveness path (default "/health")
-- opts.path_ready:  readiness path (default "/ready")
-- opts.db_check:    include DB ping (default true)
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
