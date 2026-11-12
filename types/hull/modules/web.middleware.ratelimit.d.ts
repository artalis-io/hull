// hull:web:middleware:ratelimit - in-memory sliding-window rate limiting.
declare module "hull:web:middleware:ratelimit" {
    /** One key's window: request count and window start (seconds). */
    export interface RateLimitBucket {
        /** Requests counted in this window. */
        count: number;
        /** Window start, Unix seconds. */
        windowStart: number;
    }
    /** A bucket store, such as a `hull:cache` instance. */
    export interface RateLimitStore {
        /** The key's bucket, if any. */
        get(key: string): RateLimitBucket | null | undefined;
        /** Store the key's bucket. */
        set(key: string, bucket: RateLimitBucket): unknown;
    }
    /** The outcome of one `check`. */
    export interface RateLimitResult {
        /** Whether the request is within the limit. */
        allowed: boolean;
        /** Requests left in this window. */
        remaining: number;
        /** When the window resets, Unix seconds. */
        reset: number;
    }
    /** Options for `ratelimit.middleware()`. */
    export interface RateLimitOptions {
        /** Requests per window, a finite number >= 1 (default 60). */
        limit?: number;
        /** Window length in seconds, > 0 (default 60). */
        window?: number;
        /** The limit key: a fixed string (one shared bucket) or a synchronous function of the request. Default: the client IP. */
        key?: string | ((req: Hull.Request) => string | number);
        /** Take the client IP from the last X-Forwarded-For entry (only behind your own proxy). */
        trustProxy?: boolean;
        /** Lua-parity alias of `trustProxy`. */
        trust_proxy?: boolean;
    }
    /** The `ratelimit` module object. */
    export interface RateLimitModule {
        /** Build the middleware: sets `X-RateLimit-*` headers, answers 429 with `Retry-After` when over. Throws on a bad limit / window. */
        middleware(opts?: RateLimitOptions): Hull.Middleware;
        /** Count one request for `key` at `now` (seconds) and report the window state. */
        check(buckets: RateLimitStore, key: string, limit: number, window: number, now: number,
              saturated?: Map<string, RateLimitBucket>): RateLimitResult;
    }
    /** In-memory rate limiter. */
    export const ratelimit: RateLimitModule;
}
