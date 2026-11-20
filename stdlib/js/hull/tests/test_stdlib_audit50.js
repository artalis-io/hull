// test_stdlib_audit50.js - stdlib regressions from audit 50 (JS stdlib).
//
// pwned: a denied refusal (an own `code` "denied", a host manifest.hosts does
// not admit) throws instead of failing open, whatever the hosts list holds.
// retry: a denied error is re-thrown at once, not retried.
// sftp: list() bounds the bytes of a listing, not only its entry count.
// ratelimit: a sweep at the cap that leaves more than 90% sheds down to it.
// The Lua twin is test_stdlib_audit50.lua.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c).

import { pwned } from "hull:web:pwned";
import { retry } from "hull:retry";
import { ratelimit } from "hull:web:middleware:ratelimit";
import { cache } from "hull:cache";
import "hull:ssh";   // loads hull:ssh:sftp_client, which registers its hooks

let pass = 0;
let fail = 0;

// globalThis.__audit50_case names the one case to run (one UTEST per case in
// test_js.c); unset runs them all.
async function test(name, fn) {
    if (globalThis.__audit50_case && globalThis.__audit50_case !== name) return;
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

// ── pwned: denied fails closed ─────────────────────────────────────

const P = __testHooks("hull:web:pwned");

// What the runtime throws for a host it denies (mod_http_client.c).
function denied(host) {
    return async () => {
        const e = new Error("http.async.get: host '" + host
            + "' is not in the manifest hosts allowlist");
        Object.defineProperty(e, "code", { value: "denied", enumerable: true });
        throw e;
    };
}

// A password the local blocklist does not hold, so the lookup goes to HIBP.
const PW = "audit50-not-in-the-blocklist-zq7";

await test("pwned_audit50.denied_unset_var_throws", async () => {
    // With hosts: ["$UNSET_VAR"] the runtime denies every HIBP fetch, and
    // hostsMayAdmit took the "$" entry for "may admit": check() returned
    // false (not pwned) behind one WARN.
    P.setGet(denied("api.pwnedpasswords.com"));
    let threw = false, err;
    try { await pwned.check(PW); } catch (e) { threw = true; err = e; }
    P.setGet(null);
    assertEq(threw, true, "check throws");
    assertEq(String(err && err.message).indexOf("manifest.hosts does not admit") >= 0, true,
             "the misconfiguration is named: " + (err && err.message));
});

await test("pwned_audit50.denied_cidr_throws", async () => {
    // A CIDR entry matches no DNS name; the endpoint is denied the same way.
    P.setGet(denied("hibp.internal.example"));
    let threw = false, err;
    try {
        await pwned.check(PW, { endpoint: "https://hibp.internal.example/range/" });
    } catch (e) { threw = true; err = e; }
    P.setGet(null);
    assertEq(threw, true, "check throws");
    assertEq(String(err && err.message).indexOf("hibp.internal.example") >= 0, true,
             "the endpoint is named: " + (err && err.message));
});

await test("pwned_audit50.outage_still_fails_open", async () => {
    P.setGet(async () => { throw new Error("connection refused"); });
    let r;
    try { r = await pwned.check(PW); } finally { P.setGet(null); }
    assertEq(r, false, "fails open");
});

// ── retry: denied is not retried ───────────────────────────────────

await test("retry_audit50.denied_not_retried", async () => {
    let calls = 0;
    const e = new Error("http.get: host 'x.test' is not in the manifest hosts allowlist");
    Object.defineProperty(e, "code", { value: "denied", enumerable: true });
    let threw = false, got;
    try {
        await retry.run(async () => { calls++; throw e; },
                        { maxAttempts: 3, baseMs: 0, capMs: 0 });
    } catch (x) { threw = true; got = x; }
    assertEq(threw, true, "throws");
    assertEq(calls, 1, "called once");
    assertEq(got, e, "the error unchanged");
});

// ── sftp: list() byte bound ────────────────────────────────────────

const S = __testHooks("hull:ssh:sftp_client");

// A session whose server answers OPENDIR with a handle and then `replies`
// READDIR replies of `per` names `name` each, then EOF.
function fakeSession(name, per, replies) {
    let sent = 0;
    const state = { closed: 0 };
    const s = Object.create(S.Sftp.prototype);
    Object.defineProperty(s, "request", { value: async () => {
        if (sent === 0) { sent = 1; return { type: "handle", handle: "H" }; }
        if (sent <= replies) {
            sent++;
            const names = [];
            for (let i = 0; i < per; i++) names.push({ filename: name, longname: "", attrs: {} });
            return { type: "name", names };
        }
        return { type: "status", eof: true };
    } });
    Object.defineProperty(s, "closeHandle", { value: async () => { state.closed++; return [true]; } });
    return [s, state];
}

await test("ssh_audit50.list_byte_cap", async () => {
    // maxBytes given: 4 names of 600 bytes pass 2000.
    const [s, st] = fakeSession("n".repeat(600), 2, 2);
    const [names, err] = await s.list("/d", { maxBytes: 2000 });
    assertEq(names, null, "no listing");
    assertEq(err && err.code, "too_large", "too_large");
    assertEq(err.limit, 2000, "the limit");
    assertEq(st.closed, 1, "handle closed");
    // The default bound (16 MiB): 100 names of 256 KiB, well under the entry
    // cap, were returned whole.
    const [s2, st2] = fakeSession("n".repeat(256 * 1024), 10, 10);
    const [n2, e2] = await s2.list("/d");
    assertEq(n2, null, "no listing");
    assertEq(e2 && e2.code, "too_large", "too_large by default");
    assertEq(e2.limit, 16 * 1024 * 1024, "the default limit");
    assertEq(st2.closed, 1, "handle closed");
    // Under the bound the listing is whole.
    const [s3] = fakeSession("ok", 3, 1);
    const [n3] = await s3.list("/d", { maxBytes: 2000 });
    assertEq(Array.isArray(n3), true, "a listing");
    assertEq(n3.length, 3, "three names");
    // A bad bound is a bad_argument.
    const [s4] = fakeSession("ok", 1, 1);
    const [, e4] = await s4.list("/d", { maxBytes: -1 });
    assertEq(e4 && e4.code, "bad_argument", "bad_argument");
});

// ── ratelimit: amortised sweep at the cap ──────────────────────────

await test("ratelimit_audit50.sweep_amortised_at_cap", () => {
    const MAX = 10000;
    const now = 1000000;
    const buckets = cache.new({ maxEntries: MAX });
    const saturated = new Map();
    // MAX saturated keys, ten of them expired, the rest live.
    for (let i = 1; i <= MAX; i++) {
        saturated.set("k" + i, { count: 1, windowStart: i <= 10 ? now - 120 : now });
    }
    // One more saturated key pushes the map past the cap and sweeps. Before
    // the fix the sweep freed the ten expired entries only, leaving 9991, so
    // the next keys each paid another O(MAX) sweep.
    ratelimit.check(buckets, "new", 1, 60, now + 1, saturated);
    assertEq(saturated.size <= Math.floor(MAX * 0.9), true, "shed to 90%, got " + saturated.size);
    assertEq(saturated.has("new"), true, "the new key is kept");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
