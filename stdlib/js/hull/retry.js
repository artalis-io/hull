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
import { _refusal } from "hull:_refusal";

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
 *   - `retryOn(result) -> boolean | Promise<boolean>` - retry on a "bad" success value.
 *   - `retryOnError(err) -> boolean | Promise<boolean>` - return false to re-throw
 *     immediately (no retry). Both predicates may be async; they are awaited.
 *   - `onRetry(attempt, ok, value)` - called before each backoff sleep.
 * @returns {Promise<*>} The successful (or last) result; re-throws the last
 *   error if all attempts threw.
 *
 * A runtime refusal (an Error whose `code` is "wait_refused" or "busy", see
 * hull:_refusal) is never retried: it is re-thrown at once (audit 39).
 * "wait_refused" recurs on every attempt, and "busy" is this VM's own
 * admission cap - the caller decides (jobs hands the job back uncounted, the
 * outbox gives its claim back), as in the Lua twin.
 */
retry.run = async function(fn, opts) {
    opts = opts || {};
    // Anything that is not a number of at least 1 takes the default, as in
    // the Lua twin: with -1 or 0.5 the loop never ran, fn was never called,
    // and the call threw undefined (audit 20).
    let max = opts.maxAttempts;
    if (typeof max !== "number" || !(max >= 1)) max = DEFAULTS.maxAttempts;
    max = Math.floor(max);
    let lastOk = false, lastVal;
    for (let attempt = 1; attempt <= max; attempt++) {
        let ok, val;
        try { val = await fn(); ok = true; }
        catch (e) { ok = false; val = e; }
        lastOk = ok; lastVal = val;
        // The predicates are awaited: an async one returned a Promise, which
        // is truthy - every success counted as "bad" and was repeated (a
        // charge sent maxAttempts times), and every error as retryable.
        if (ok) {
            if (!(opts.retryOn && (await opts.retryOn(val)))) return val;
        } else {
            if (_refusal.code(val)) throw val;
            if (opts.retryOnError && !(await opts.retryOnError(val))) throw val;
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
