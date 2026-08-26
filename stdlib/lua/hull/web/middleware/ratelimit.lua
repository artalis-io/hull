--- In-memory rate limiting middleware factory.
--
-- @module hull.web.middleware.ratelimit
-- @license AGPL-3.0-or-later
--
-- Sliding-window counter per key. **In-memory only - resets on restart.**
-- For production-grade rate limiting across multiple instances, layer
-- a DB-backed limiter on top.
--
-- The per-key buckets live in a bounded `hull.cache` instance, so memory is
-- capped and stale buckets are evicted automatically (LRU) - no hand-rolled
-- sweep. The window logic uses the caller-passed `now`, so `check` stays a
-- pure, deterministically-testable function of its inputs.

local time = require("hull.time")
local cache = require("hull.cache")

local ratelimit = {}

local MAX_BUCKETS = 10000   -- max unique keys per limiter (cache LRU cap)

--- Check rate-limit state for a key. Pure over the passed clock `now`.
--
-- @tparam table buckets  Bucket store: a `hull.cache` instance (from
--   `cache.new`) mapping key -> `{ count, window_start }`.
-- @tparam string key     Limit key (e.g. user id, IP).
-- @tparam integer limit  Max hits per window.
-- @tparam integer window Window length (seconds).
-- @tparam integer now    Current time (seconds).
-- @treturn table  `{ allowed, remaining, reset }`.
function ratelimit.check(buckets, key, limit, window, now)
    local bucket = buckets.get(key)
    if not bucket or (now - bucket.window_start) >= window then
        -- New window. cache.set bounds the store at max_entries by evicting
        -- the least-recently-used bucket, so a flood of unique keys never
        -- rejects a legitimate new key (it drops an idle one instead).
        bucket = { count = 1, window_start = now }
        buckets.set(key, bucket)
    else
        -- Live window: bump in place. buckets.get already refreshed this
        -- bucket's LRU rank, so an active key won't be the eviction victim.
        bucket.count = bucket.count + 1
    end

    local remaining = limit - bucket.count
    if remaining < 0 then remaining = 0 end

    return {
        allowed = bucket.count <= limit,
        remaining = remaining,
        reset = bucket.window_start + window,
    }
end

--- Build a rate-limit middleware.
--
-- On limit exceeded: sends `429 {"error":"rate limit exceeded"}` and
-- returns `1` (short-circuit). On allowed: sets `X-RateLimit-Limit` /
-- `-Remaining` / `-Reset` headers and returns `0`.
--
-- Memory cap: 10_000 unique keys per limiter instance. Beyond that, the
-- least-recently-used bucket is evicted to admit the new key (a unique-key
-- flood cannot deny service to genuine new clients).
--
-- Concurrency: the bucket store is per-process and in-memory. Multi-
-- process deployments (multiple Hull workers behind a load balancer,
-- horizontal scaling) will multiply the effective limit by the
-- worker count - each worker enforces independently. Apps that need
-- a globally-coherent limit must front this with a shared store
-- (Redis bucket, edge rate-limit at the LB, etc.).
--
-- @tparam[opt] table opts
--
--   - `limit`  (integer, default `60`): max requests per window.
--   - `window` (integer, default `60`): window length in seconds.
--   - `key`    (string or `function(req) -> string`): limit key.
--     Default `"global"` (single bucket for all clients).
--
-- @treturn function  Middleware `(req, res) -> integer`.
-- @usage
-- -- Per-user rate limit
-- app.use("*", "/api/*", ratelimit.middleware({
--     limit = 60, window = 60,
--     key = function(req) return req.ctx.user_id or req.headers["x-forwarded-for"] or "anon" end,
-- }))
function ratelimit.middleware(opts)
    opts = opts or {}

    local limit = opts.limit or 60
    local window = opts.window or 60
    local key_fn = opts.key
    local buckets = cache.new({ max_entries = MAX_BUCKETS })

    -- Normalize key option into a function
    if type(key_fn) ~= "function" then
        local fixed_key = key_fn or "global"
        key_fn = function(_req) return fixed_key end
    end

    return function(req, res)
        local key = key_fn(req)
        local now = time.now()

        local result = ratelimit.check(buckets, key, limit, window, now)

        res:header("X-RateLimit-Limit", tostring(limit))
        res:header("X-RateLimit-Remaining", tostring(result.remaining))
        res:header("X-RateLimit-Reset", tostring(result.reset))

        if not result.allowed then
            res:status(429):json({ error = "rate limit exceeded", retry_after = window })
            return 1
        end

        return 0
    end
end

return ratelimit
