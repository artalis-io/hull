--
-- hull.middleware.transaction -- Wrap mutation handlers in a database transaction
--
-- Registers as post-body middleware. All db.exec()/db.query() calls within the
-- handler run inside a single BEGIN IMMEDIATE..COMMIT transaction. If the handler
-- errors, the transaction is rolled back automatically (via db.batch semantics).
--
-- Usage:
--   local transaction = require("hull.middleware.transaction")
--   app.use_post("POST", "/api/*", transaction.middleware())
--   app.use_post("PUT",  "/api/*", transaction.middleware())
--   app.use_post("DELETE", "/api/*", transaction.middleware())
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local transaction = {}

--- Marks the request as needing transactional wrapping. Handlers call
-- transaction.run(fn) (or transaction.try(fn)) to actually wrap their
-- DB work in BEGIN IMMEDIATE..COMMIT — see those helpers below.
-- L-4: trimmed the long historical-design comment block.
function transaction.middleware(_opts)
    return function(req, _res)
        req.ctx._txn = true
        return 0
    end
end

--- Execute fn inside a transaction. If already inside a db.batch(), fn runs
-- directly (no nesting — SQLite doesn't support nested transactions).
-- On error, the transaction is rolled back and the error is re-raised.
--
-- Usage inside a handler:
--   transaction.run(function()
--       db.exec("INSERT INTO ...", {...})
--       db.exec("UPDATE ...", {...})
--   end)
function transaction.run(fn)
    db.batch(fn)
end

--- Convenience: execute fn in a transaction, catch errors, and return
-- ok, result_or_error. Does NOT re-raise.
--
-- Usage:
--   local ok, err = transaction.try(function()
--       db.exec("INSERT INTO ...", {...})
--   end)
--   if not ok then
--       res:status(500):json({ error = tostring(err) })
--       return
--   end
function transaction.try(fn)
    return pcall(db.batch, fn)
end

return transaction
