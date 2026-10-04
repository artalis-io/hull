--- Generic retry-with-backoff for any fallible operation.
--
-- `retry.run(fn, opts)` calls `fn` and retries it on failure with exponential
-- backoff. `fn` is ARBITRARY - a plain function, an async op that yields
-- (`http.fetch`, `compute.async.call`, `gpu.async.dispatch`, `db.async.*`), or a
-- blocking compute call. There is no HTTP coupling: to retry a fetch, wrap it
-- (`retry.run(function() return http.fetch(url) end, {...})`); the same shape
-- retries a WASM/GPU dispatch or a DB query. Sleeps between attempts via
-- `hull.sleep`, so it must run on the event-loop thread (a handler, `app.main`,
-- a timer).
--
-- Failure = `fn` raising an error, OR (when `opts.retry_on` is given) `fn`
-- returning a value the predicate flags as retryable (e.g. an HTTP 503, a
-- compute result that didn't converge).
--
-- @module hull.retry
-- @license AGPL-3.0-or-later
-- @usage
--   local retry = require("hull.retry")
--   -- retry an HTTP call on 5xx / network error:
--   local res = retry.run(function() return http.fetch(url) end, {
--       max_attempts = 4,
--       retry_on = function(r) return r.status >= 500 or r.status == 429 end,
--   })
--   -- retry a GPU dispatch that can transiently fail:
--   local out = retry.run(function() return gpu.async.dispatch("scan", opts) end)

local crypto = require("hull.crypto")

local retry = {}

local DEFAULTS = { max_attempts = 3, base_ms = 100, factor = 2, cap_ms = 30000 }

--- Backoff delay (ms) for a 1-based attempt number: `base * factor^(n-1)`,
-- clamped to `cap`, with optional full jitter (uniform in `[0, delay]`).
-- Exposed so schedulers can share one backoff curve.
-- @tparam integer attempt  1-based attempt number.
-- @tparam[opt] table opts  `base_ms` (100), `factor` (2), `cap_ms` (30000),
--   `jitter` (boolean, default false).
-- @treturn integer  Delay in milliseconds.
function retry.backoff(attempt, opts)
    opts = opts or {}
    local base = opts.base_ms or DEFAULTS.base_ms
    local factor = opts.factor or DEFAULTS.factor
    local cap = opts.cap_ms or DEFAULTS.cap_ms
    local d = base * (factor ^ (attempt - 1))
    if d > cap then d = cap end
    if opts.jitter then
        local b = { crypto.random(4):byte(1, 4) }
        local u = ((b[1] * 256 + b[2]) * 256 + b[3]) * 256 + b[4]
        d = d * (u / 4294967296.0)   -- uniform in [0, d)
    end
    return math.floor(d)
end

--- Run `fn` with retries.
--
-- @tparam function fn  The operation. ALL its return values are passed
--   through on success; a raised error or a `retry_on`-flagged result
--   triggers a retry. A failure must RAISE (or be flagged by `retry_on`): a
--   Lua-style `return nil, err` is a successful call that returned nil. To
--   retry those, pass `retry_on = function(v, err) return v == nil end`.
--   (Only the first value used to come back, so `nil, err` arrived as plain
--   `nil` with the reason lost.)
-- @tparam[opt] table opts
--   - `max_attempts`   (integer, default 3) total attempts, including the first.
--   - `base_ms` / `factor` / `cap_ms` / `jitter` - see @{retry.backoff}.
--   - `retry_on`       `function(...) -> boolean` - called with every value
--                      `fn` returned; retry on a "bad" success (e.g. an HTTP
--                      5xx, or `nil, err`). Absent: only a raised error
--                      retries.
--   - `retry_on_error` `function(err) -> boolean` - return false to treat an
--                      error as PERMANENT and re-raise immediately (no retry).
--                      Absent: every error retries.
--   - `on_retry`       `function(attempt, ok, value)` - called before each
--                      backoff sleep (for logging / metrics).
-- @return Every value the successful (or last) call of `fn` returned.
--   Re-raises the last error if all attempts errored.
function retry.run(fn, opts)
    opts = opts or {}
    local max = opts.max_attempts or DEFAULTS.max_attempts
    local last = { n = 1, false }
    for attempt = 1, max do
        local r = table.pack(pcall(fn))
        last = r
        if r[1] then
            if not (opts.retry_on and opts.retry_on(table.unpack(r, 2, r.n))) then
                return table.unpack(r, 2, r.n)
            end
        else
            if opts.retry_on_error and not opts.retry_on_error(r[2]) then
                error(r[2])
            end
        end
        if attempt < max then
            if opts.on_retry then opts.on_retry(attempt, r[1], r[2]) end
            hull.sleep(retry.backoff(attempt, opts))
        end
    end
    -- Exhausted. A trailing error re-raises; a retry_on-flagged success returns
    -- the last values (the caller's predicate can inspect them).
    if last[1] then return table.unpack(last, 2, last.n) end
    error(last[2])
end

return retry
