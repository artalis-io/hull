-- test_transaction.lua — Tests for hull.middleware.transaction
--
-- Requires db, time globals (run via hull test harness).

local transaction = require('hull.middleware.transaction')

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

-- ── Setup ─────────────────────────────────────────────────────────────

db.exec("CREATE TABLE IF NOT EXISTS _txn_test (id INTEGER PRIMARY KEY, val TEXT)")

-- ── middleware ─────────────────────────────────────────────────────────

test("middleware returns a function", function()
    local mw = transaction.middleware()
    assert(type(mw) == "function", "expected function")
end)

test("middleware sets req.ctx._txn", function()
    local mw = transaction.middleware()
    local req = { ctx = {} }
    local res = {}
    local result = mw(req, res)
    assert_eq(result, 0)
    assert_eq(req.ctx._txn, true)
end)

-- ── run ─────────────────────────────────────────────────────────────

test("run commits on success", function()
    db.exec("DELETE FROM _txn_test")
    transaction.run(function()
        db.exec("INSERT INTO _txn_test (val) VALUES (?)", { "committed" })
    end)
    local rows = db.query("SELECT val FROM _txn_test WHERE val = 'committed'")
    assert_eq(#rows, 1)
end)

test("run rolls back on error", function()
    db.exec("DELETE FROM _txn_test")
    local ok, _err = pcall(function()
        transaction.run(function()
            db.exec("INSERT INTO _txn_test (val) VALUES (?)", { "should_rollback" })
            error("deliberate error")
        end)
    end)
    assert_eq(ok, false)
    local rows = db.query("SELECT val FROM _txn_test WHERE val = 'should_rollback'")
    assert_eq(#rows, 0, "rollback")
end)

-- ── try ─────────────────────────────────────────────────────────────

test("try returns true on success", function()
    db.exec("DELETE FROM _txn_test")
    local ok, _err = transaction.try(function()
        db.exec("INSERT INTO _txn_test (val) VALUES (?)", { "try_ok" })
    end)
    assert_eq(ok, true)
    local rows = db.query("SELECT val FROM _txn_test WHERE val = 'try_ok'")
    assert_eq(#rows, 1)
end)

test("try returns false on error without raising", function()
    db.exec("DELETE FROM _txn_test")
    local ok, _err = transaction.try(function()
        db.exec("INSERT INTO _txn_test (val) VALUES (?)", { "try_fail" })
        error("deliberate")
    end)
    assert_eq(ok, false)
    local rows = db.query("SELECT val FROM _txn_test WHERE val = 'try_fail'")
    assert_eq(#rows, 0, "rollback")
end)

return {pass = pass, fail = fail}
