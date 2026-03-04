// test_transaction.js — Tests for hull:middleware:transaction
//
// Requires db globals (run via hull test harness).

import { transaction } from "hull:middleware:transaction";
import { db } from "hull:db";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        print("FAIL: " + name + ": " + e.message);
    }
}

function assertEq(a, b, msg) {
    if (a !== b)
        throw new Error((msg || "") + " expected " + b + ", got " + a);
}

// ── Setup ─────────────────────────────────────────────────────────────

db.exec("CREATE TABLE IF NOT EXISTS _txn_test (id INTEGER PRIMARY KEY, val TEXT)");

// ── middleware ─────────────────────────────────────────────────────────

test("middleware returns a function", () => {
    const mw = transaction.middleware();
    assertEq(typeof mw, "function");
});

test("middleware sets req.ctx._txn", () => {
    const mw = transaction.middleware();
    const req = { ctx: {}, header() { return null; } };
    const res = {};
    const result = mw(req, res);
    assertEq(result, 0);
    assertEq(req.ctx._txn, true);
});

// ── run ─────────────────────────────────────────────────────────────

test("run commits on success", () => {
    db.exec("DELETE FROM _txn_test");
    transaction.run(() => {
        db.exec("INSERT INTO _txn_test (val) VALUES (?)", ["committed"]);
    });
    const rows = db.query("SELECT val FROM _txn_test WHERE val = 'committed'");
    assertEq(rows.length, 1);
});

test("run rolls back on error", () => {
    db.exec("DELETE FROM _txn_test");
    let caught = false;
    try {
        transaction.run(() => {
            db.exec("INSERT INTO _txn_test (val) VALUES (?)", ["should_rollback"]);
            throw new Error("deliberate error");
        });
    } catch (e) {
        caught = true;
    }
    assertEq(caught, true);
    const rows = db.query("SELECT val FROM _txn_test WHERE val = 'should_rollback'");
    assertEq(rows.length, 0, "rollback");
});

// ── attempt ─────────────────────────────────────────────────────────

test("attempt returns [true, null] on success", () => {
    db.exec("DELETE FROM _txn_test");
    const [ok, err] = transaction.attempt(() => {
        db.exec("INSERT INTO _txn_test (val) VALUES (?)", ["attempt_ok"]);
    });
    assertEq(ok, true);
    const rows = db.query("SELECT val FROM _txn_test WHERE val = 'attempt_ok'");
    assertEq(rows.length, 1);
});

test("attempt returns [false, error] without throwing", () => {
    db.exec("DELETE FROM _txn_test");
    const [ok, err] = transaction.attempt(() => {
        db.exec("INSERT INTO _txn_test (val) VALUES (?)", ["attempt_fail"]);
        throw new Error("deliberate");
    });
    assertEq(ok, false);
    const rows = db.query("SELECT val FROM _txn_test WHERE val = 'attempt_fail'");
    assertEq(rows.length, 0, "rollback");
});

export default { pass, fail };
