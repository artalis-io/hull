// test_stdlib_audit44.js - stdlib regressions from audit 44 (JS stdlib).
//
// A batch of Lows from the Lua / JS stdlib audits: auth-flows checks a reset
// token before the pwned-password lookup (an outbound fetch); session.update
// takes only a session id; audit-log metadata is capped in UTF-8 bytes and
// made storable; rbac.defineRole takes one permission bare and refuses any
// other non-array; validate refuses an unknown type; ratelimit checks limit /
// window at factory time and sends Retry-After; jobs checks pollMs /
// concurrency / maxEmptyPolls / await's interval, queue / type / trace,
// history / historyRetention, and parks a cron row whose enqueue cannot
// succeed; idempotency's method gate and kv's backend gate have no
// prototype; search's snippet / highlight column and tokens are integers;
// retry.run caps maxAttempts and refuses a negative baseMs / capMs; and
// hull:_text's storable keeps its ellipsis inside max. See the Lua twin
// (test_stdlib_audit44.lua).
//
// Each case runs in a fresh caps-bearing context and a fresh :memory:
// database (run_audit44_case in tests/hull/runtime/js/test_js.c), named in
// globalThis.__audit44_case.

import { authFlows } from "hull:web:auth-flows";
import { session } from "hull:web:middleware:session";
import { auditLog } from "hull:web:middleware:audit-log";
import { rbac } from "hull:web:middleware:rbac";
import { validate } from "hull:validate";
import { ratelimit } from "hull:web:middleware:ratelimit";
import { idempotency } from "hull:web:middleware:idempotency";
import { jobs } from "hull:jobs";
import { kv } from "hull:kv";
import { search } from "hull:search";
import { retry } from "hull:retry";
import { crypto } from "hull:crypto";
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

function raises(fn, needle, msg) {
    let threw = false, err;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error((msg || "") + ": expected an error");
    if (needle && String(err && err.message).indexOf(needle) < 0)
        throw new Error((msg || "") + ": expected an error naming '" + needle
            + "', got " + (err && err.message));
}

async function rejects(fn, needle, msg) {
    let threw = false, err;
    try { await fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error((msg || "") + ": expected a rejection");
    if (needle && String(err && err.message).indexOf(needle) < 0)
        throw new Error((msg || "") + ": expected an error naming '" + needle
            + "', got " + (err && err.message));
}

function mkRes() {
    const r = { code: 200, headers: {}, body: undefined };
    r.status = (c) => { r.code = c; return r; };
    r.json = (v) => { r.body = v; return r; };
    r.html = (v) => { r.body = v; return r; };
    r.text = (v) => { r.body = v; return r; };
    r.header = (k, v) => { r.headers[k] = v; return r; };
    r.redirect = (p, c) => { r.code = c || 302; r.location = p; return r; };
    return r;
}

const cases = {};

// Item 1: the reset token is checked (not consumed) before the pwned lookup.
cases.pwned_reset_order = async () => {
    const USER = { id: "a44", email: "a44@a44.test", email_verified: true,
                   password_hash: crypto.hashPassword("old-password-44") };
    const AF = __testHooks("hull:web:auth-flows");
    AF.reset();
    authFlows.init({
        secret: "s".repeat(32),
        trustRequestHost: true,
        emailRateLimit: false,
        checkPwnedPasswords: true,
        emailSend: () => {},
        templates: {},
        userFindByEmail: (e) => (e === USER.email ? USER : null),
        userGet: (id) => (id === USER.id ? USER : null),
        userCreate: () => 1,
        userSetPassword: () => {},
        userSetEmail: () => {},
        userSetEmailVerified: () => {},
        onLogin: (_req, res) => { res.json({ ok: true }); },
        onPasswordReset: () => {},
    });
    const req = (body) => ({ method: "POST", path: "/auth/password-reset/confirm",
        headers: { host: "app.test", "content-type": "application/json",
                   "sec-fetch-site": "same-origin" },
        body: JSON.stringify(body), ctx: {}, remote_addr: "192.0.2.44" });
    // The HIBP fetch is stubbed and counted; "password" is also on the local
    // breach list, so the order shows in the answer too.
    let fetches = 0;
    __testHooks("hull:web:pwned").setGet(async () => {
        fetches++;
        return { status: 200, body: "" };
    });
    await test("a garbage token is refused before the pwned lookup", async () => {
        const res = mkRes();
        await AF.handlers.passwordResetConfirm(
            req({ token: "garbage", password: "a-random-long-password-44" }), res);
        assertEq(res.code, 400, "400");
        assertEq(String(res.body && res.body.error).slice(0, 13), "reset failed:", "token error");
        assertEq(fetches, 0, "no HIBP fetch");
        const res2 = mkRes();
        await AF.handlers.passwordResetConfirm(req({ token: "garbage", password: "password" }), res2);
        assertEq(String(res2.body && res2.body.error).slice(0, 13), "reset failed:",
                 "token error, not the pwned one");
    });
    await test("a used token is refused before the pwned lookup", async () => {
        const tok = AF.issueToken(USER.id, AF.ACTIONS.password_reset, 600);
        assertEq(AF.markTokenUsed(tok, time.now() + 600), true, "marked");
        const res = mkRes();
        await AF.handlers.passwordResetConfirm(req({ token: tok, password: "password" }), res);
        assertEq(res.body && res.body.error, "reset failed: replayed", "replayed");
    });
    await test("a valid token with a pwned password is refused and not consumed", async () => {
        const tok = AF.issueToken(USER.id, AF.ACTIONS.password_reset, 600);
        const res = mkRes();
        await AF.handlers.passwordResetConfirm(req({ token: tok, password: "password" }), res);
        assertEq(res.code, 400, "400");
        assertEq(String(res.body.error).indexOf("breaches") >= 0, true, "the pwned answer");
        assertEq(AF.tokenAlreadyUsed(tok), false, "token still usable");
    });
    __testHooks("hull:web:pwned").setGet(null);
};

// Item 2: session.update takes only an id session.create makes.
cases.session_update_sid = async () => {
    session.init({ cleanup: false });
    await test("an id that is not a session id is refused before anything else", () => {
        let r;
        try { r = session.update("not-a-session-id\0", {}, { ttl: -1 }); }
        catch (e) { throw new Error("raised: " + e.message); }
        assertEq(r, false, "false");
        let called = false;
        r = session.update("x".repeat(64), { toJSON() { called = true; return {}; } });
        assertEq(r, false, "false");
        assertEq(called, false, "data not encoded");
        assertEq(session.update("A".repeat(64), {}), false, "uppercase hex");
    });
    await test("a real session still updates", () => {
        const sid = session.create({ user_id: "u44" });
        assertEq(session.update(sid, { user_id: "u44", n: 2 }), true, "updated");
        assertEq(session.load(sid).n, 2, "data");
    });
};

// Item 3: audit-log metadata is capped in UTF-8 bytes and made storable.
cases.audit_log_meta = async () => {
    auditLog.init({ fingerprintSalt: "salt-a44-salt" });
    const req = { headers: { "user-agent": "t" }, remote_addr: "192.0.2.4" };
    await test("metadata over 4096 UTF-8 bytes is dropped", () => {
        // ~2000 UTF-16 units, ~6000 UTF-8 bytes (the Lua sibling's #meta).
        auditLog.record("a44u", "big", req, { metadata: { s: "€".repeat(2000) } });
        const rows = auditLog.list("a44u", { kinds: ["big"] });
        assertEq(rows.length, 1, "recorded");
        assertEq(rows[0].metadata, null, "metadata dropped");
    });
    await test("metadata under the cap is kept", () => {
        auditLog.record("a44u", "small", req, { metadata: { s: "€".repeat(100) } });
        const rows = auditLog.list("a44u", { kinds: ["small"] });
        assertEq(rows[0].metadata && rows[0].metadata.s, "€".repeat(100), "kept");
    });
};

// Item 4: rbac.defineRole takes one permission bare.
cases.rbac_string_permission = async () => {
    rbac.init();
    await test("a bare permission name is one permission", () => {
        rbac.defineRole("r44", "users.write");
        rbac.assign("u44", "r44");
        assertEq(rbac.hasPermission("u44", "users.write"), true, "granted");
        assertEq(rbac.hasPermission("u44", "u"), false, "not per character");
    });
    await test("any other non-array is refused", () => {
        raises(() => rbac.defineRole("r44b", 5), "permissions", "number");
        raises(() => rbac.defineRole("r44c", { length: 1, 0: "x" }), "permissions", "array-like");
        rbac.defineRole("r44d");
        rbac.defineRole("r44e", ["a.b"]);
    });
};

// Item 5: an unknown validate type is a schema error.
cases.validate_unknown_type = async () => {
    await test("an unknown type throws", () => {
        raises(() => validate.check({ x: 1 }, { x: { type: "array" } }), "unknown type", "array");
        raises(() => validate.check({}, { x: { type: "int" } }), "unknown type", "no value");
    });
    await test("the known types still check", () => {
        const [ok] = validate.check({ a: "s", b: 1.5, c: 2, d: true },
            { a: { type: "string" }, b: { type: "number" },
              c: { type: "integer" }, d: { type: "boolean" } });
        assertEq(ok, true, "ok");
    });
};

// Item 6: ratelimit limit / window checked up front; Retry-After on 429.
cases.ratelimit_options = async () => {
    await test("a bad limit or window throws at factory time", () => {
        for (const bad of [0, -1, NaN, Infinity, "60", null]) {
            raises(() => ratelimit.middleware({ limit: bad }), "limit", "limit " + String(bad));
        }
        for (const bad of [0, -5, NaN, Infinity, "60", null]) {
            raises(() => ratelimit.middleware({ window: bad }), "window", "window " + String(bad));
        }
    });
    await test("a refused request carries Retry-After", () => {
        const mw = ratelimit.middleware({ limit: 1, window: 60, key: "k44" });
        const req = { method: "GET", path: "/", headers: {}, remote_addr: "192.0.2.1" };
        assertEq(mw(req, mkRes()), 0, "first allowed");
        const res = mkRes();
        assertEq(mw(req, res), 1, "second refused");
        assertEq(res.code, 429, "429");
        const ra = Number(res.headers["Retry-After"]);
        assertEq(Number.isInteger(ra) && ra >= 1 && ra <= 60, true,
                 "Retry-After " + res.headers["Retry-After"]);
    });
};

// Item 7: runWorker's pollMs / concurrency / maxEmptyPolls, await interval.
cases.jobs_poll_options = async () => {
    jobs.init();
    await test("a bad pollMs rejects before the loop", async () => {
        for (const bad of [NaN, -1, Infinity, "fast", {}]) {
            await rejects(() => jobs.runWorker({ pollMs: bad, drain: true }), "pollMs",
                          "pollMs " + String(bad));
        }
    });
    await test("a bad await interval or timeout rejects", async () => {
        // An unknown id: before the check, await answered null at once
        // instead of waiting on a job no worker runs.
        const id = 999999;
        await rejects(() => jobs.await(id, { interval: NaN }), "interval", "NaN");
        await rejects(() => jobs.await(id, { timeout: NaN }), "timeout", "NaN timeout");
    });
    await test("a small pollMs and odd counts still run", async () => {
        jobs.handler("a44p", () => true);
        jobs.enqueue("a44p", {}, { queue: "p44" });
        const n = await jobs.runWorker({ queue: "p44", pollMs: 0, maxEmptyPolls: "1",
                                         concurrency: 1.5 });
        assertEq(n, 1, "processed");
    });
};

// Item 8: queue / type / trace checks; a payload of false survives (parity).
cases.jobs_key_options = async () => {
    jobs.init();
    await test("queue, type and trace are checked", () => {
        raises(() => jobs.enqueue("t", {}, { queue: 5 }), "queue", "numeric queue");
        raises(() => jobs.enqueue("t", {}, { queue: {} }), "queue", "object queue");
        raises(() => jobs.enqueue("t", {}, { queue: "q".repeat(256) }), "queue", "long queue");
        raises(() => jobs.enqueue("t".repeat(256), {}), "type", "long type");
        raises(() => jobs.enqueue("t", {}, { trace: {} }), "trace", "object trace");
        raises(() => jobs.enqueue("t", {}, { trace: "x".repeat(256) }), "trace", "long trace");
        raises(() => jobs.cron("c44", "* * * * *", null, { queue: 7 }), "queue", "cron queue");
        raises(() => jobs.pause(9), "queue", "pause");
        raises(() => jobs.purge({}), "queue", "purge");
        raises(() => jobs.claim({ queues: ["ok", 3] }), "queue", "claim list");
    });
    await test("a payload of false reaches the handler as false", async () => {
        let seen = "unset";
        jobs.handler("a44false", (job) => { seen = job.data; });
        jobs.enqueue("a44false", false, { queue: "f44", trace: "00-abc" });
        await jobs.work({ batch: 1, queue: "f44" });
        assertEq(seen, false, "false kept");
    });
};

// Item 9: history / historyRetention are checked.
cases.jobs_history_options = async () => {
    await test("a bad historyRetention throws", () => {
        for (const bad of [NaN, -1, Infinity, "week", {}]) {
            raises(() => jobs.init({ historyRetention: bad }), "historyRetention", String(bad));
        }
    });
    await test("a bad history throws", () => {
        raises(() => jobs.init({ history: "yes" }), "history", "string");
        raises(() => jobs.init({ history: { queues: [1] } }), "history", "queues");
    });
    await test("good values are kept", () => {
        jobs.init({ history: { queues: ["h44"] }, historyRetention: 3600 });
        jobs.init({ history: true, historyRetention: null });
    });
};

// Item 10: a cron row whose enqueue cannot succeed is parked.
cases.jobs_cron_park = async () => {
    const hooks = __testHooks("hull:jobs");
    jobs.init({ events: true });
    await test("a stored row with an empty type is parked with its error", () => {
        jobs.cron("park44", "* * * * *");
        hooks.cronSetField("park44", "type", "");
        hooks.cronSetNextRunAt("park44", 1000);
        hooks.tick(2000);
        const r = hooks.cronRow("park44");
        assertEq(r.next_run_at, hooks.CRON_DISABLED_AT, "parked");
        assertEq(typeof r.last_error === "string" && r.last_error.indexOf("type") >= 0, true,
                 "last_error: " + r.last_error);
        const evs = jobs.events({ types: ["cron_disabled"] });
        assertEq(evs.length >= 1, true, "cron_disabled emitted");
    });
    await test("a stored max_attempts of 0 is parked too", () => {
        jobs.cron("park44b", "* * * * *");
        hooks.cronSetField("park44b", "max_attempts", 0);
        hooks.cronSetNextRunAt("park44b", 1000);
        hooks.tick(2000);
        assertEq(hooks.cronRow("park44b").next_run_at, hooks.CRON_DISABLED_AT, "parked");
    });
    await test("a good row still fires", () => {
        jobs.cron("fire44", "* * * * *", null, { queue: "c44" });
        hooks.cronSetNextRunAt("fire44", 1000);
        hooks.tick(2000);
        assertEq(hooks.cronRow("fire44").next_run_at !== hooks.CRON_DISABLED_AT, true, "not parked");
        assertEq(jobs.stats({ queue: "c44" }).pending, 1, "enqueued");
    });
};

// Item 11: the idempotency method gate and kv's backend gate have no prototype.
cases.proto_lookups = async () => {
    idempotency.init();
    await test("a method named like an Object.prototype member is not gated in", () => {
        const mw = idempotency.middleware({});
        let asked = false;
        const req = { method: "constructor", path: "/x", headers: {}, ctx: {},
                      header: () => { asked = true; return "k-44"; } };
        assertEq(mw(req, mkRes()), 0, "passed through");
        assertEq(asked, false, "the key was never read");
    });
    await test("a backend named like an Object.prototype member is refused", () => {
        const conn = { exec() { return 0; }, query() { return []; }, backendName: "toString" };
        raises(() => kv.open({ backend: "sqlite", database: conn, namespace: "p44" }),
               "does not support", "toString backend");
    });
};

// Item 12: snippet / highlight column and tokens are integers.
cases.search_snippet_options = async () => {
    search.createIndex("a44docs", ["title", "body"]);
    search.index("a44docs", 1, { title: "hello", body: "a needle here" });
    await test("a non-integer column or tokens throws", () => {
        for (const bad of [Infinity, NaN, 1e300, 1.5, "1", -1, true]) {
            raises(() => search.query("a44docs", "needle", { snippet: { column: bad } }),
                   "column", "snippet column " + String(bad));
            raises(() => search.query("a44docs", "needle", { highlight: { column: bad } }),
                   "column", "highlight column " + String(bad));
        }
        for (const bad of [Infinity, NaN, 0, 65, 2.5, "8"]) {
            raises(() => search.query("a44docs", "needle", { snippet: { column: 2, tokens: bad } }),
                   "tokens", "tokens " + String(bad));
        }
    });
    await test("integers still work", () => {
        const rows = search.query("a44docs", "needle", { snippet: { column: 2, tokens: 8 } });
        assertEq(rows.length, 1, "one row");
        assertEq(rows[0].snippet.indexOf("needle") >= 0, true, "snippet");
    });
};

// Item 13: retry.run caps maxAttempts and refuses negative base / cap.
cases.retry_options = async () => {
    // The maxAttempts cap is js_stdlib_audit44.retry_max_attempts_capped in
    // test_js.c: its retries sleep, and this context has no event loop.
    await test("a negative or infinite baseMs / capMs rejects before fn runs", async () => {
        for (const k of ["baseMs", "capMs"]) {
            for (const bad of [-1, Infinity, "5"]) {
                let calls = 0;
                await rejects(() => retry.run(() => { calls++; }, { [k]: bad }), k,
                              k + " " + String(bad));
                assertEq(calls, 0, "fn not run");
            }
            raises(() => retry.backoff(1, { [k]: -1 }), k, "backoff " + k);
        }
    });
};

// Item 14: hull:_text's storable edge cases.
cases.text_storable = async () => {
    const T = __testHooks("hull:_text");
    const enc = (s) => {
        let n = 0;
        for (const ch of s) {
            const c = ch.codePointAt(0);
            n += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        }
        return n;
    };
    await test("the ellipsis fits inside max", () => {
        const s = T.storable("a".repeat(100), 10, true);
        assertEq(s, "aaaaaaa...", "cut at max - 3");
        assertEq(T.storable("a".repeat(10), 10, true), "a".repeat(10), "no cut, no ellipsis");
        const e = T.storable("€".repeat(10), 10, true);
        assertEq(e, "€€...", "two euros and the ellipsis");
        assertEq(enc(e) <= 10, true, "<= 10 bytes");
        assertEq(T.storable("\u{1F600}".repeat(5), 9, true), "\u{1F600}...", "astral");
        assertEq(T.storable("a".repeat(10), 2, true), "aa", "a cap under 3 takes none");
    });
    await test("NULs are dropped", () => {
        assertEq(T.storable("a\0b\0c"), "abc", "NUL");
    });
    await test("lone surrogates become U+FFFD", () => {
        assertEq(T.storable("x\ud800y\udc00z"), "x�y�z", "lone surrogates");
        assertEq(T.storable("\u{1F600}"), "\u{1F600}", "a pair is kept");
    });
    await test("the cut is on a code point", () => {
        assertEq(T.storable("ab€", 4), "ab", "no half character");
        assertEq(T.storable("a\u{1F600}", 4), "a", "no half pair");
    });
    await test("a throwing toString or getter gives the fallback", () => {
        const v = { toString() { throw new Error("no"); } };
        assertEq(T.storable(v).indexOf("could not be converted") >= 0, true, "default fallback");
        assertEq(T.storable(v, 100, false, "fb"), "fb", "caller fallback");
        const g = {};
        Object.defineProperty(g, "toString", { get() { throw new Error("getter"); } });
        assertEq(T.storable(g, 100, false, "fb"), "fb", "getter");
    });
};

await test("the case is known", () => {
    assertEq(typeof cases[globalThis.__audit44_case], "function",
             String(globalThis.__audit44_case));
});
if (cases[globalThis.__audit44_case]) await cases[globalThis.__audit44_case]();

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
