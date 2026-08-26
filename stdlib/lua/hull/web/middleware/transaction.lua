--- Wrap mutation handlers in a single SQLite transaction.
--
-- `transaction.middleware()` is a marker - it sets `req.ctx._txn = true`
-- so downstream code knows the handler should be wrapped. The real work
-- happens in `transaction.run(fn)` / `transaction.try(fn)`, which call
-- `db.batch()` (BEGIN IMMEDIATE → fn → COMMIT, with ROLLBACK on error).
--
-- SQLite does not support nested transactions, so wrapping handlers that
-- are already inside a `db.batch` is a no-op via the batch semantics.
--
-- @module hull.web.middleware.transaction
-- @license AGPL-3.0-or-later
-- @usage
--   local transaction = require("hull.web.middleware.transaction")
--   app.use_post("POST",   "/api/*", transaction.middleware())
--   app.use_post("PUT",    "/api/*", transaction.middleware())
--   app.use_post("DELETE", "/api/*", transaction.middleware())
--
--   -- inside a handler:
--   transaction.run(function()
--       db.exec("INSERT INTO orders ...", { ... })
--       db.exec("UPDATE inventory ...",   { ... })
--   end)

local db = require("hull.db").default()

local transaction = {}

--- Build a transaction marker middleware.
--
-- Always returns `0` (continue). Sets `req.ctx._txn = true` so handlers
-- and other middleware can tell that DB work in this request should
-- run under `transaction.run` / `transaction.try`.
--
-- @function transaction.middleware
-- @tparam[opt] table _opts Reserved for future options.
-- @treturn function(req, res) -> 0  Post-body middleware.
function transaction.middleware(_opts)
    return function(req, _res)
        req.ctx._txn = true
        return 0
    end
end

--- Run `fn` inside `BEGIN IMMEDIATE..COMMIT`.
--
-- On error, the transaction is rolled back and the error re-raised.
-- If already inside a `db.batch`, `fn` runs without an extra BEGIN.
--
-- @function transaction.run
-- @tparam function fn  Function whose DB writes should be atomic.
-- @raise Re-raises any error from `fn` after rollback.
-- @usage
--   transaction.run(function()
--       db.exec("INSERT INTO ...", { ... })
--       db.exec("UPDATE ...",      { ... })
--   end)
function transaction.run(fn)
    db.batch(fn)
end

--- Run `fn` inside a transaction; return `(ok, err_or_result)`.
--
-- Same as `transaction.run` but uses `pcall` instead of re-raising.
--
-- @function transaction.try
-- @tparam function fn  Function whose DB writes should be atomic.
-- @treturn boolean ok     `true` on success, `false` on error.
-- @treturn any   value    `fn`'s return value on success, error message on failure.
-- @usage
--   local ok, err = transaction.try(function()
--       db.exec("INSERT INTO ...", { ... })
--   end)
--   if not ok then
--       res:status(500):json({ error = tostring(err) })
--       return
--   end
function transaction.try(fn)
    return pcall(db.batch, fn)
end

return transaction
