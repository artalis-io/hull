// test_stdlib_audit18.js - stdlib regressions from audit 18 (JS stdlib).
//
// jobs.events / jobs.purge / jobs.cleanup built their "?" placeholder lists
// with `.map(...).join(...)` on the caller's array. A real array with its own
// `map` passes Array.isArray, and the string its `join` returned became SQL
// run from the stdlib frame, past the `_hull_*` namespace check: a UNION read
// another table, an `OR (1=1` deleted every job. The lists are now copied by a
// counted loop over checked strings and the placeholders built from the count,
// so a crafted array is data (or refused), and its methods never run.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): jobs needs the db.

import { jobs } from "hull:jobs";
import { time } from "hull:time";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function assertThrows(fn, msg) {
    let threw = false;
    try { fn(); } catch (_e) { threw = true; }
    if (!threw) throw new Error((msg || "") + " expected a throw");
}

// A real array (Array.isArray is true) whose own map / join / slice / concat
// return attacker SQL. `called` records whether any of them ran. Defined, not
// assigned: Array.prototype is frozen (audit 18), so `a.map = ...` throws -
// an own property is still an app's to define.
function crafted(items, sql) {
    const a = items.slice();
    const st = { called: false };
    const join = () => { st.called = true; return sql; };
    const own = (k, v) => Object.defineProperty(a, k,
        { value: v, writable: true, enumerable: false, configurable: true });
    own("map", () => { st.called = true; return { join }; });
    own("join", join);
    own("slice", () => { st.called = true; return items.slice(); });
    own("concat", () => { st.called = true; return items.slice(); });
    return { a, st };
}

jobs.init();

// The secret a UNION through jobs.events would read: another _hull_* table.
const secretId = jobs.enqueue("secret-type", { token: "s3cret-a18" }, { queue: "vault" });

test("events: a crafted types array is data, never another table's rows", () => {
    const { a, st } = crafted(["no-such-event"],
        "?) AND 1=0 UNION ALL SELECT id, 0, type, id, type, queue, payload " +
        "FROM _hull_jobs WHERE (1=1");
    const rows = jobs.events({ types: a });
    assertEq(st.called, false, "caller method ran");
    assertEq(rows.length, 0, "rows");
});

test("events: a non-string type is refused", () => {
    assertThrows(() => jobs.events({ types: [{ toString: () => "x) OR (1=1" }] }), "object");
    assertThrows(() => jobs.events({ types: "dead" }), "string, not array");
});

test("events: a Proxy array lying about its length is refused", () => {
    const p = new Proxy(["dead"], {
        get(t, k) { return k === "length" ? "1) OR (1" : t[k]; },
    });
    assertThrows(() => jobs.events({ types: p }), "proxy");
});

test("events: a plain types list still filters", () => {
    assertEq(Array.isArray(jobs.events({ types: ["dead", "enqueued"] })), true, "array");
});

test("purge: a crafted statuses array deletes only what it names", () => {
    const { a, st } = crafted(["no-such-status"], "?) OR (1=1");
    const n = jobs.purge("default", { statuses: a });
    assertEq(st.called, false, "caller method ran");
    assertEq(n, 0, "deleted");
    assertEq(jobs.get(secretId) !== null, true, "other queue's job survived");
});

test("purge: a non-string status is refused", () => {
    assertThrows(() => jobs.purge("default", { statuses: [1] }), "number");
});

test("cleanup: a crafted statuses array deletes only what it names", () => {
    const { a, st } = crafted(["no-such-status"], "?) OR (1=1");
    jobs.cleanup({ statuses: a, before: time.now() + 1000000 });
    assertEq(st.called, false, "caller method ran");
    assertEq(jobs.get(secretId) !== null, true, "pending job survived");
});

test("cleanup: an empty statuses list deletes no job", () => {
    assertEq(jobs.cleanup({ statuses: [], before: time.now() + 1000000 }), 0, "deleted");
    assertEq(jobs.get(secretId) !== null, true, "job survived");
});

test("subscribe: a crafted types array's join never runs", () => {
    const { a, st } = crafted(["dead"], "dead");
    jobs.subscribe("a18-sub", () => {}, { types: a });
    assertEq(st.called, false, "caller method ran");
    jobs.unsubscribe("a18-sub");
});

test("purge: a plain statuses list still deletes", () => {
    const id = jobs.enqueue("purge-me", null, { queue: "a18" });
    assertEq(jobs.purge("a18", { statuses: ["pending"] }), 1, "deleted");
    assertEq(jobs.get(id), null, "gone");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
