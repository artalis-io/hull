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
local _request = require("hull.web._request")

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
function ratelimit.check(buckets, key, limit, window, now, saturated)
    local bucket = buckets.get(key)
    -- An exhausted bucket the LRU cache evicted (a flood of other keys pushes
    -- out the least recently used) is still exhausted: put it back rather than
    -- open a fresh window. Without this, sending from ~10,000 other keys bought
    -- a full allowance again against the key you were blocked on.
    if not bucket and saturated then
        local s = saturated.map[key]
        if s and (now - s.window_start) < window then
            bucket = s
            buckets.set(key, s)
        end
    end
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
    -- A bucket whose allowance is SPENT (count >= limit, audit 9) is kept
    -- apart too, not only one already over it: recorded at count > limit, a
    -- client that sent exactly `limit` requests, flooded the bucket out of
    -- the cache and came back had a fresh allowance every round - without
    -- ever being refused, so it was never recorded.
    if saturated and bucket.count >= limit and saturated.map[key] ~= bucket then
        -- Kept apart from the cache, so eviction cannot reset it. Recorded
        -- whenever the map holds a DIFFERENT bucket for the key: testing
        -- only for presence kept the first window's (expired) entry, so
        -- from the second window on the restore above found nothing live.
        if not saturated.map[key] then saturated.n = saturated.n + 1 end
        saturated.map[key] = bucket
        if saturated.n > MAX_BUCKETS then
            -- Drop expired entries; if every entry is live (an attacker
            -- spending `limit` requests per key), drop the ones
            -- closest to expiry down to 90% of the cap, so the map has a
            -- hard bound and each new key does not pay a full sweep.
            local live = {}
            for k, b in pairs(saturated.map) do
                if (now - b.window_start) >= window then
                    saturated.map[k] = nil
                else
                    live[#live + 1] = k
                end
            end
            if #live > MAX_BUCKETS then
                local m = saturated.map
                table.sort(live, function(a, b)
                    return m[a].window_start < m[b].window_start
                end)
                for i = 1, #live - math.floor(MAX_BUCKETS * 0.9) do
                    m[live[i]] = nil
                end
                saturated.n = math.floor(MAX_BUCKETS * 0.9)
            else
                saturated.n = #live
            end
        end
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
--     Default: the client IP (one bucket per client). A fixed string
--     makes one bucket shared by every client.
--   - `trust_proxy` (boolean, default `false`): take the client IP from
--     the proxy-appended (last) `X-Forwarded-For` entry. Only behind a
--     proxy you control - the header is client-supplied otherwise.
--
-- @treturn function  Middleware `(req, res) -> integer`.
-- @usage
-- -- Per-user rate limit
-- app.use("*", "/api/*", ratelimit.middleware({
--     limit = 60, window = 60,
--     key = function(req) return req.ctx.user_id or req.remote_addr or "anon" end,
-- }))
function ratelimit.middleware(opts)
    opts = opts or {}

    local limit = opts.limit or 60
    local window = opts.window or 60
    local key_fn = opts.key
    local buckets = cache.new({ max_entries = MAX_BUCKETS })
    local saturated = { map = {}, n = 0 }   -- see ratelimit.check

    -- Normalize key option into a function
    if key_fn == nil then
        -- Per client. A single shared bucket let one client spend the limit
        -- for everyone.
        local trust_proxy = opts.trust_proxy == true
        key_fn = function(req)
            return _request.limit_key(_request.client_ip(req, trust_proxy)) or "unknown"
        end
    elseif type(key_fn) ~= "function" then
        local fixed_key = key_fn
        key_fn = function(_req) return fixed_key end
    end

    return function(req, res)
        -- Buckets are keyed by value: a table is a fresh key on every request
        -- (and nil cannot index), so nothing was ever limited. Only a string
        -- or a number is a key.
        local key = key_fn(req)
        if type(key) == "number" and key == key then key = tostring(key) end
        if type(key) ~= "string" then
            error("ratelimit: key must return a string or a number", 2)
        end
        local now = time.now()

        local result = ratelimit.check(buckets, key, limit, window, now, saturated)

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
