// test_stdlib_audit23.js - stdlib regressions from audit 23 (JS stdlib).
//
// template.compile returns the documented (data) => string (it returned the
// generated (data, escape, filters) function); config refuses a non-finite
// number; the idempotency default principal reads user ids as the rest of
// the stdlib does and matches the Lua twin; jobs.dead clamps its limit /
// offset; attachment.sweep takes an integer limit; and the idempotency /
// pwned helpers are test hooks, not module exports.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): jobs and idempotency need the db.

import { template } from "hull:template";
import { config } from "hull:config";
import { idempotency } from "hull:web:middleware:idempotency";
import { pwned } from "hull:web:pwned";
import { jobs } from "hull:jobs";
import { attachment } from "hull:attachment";

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

function threw(fn) {
    try { fn(); } catch (_) { return true; }
    return false;
}

// ── template.compile ───────────────────────────────────────────────

await test("template.compile's function takes data alone", () => {
    const T = __testHooks("hull:template");
    const f = T.compileString("Hi {{ name }}{% if x %}!{% end %}");
    assertEq(f({ name: "<b>", x: true }), "Hi &lt;b&gt;!", "escaped");
    assertEq(f(), "Hi ", "no data is {}");
    assertEq(f({ name: "y" }, () => "BAD"), "Hi y", "a caller cannot hand it another escape");
    assertEq(template.renderString("S {{ v }}", { v: 1 }), "S 1", "renderString");
});

// ── config ─────────────────────────────────────────────────────────

await test("config: a non-finite number is not a number", () => {
    const hooks = __testHooks("hull:config");
    try {
        for (const raw of ["1e400", "-1e400", "Infinity", "NaN"]) {
            hooks.setIo({ get: () => raw, allowed: () => true });
            assertEq(threw(() => config.get("A23_X", { type: "number" })), true, "number " + raw);
            assertEq(threw(() => config.get("A23_X", { type: "integer" })), true, "integer " + raw);
        }
        hooks.setIo({ get: () => "42", allowed: () => true });
        assertEq(config.get("A23_X", { type: "integer" }), 42);
    } finally { hooks.setIo(null); }
});

// ── idempotency ────────────────────────────────────────────────────

await test("idempotency: the default principal normalizes user ids", () => {
    const I = __testHooks("hull:web:middleware:idempotency");
    const p = (ctx) => I.defaultPrincipal({ ctx });
    assertEq(p({ session: { user_id: "" } }), "__anon", "an empty session id is none");
    assertEq(p({ session: { user_id: 42.0 } }), "session:42", "42.0 is 42");
    assertEq(p({ session: { user_id: 0 } }), "session:0", "0 is a user");
    assertEq(p({ session: { user_id: {} }, user: { id: 7 } }), "user:7",
             "an object session id is none");
    assertEq(p({ user: { sub: "", id: 7 } }), "user:7", "the first JWT id that is one");
    assertEq(p({ user: { sub: {}, user_id: "u" } }), "user:u");
    assertEq(p({ user: { sub: 1.5 } }), "__anon", "a fraction is no id");
    assertEq(p({}), "__anon");
});

await test("idempotency / pwned helpers are not module exports", () => {
    assertEq(idempotency._defaultPrincipal, undefined);
    assertEq(idempotency._normPrincipal, undefined);
    assertEq(typeof __testHooks("hull:web:middleware:idempotency").normPrincipal, "function");
    assertEq(pwned._sha1Hex, undefined);
    assertEq(pwned._inLocalBlocklist, undefined);
    const P = __testHooks("hull:web:pwned");
    assertEq(P.sha1Hex("password"), "5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8");
    assertEq(P.inLocalBlocklist(P.sha1Hex("password")), true, "a top password");
});

// ── jobs.dead ──────────────────────────────────────────────────────

await test("jobs.dead clamps limit to 1..1000 and floors offset at 0", async () => {
    jobs.init();
    jobs.handler("a23boom", () => { throw new Error("boom"); });
    for (let i = 0; i < 3; i++) jobs.enqueue("a23boom", {}, { maxAttempts: 1 });
    await jobs.work({ batch: 3 });
    assertEq(jobs.dead().length, 3, "three dead");
    assertEq(jobs.dead({ limit: -1 }).length, 1, "a negative limit is 1, not 'no limit'");
    assertEq(jobs.dead({ limit: 0 }).length, 1, "0 is 1");
    assertEq(jobs.dead({ limit: NaN }).length, 3, "NaN is the default");
    assertEq(jobs.dead({ limit: Infinity }).length, 3, "Infinity is 1000");
    assertEq(jobs.dead({ offset: -5 }).length, 3, "a negative offset is 0");
    assertEq(jobs.dead({ offset: NaN }).length, 3, "NaN offset is 0");
    assertEq(jobs.dead({ limit: 1.9, offset: 1.7 }).length, 1, "fractions are floored");
});

// ── attachment.sweep ───────────────────────────────────────────────

await test("attachment.sweep takes an integer limit", () => {
    for (const bad of [Infinity, -Infinity, NaN, 1.5, "5", 2 ** 60]) {
        assertEq(threw(() => attachment.sweep({ limit: bad })), true, String(bad));
    }
    assertEq(attachment.sweep({ limit: 0 }), 0, "below 1 sweeps nothing");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
