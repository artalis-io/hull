/**
 * @file hull:web:middleware:ratelimit
 * @module hull:web:middleware:ratelimit
 * @description In-memory rate limiting middleware factory. Lua parity:
 *   `hull.web.middleware.ratelimit`.
 *
 * Sliding-window counter per key. **In-memory only — resets on restart.**
 * For multi-instance production rate limiting, layer a DB-backed limiter.
 *
 * @license AGPL-3.0-or-later
 */

import { time } from "hull:time";

const MAX_BUCKETS = 10000;
const SWEEP_INTERVAL = 100;

/* M-4: bucket counter is per-store, not module-global. Multiple
 * ratelimit.middleware() instances would otherwise share the same
 * MAX_BUCKETS budget and starve each other. The store object holds the
 * bucket map plus its own count. */
function makeStore() {
    // Null-prototype map: a request-derived rate-limit key of "__proto__"
    // must be an ordinary key, not the Object.prototype setter.
    return { buckets: Object.create(null), count: 0 };
}

function sweepExpired(store, window, now) {
    const buckets = store.buckets;
    for (const k in buckets) {
        if ((now - buckets[k].windowStart) >= window) {
            delete buckets[k];
            store.count--;
        }
    }
}

function check(store, key, limit, window, now) {
    /* Back-compat: the exported `ratelimit.check(buckets, key, ...)` API
     * accepts either a bare object (legacy) or a Store. Wrap on demand.
     *
     * @deprecated The bare-bucket-map form is legacy. New callers should
     * use ratelimit.middleware() which manages its own per-instance Store
     * via makeStore(). The bare form is preserved only for existing
     * direct callers of `ratelimit.check` in tests / app code; it recomputes
     * Object.keys(b).length per call and the MAX_BUCKETS cap enforcement
     * is effectively pass-through because the transient store's count is
     * thrown away. */
    if (!store || typeof store.count !== "number") {
        // Treat as a bare bucket map; use a transient store with count
        // derived from Object.keys length. Cap enforcement still applies.
        const b = store || {};
        const transient = { buckets: b, count: Object.keys(b).length };
        return check(transient, key, limit, window, now);
    }
    const buckets = store.buckets;
    let bucket = buckets[key];
    if (!bucket || (now - bucket.windowStart) >= window) {
        // Enforce max-entries cap before creating a new bucket
        if (!bucket && store.count >= MAX_BUCKETS) {
            sweepExpired(store, window, now);
            if (store.count >= MAX_BUCKETS) {
                return { allowed: false, remaining: 0, reset: now + window };
            }
        }
        if (!bucket) store.count++;
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

/**
 * Build a rate-limit middleware.
 *
 * On limit exceeded: sends `429` and returns `1` (short-circuit). On
 * allowed: sets `X-RateLimit-Limit` / `-Remaining` / `-Reset` headers
 * and returns `0`.
 *
 * Memory cap: 10_000 unique keys per limiter instance (Phase 6 fix: now
 * per-instance, not module-global).
 *
 * Concurrency: the bucket map is per-process and in-memory. Multi-
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
    const store = makeStore();
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
            sweepExpired(store, window, now);
        }

        const result = check(store, key, limit, window, now);

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
