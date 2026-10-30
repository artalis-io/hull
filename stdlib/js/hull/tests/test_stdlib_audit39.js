// test_stdlib_audit39.js - stdlib regressions from audit 39 (JS stdlib).
//
// retry.run retried a runtime refusal: "wait_refused" recurs on every
// attempt, and "busy" is the VM's own admission cap, which the caller decides
// about (jobs hands the job back uncounted). A refusal is now re-thrown at
// once, the same Error, its code intact. See the Lua twin
// (test_stdlib_audit39.lua). The jobs workflow cases are in
// test_jobs_hooks.js.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c).

import { retry } from "hull:retry";

let pass = 0;
let fail = 0;

async function atest(name, fn) {
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

for (const code of ["busy", "wait_refused"]) {
    await atest("a " + code + " refusal is re-thrown at once, unchanged", async () => {
        let calls = 0;
        const refusal = new Error("http.fetch: refused");
        refusal.code = code;
        let caught = null;
        try {
            await retry.run(() => { calls++; throw refusal; }, { maxAttempts: 5 });
        } catch (e) { caught = e; }
        assertEq(caught, refusal, "the same Error");
        assertEq(caught.code, code, "code kept");
        assertEq(calls, 1, "not retried");
    });
}

await atest("a remote error with another code is not a refusal", async () => {
    let calls = 0;
    const e = new Error("remote");
    e.code = "ECONNRESET";
    let caught = null;
    try {
        await retry.run(() => { calls++; throw e; },
            { maxAttempts: 3, retryOnError: () => false });
    } catch (x) { caught = x; }
    assertEq(caught, e, "the same Error");
    assertEq(calls, 1, "retryOnError decided");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
