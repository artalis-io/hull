/**
 * @file hull:web:middleware:transaction
 * @module hull:web:middleware:transaction
 * @description Wrap mutation handlers in a SQLite transaction. Lua parity:
 *   `hull.web.middleware.transaction`.
 *
 * `transaction.middleware()` is a marker — it sets `req.ctx._txn = true`
 * so downstream code knows the handler should run under a transaction.
 * The real work happens in `transaction.run(fn)` / `transaction.attempt(fn)`
 * which call `db.batch()` (`BEGIN IMMEDIATE → fn → COMMIT`, with
 * `ROLLBACK` on error).
 *
 * @license AGPL-3.0-or-later
 */

import { db } from "hull:db";

/**
 * Build a transaction marker middleware.
 *
 * Always returns `0` (continue). Sets `req.ctx._txn = true` so handlers
 * and other middleware know DB work for this request should be wrapped
 * in `transaction.run` / `transaction.attempt`.
 *
 * @param {Object} [opts]  Reserved for future options.
 * @returns {(req, res) => number}
 *
 * @example
 * app.usePost("POST",   "/api/*", transaction.middleware());
 * app.usePost("PUT",    "/api/*", transaction.middleware());
 * app.usePost("DELETE", "/api/*", transaction.middleware());
 */
function middleware(opts) {
    return function(req, _res) {
        if (!req.ctx) req.ctx = {};
        req.ctx._txn = true;
        return 0;
    };
}

/**
 * Run `fn` inside `BEGIN IMMEDIATE..COMMIT`.
 *
 * On error the transaction is rolled back and the error is re-thrown.
 * SQLite doesn't support nested transactions — calling this while
 * already inside a `db.batch` will error.
 *
 * @param {() => void} fn  Function whose DB writes should be atomic.
 * @throws Re-throws any error from `fn` after rollback.
 */
function run(fn) {
    db.batch(fn);
}

/**
 * Run `fn` inside a transaction; return `[ok, errOrNull]`.
 *
 * Same as `run` but catches errors instead of re-throwing. Aliased as
 * `try` (quoted because `try` is reserved) for parity with Lua's
 * `transaction.try`.
 *
 * @param {() => void} fn
 * @returns {[true, null] | [false, Error]}
 *
 * @example
 * const [ok, err] = transaction.attempt(() => {
 *     db.exec("INSERT INTO ...", [...]);
 * });
 * if (!ok) {
 *     res.status(500).json({ error: String(err) });
 *     return;
 * }
 */
function attempt(fn) {
    try {
        db.batch(fn);
        return [true, null];
    } catch (e) {
        return [false, e];
    }
}

// L-1 (parity with Lua transaction.try): expose under both `attempt` and
// `try` so cross-runtime code can use the documented Lua name. `try` is
// a reserved word so the object literal must quote it.
const transaction = { middleware, run, attempt, "try": attempt };
export { transaction };
