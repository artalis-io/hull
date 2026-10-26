// test_stdlib_audit29.js - stdlib regressions from audit 29 (JS).
//
// jobs: dedupKey / concurrencyKey are stored as strings (an integer is its
// decimal string), and keys / step / signal names over the VARCHAR(255)
// column (counted in UTF-8 bytes, as the Lua twin) are refused. csrf: an
// empty token header falls back to the _csrf body field, and more than 256
// form pairs is a 413. auth: the Bearer scheme is case-insensitive, spaces /
// tabs around the token are dropped, and an empty token is a 401. The Lua
// twin is test_stdlib_audit29.lua.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): jobs needs the db.

import { jobs } from "hull:jobs";
import { csrf } from "hull:web:middleware:csrf";
import { auth } from "hull:web:middleware:auth";
import { jwt } from "hull:jwt";
import { time } from "hull:time";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function refused(fn) {
    try { fn(); return false; } catch (_e) { return true; }
}

function mockRes() {
    const res = { code: 200 };
    res.status = (c) => { res.code = c; return res; };
    res.json = (b) => { res.body = b; return res; };
    res.header = () => res;
    return res;
}

function mockReq(method, headers, extra) {
    const req = {
        method, path: "/api", ctx: {}, headers,
        header(name) {
            const want = name.toLowerCase();
            for (const k of Object.keys(headers))
                if (k.toLowerCase() === want) return headers[k];
            return undefined;
        },
    };
    return Object.assign(req, extra || {});
}

// ── jobs ─────────────────────────────────────────────────────────────

await test("jobs.enqueue: an integer dedupKey is its decimal string", () => {
    jobs.init();
    const id = jobs.enqueue("a29int", {}, { queue: "a29q", dedupKey: 7 });
    if (!id) throw new Error("first enqueue returns an id");
    assertEq(jobs.enqueue("a29int", {}, { queue: "a29q", dedupKey: "7" }), null,
             "string twin collapsed");
    const id2 = jobs.enqueue("a29int", {}, { queue: "a29q", dedupKey: "07" });
    if (!id2 || id2 === id) throw new Error("\"07\" is a distinct key");
});

await test("jobs.enqueue: a non-string, non-integer key is refused", () => {
    jobs.init();
    assertEq(refused(() => jobs.enqueue("a29bad", {}, { dedupKey: {} })), true, "object");
    assertEq(refused(() => jobs.enqueue("a29bad", {}, { dedupKey: 1.5 })), true, "float");
    assertEq(refused(() => jobs.enqueue("a29bad", {}, { dedupKey: true })), true, "boolean");
    assertEq(refused(() => jobs.enqueue("a29bad", {},
        { concurrencyKey: {}, concurrency: 1 })), true, "concurrencyKey object");
});

await test("jobs.enqueue: a key over 255 bytes is refused", () => {
    jobs.init();
    assertEq(refused(() => jobs.enqueue("a29long", {}, { dedupKey: "k".repeat(256) })),
             true, "dedupKey 256");
    assertEq(refused(() => jobs.enqueue("a29long", {},
        { concurrencyKey: "k".repeat(256), concurrency: 1 })), true, "concurrencyKey 256");
    // 86 x 3-byte characters = 258 bytes, though only 86 UTF-16 units.
    assertEq(refused(() => jobs.enqueue("a29long", {}, { dedupKey: "€".repeat(86) })),
             true, "multi-byte over 255 bytes");
    assertEq(refused(() => jobs.enqueue("a29long", {}, { dedupKey: "a\uD800" })),
             true, "unpaired surrogate");
    if (!jobs.enqueue("a29long", {}, { queue: "a29l", dedupKey: "k".repeat(255) }))
        throw new Error("255 bytes fits");
});

await test("jobs.signal: a name over 200 bytes is refused", () => {
    jobs.init();
    assertEq(refused(() => jobs.signal(1, "s".repeat(201))), true, "201");
});

// ── csrf ─────────────────────────────────────────────────────────────

const SECRET = "audit29-csrf-secret-0123456789abcdef";

await test("csrf: more than 256 form pairs is a 413", () => {
    const mw = csrf.middleware({ secret: SECRET });
    const parts = [];
    for (let i = 1; i <= 257; i++) parts.push("f" + i + "=v");
    let req = mockReq("POST", {}, { ctx: { session_id: "s1" }, body: parts.join("&") });
    let res = mockRes();
    assertEq(mw(req, res), 1, "short-circuit");
    assertEq(res.code, 413, "status");
    req = mockReq("POST", {}, { ctx: { session_id: "s1" }, body: "a=b" + "&".repeat(256) });
    res = mockRes();
    assertEq(mw(req, res), 1, "empty pieces");
    assertEq(res.code, 413, "status (empty pieces)");
    parts.pop();
    parts[255] = "_csrf=" + csrf.generate("s1", SECRET);
    req = mockReq("POST", {}, { ctx: { session_id: "s1" }, body: parts.join("&") });
    res = mockRes();
    assertEq(mw(req, res), 0, "256 pairs with the token at the end");
});

await test("csrf: an empty token header falls back to the body field", () => {
    const mw = csrf.middleware({ secret: SECRET });
    const tok = csrf.generate("s2", SECRET);
    let req = mockReq("POST", { "x-csrf-token": "" },
                      { ctx: { session_id: "s2" }, body: "_csrf=" + tok });
    let res = mockRes();
    assertEq(mw(req, res), 0, "body token read");
    req = mockReq("POST", { "x-csrf-token": "" }, { ctx: { session_id: "s2" }, body: "x=1" });
    res = mockRes();
    assertEq(mw(req, res), 1, "no token");
    assertEq(res.code, 403, "status");
});

// ── auth Bearer parsing ──────────────────────────────────────────────

await test("auth.jwtMiddleware: one Bearer rule", () => {
    const secret = "audit29-jwt-secret-0123456789abcdef";
    const mw = auth.jwtMiddleware({ secret });
    const tok = jwt.sign({ sub: "u1", exp: time.now() + 600 }, secret);
    const run = (h) => {
        const res = mockRes();
        const rc = mw(mockReq("GET", { authorization: h }), res);
        return [rc, res.code];
    };
    for (const h of ["Bearer " + tok, "bearer " + tok, "BEARER " + tok,
                     "Bearer\t" + tok, "Bearer   " + tok + "  "])
        assertEq(run(h)[0], 0, "accepted: " + h.substring(0, 9));
    for (const h of ["Bearer ", "Bearer    ", "Bearer", "Bearer" + tok, "Basic " + tok]) {
        const [rc, code] = run(h);
        assertEq(rc, 1, "refused: " + h.substring(0, 9));
        assertEq(code, 401, "401: " + h.substring(0, 9));
    }
});

console.log("stdlib audit 29: " + pass + " passed, " + fail + " failed");
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
