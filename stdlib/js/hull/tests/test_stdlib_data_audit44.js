// test_stdlib_data_audit44.js - stdlib data-layer regressions from audit 44
// (JS; the Lua twin is test_stdlib_data_audit44.lua).
//
// jobs: a typed subscription moves its cursor past events it does not take
// (M10), cleanup keeps the newest job / event row as a high-water mark,
// timestamp columns are BIGINT, and the type lists / queue scopes are
// checked; idempotency: the in-flight lease outlives a parked handler (M11)
// and respond encodes once inside the store's guard; attachment.readToFile
// checks the blob's hash; kv.open at its cap is a "busy" refusal; retry
// checks factor; search snippet / highlight / index parity; inbox / outbox
// parity.
//
// Each case runs in a fresh caps-bearing context and a fresh :memory:
// database (run_data44_case in tests/hull/runtime/js/test_js.c), named in
// globalThis.__data44_case.

import { jobs } from "hull:jobs";
import { authFlows } from "hull:web:auth-flows";
import { auditLog } from "hull:web:middleware:audit-log";
import { idempotency } from "hull:web:middleware:idempotency";
import { attachment } from "hull:attachment";
import { blob } from "hull:blob";
import { fs } from "hull:fs";
import { retry } from "hull:retry";
import { search } from "hull:search";
import { inbox } from "hull:web:middleware:inbox";
import { outbox } from "hull:web:middleware:outbox";
import { encoding } from "hull:encoding";
import { json } from "hull:json";
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

const cases = {};

// M10: a typed subscription's cursor moves past the events it does not take.
cases.jobs_typed_cursor = async () => {
    const hooks = __testHooks("hull:jobs");
    jobs.init({ events: true });
    await test("a typed subscription moves past unmatched events", () => {
        jobs.subscribe("dead44", () => {}, { types: ["cancelled"], from: "beginning" });
        for (let i = 1; i <= 5; i++) jobs.enqueue("t44", { i }, { queue: "q44" });
        const ids = hooks.eventIds();
        assertEq(ids.length, 5, "five enqueued events");
        const r = hooks.eventsDrain("dead44", { grace: 0 });
        assertEq(r.delivered, 0, "none matched");
        assertEq(hooks.cursor("dead44"), ids[4], "cursor at the scanned horizon");
    });
    await test("matching events are still delivered, in order", () => {
        const seen = [];
        jobs.subscribe("canc44", (ev) => { seen.push(ev.type); },
                       { types: ["cancelled"], from: "beginning" });
        const a = jobs.enqueue("t44", {}, { queue: "q44" });
        jobs.cancel(a);
        jobs.enqueue("t44", {}, { queue: "q44" });
        const ids = hooks.eventIds();
        const r = hooks.eventsDrain("canc44", { grace: 0 });
        assertEq(r.delivered, 1, "the cancelled event");
        assertEq(seen[0], "cancelled", "type");
        assertEq(hooks.cursor("canc44"), ids[ids.length - 1], "past the later unmatched one");
    });
    await test("a full batch does not jump past undelivered matches", () => {
        let n = 0;
        jobs.subscribe("full44", () => { n++; }, { types: ["enqueued"], from: "beginning" });
        const ids = hooks.eventIds();
        const r = hooks.eventsDrain("full44", { grace: 0, batch: 1 });
        assertEq(r.delivered, 1, "one");
        assertEq(hooks.cursor("full44"), ids[0], "only the delivered one");
    });
    await test("the commit-order horizon bounds the move", () => {
        jobs.subscribe("young44", () => {}, { types: ["cancelled"], from: "now" });
        const before = hooks.cursor("young44");
        jobs.enqueue("t44", {}, { queue: "q44" });
        hooks.eventsDrain("young44", { grace: 3600 });
        assertEq(hooks.cursor("young44"), before, "held back");
    });
    await test("cleanup then reclaims the scanned events", () => {
        const now = time.now();
        hooks.eventsDrain("dead44", { grace: 0 });
        hooks.eventsDrain("canc44", { grace: 0 });
        jobs.unsubscribe("young44");
        jobs.unsubscribe("full44");
        jobs.cleanup({ before: now + 10 });
        assertEq(hooks.eventIds().length, 1, "only the high-water event is left");
    });
};

// Plausible Medium: cleanup never deletes the newest job / event row.
cases.jobs_high_water = async () => {
    const hooks = __testHooks("hull:jobs");
    const now = time.now();
    jobs.init({ events: true });
    await test("the newest event and job rows survive a cleanup that covers all", () => {
        for (let i = 1; i <= 3; i++) jobs.enqueue("hw44", { i });
        const ev = hooks.eventIds(), jb = hooks.jobIds();
        jobs.cleanup({ before: now + 10, statuses: ["pending"] });
        const ev2 = hooks.eventIds(), jb2 = hooks.jobIds();
        assertEq(ev2.length, 1, "one event left");
        assertEq(ev2[0], ev[ev.length - 1], "the newest event");
        assertEq(jb2.length, 1, "one job left");
        assertEq(jb2[0], jb[jb.length - 1], "the newest job");
    });
    await test("a subscription cursor at the head keeps the head too", () => {
        jobs.subscribe("hw44", () => {}, { from: "now" });
        jobs.enqueue("hw44", {});
        const ev = hooks.eventIds();
        hooks.eventsDrain("hw44", { grace: 0 });
        jobs.cleanup({ before: now + 10, statuses: ["pending"] });
        const ev2 = hooks.eventIds();
        assertEq(ev2[ev2.length - 1], ev[ev.length - 1], "head kept");
    });
};

// Low: timestamp columns are BIGINT; a schedule past 2^31 is stored.
cases.jobs_schedule_bigint = async () => {
    const hooks = __testHooks("hull:jobs");
    jobs.init({ events: true });
    await test("the timestamp columns are declared BIGINT", () => {
        const cols = [
            ["_hull_jobs", "run_at"], ["_hull_jobs", "claimed_at"],
            ["_hull_jobs", "created_at"], ["_hull_jobs", "updated_at"],
            ["_hull_jobs", "claim_started_at"],
            ["_hull_cron", "next_run_at"], ["_hull_cron", "last_run_at"],
            ["_hull_cron", "updated_at"], ["_hull_job_events", "ts"],
            ["_hull_job_subscriptions", "lease_until"],
            ["_hull_job_subscriptions", "updated_at"],
            ["_hull_workflow_steps", "created_at"],
            ["_hull_workflow_signals", "created_at"],
            ["_hull_workflow_signals", "consumed_at"],
            ["_hull_ratelimit", "window_start"],
        ];
        for (const tc of cols) assertEq(hooks.columnType(tc[0], tc[1]), "BIGINT", tc[0] + "." + tc[1]);
    });
    await test("a schedule past 2^31 is stored as given", () => {
        const at = Math.pow(2, 40);
        const id = jobs.enqueue("far44", {}, { at });
        assertEq(jobs.get(id).runAt, at, "runAt");
    });
};

// Low: event-type lists and queue scopes are checked.
cases.jobs_types_queue = async () => {
    jobs.init({ events: true });
    await test("jobs.events takes checked type lists", () => {
        raises(() => jobs.events({ types: [1] }), "types", "number");
        raises(() => jobs.events({ types: "dead" }), "types", "string");
        raises(() => jobs.events({ types: ["a,b"] }), "types", "comma");
        assertEq(jobs.events({ types: ["dead"] }).length, 0, "a good list");
        assertEq(jobs.events({ types: [] }).length, 0, "empty is no filter");
    });
    await test("jobs.subscribe takes checked type lists of at most 255 bytes", () => {
        raises(() => jobs.subscribe("s", () => {}, { types: [1] }), "types");
        raises(() => jobs.subscribe("s", () => {}, { types: ["x,y"] }), "types");
        raises(() => jobs.subscribe("s", () => {}, { types: [""] }), "types");
        const long = [];
        for (let i = 1; i <= 10; i++) long.push("t".repeat(30) + i);
        raises(() => jobs.subscribe("s", () => {}, { types: long }), "255");
        jobs.subscribe("s", () => {}, { types: ["dead", "cancelled"] });
    });
    await test("dead / stats / metrics check the queue scope", () => {
        for (const f of [jobs.dead, jobs.stats, jobs.metrics]) {
            raises(() => f({ queue: 5 }), "queue", "number");
            raises(() => f({ queue: {} }), "queue", "object");
            raises(() => f({ queue: "q".repeat(256) }), "queue", "long");
        }
        jobs.enqueue("dq44", {}, { queue: "dq" });
        assertEq(jobs.stats({ queue: "" }).pending, 1, "\"\" is every queue");
        assertEq(jobs.stats({ queue: "dq" }).pending, 1, "a named queue");
        assertEq(jobs.stats({ queue: "other" }).pending, 0, "another queue");
    });
};

function mkreq(key) {
    const hdrs = { "idempotency-key": key };
    return { method: "POST", path: "/a44", body: "{}", headers: hdrs, ctx: {},
             header(name) { return hdrs[String(name).toLowerCase()] || null; } };
}

function mkres() {
    const r = { headers: {} };
    r.status = (c) => { r.code = c; return r; };
    r.json = (v) => { r.body = json.encode(v); r.via = "json"; return r; };
    r.text = (v) => { r.body = v; r.via = "text"; return r; };
    r.html = (v) => { r.body = v; return r; };
    r.header = (k, v) => { r.headers[k] = v; return r; };
    return r;
}

// M11: the in-flight lease outlives a parked handler.
cases.idem_inflight_lease = async () => {
    const H = __testHooks("hull:web:middleware:idempotency");
    idempotency.init({ ttl: 60 });
    const mw = idempotency.middleware();
    await test("an in-flight claim lasts the fetch ceiling plus a minute", () => {
        const req = mkreq("k44");
        assertEq(mw(req, mkres()), 0, "claimed");
        const row = H.rows("k44")[0];
        assertEq(row.state, "inflight", "state");
        assertEq(row.expires_at - row.created_at, 660, "lease");
        assertEq(H.inflightLease, 660, "constant");
    });
};

// Low: respond encodes once, inside the store's guard.
cases.idem_respond_encode = async () => {
    const H = __testHooks("hull:web:middleware:idempotency");
    idempotency.init({ ttl: 600 });
    const mw = idempotency.middleware();
    function claim(key) {
        const req = mkreq(key);
        assertEq(mw(req, mkres()), 0, "claimed " + key);
        return req;
    }
    await test("an encode error marks the key unstored and is thrown", () => {
        const cyc = {};
        cyc.self = cyc;
        raises(() => json.encode(cyc), null, "the value cannot be encoded");
        const req = claim("enc44");
        raises(() => idempotency.respond(req, mkres(), 201, cyc), null, "thrown");
        assertEq(H.rows("enc44")[0].state, "unstored", "not left in flight");
    });
    await test("the stored string is the one sent", () => {
        const req = claim("ok44");
        const res = mkres();
        idempotency.respond(req, res, 201, { a: 1, b: "x" });
        assertEq(res.code, 201, "status");
        assertEq(res.via, "text", "the encoded string, not a second encode");
        assertEq(res.headers["Content-Type"], "application/json", "content type");
        const row = H.rows("ok44")[0];
        assertEq(row.state, "complete", "stored");
        assertEq(encoding.base64.decode(row.response_body), res.body, "same bytes");
    });
};

function statOrNull(p) {
    try { return fs.stat(p); } catch (_e) { return null; }
}

// Low: readToFile checks the blob's hash.
cases.attachment_tamper = async () => {
    blob.init({ dir: "blobs" });
    attachment.init();
    function part(text) {
        return {
            filename: "a.txt", contentType: "text/plain",
            chunks() { return [TextEncoderLike(text)]; },   // for-await takes an array
        };
    }
    // Bytes as an ArrayBuffer (no TextEncoder in the runtime: ASCII only).
    function TextEncoderLike(text) {
        const u = new Uint8Array(text.length);
        for (let i = 0; i < text.length; i++) u[i] = text.charCodeAt(i);
        return u.buffer;
    }
    await test("a blob changed on disk is not written out", async () => {
        const id = await attachment.store(part("hello attachment 44"));
        const meta = attachment.metadata(id);
        assertEq(attachment.readToFile(id, "out_ok.bin"), 19, "intact");
        assertEq(new Uint8Array(fs.read("out_ok.bin"))[0], 104, "bytes");
        let path = null;
        for (const p of ["blobs/blobs/" + meta.blob_id.slice(0, 2) + "/" + meta.blob_id,
                         "blobs/" + meta.blob_id.slice(0, 2) + "/" + meta.blob_id])
            if (statOrNull(p)) path = p;
        assertEq(path !== null, true, "found the blob file");
        fs.write(path, "HELLO attachment 44");
        assertEq(attachment.read(id), null, "read refuses it");
        assertEq(attachment.readToFile(id, "out_bad.bin"), null, "readToFile refuses it");
        assertEq(statOrNull("out_bad.bin"), null, "nothing written");
    });
};

// Low: kv.open at its cap is a coded busy refusal (globalThis.__kvn is the
// native open, a fake backend behind it; see the C runner).
cases.kv_open_busy = async () => {
    await test("the cap refusal carries code busy", () => {
        const open = [];
        for (let i = 0; i < 16; i++) open.push(__kvn("redis://127.0.0.1:1"));
        let err = null;
        try { __kvn("redis://127.0.0.1:1"); } catch (e) { err = e; }
        assertEq(err !== null, true, "refused");
        assertEq(err.code, "busy", String(err && err.message));
        for (const c of open) c.close();
        __kvn("redis://127.0.0.1:1").close();
    });
};

// Low: retry checks factor.
cases.retry_factor = async () => {
    await test("backoff and run take a finite factor >= 1", async () => {
        for (const bad of [-2, 0, 0.5, NaN, Infinity, "2", {}]) {
            raises(() => retry.backoff(2, { factor: bad }), "factor", String(bad));
            let threw = false;
            try { await retry.run(() => 1, { factor: bad }); } catch (e) {
                threw = String(e && e.message).indexOf("factor") >= 0;
            }
            assertEq(threw, true, "run " + String(bad));
        }
        assertEq(retry.backoff(3, { baseMs: 10, factor: 3 }), 90, "3^2 * 10");
        assertEq(retry.backoff(3, { baseMs: 10, factor: 1 }), 10, "1");
        assertEq(retry.backoff(2, { baseMs: 10 }), 20, "default 2");
    });
};

// Low: search parity with the Lua twin.
cases.search_parity = async () => {
    search.createIndex("s44", ["title", "body"]);
    search.index("s44", "doc1", { title: "hello world", body: "other words" });
    await test("snippet and highlight default to column 1, and both are honoured", () => {
        const rows = search.query("s44", "hello", { snippet: {}, highlight: {} });
        assertEq(rows.length, 1, "a hit");
        assertEq(typeof rows[0].snippet === "string" &&
                 rows[0].snippet.indexOf("<b>hello</b>") >= 0, true,
                 "snippet of the title: " + rows[0].snippet);
        assertEq(rows[0].highlight, "<b>hello</b> world", "highlight of the title");
    });
    await test("index refuses no column and non-string values", () => {
        raises(() => search.index("s44", "d2", {}), "at least one column");
        raises(() => search.index("s44", "d2", { title: null }), "at least one column");
        raises(() => search.index("s44", "d2", { title: true }), "strings or numbers");
        let called = false;
        const obj = { toString() { called = true; return "x"; } };
        raises(() => search.index("s44", "d2", { title: obj }), "strings or numbers");
        assertEq(called, false, "no toString ran");
        search.index("s44", "d3", { title: 44 });
        assertEq(search.query("s44", "44").length, 1, "a number is indexed");
    });
    await test("maxQueryLen counts bytes", () => {
        raises(() => search.query("s44", "ééé", { maxQueryLen: 5 }), "too long");
    });
};

// Low: the auth-flows and audit-log timestamp columns are BIGINT too.
cases.authflows_timestamps = async () => {
    const AF = __testHooks("hull:web:auth-flows");
    const AL = __testHooks("hull:web:middleware:audit-log");
    AF.reset();
    authFlows.init({
        secret: "s".repeat(32), trustRequestHost: true, emailRateLimit: false,
        emailSend: () => {}, templates: {},
        userFindByEmail: () => null,
        userGet: () => null,
        userCreate: () => 1,
        userSetPassword: () => {},
        userSetEmail: () => {},
        userSetEmailVerified: () => {},
        onLogin: (_req, res) => { res.json({ ok: true }); },
        onPasswordReset: () => {},
    });
    auditLog.init({ fingerprintSalt: "salt-a44-salt" });
    await test("the auth-flows timestamp columns are declared BIGINT", () => {
        const cols = [
            ["_hull_auth_used_tokens", "used_at"], ["_hull_auth_used_tokens", "expires_at"],
            ["_hull_auth_pending_email_changes", "created_at"],
            ["_hull_auth_pending_email_changes", "expires_at"],
            ["_hull_auth_pending_email_changes", "confirmed_at"],
            ["_hull_auth_login_attempts", "last_failed_at"],
            ["_hull_auth_login_attempts", "locked_until"],
        ];
        for (const tc of cols)
            assertEq(AF.columnType(tc[0], tc[1]), "BIGINT", tc[0] + "." + tc[1]);
    });
    await test("audit-log event_at is declared BIGINT", () => {
        assertEq(AL.columnType("_hull_audit_log", "event_at"), "BIGINT", "event_at");
    });
};

// Low: inbox source parity.
cases.inbox_source = async () => {
    inbox.init();
    await test("an empty source is the default source", () => {
        inbox.markProcessed("m44", "");
        assertEq(inbox.isDuplicate("m44"), true, "seen under the default source");
        assertEq(inbox.isDuplicate("m44", "default"), true, "named default");
        assertEq(inbox.checkAndMark("m45", ""), false, "first");
        assertEq(inbox.checkAndMark("m45"), true, "the same source");
    });
};

// Low: outbox cleanup / empty key parity.
cases.outbox_cleanup = async () => {
    const H = __testHooks("hull:web:middleware:outbox");
    outbox.init();
    await test("cleanup checks maxAge and takes alsoFailed", () => {
        for (const bad of [-1, NaN, Infinity, "60", {}])
            raises(() => outbox.cleanup(bad), "maxAge", String(bad));
        assertEq(outbox.cleanup(0, { alsoFailed: true }), 0, "alsoFailed accepted");
    });
    await test("an empty idempotency key is stored as NULL", () => {
        const id = outbox.enqueue({ kind: "webhook", destination: "https://a44.test/h",
                                    payload: "{}", idempotencyKey: "" });
        const v = H.rows(id)[0].idempotency_key;
        assertEq(v === null || v === undefined, true, "NULL");
    });
};

await test("the case is known", () => {
    assertEq(typeof cases[globalThis.__data44_case], "function",
             String(globalThis.__data44_case));
});
if (cases[globalThis.__data44_case]) await cases[globalThis.__data44_case]();

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
