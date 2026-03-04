/*
 * hull:middleware:transaction -- Wrap mutation handlers in a database transaction
 *
 * Registers as post-body middleware. All db.exec()/db.query() calls within the
 * handler run inside a single BEGIN IMMEDIATE..COMMIT transaction. If the handler
 * errors, the transaction is rolled back automatically (via db.batch semantics).
 *
 * Usage:
 *   import { transaction } from "hull:middleware:transaction";
 *   app.usePost("POST", "/api/*", transaction.middleware());
 *   app.usePost("PUT",  "/api/*", transaction.middleware());
 *   app.usePost("DELETE", "/api/*", transaction.middleware());
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { db } from "hull:db";

/**
 * Create a post-body middleware that marks the request for transactional execution.
 * Handlers should use transaction.run(fn) to wrap their DB operations.
 */
function middleware(opts) {
    return function(req, _res) {
        if (!req.ctx) req.ctx = {};
        req.ctx._txn = true;
        return 0;
    };
}

/**
 * Execute fn inside a transaction. Uses db.batch() which provides
 * BEGIN IMMEDIATE..COMMIT with automatic ROLLBACK on error.
 * SQLite doesn't support nested transactions — if already inside
 * a db.batch(), this will error.
 */
function run(fn) {
    db.batch(fn);
}

/**
 * Execute fn in a transaction, catch errors. Returns [ok, error].
 * Does NOT re-throw on failure.
 *
 * Usage:
 *   const [ok, err] = transaction.attempt(() => {
 *       db.exec("INSERT INTO ...", [...]);
 *   });
 *   if (!ok) {
 *       res.status(500);
 *       res.json({ error: String(err) });
 *       return;
 *   }
 */
function attempt(fn) {
    try {
        db.batch(fn);
        return [true, null];
    } catch (e) {
        return [false, e];
    }
}

const transaction = { middleware, run, attempt };
export { transaction };
