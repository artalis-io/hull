// test_stdlib_audit24.js - stdlib regressions from audit 24 (JS stdlib).
//
// csv.encode takes opts.columns and the union of every row's keys (Lua's
// rule); the jobs / audit-log / outbox LIMITs are clamped integers that
// treat null / true as the default; session.listForUser names sessions by
// ref (never by id) and session.destroyRef signs one out; outbox claims
// outlast http.fetch's 10 min ceiling; idempotency fingerprints a body
// that is not valid UTF-8 by its bytes; jwt refuses a non-finite exp /
// nbf; totp.init refuses malformed counts; validate's number type and bounds
// refuse NaN / Infinity; a trusted host no longer matches a non-default
// port; template paths read own properties only; and the email-change
// handler takes user id 0.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): jobs, session, audit-log and outbox need
// the db.

import { csv } from "hull:csv";
import { jobs } from "hull:jobs";
import { jwt } from "hull:jwt";
import { validate } from "hull:validate";
import { template } from "hull:template";
import { crypto } from "hull:crypto";
import { encoding } from "hull:encoding";
import { session } from "hull:web:middleware:session";
import { auditLog } from "hull:web:middleware:audit-log";
import { outbox } from "hull:web:middleware:outbox";
import { idempotency } from "hull:web:middleware:idempotency";
import { totp } from "hull:web:middleware:totp";
import { cookie } from "hull:web:cookie";
import { authFlows } from "hull:web:auth-flows";

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
    try { fn(); return false; } catch (_e) { return true; }
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

// ── csv ────────────────────────────────────────────────────────────

await test("csv.encode: headers are every row's keys, first-seen; columns honoured", () => {
    const out = csv.encode([{ a: 1, b: 2, c: 3 }, { a: 1, c: 3 }, { c: 3 }], { headers: true });
    assertEq(out, "a,b,c\n1,2,3\n1,,3\n,,3\n", "headers mode");
    // First-seen order since audit 27 (audit 24 sorted the union).
    assertEq(csv.encode([{ c: 1 }, { a: 2 }], { headers: true }), "c,a\n1,\n,2\n",
             "a key missing from row 1");
    assertEq(csv.encode([{ a: 1, b: 2 }], { headers: true, columns: ["b", "a"] }),
             "b,a\n2,1\n", "columns order");
    assertEq(csv.encode([{}], { headers: true, columns: ["constructor"] }),
             "constructor\n\n", "own properties only");
    assertEq(threw(() => csv.encode([{ a: 1 }], { headers: true, columns: ["a", {}] })),
             true, "a non-name column is refused");
});

// ── jobs ───────────────────────────────────────────────────────────

await test("jobs: null / true limits take the default; batches are clamped", async () => {
    jobs.init();
    jobs.handler("a24boom", () => { throw new Error("boom"); });
    for (let i = 0; i < 3; i++) jobs.enqueue("a24boom", {}, { maxAttempts: 1 });
    await jobs.work({ batch: 3 });
    assertEq(jobs.dead().length, 3, "three dead");
    assertEq(jobs.dead({ limit: null }).length, 3, "null is the default, not 1");
    assertEq(jobs.dead({ limit: true }).length, 3, "true is the default, not 1");
    assertEq(jobs.dead({ limit: "2" }).length, 2, "a numeric string counts (Lua's tonumber)");
    assertEq(jobs.dead({ offset: null }).length, 3, "null offset is 0");
    for (let i = 0; i < 3; i++) jobs.enqueue("a24claim", {}, { queue: "a24q" });
    assertEq(jobs.claim({ queue: "a24q", batch: -5 }).length, 1, "a negative batch is 1");
    assertEq(jobs.claim({ queue: "a24q", batch: NaN }).length, 2, "NaN is the default");
});

await test("jobs.purge / cleanup: an empty status list deletes no job", () => {
    jobs.enqueue("a24purge", {}, { queue: "a24p" });
    assertEq(jobs.purge("a24p", { statuses: [] }), 0);
    assertEq(threw(() => jobs.purge("a24p", { statuses: [1] })), true, "a number");
    assertEq(jobs.cleanup({ statuses: [] }), 0);
    assertEq(jobs.purge("a24p", { statuses: ["pending"] }), 1);
});

// ── audit-log ──────────────────────────────────────────────────────

await test("auditLog: list limit is clamped, window_days must be finite > 0", () => {
    auditLog.init({ fingerprintSalt: "a24-salt-0123456789" });
    const req = { headers: { "user-agent": "curl/8" }, remote_addr: "10.0.0.1" };
    for (let i = 0; i < 3; i++) auditLog.record(2401, "login", req);
    assertEq(auditLog.list(2401).length, 3);
    assertEq(auditLog.list(2401, { limit: -1 }).length, 1, "a negative limit is 1");
    assertEq(auditLog.list(2401, { limit: NaN }).length, 3, "NaN is the default");
    assertEq(auditLog.list(2401, { limit: null }).length, 3, "null is the default");
    assertEq(auditLog.list(2401, { limit: 1.5 }).length, 1, "floored");
    for (const bad of [NaN, Infinity, -1, 0, "30", true]) {
        assertEq(threw(() => auditLog.listDevices(2401, { window_days: bad })), true,
                 "listDevices " + String(bad));
        assertEq(threw(() => auditLog.isNewDevice(2401, req, { window_days: bad })), true,
                 "isNewDevice " + String(bad));
    }
    assertEq(auditLog.listDevices(2401, { window_days: 1 }).length, 1);
    assertEq(auditLog.isNewDevice(2401, req, { window_days: 1 }), false);
});

// ── session device list ────────────────────────────────────────────

await test("session.listForUser: refs, not ids; destroyRef is per user", () => {
    session.init({ ttl: 3600, cleanup: false });
    const a = session.create({ user_id: 2402 });
    const b = session.create({ user_id: 2402 });
    const other = session.create({ user_id: 2403 });
    const rows = session.listForUser(2402, a);
    assertEq(rows.length, 2);
    let current = 0, refB = null;
    for (const r of rows) {
        assertEq("id" in r, false, "no id");
        assertEq(/^[0-9a-f]{32}$/.test(r.ref), true, "a 32-hex ref");
        assertEq(r.ref !== a && r.ref !== b, true, "the ref is not the id");
        if (r.current === true) current++; else refB = r.ref;
    }
    assertEq(current, 1, "one current session");
    let n = 0;
    for (const r of session.listForUser(2402, { ctx: { session_id: b } })) {
        if (r.current) { n++; assertEq(r.ref, refB, "b is current"); }
    }
    assertEq(n, 1);
    const otherRef = session.listForUser(2403)[0].ref;
    assertEq(session.destroyRef(2402, otherRef), 0, "another user's ref");
    assertEq(session.load(other) !== null, true, "the other user's session lives");
    assertEq(session.destroyRef(2402, "zz"), 0, "a malformed ref");
    assertEq(session.destroyRef(2402, refB), 1, "its own ref");
    assertEq(session.load(b), null, "b is gone");
    assertEq(session.load(a) !== null, true, "a lives");
});

// ── outbox ─────────────────────────────────────────────────────────

await test("outbox.flush: limit clamped, delivery timeout below the lease", async () => {
    outbox.init();
    const hooks = __testHooks("hull:web:middleware:outbox");
    for (let i = 1; i <= 3; i++)
        outbox.enqueue({ kind: "webhook", destination: "http://a24.test/" + i, payload: "{}" });
    const seen = [];
    hooks.setPost((_url, _body, opts) => { seen.push(opts.timeoutMs); return { status: 200 }; });
    let r1, r2;
    try {
        r1 = await outbox.flush({ limit: -1 });
        r2 = await outbox.flush({ limit: null });
    } finally {
        hooks.setPost(null);
    }
    assertEq(r1.delivered, 1, "a negative limit is 1");
    assertEq(r2.delivered, 2, "null is the default");
    assertEq(seen.length, 3);
    for (const t of seen)
        assertEq(t, undefined, "the app's own http timeout is kept");
    assertEq(hooks.claimLease > 600, true, "the lease outlasts http.fetch's 10 min ceiling");
});

// ── idempotency ────────────────────────────────────────────────────

await test("idempotency: a body that is not valid UTF-8 is fingerprinted by its bytes", () => {
    const fp = __testHooks("hull:web:middleware:idempotency").computeFingerprint;
    const base = { method: "POST", path: "/a24", query: {} };
    // Both read as the same lossy text; only their bytes differ.
    const a = fp({ ...base, body: "ab\uFFFDcd", bodyBytes: new Uint8Array([97, 98, 0xff, 99, 100]).buffer });
    const b = fp({ ...base, body: "ab\uFFFDcd", bodyBytes: new Uint8Array([97, 98, 0xfe, 99, 100]).buffer });
    const a2 = fp({ ...base, body: "ab\uFFFDcd", bodyBytes: new Uint8Array([97, 98, 0xff, 99, 100]).buffer });
    assertEq(a !== b, true, "different bytes, different fingerprints");
    assertEq(a, a2, "same bytes, same fingerprint");
    assertEq(fp({ ...base, body: "abc" }), fp({ ...base, body: "abc" }), "text bodies unchanged");
    assertEq(fp({ ...base, body: "ab\uFFFDcd" }) !== a, true, "bytes and text forms differ");
});

// ── jwt ────────────────────────────────────────────────────────────

function rawHs256(payloadJson, secret) {
    const b64 = (u8) => encoding.base64.encode(u8, { url: true });
    const input = b64(encoding.utf8.encode('{"alg":"HS256","typ":"JWT"}')) + "."
        + b64(encoding.utf8.encode(payloadJson));
    return input + "." + b64(crypto.hmacSha256(input, encoding.bytes.toU8(secret)));
}

await test("jwt.verify: a non-finite exp / nbf is invalid", () => {
    const secret = "k".repeat(32);
    const [okClaims] = jwt.verify(rawHs256('{"sub":"a","exp":4000000000}', secret), secret);
    assertEq(okClaims !== null, true, "a finite exp verifies");
    for (const p of ['{"sub":"a","exp":1e999}', '{"sub":"a","nbf":-1e999}']) {
        const [claims] = jwt.verify(rawHs256(p, secret), secret);
        assertEq(claims, null, p);
    }
});

// ── totp ───────────────────────────────────────────────────────────

await test("totp.init: counts are whole numbers > 0, window 0..10", () => {
    for (const [k, v] of [
        ["maxFailedAttempts", NaN], ["maxFailedAttempts", 0],
        ["maxFailedAttemptsPerIp", 1.5], ["lockoutDuration", "900"],
        ["recoveryCodes", -1], ["period", Infinity],
        ["window", 1.5], ["window", -1], ["window", 11], ["window", "1"],
    ]) {
        assertEq(threw(() => totp.init({ [k]: v })), true, k + " = " + String(v));
    }
});

// ── validate ───────────────────────────────────────────────────────

await test("validate: number refuses NaN / Infinity, bounds refuse NaN", () => {
    for (const v of [NaN, Infinity, -Infinity]) {
        assertEq(validate.check({ x: v }, { x: { type: "number" } })[0], false,
                 "type number " + String(v));
    }
    assertEq(validate.check({ x: NaN }, { x: { min: 1 } })[0], false, "NaN vs min");
    assertEq(validate.check({ x: NaN }, { x: { max: 1 } })[0], false, "NaN vs max");
    assertEq(validate.check({ x: 5 }, { x: { min: NaN } })[0], false, "NaN min");
    assertEq(validate.check({ x: 5 }, { x: { type: "number", min: 1, max: 9 } })[0], true);
});

// ── template ───────────────────────────────────────────────────────

await test("template: a path reads own properties only", () => {
    const out = template.renderString(
        "[{{ user.constructor }}][{{ constructor }}][{{ name.length }}][{{ items.length }}]"
        + "{% for it in items %}<{{ it.toString }}{{ it.v }}>{% end %}",
        { user: {}, name: "abc", items: [{ v: 1 }, { v: 2 }] });
    assertEq(out, "[][][3][2]<1><2>");
});

// ── trusted hosts and ports ────────────────────────────────────────

await test("session.logoutHandler: a bare trusted host means its default port", () => {
    session.init({ cleanup: false });
    const lo = session.logoutHandler(cookie, { trustedHosts: ["app.example.com",
                                                              "admin.example.com:8443"] });
    const run = (origin) => {
        const res = mkRes();
        lo({ method: "POST", headers: { host: "internal", origin } }, res);
        return res.code;
    };
    assertEq(run("https://app.example.com"), 200);
    assertEq(run("https://app.example.com:443"), 200, "https default port");
    assertEq(run("http://app.example.com:80"), 200, "http default port");
    assertEq(run("https://app.example.com:8443"), 403, "another port");
    assertEq(run("http://app.example.com:443"), 403, "443 is not http's default");
    assertEq(run("https://admin.example.com:8443"), 200, "host:port entry");
    assertEq(run("https://admin.example.com"), 403, "host:port entry, default port");
});

// ── auth-flows ─────────────────────────────────────────────────────

let users = new Map();
function afInit() {
    __testHooks("hull:web:auth-flows").reset();
    authFlows.init({
        secret: "s".repeat(32),
        publicOrigin: "https://app.example.com",
        trustedHosts: ["app.example.com"],
        emailRateLimit: false,
        emailSend: () => {},
        templates: {
            email_change:        (c) => ({ subject: "e", text: c.link }),
            email_change_notify: (c) => ({ subject: "n", text: c.revoke_url }),
        },
        userFindByEmail: (e) => {
            for (const u of users.values()) if (u.email === e) return u;
            return null;
        },
        userGet: (id) => users.get(String(id)) || null,
        userCreate: () => null,
        userSetPassword: () => {},
        userSetEmail: (id, e) => { users.get(String(id)).email = e; },
        userSetEmailVerified: () => {},
        onLogin: (_req, res) => { res.json({ ok: true }); },
        onPasswordReset: () => {},
        emailChangeReauth: () => true,
    });
}

await test("authFlows: trustedHosts matches a non-default port only exactly", () => {
    afInit();
    const so = __testHooks("hull:web:auth-flows").sameOriginRequest;
    const run = (origin) => so({ headers: { host: "internal", origin,
                                            "content-type": "application/json" } });
    assertEq(run("https://app.example.com:443"), true, "default port");
    assertEq(run("https://app.example.com:8443"), false, "another port");
});

await test("authFlows: email change takes user id 0, refuses a non-id", async () => {
    users = new Map([["0", { id: 0, email: "zero@a24.test", email_verified: true }]]);
    afInit();
    const H = __testHooks("hull:web:auth-flows").handlers;
    const run = async (uid) => {
        const res = mkRes();
        await H.emailChange({ method: "POST", path: "/auth/email-change",
                              headers: { host: "app.example.com",
                                         "content-type": "application/json",
                                         "sec-fetch-site": "same-origin" },
                              body: JSON.stringify({ new_email: "new0@a24.test" }),
                              ctx: { user_id: uid }, remote_addr: "192.0.2.9" }, res);
        return res;
    };
    assertEq((await run("")).code, 401, "an empty id");
    assertEq((await run({})).code, 401, "an object id");
    assertEq((await run(1.5)).code, 401, "a fraction");
    const r = await run(0);
    assertEq(r.code, 200, "user 0 is a user");
    assertEq(r.body && r.body.ok, true);
});

// Counts go back to the C harness (run_js_test in
// tests/hull/runtime/js/test_js.c), which reads them off the global object.
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
