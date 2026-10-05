// test_middleware_audit6.js - middleware regressions from audit 6 (JS stdlib).
//
// auth.sessionMiddleware({ loginPath }) redirected through res.body, which the
// response object does not have (every unauthenticated request was a 500);
// ratelimit took an object / Promise key as a fresh bucket per request;
// health pinged the handles-only hull:db module; auditLog.list built its IN
// list with the app's own .map / .join. Runs in the caps-bearing context
// (run_js_test in tests/hull/runtime/js/test_js.c): session and audit-log need
// the db.

import { auth } from "hull:web:middleware:auth";
import { session } from "hull:web:middleware:session";
import { ratelimit } from "hull:web:middleware:ratelimit";
import { health } from "hull:web:middleware:health";
import { auditLog } from "hull:web:middleware:audit-log";
import { csrf } from "hull:web:middleware:csrf";
import { db as dbModule } from "hull:db";

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
    if (a !== b)
        throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function threw(fn) {
    try { fn(); return false; } catch (_e) { return true; }
}

// Exactly the methods the real response object has
// (runtime/js/bindings_response.c); anything else is a TypeError here too.
function mockRes() {
    const rec = { status: 200, headers: {}, body: undefined, redirect: undefined };
    const res = {
        rec,
        status(c) { rec.status = c; return res; },
        header(n, v) { rec.headers[n] = v; return res; },
        json(v) { rec.body = v; return res; },
        html(s) { rec.body = s; return res; },
        text(s) { rec.body = s; return res; },
        bytes(s) { rec.body = s; return res; },
        redirect(p) { rec.status = 302; rec.redirect = p; return res; },
    };
    return res;
}

function mockReq(cookieHeader, extra) {
    const headers = Object.create(null);
    if (cookieHeader) headers.cookie = cookieHeader;
    const req = {
        method: "GET", path: "/app/x", headers, ctx: {},
        header(n) { return headers[String(n).toLowerCase()]; },
    };
    return Object.assign(req, extra || {});
}

session.init();

test("sessionMiddleware loginPath redirects without a cookie", () => {
    const mw = auth.sessionMiddleware({ loginPath: "/login" });
    const res = mockRes();
    assertEq(mw(mockReq(null), res), 1);
    assertEq(res.rec.status, 302);
    assertEq(res.rec.redirect, "/login");
});

test("sessionMiddleware loginPath redirects for an unknown / expired session", () => {
    const mw = auth.sessionMiddleware({ loginPath: "/login" });
    const res = mockRes();
    assertEq(mw(mockReq("hull_session=" + "ab".repeat(32)), res), 1);
    assertEq(res.rec.status, 302);
    assertEq(res.rec.redirect, "/login");
});

test("ratelimit refuses an object or Promise key", () => {
    const objMw = ratelimit.middleware({ limit: 1, key: () => ({}) });
    assertEq(threw(() => objMw(mockReq(null), mockRes())), true, "object key");
    const asyncMw = ratelimit.middleware({ limit: 1, key: async () => "u1" });
    assertEq(threw(() => asyncMw(mockReq(null), mockRes())), true, "async key");
});

test("ratelimit limits by a numeric key", () => {
    const mw = ratelimit.middleware({ limit: 1, key: () => 7 });
    assertEq(mw(mockReq(null), mockRes()), 0);
    const res = mockRes();
    assertEq(mw(mockReq(null), res), 1);
    assertEq(res.rec.status, 429);
});

test("health pings the default connection, or the hull:db module's", () => {
    assertEq(health.runChecks().checks.db.status, "ok", "default");
    health.setDb(dbModule);
    assertEq(health.runChecks().checks.db.status, "ok", "module");
    health.setDb(null);
});

test("auditLog.list refuses a non-array kinds", () => {
    auditLog.init({ fingerprintSalt: "audit6-test-salt" });
    const fake = { length: 1, map: () => ({ join: () => "?) OR 1=1 --" }) };
    assertEq(threw(() => auditLog.list("u1", { kinds: fake })), true, "object");
    assertEq(threw(() => auditLog.list("u1", { kinds: [1] })), true, "number");
    assertEq(Array.isArray(auditLog.list("u1", { kinds: ["login"] })), true, "array ok");
});

test("csrf honours safeMethods", () => {
    const mw = csrf.middleware({ secret: "x".repeat(32), safeMethods: ["GET"],
                                 requireSession: true });
    const res = mockRes();
    // HEAD is no longer safe: checked, and with no session refused.
    assertEq(mw(mockReq(null, { method: "HEAD" }), res), 1);
    assertEq(res.rec.status, 403);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
