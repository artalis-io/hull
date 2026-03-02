/*
 * hull:ratelimit -- In-memory rate limiting middleware factory
 *
 * ratelimit.middleware(opts)                         - returns middleware function
 * ratelimit.check(buckets, key, limit, window, now) - pure helper, testable
 *
 * In-memory only (resets on restart). For production, use a DB-backed limiter.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { time } from "hull:time";

const MAX_BUCKETS = 10000;
const SWEEP_INTERVAL = 100;

function sweepExpired(buckets, window, now) {
    for (const k in buckets) {
        if ((now - buckets[k].windowStart) >= window) {
            delete buckets[k];
        }
    }
}

function bucketCount(buckets) {
    let n = 0;
    for (const k in buckets) n++;
    return n;
}

function check(buckets, key, limit, window, now) {
    let bucket = buckets[key];
    if (!bucket || (now - bucket.windowStart) >= window) {
        // Enforce max-entries cap before creating a new bucket
        if (!bucket && bucketCount(buckets) >= MAX_BUCKETS) {
            sweepExpired(buckets, window, now);
            if (bucketCount(buckets) >= MAX_BUCKETS) {
                return { allowed: false, remaining: 0, reset: now + window };
            }
        }
        buckets[key] = { count: 1, windowStart: now };
        bucket = buckets[key];
    } else {
        bucket.count++;
    }

    const remaining = Math.max(0, limit - bucket.count);

    return {
        allowed: bucket.count <= limit,
        remaining: remaining,
        reset: bucket.windowStart + window,
    };
}

function middleware(opts) {
    const o = opts || {};

    const limit = o.limit !== undefined ? o.limit : 60;
    const window = o.window !== undefined ? o.window : 60;
    let keyFn = o.key;
    const buckets = {};
    let checkCount = 0;

    // Normalize key option into a function
    if (typeof keyFn !== "function") {
        const fixedKey = keyFn || "global";
        keyFn = function(_req) { return fixedKey; };
    }

    return function ratelimitMiddleware(req, res) {
        const key = keyFn(req);
        const now = time.now();

        // Periodic sweep of expired buckets to prevent unbounded growth
        checkCount++;
        if (checkCount % SWEEP_INTERVAL === 0) {
            sweepExpired(buckets, window, now);
        }

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
