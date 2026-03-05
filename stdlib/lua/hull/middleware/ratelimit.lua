--
-- hull.ratelimit -- In-memory rate limiting middleware factory
--
-- ratelimit.middleware(opts)                          - returns middleware function
-- ratelimit.check(buckets, key, limit, window, now)  - pure helper, testable
--
-- In-memory only (resets on restart). For production, use a DB-backed limiter.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local ratelimit = {}

local SWEEP_INTERVAL = 100  -- sweep every N checks
local MAX_BUCKETS = 10000   -- max unique keys before forced eviction

--- Sweep expired buckets from the table.
-- Returns the number of remaining buckets after sweep.
local function sweep_expired(buckets, window, now)
    local remaining = 0
    for k, v in pairs(buckets) do
        if (now - v.window_start) >= window then
            buckets[k] = nil
        else
            remaining = remaining + 1
        end
    end
    return remaining
end

--- Check rate limit for a key. Pure function, no side effects on the request.
-- buckets: table of { [key] = { count, window_start } }
-- bucket_count: current number of entries (tracked externally for O(1) access)
-- Returns result, new_bucket_count
function ratelimit.check(buckets, key, limit, window, now, bucket_count)
    bucket_count = bucket_count or 0
    local bucket = buckets[key]
    if not bucket or (now - bucket.window_start) >= window then
        -- Cap check: prevent unbounded memory growth from unique keys
        if not bucket then
            if bucket_count >= MAX_BUCKETS then
                bucket_count = sweep_expired(buckets, window, now)
                if bucket_count >= MAX_BUCKETS then
                    return { allowed = false, remaining = 0, reset = now + window }, bucket_count
                end
            end
            bucket_count = bucket_count + 1
        end
        buckets[key] = { count = 1, window_start = now }
        bucket = buckets[key]
    else
        bucket.count = bucket.count + 1
    end

    local remaining = limit - bucket.count
    if remaining < 0 then remaining = 0 end

    return {
        allowed = bucket.count <= limit,
        remaining = remaining,
        reset = bucket.window_start + window,
    }, bucket_count
end

--- Create a rate limiting middleware function for use with app.use().
-- opts.limit: max requests per window (default 60)
-- opts.window: window in seconds (default 60)
-- opts.key: fixed string key (default "global"), or function(req) -> string
function ratelimit.middleware(opts)
    opts = opts or {}

    local limit = opts.limit or 60
    local window = opts.window or 60
    local key_fn = opts.key
    local buckets = {}
    local bucket_count = 0
    local check_count = 0

    -- Normalize key option into a function
    if type(key_fn) ~= "function" then
        local fixed_key = key_fn or "global"
        key_fn = function(_req) return fixed_key end
    end

    return function(req, res)
        local key = key_fn(req)
        local now = time.now()

        -- Periodic sweep of expired buckets to prevent unbounded growth
        check_count = check_count + 1
        if check_count % SWEEP_INTERVAL == 0 then
            bucket_count = sweep_expired(buckets, window, now)
        end

        local result
        result, bucket_count = ratelimit.check(buckets, key, limit, window, now, bucket_count)

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
