// --vm-thread fixture (tests/e2e_vm_thread.sh). hull._testNativeSleep is a
// native call that ignores the run's stop flag (cap/vm_thread.h); it exists
// only under HULL_TEST_NATIVE_SLEEP=1.
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import { app } from "hull:app";

app.manifest({
    modules: ["hull/http-server@1", "hull/timers@1"],
});

let timerRuns = 0, timerStuck = false;

app.use("*", "/mw/*", (req, _res) => {
    if (req.query.ms) hull._testNativeSleep(Number(req.query.ms));
    return 0;
});

app.get("/ping", (_req, res) => res.json({ ok: true }));

// A handler stuck in a native call for ?ms.
app.get("/stuck", (req, res) => {
    hull._testNativeSleep(Number(req.query.ms) || 4000);
    res.json({ done: true });
});

// Stuck in a middleware instead.
app.get("/mw/stuck", (_req, res) => res.json({ handler: true }));

// A handler that parks (hull.sleep) and resumes: still works on the thread.
app.get("/park", async (_req, res) => {
    await hull.sleep(20);
    res.json({ parked: true });
});

// A timer that gets stuck once, on request.
app.get("/timer/stick", (_req, res) => {
    timerStuck = true;
    res.json({ armed: true });
});
app.get("/timer/runs", (_req, res) => res.json({ runs: timerRuns }));
app.every(100, () => {
    timerRuns++;
    if (timerStuck) {
        timerStuck = false;
        hull._testNativeSleep(3000);
    }
});
