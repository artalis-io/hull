/**
 * @file hull:web:middleware:ratelimit
 * @module hull:web:middleware:ratelimit
 * @description In-memory rate limiting middleware factory. Lua parity:
 *   `hull.web.middleware.ratelimit`.
 *
 * Sliding-window counter per key. **In-memory only — resets on restart.**
 * For multi-instance production rate limiting, layer a DB-backed limiter.
 *
 * The per-key buckets live in a bounded `hull:cache` instance, so memory is
 * capped and stale buckets are evicted automatically (LRU) - no hand-rolled
 * sweep. (The cache's internal Map also treats a `__proto__` limit key as an
 * ordinary key, so no prototype-pollution guard is needed here.) The window
 * logic uses the caller-passed `now`, so `check` stays a pure, testable
 * function of its inputs.
 *
 * @license AGPL-3.0-or-later
 */

import { time } from "hull:time";
import { cache } from "hull:cache";

const MAX_BUCKETS = 10000;

/**
 * Check rate-limit state for a key. Pure over the passed clock `now`.
 * @param {Object} buckets  Bucket store: a hull:cache instance (from cache.new)
 *   mapping key -> { count, windowStart }.
 * @param {string} key
 * @param {number} limit
 * @param {number} window  seconds
 * @param {number} now     seconds
 * @returns {{allowed: boolean, remaining: number, reset: number}}
 */
function check(buckets, key, limit, window, now) {
    let bucket = buckets.get(key);
    if (!bucket || (now - bucket.windowStart) >= window) {
        // New window. cache.set bounds the store at maxEntries by evicting the
        // least-recently-used bucket, so a flood of unique keys never rejects a
        // legitimate new key (it drops an idle one instead).
        bucket = { count: 1, windowStart: now };
        buckets.set(key, bucket);
    } else {
        // Live window: bump in place. buckets.get already refreshed this
        // bucket's LRU rank, so an active key won't be the eviction victim.
        bucket.count++;
    }

    const remaining = Math.max(0, limit - bucket.count);

    return {
        allowed: bucket.count <= limit,
        remaining: remaining,
        reset: bucket.windowStart + window,
    };
}

/**
 * Build a rate-limit middleware.
 *
 * On limit exceeded: sends `429` and returns `1` (short-circuit). On
 * allowed: sets `X-RateLimit-Limit` / `-Remaining` / `-Reset` headers
 * and returns `0`.
 *
 * Memory cap: 10_000 unique keys per limiter instance. Beyond that, the
 * least-recently-used bucket is evicted to admit the new key (a unique-key
 * flood cannot deny service to genuine new clients).
 *
 * Concurrency: the bucket store is per-process and in-memory. Multi-
 * process deployments (multiple Hull workers behind a load balancer,
 * horizontal scaling) will multiply the effective limit by the
 * worker count — each worker enforces independently. Apps that need
 * a globally-coherent limit must front this with a shared store
 * (Redis bucket, edge rate-limit at the LB, etc.).
 *
 * @param {Object}  [opts]
 * @param {number}  [opts.limit=60]
 * @param {number}  [opts.window=60]  Seconds.
 * @param {string|((req) => string)} [opts.key="global"]  Limit key.
 * @returns {(req, res) => number}
 *
 * @example
 * app.use("*", "/api/*", ratelimit.middleware({
 *     limit: 60, window: 60,
 *     key: (req) => req.ctx.userId || req.headers["x-forwarded-for"] || "anon",
 * }));
 */
function middleware(opts) {
    const o = opts || {};

    const limit = o.limit !== undefined ? o.limit : 60;
    const window = o.window !== undefined ? o.window : 60;
    let keyFn = o.key;
    const buckets = cache.new({ maxEntries: MAX_BUCKETS });

    // Normalize key option into a function
    if (typeof keyFn !== "function") {
        const fixedKey = keyFn || "global";
        keyFn = function(_req) { return fixedKey; };
    }

    return function ratelimitMiddleware(req, res) {
        const key = keyFn(req);
        const now = time.now();

        const result = check(buckets, key, limit, window, now);

        res.header("X-RateLimit-Limit", String(limit));
        res.header("X-RateLimit-Remaining", String(result.remaining));
        res.header("X-RateLimit-Reset", String(result.reset));

        if (!result.allowed) {
            res.status(429).json({ error: "rate limit exceeded", retry_after: window });
            return 1;
        }

        return 0;
    };
}

const ratelimit = { middleware, check };
export { ratelimit };
