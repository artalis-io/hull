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

test("the case is known", () => {
    assertEq(typeof cases[globalThis.__jobs_case], "function", String(globalThis.__jobs_case));
});
if (cases[globalThis.__jobs_case]) cases[globalThis.__jobs_case]();

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
