/**
 * @file hull:retry
 * @module hull:retry
 * @description Generic retry-with-backoff for any fallible operation. Lua
 *   parity: `hull.retry`.
 *
 * `retry.run(fn, opts)` calls `fn` and retries it on failure with exponential
 * backoff. `fn` is ARBITRARY - it may return a Promise (`http.fetch`,
 * `compute.async.call`, `gpu.async.dispatch`, `db.async.*`) or a plain value.
 * No HTTP coupling: wrap a fetch (`retry.run(() => http.fetch(url), {...})`); the
 * same shape retries a WASM/GPU dispatch or a DB query. `run` is async and
 * `await`s `fn`; it sleeps between attempts via `hull.sleep`, so it must run on
 * the event-loop thread (a handler, `app.main`, a timer).
 *
 * Failure = `fn` throwing/rejecting, OR (when `opts.retryOn` is given) `fn`
 * resolving to a value the predicate flags as retryable.
 *
 * @license AGPL-3.0-or-later
 * @example
 *   import { retry } from "hull:retry";
 *   const res = await retry.run(() => http.fetch(url), {
 *       maxAttempts: 4,
 *       retryOn: (r) => r.status >= 500 || r.status === 429,
 *   });
 *   const out = await retry.run(() => gpu.async.dispatch("scan", opts));
 */

import { crypto } from "hull:crypto";

const retry = {};

const DEFAULTS = { maxAttempts: 3, baseMs: 100, factor: 2, capMs: 30000 };

/**
 * Backoff delay (ms) for a 1-based attempt: `base * factor^(n-1)`, clamped to
 * `cap`, with optional full jitter (uniform in `[0, delay)`).
 * @param {number} attempt  1-based attempt number.
 * @param {Object} [opts]  `baseMs` (100), `factor` (2), `capMs` (30000),
 *   `jitter` (boolean, default false).
 * @returns {number} Delay in milliseconds.
 */
retry.backoff = function(attempt, opts) {
    opts = opts || {};
    const base = opts.baseMs || DEFAULTS.baseMs;
    const factor = opts.factor || DEFAULTS.factor;
    const cap = opts.capMs || DEFAULTS.capMs;
    let d = base * Math.pow(factor, attempt - 1);
    if (d > cap) d = cap;
    if (opts.jitter) {
        const b = new Uint8Array(crypto.random(4));
        const u = ((b[0] * 256 + b[1]) * 256 + b[2]) * 256 + b[3];
        d = d * (u / 4294967296);   // uniform in [0, d)
    }
    return Math.floor(d);
};

/**
 * Run `fn` with retries.
 * @param {Function} fn  The operation (may return a Promise). Its resolved value
 *   is passed through on success; a throw/reject or a `retryOn`-flagged result
 *   triggers a retry.
 * @param {Object} [opts]
 *   - `maxAttempts` (default 3) total attempts incl. the first.
 *   - `baseMs` / `factor` / `capMs` / `jitter` - see backoff.
 *   - `retryOn(result) -> boolean` - retry on a "bad" success value.
 *   - `retryOnError(err) -> boolean` - return false to re-throw immediately (no retry).
 *   - `onRetry(attempt, ok, value)` - called before each backoff sleep.
 * @returns {Promise<*>} The successful (or last) result; re-throws the last
 *   error if all attempts threw.
 */
retry.run = async function(fn, opts) {
    opts = opts || {};
    const max = opts.maxAttempts || DEFAULTS.maxAttempts;
    let lastOk = false, lastVal;
    for (let attempt = 1; attempt <= max; attempt++) {
        let ok, val;
        try { val = await fn(); ok = true; }
        catch (e) { ok = false; val = e; }
        lastOk = ok; lastVal = val;
        if (ok) {
            if (!(opts.retryOn && opts.retryOn(val))) return val;
        } else {
            if (opts.retryOnError && !opts.retryOnError(val)) throw val;
        }
        if (attempt < max) {
            if (opts.onRetry) opts.onRetry(attempt, ok, val);
            await hull.sleep(retry.backoff(attempt, opts));
        }
    }
    if (lastOk) return lastVal;
    throw lastVal;
};

export { retry };
export default retry;
