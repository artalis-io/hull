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

--- Check rate limit for a key. Pure function, no side effects on the request.
-- buckets: table of { [key] = { count, window_start } }
-- Returns { allowed = bool, remaining = int, reset = int }
function ratelimit.check(buckets, key, limit, window, now)
    local bucket = buckets[key]
    if not bucket or (now - bucket.window_start) >= window then
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
    }
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
