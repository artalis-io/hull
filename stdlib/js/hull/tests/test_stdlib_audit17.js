// test_stdlib_audit17.js - stdlib regressions from audit 17 (JS stdlib).
//
// etag.text / etag.html with an empty body: compute returns null, and the
// response went out with "ETag: null" (and a 304 for If-None-Match: *). An
// empty body now goes out with no ETag and is never answered 304. jobs._config
// (the live config, settable past init's checks) and the clock-injectable
// seams (jobs._tick, _cronNext, _eventsDrain) are test hooks, no longer on the
// module.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): etag hashes, jobs needs the db.

import { etag } from "hull:web:middleware:etag";
import { jobs } from "hull:jobs";

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

function mockRes(helper) {
    const res = {
        code: 200, sent: false, headers: {},
        status(code) { this.code = code; return this; },
        header(name, value) { this.headers[name] = value; return this; },
    };
    res[helper] = function () { this.sent = true; return this; };
    return res;
}

// ── etag: an empty body ─────────────────────────────────────────────

for (const helper of ["text", "html"]) {
    for (const body of ["", undefined, null]) {
        const label = helper + " " + JSON.stringify(body === undefined ? "undefined" : body) + " body";
        test(label + " is sent with no ETag", () => {
            const res = mockRes(helper);
            etag[helper]({ method: "GET", header: () => null }, res, body);
            assertEq(res.sent, true, "sent");
            assertEq(res.code, 200, "status");
            assertEq("ETag" in res.headers, false, "no ETag");
        });
        test(label + " is never a 304, even for If-None-Match: *", () => {
            const res = mockRes(helper);
            etag[helper]({ method: "GET", header: () => "*" }, res, body);
            assertEq(res.sent, true, "sent");
            assertEq(res.code, 200, "status");
        });
    }
}

test("a non-empty body still gets its ETag", () => {
    const res = mockRes("text");
    etag.text({ method: "GET", header: () => null }, res, "hello");
    assertEq(res.sent, true, "sent");
    assertEq(typeof res.headers.ETag, "string", "ETag");
});

// ── jobs: no exported config ────────────────────────────────────────

test("jobs._config is not exported", () => {
    assertEq(jobs._config, undefined);
});

test("the config is a test hook", () => {
    const hooks = __testHooks("hull:jobs");
    assertEq(typeof (hooks && hooks.config), "object", "hook");
    assertEq(typeof hooks.config.backoff, "function", "live config");
});

// The clock-injectable seams wrote _hull_* rows at a time app code chose
// (jobs._tick(1e12): every nextRunAt decades out; a drain at now = 1e15: the
// lease held for ever). They are test hooks too.
for (const k of ["_tick", "_cronNext", "_eventsDrain"]) {
    test("jobs." + k + " is not exported", () => {
        assertEq(jobs[k], undefined);
        assertEq(k in jobs, false, "own key");
    });
}

test("the cron tick, cron math and drain are test hooks", () => {
    const hooks = __testHooks("hull:jobs");
    assertEq(typeof hooks.tick, "function", "tick");
    assertEq(typeof hooks.cronNext, "function", "cronNext");
    assertEq(typeof hooks.eventsDrain, "function", "eventsDrain");
});

// Counts go back to the C harness (run_js_test in
// tests/hull/runtime/js/test_js.c), which reads them off the global object.
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
