// test_jobs_hooks.js - hull:jobs through its clock-injectable test hooks (JS).
//
// The cron tick, the cron math and the subscription drain take a caller-chosen
// clock. Exported on the module (jobs._tick / _cronNext / _eventsDrain), app
// code wrote _hull_* rows at a time it chose: jobs._tick(1e12) pushed every
// schedule's nextRunAt decades out, a drain at now = 1e15 held its
// subscription's lease for ever, and app SQL cannot repair either (audit 17).
// They are hull:_test_hooks entries now, reached here through the harness's
// __testHooks. These cases moved from tests/e2e_jobs.sh, which can no longer
// reach them: v1.1 cron, durable events Phase 2 / Phase 3, the v1.5 polish
// tz-offset hour, audit 9 HORIZON and audit 10 CRONTZ / STRICT / ASYNCSUB.
//
// Each case needs a fresh database (event ids, log depth), so the C harness
// (tests/hull/runtime/js/test_js.c, run_jobs_hooks_case) runs this script
// once per case in a new context, naming the case in globalThis.__jobs_case.

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

const hooks = __testHooks("hull:jobs");
const cases = {};

// v1.1 cron + v1.5 polish + audit 10 CRONTZ / STRICT.
cases.cron = () => {
    jobs.init();
    test("cronNext math on a fixed epoch", () => {
        const b = 1609459200;   // 2021-01-01 00:00:00 UTC
        assertEq(hooks.cronNext("*/15 * * * *", b) - b, 900, "*/15");
        assertEq(hooks.cronNext("0 0 * * *", b) - b, 86400, "midnight");
        assertEq(hooks.cronNext("30 2 * * *", b) - b, 9000, "02:30");
    });
    test("a fixed-offset tz moves the hour", () => {
        const nx = hooks.cronNext("0 9 * * *", 1735689600, 2 * 3600);
        assertEq(Math.floor((nx % 86400) / 3600), 7, "09:00+02:00 in UTC");
    });
    test("a tz offset skips on local day / hour boundaries", () => {
        const b = 1735689600;   // 2025-01-01T00:00Z
        assertEq(hooks.cronNext("0 9 * * *", b, 19800) - b, 12600, "+05:30");
        assertEq(hooks.cronNext("0 0 * * 1", b, 7200) - b, 424800, "Mon +02:00");
        const chain = []; let t = b;
        for (let i = 0; i < 5; i++) { t = hooks.cronNext("0 9 * * 1-5", t, -18000); chain.push(t - b); }
        assertEq(chain.join(","), "50400,136800,223200,482400,568800", "1-5 -05:00");
        assertEq(hooks.cronNext("0 1 * * 0", b, -10800) - b, 360000, "Sun -03:00");
    });
    test("cron fields are plain decimal digits", () => {
        const b = 1735689600;
        for (const s of ["5abc * * * *", "0x5 * * * *", "5.0 * * * *", "1e1 * * * *",
                         "1,,2 * * * *", ",1 * * * *", "*/5/2 * * * *", "1-2-3 * * * *"]) {
            assertEq(hooks.cronNext(s, b), null, s);
        }
        assertEq(hooks.cronNext("5,10-12 */2 * * 1-5", b) !== null, true, "a valid spec");
    });
    test("a bad tz is refused", () => {
        for (const tz of [NaN, Infinity, 2000, 1.5, "+25:00", "+05:75", "+18:59", "-1801"]) {
            let refused = false;
            try { jobs.cron("tzx", "0 9 * * *", null, { tz }); } catch (_e) { refused = true; }
            assertEq(refused, true, String(tz));
        }
    });
    test("a due schedule fires once; a second tick does not double-fire", () => {
        jobs.cron("beep", "* * * * *");
        hooks.tick(time.now() + 120);
        const c1 = jobs.claim({ batch: 10 });
        assertEq(c1.length, 1, "fired");
        assertEq(c1[0].type, "beep", "type");
        hooks.tick(time.now() + 120);
        assertEq(jobs.claim({ batch: 10 }).length, 0, "no double fire");
        assertEq(jobs.uncron("beep"), true, "uncron");
    });
};

// durable events Phase 2: capture, at-least-once, lease reclaim, retention.
cases.events = () => {
    jobs.init({ events: true });
    const future = time.now() + 100;   // past every event ts
    jobs.enqueue("ok", {}); jobs.enqueue("ok", {}); jobs.enqueue("ok", {});
    test("a drain delivers in id order, once", () => {
        const seen = [];
        jobs.subscribe("cap", (ev) => { seen.push(ev.id); }, { from: "beginning" });
        assertEq(hooks.eventsDrain("cap", { now: 1000 }).delivered, 3, "delivered");
        assertEq(hooks.eventsDrain("cap", { now: 1000 }).delivered, 0, "recaught");
        assertEq(seen.join(","), "1,2,3", "order");
    });
    test("a crash before the cursor write re-delivers", () => {
        let dc = 0;
        jobs.subscribe("dup", () => { dc++; }, { from: "beginning" });
        hooks.eventsDrain("dup", { now: 1000, commitCursor: false });
        hooks.eventsDrain("dup", { now: 1000 });
        assertEq(dc, 6, "delivered twice");
    });
    test("a dead drainer's lease is reclaimed once it expires", () => {
        jobs.subscribe("lease", () => {}, { from: "beginning" });
        hooks.eventsDrain("lease", { now: 1000, commitCursor: false, releaseLease: false });
        assertEq(hooks.eventsDrain("lease", { now: 1000 }).leased, false, "held");
        assertEq(hooks.eventsDrain("lease", { now: 1040 }).delivered, 3, "reclaimed");
    });
    test("retention never passes the slowest cursor", () => {
        jobs.subscribe("lag", () => {}, { from: "beginning" });
        const before = jobs.events({ limit: 100 }).length;
        jobs.cleanup({ before: future });
        assertEq(jobs.events({ limit: 100 }).length, before, "retained");
        hooks.eventsDrain("lag", { now: 1000 });
        jobs.cleanup({ before: future });
        assertEq(jobs.events({ limit: 100 }).length, 0, "purged");
    });
};

// durable events Phase 3: poison-event skip-after-N + metrics.
cases.poison = () => {
    jobs.init({ events: true });
    jobs.enqueue("ok", {}); jobs.enqueue("ok", {}); jobs.enqueue("ok", {});
    test("a poison event is skipped after maxFailures", () => {
        const seen = [];
        jobs.subscribe("p", (ev) => {
            if (ev.id === 2) throw new Error("poison");
            seen.push(ev.id);
        }, { from: "beginning", maxFailures: 2 });
        const d1 = hooks.eventsDrain("p", { now: 1000 });
        const d2 = hooks.eventsDrain("p", { now: 1000 });
        hooks.eventsDrain("p", { now: 1000 });
        assertEq(d1.delivered, 1, "d1");
        assertEq(d2.skipped, true, "d2 skipped");
        assertEq(seen.join(","), "1,3,4", "seen");
        assertEq(jobs.events({ types: ["subscription_skipped"] }).length, 1, "skip event");
        const m = jobs.metrics();
        const sub = m.events && m.events.subscriptions[0];
        assertEq(sub && sub.lag, 0, "lag");
        assertEq(sub && sub.failures, 0, "failures");
        assertEq(m.events && m.events.logDepth, 4, "log depth");
    });
};

// audit 9 HORIZON + audit 10 ASYNCSUB.
cases.horizon = () => {
    jobs.init({ events: true });
    test("events younger than the grace are held back", () => {
        jobs.subscribe("h", () => {}, { from: "now" });
        jobs.enqueue("e1", {}); jobs.enqueue("e2", {}); jobs.enqueue("e3", {});
        const now = time.now();
        assertEq(hooks.eventsDrain("h", { now, grace: 5 }).delivered, 0, "young");
        assertEq(hooks.eventsDrain("h", { now: now + 60, grace: 5 }).delivered, 3, "old");
    });
    test("eventsGrace sets the default", () => {
        jobs.init({ eventsGrace: 5 });
        jobs.subscribe("h2", () => {}, { from: "now" });
        jobs.enqueue("e4", {});
        assertEq(hooks.eventsDrain("h2", { now: time.now() }).delivered, 0, "cfg young");
    });
    test("an async subscriber is refused; a returned Promise is a failure", () => {
        let refused = false;
        try { jobs.subscribe("as", async () => {}); } catch (_e) { refused = true; }
        assertEq(refused, true, "refused");
        jobs.subscribe("ps", () => Promise.resolve(1), { from: "beginning" });
        assertEq(hooks.eventsDrain("ps", { grace: 0 }).delivered, 0, "delivered");
        assertEq(hooks.eventsDrain("ps", { grace: 0 }).delivered, 0, "still failing");
    });
};

// Audit 20: metrics threw for a queue named after an Object.prototype member;
// enqueue wrote its row before a bad dependency edge failed; a cron spec with
// NBSP between fields was valid here and invalid in the Lua twin.
cases.audit20 = () => {
    jobs.init();
    test("metrics with queues named constructor / toString", () => {
        jobs.enqueue("t", {}, { queue: "constructor" });
        jobs.enqueue("t", {}, { queue: "toString" });
        const m = jobs.metrics();
        assertEq(m.queues["constructor"].pending, 1, "constructor bucket");
        assertEq(m.queues["toString"].pending, 1, "toString bucket");
    });
    test("dependsOn with a duplicate or a non-id is refused, no row written", () => {
        const a = jobs.enqueue("dep", {});
        const before = jobs.metrics().totals.blocked;
        for (const bad of [[a, a], [null], [a, 1.5], ["x"]]) {
            let threw = false;
            try { jobs.enqueue("child", {}, { dependsOn: bad }); }
            catch (e) { threw = /distinct job ids/.test(String(e && e.message)); }
            assertEq(threw, true, "refused " + JSON.stringify(bad));
        }
        assertEq(jobs.metrics().totals.blocked, before, "no blocked row left");
        const ok = jobs.enqueue("child", {}, { dependsOn: [a] });
        assertEq(typeof ok, "number", "a valid dependsOn still enqueues");
        assertEq(jobs.metrics().totals.blocked, before + 1, "one blocked row");
    });
    test("a cron spec splits on ASCII white space only", () => {
        assertEq(hooks.cronNext("0\u00a00 * * *", 1609459200), null, "NBSP is not a separator");
        assertEq(hooks.cronNext(" 0  0 * * * ", 1609459200) - 1609459200, 86400, "ASCII spaces");
    });
    test("a21: a dependsOn that is not an array is refused", () => {
        const a = jobs.enqueue("dep", {});
        const before = jobs.metrics().totals.pending;
        for (const bad of [a, String(a), { 0: a }]) {
            let threw = false;
            try { jobs.enqueue("child", {}, { dependsOn: bad }); }
            catch (e) { threw = /array of job ids/.test(String(e && e.message)); }
            assertEq(threw, true, "refused " + JSON.stringify(bad));
        }
        assertEq(jobs.metrics().totals.pending, before, "no pending row left");
    });
};

// Audit 23: run_at is INTEGER, and a fractional second went to Postgres as
// a double it refuses (SQLite stores it as REAL, which shows here as a
// fractional runAt); the failure path ran app code - the backoff, an
// error's message getter - unprotected, so a throw there escaped work(); and
// waitSignal concatenated a string timeout onto time.now().
async function atest(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

cases.audit23 = async () => {
    jobs.init();
    await atest("a fractional ctx.sleep parks on a whole second", async () => {
        jobs.workflow("halfsleep", async (w) => { await w.sleep(0.5); return { ok: true }; });
        // Its own queue: parked, it falls due at the next whole second, and a
        // later batch-1 claim on "default" in that second took it (lower id)
        // instead of the job that case was about.
        const before = time.now();
        const id = jobs.start("halfsleep", {}, { queue: "halfsleep" });
        await jobs.work({ batch: 1, queue: "halfsleep" });
        const j = jobs.get(id);
        assertEq(j.status, "pending", "parked");
        assertEq(Number.isInteger(j.runAt), true, "runAt " + j.runAt);
        assertEq(j.runAt >= before + 1, true, "ran and parked: runAt " + j.runAt);
    });
    await atest("a string or fractional waitSignal timeout is a number of seconds", async () => {
        jobs.workflow("strwait", async (w) => {
            await w.waitSignal("go", { timeout: "2" }); return { ok: true }; });
        const before = time.now();
        const id = jobs.start("strwait", {});
        await jobs.work({ batch: 1 });
        const j = jobs.get(id);
        assertEq(j.status, "waiting", "parked");
        assertEq(typeof j.runAt, "number", "runAt " + j.runAt);
        assertEq(j.runAt >= before + 2 && j.runAt <= time.now() + 2, true, "runAt " + j.runAt);
        jobs.workflow("halfwait", async (w) => {
            await w.waitSignal("go", { timeout: 1.5 }); return { ok: true }; });
        const id2 = jobs.start("halfwait", {});
        await jobs.work({ batch: 1 });
        assertEq(Number.isInteger(jobs.get(id2).runAt), true, "runAt " + jobs.get(id2).runAt);
    });
    await atest("a fractional backoff retries on a whole second", async () => {
        jobs.init({ backoff: () => 2.5 });
        jobs.handler("fails1", () => { throw new Error("boom"); });
        const id = jobs.enqueue("fails1", {});
        await jobs.work({ batch: 1 });
        const j = jobs.get(id);
        assertEq(j.status, "pending", "retried");
        assertEq(Number.isInteger(j.runAt), true, "runAt " + j.runAt);
    });
    await atest("a throwing or NaN backoff falls back to the default", async () => {
        const bads = [() => { throw new Error("bad backoff"); }, () => NaN, () => "soon"];
        for (let i = 0; i < bads.length; i++) {
            jobs.init({ backoff: bads[i] });
            jobs.handler("failsB" + i, () => { throw new Error("boom"); });
            const id = jobs.enqueue("failsB" + i, {});
            await jobs.work({ batch: 1 });
            const j = jobs.get(id);
            assertEq(j.status, "pending", "retried " + i);
            assertEq(Number.isInteger(j.runAt), true, "runAt " + i);
        }
        jobs.init({ backoff: () => 1 });
    });
    await atest("init refuses a backoff that is not a function", async () => {
        let threw = false;
        try { jobs.init({ backoff: 5 }); } catch (e) { threw = e instanceof TypeError; }
        assertEq(threw, true, "refused");
    });
    await atest("an error whose message getter throws is still recorded", async () => {
        const bad = { get message() { throw new Error("again"); } };
        jobs.handler("badmsg", () => { throw bad; });
        const id = jobs.enqueue("badmsg", {}, { queue: "badmsg" });
        await jobs.work({ batch: 1, queue: "badmsg" });
        const j = jobs.get(id);
        assertEq(j.status, "pending", "retried");
        assertEq(typeof j.lastError, "string", "recorded");
    });
};

// audit 30: jobs.cron at every start must not drop a tick that came due
// while no worker ran; a changed spec / tz or a parked schedule recomputes.
cases.audit30 = () => {
    jobs.init();
    const SPEC = "0 0 * * *";
    test("re-registering keeps a due tick, which fires exactly once", () => {
        jobs.cron("nightly", SPEC);
        const due = time.now() - 30;
        hooks.cronSetNextRunAt("nightly", due);
        jobs.cron("nightly", SPEC, { v: 2 }, { queue: "q30" });
        assertEq(hooks.cronNextRunAt("nightly"), due, "kept");
        hooks.tick(time.now());
        const c = jobs.claim({ batch: 10, queue: "q30" });
        assertEq(c.length, 1, "fired once");
        assertEq(c[0].type, "nightly", "type");
        assertEq(c[0].data.v, 2, "the new payload");
        hooks.tick(time.now());
        assertEq(jobs.claim({ batch: 10, queue: "q30" }).length, 0, "no second fire");
        assertEq(hooks.cronNextRunAt("nightly") > time.now(), true, "advanced");
    });
    test("a changed spec recomputes next_run_at", () => {
        hooks.cronSetNextRunAt("nightly", time.now() - 30);
        jobs.cron("nightly", "30 1 * * *");
        const nx = hooks.cronNextRunAt("nightly");
        assertEq(nx > time.now(), true, "recomputed");
        assertEq(nx, hooks.cronNext("30 1 * * *", time.now()), "the new spec");
    });
    test("a changed tz recomputes next_run_at", () => {
        hooks.cronSetNextRunAt("nightly", time.now() - 30);
        jobs.cron("nightly", "30 1 * * *", null, { tz: "+02:00" });
        assertEq(hooks.cronNextRunAt("nightly") > time.now(), true, "recomputed");
    });
    test("re-registering a parked schedule re-enables it", () => {
        hooks.cronSetNextRunAt("nightly", hooks.CRON_DISABLED_AT);
        jobs.cron("nightly", "30 1 * * *", null, { tz: "+02:00" });
        const nx = hooks.cronNextRunAt("nightly");
        assertEq(nx < hooks.CRON_DISABLED_AT && nx > time.now(), true, "re-enabled");
    });
    test("a fresh schedule gets its next occurrence", () => {
        jobs.cron("fresh", SPEC);
        assertEq(hooks.cronNextRunAt("fresh"), hooks.cronNext(SPEC, time.now()), "computed");
    });
};

// Audit 38: a handler's or subscriber's error text went to the database as it
// was, and Postgres refuses a NUL or a lone surrogate (MySQL also more than
// 64 KiB in TEXT): the write threw out of work() / the drain, leaving the
// batch 'running' and the worker dead. SQLite stores such text, so here the
// checks are on what is stored: storable text, the worker alive, the
// batch-mates done, the poison skipped (tests/e2e_postgres.sh and
// e2e_mysql.sh run the same against the servers that refused it).
cases.audit38 = async () => {
    jobs.init({ events: true, history: true, backoff: () => 0 });
    const storable = (s) => typeof s === "string" && !s.includes("\u0000") &&
        !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(s) &&
        s.length <= 4096 + 3;
    await atest("a handler error the database would refuse is stored as safe text", async () => {
        jobs.handler("poison", () => {
            throw new Error("upstream: a\u0000b " + "ok \u{1F600}".slice(0, 4) + " " + "y".repeat(100000));
        });
        jobs.handler("fine", () => true);
        const p = jobs.enqueue("poison", {});
        const a = jobs.enqueue("fine", {}), b = jobs.enqueue("fine", {});
        let threw = null;
        try { await jobs.work({ batch: 10 }); } catch (e) { threw = e; }
        assertEq(threw, null, "work did not throw");
        const j = jobs.get(p);
        assertEq(j.status, "pending", "re-pended");
        assertEq(storable(j.lastError), true, "storable lastError");
        assertEq(j.lastError.slice(0, 13), "upstream: ab ", "NUL dropped");
        assertEq(j.lastError.includes("ok �"), true, "U+FFFD");
        assertEq(j.lastError.slice(-3), "...", "capped");
        assertEq(jobs.get(a).status, "done", "batch-mate a");
        assertEq(jobs.get(b).status, "done", "batch-mate b");
        const h = jobs.history(p);
        assertEq(h.length, 1, "attempt recorded");
        assertEq(storable(h[0].error), true, "storable attempt error");
    });
    await atest("an error whose message getter throws is recorded as a fixed text", async () => {
        jobs.handler("weird", () => {
            const e = new Error("x");
            Object.defineProperty(e, "message", { get() { throw new Error("no"); } });
            throw e;
        });
        const id = jobs.enqueue("weird", {}, { queue: "w38" });
        await jobs.work({ batch: 1, queue: "w38" });
        assertEq(jobs.get(id).lastError,
            "(an error value that could not be converted to text)", "fixed text");
    });
    // Audit 39: a result holding a lone surrogate / a NUL completes and reads
    // back equal (the Lua twin stores bytes that are not UTF-8 base64).
    await atest("a result holding a lone surrogate completes and reads back", async () => {
        const S = "\ud800x\u0000y";
        jobs.handler("binres", () => ({ s: S }));
        const id = jobs.enqueue("binres", {}, { queue: "bin39" });
        await jobs.work({ batch: 1, queue: "bin39" });
        const r = jobs.result(id);
        assertEq(r.status, "done", "done: " + String(jobs.get(id).lastError));
        assertEq(r.result && r.result.s, S, "result");
    });
    await atest("a result that cannot be stored dead-letters the job once", async () => {
        jobs.handler("badres", () => { const o = {}; o.self = o; return o; });
        const id = jobs.enqueue("badres", {}, { queue: "r38" });
        await jobs.work({ batch: 1, queue: "r38" });
        const j = jobs.get(id);
        assertEq(j.status, "dead", "dead");
        assertEq(j.attempts, 1, "ran once");
        assertEq(j.lastError.includes("job result cannot be stored"), true, String(j.lastError));
    });
    await atest("a runtime busy refusal is handed back without spending an attempt", async () => {
        jobs.handler("busy", () => {
            const e = new Error("fetch cap full"); e.code = "busy"; throw e;
        });
        const id = jobs.enqueue("busy", {}, { queue: "b38", maxAttempts: 1 });
        await jobs.work({ batch: 1, queue: "b38" });
        const j = jobs.get(id);
        assertEq(j.status, "pending", "pending");
        assertEq(j.attempts, 0, "uncounted");
        assertEq(j.runAt > time.now() - 1, true, "deferred");
    });
    test("a poison subscription event with such text is skipped", () => {
        jobs.subscribe("p38", (ev) => {
            if (ev.type === "dead") throw new Error("forward failed: " + "\u{1F600}".slice(0, 1) + "\u0000");
        }, { from: "beginning", maxFailures: 1 });
        jobs.subscribe("q38", (ev) => {
            if (ev.type === "dead") {
                const e = new Error("x");
                Object.defineProperty(e, "message", { get() { throw new Error("no"); } });
                throw e;
            }
        }, { from: "beginning", maxFailures: 1 });
        const d = hooks.eventsDrain("p38", { now: time.now() + 100 });
        assertEq(d.skipped, true, "p38 skipped");
        const d2 = hooks.eventsDrain("q38", { now: time.now() + 100 });
        assertEq(d2.skipped, true, "q38 skipped");
        const sk = jobs.events({ types: ["subscription_skipped"] });
        assertEq(sk.length, 2, "two skip events");
        for (const e of sk) assertEq(storable(e.data && e.data.error), true, "storable skip text");
        const d3 = hooks.eventsDrain("p38", { now: time.now() + 100 });
        assertEq(d3.leased, true, "lease released");
    });
    test("options that wedged a worker are refused up front", () => {
        const refused = (fn) => { try { fn(); return false; } catch (_e) { return true; } };
        assertEq(refused(() => jobs.cron("c38", "* * * * *", null, { type: 5 })), true, "cron type");
        jobs.cron("c38", "* * * * *", null, { type: "" });   // falls back to the name
        assertEq(refused(() => jobs.limit("q", { rate: 1.5 })), true, "fractional rate");
        assertEq(refused(() => jobs.limit("q", { rate: NaN })), true, "NaN rate");
        assertEq(refused(() => jobs.limit("q", { rate: 2, per: 0 })), true, "per 0");
        assertEq(refused(() => jobs.limit("q", { rate: 2, per: "x" })), true, "per string");
        assertEq(refused(() => jobs.limit("q", { rate: 2, per: 0.5 })), false, "per 0.5");
        jobs.limit("q", null);
        assertEq(refused(() => jobs.metrics({ window: 0 })), true, "window 0");
        assertEq(refused(() => jobs.metrics({ window: "300" })), true, "window string");
        assertEq(refused(() => jobs.metrics({ window: 60 })), false, "window 60");
    });
    await atest("ctx.patched refuses an id too long for its step key", async () => {
        jobs.workflow("longpatch", (w) => { w.patched("p".repeat(300)); return true; });
        const id = jobs.start("longpatch", {}, { queue: "l38" });
        await jobs.work({ batch: 1, queue: "l38" });
        const j = jobs.get(id);
        assertEq(String(j.lastError).includes("too long"), true, String(j.lastError));
    });
};

// Audit 39: a busy refusal is never compensated, a compensated step is never
// replayed as done, and busy hand-backs are bounded (an app can forge the code).
cases.audit39 = async () => {
    jobs.init({ backoff: () => 0 });
    const busy = (m) => { const e = new Error(m); e.code = "busy"; return e; };
    let charged = 0, refunded = 0, ship = 0;
    jobs.workflow("order39", async (c) => {
        await c.step("charge", () => { charged++; return "ch_1"; },
            { compensate: () => { refunded++; } });
        await c.step("ship", () => {
            ship++;
            if (ship === 1) throw busy("http.fetch: 64 requests in flight");
            return "shipped";
        });
        return "ok";
    });
    await atest("a busy step on the last attempt is handed back, not compensated", async () => {
        const id = jobs.start("order39", {}, { queue: "o39", maxAttempts: 1 });
        await jobs.work({ batch: 1, queue: "o39" });
        const j = jobs.get(id);
        assertEq(j.status, "pending", "handed back");
        assertEq(j.attempts, 0, "uncounted");
        assertEq(refunded, 0, "not compensated");
        hooks.makeDue(id);
        await jobs.work({ batch: 1, queue: "o39" });
        assertEq(jobs.get(id).status, "done", "done");
        assertEq(charged, 1, "charged once");
        assertEq(refunded, 0, "still not compensated");
        assertEq(ship, 2, "ship re-ran");
    });
    await atest("busy hand-backs are bounded: past the streak cap the attempt counts", async () => {
        jobs.handler("busy39", () => { throw busy("forged"); });
        const id = jobs.enqueue("busy39", {}, { queue: "b39", maxAttempts: 1 });
        hooks.busyStreak(id, 20);
        await jobs.work({ batch: 1, queue: "b39" });
        const j = jobs.get(id);
        assertEq(j.status, "dead", "counted");
        assertEq(j.attempts, 1, "one attempt");
        assertEq(hooks.busyStreak(id), undefined, "streak cleared");
    });
    await atest("a workflow past the cap compensates and carries no refusal code", async () => {
        let r = 0;
        jobs.workflow("cap39", async (c) => {
            await c.step("a", () => 1, { compensate: () => { r++; } });
            await c.step("b", () => { throw busy("forged"); });
        });
        const id = jobs.start("cap39", {}, { queue: "c39", maxAttempts: 1 });
        hooks.busyStreak(id, 20);
        await jobs.work({ batch: 1, queue: "c39" });
        const j = jobs.get(id);
        assertEq(j.status, "dead", "dead");
        assertEq(r, 1, "compensated");
        assertEq(String(j.lastError).slice(0, 17), "workflow failed: ", String(j.lastError));
    });
    await atest("a compensated step is not replayed as done", async () => {
        let r = 0, calls = 0;
        jobs.workflow("undone39", async (c) => {
            await c.step("a", () => 1, { compensate: () => { r++; } });
            await c.step("b", () => {
                calls++;
                if (calls === 1) throw new Error("transient");
                return 2;
            });
            return "ok";
        });
        const id = jobs.start("undone39", {}, { queue: "u39", maxAttempts: 3 });
        await jobs.work({ batch: 1, queue: "u39" });
        assertEq(jobs.get(id).status, "pending", "retrying");
        hooks.markStepCompensated(id, "a");
        hooks.makeDue(id);
        await jobs.work({ batch: 1, queue: "u39" });
        const j = jobs.get(id);
        assertEq(j.status, "dead", "dead, not done");
        assertEq(String(j.lastError).includes("was compensated"), true, String(j.lastError));
        assertEq(calls, 1, "b not run past the undone step");
        assertEq(r, 0, "the compensated step is not compensated again");
    });
};

// Audit 41: a ctx.step whose result cannot be stored (a BigInt, a cycle; on
// MySQL a result over 64 KiB, covered by tests/e2e_mysql.sh) ran its side
// effect again on every retry, until maxAttempts.
cases.audit41 = async () => {
    jobs.init({ backoff: () => 0 });
    const cyclic = () => { const r = { ok: true }; r.self = r; return r; };
    for (const [label, make] of [["BigInt", () => ({ n: 10n })], ["cycle", cyclic]]) {
        await atest("an unstorable step result (" + label + ") runs its side effect once", async () => {
            let charged = 0, refunded = 0;
            const name = "cyc41" + label;
            jobs.workflow(name, async (c) => {
                await c.step("charge", () => { charged++; return make(); },
                    { compensate: () => { refunded++; } });
                return "done";
            });
            const id = jobs.start(name, {}, { queue: "cy41" + label, maxAttempts: 5 });
            for (let i = 0; i < 6; i++) {
                await jobs.work({ batch: 1, queue: "cy41" + label });
                hooks.makeDue(id);
            }
            const j = jobs.get(id);
            assertEq(j.status, "dead", "dead");
            assertEq(j.attempts, 1, "dead-lettered on the first attempt");
            assertEq(charged, 1, "the side effect ran once");
            assertEq(refunded, 1, "its own compensation ran");
            assertEq(String(j.lastError).includes("step 'charge' ran but its result cannot be stored"),
                true, String(j.lastError));
        });
    }
    await atest("a body that swallows the error still never re-runs the step", async () => {
        let runs = 0;
        jobs.workflow("swallow41", async (c) => {
            try { await c.step("s", () => { runs++; return { n: 1n }; }); } catch (_) { /* swallowed */ }
            throw new Error("body failed");
        });
        const id = jobs.start("swallow41", {}, { queue: "sw41", maxAttempts: 3 });
        for (let i = 0; i < 4; i++) {
            await jobs.work({ batch: 1, queue: "sw41" });
            hooks.makeDue(id);
        }
        assertEq(jobs.get(id).status, "dead", "dead after its attempts");
        assertEq(runs, 1, "the step ran once across retries");
    });
    await atest("a storable step still memoizes and replays", async () => {
        let runs = 0, attempts = 0, seen;
        jobs.workflow("memo41", async (c) => {
            const v = await c.step("s", () => { runs++; return { n: 42 }; });
            attempts++;
            if (attempts === 1) throw new Error("again");
            seen = v.n;
            return "ok";
        });
        const id = jobs.start("memo41", {}, { queue: "m41", maxAttempts: 3 });
        await jobs.work({ batch: 1, queue: "m41" });
        hooks.makeDue(id);
        await jobs.work({ batch: 1, queue: "m41" });
        assertEq(jobs.get(id).status, "done", "done");
        assertEq(runs, 1, "ran once");
        assertEq(seen, 42, "replayed");
    });
    await atest("an operator's jobs.retry runs the unstored step again", async () => {
        let runs = 0, bad = true;
        jobs.workflow("retry41", async (c) => {
            await c.step("s", () => { runs++; return bad ? { n: 1n } : 1; });
            return "ok";
        });
        const id = jobs.start("retry41", {}, { queue: "r41", maxAttempts: 3 });
        await jobs.work({ batch: 1, queue: "r41" });
        assertEq(jobs.get(id).status, "dead", "dead");
        bad = false;
        assertEq(jobs.retry(id), true, "requeued");
        await jobs.work({ batch: 1, queue: "r41" });
        assertEq(jobs.get(id).status, "done", "done");
        assertEq(runs, 2, "ran again after the retry");
    });
};

test("the case is known", () => {
    assertEq(typeof cases[globalThis.__jobs_case], "function", String(globalThis.__jobs_case));
});
if (cases[globalThis.__jobs_case]) await cases[globalThis.__jobs_case]();

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
