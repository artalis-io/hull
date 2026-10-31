// --vm-thread turned on by the manifest alone (tests/e2e_vm_thread.sh).
//
// SPDX-License-Identifier: AGPL-3.0-or-later
import { app } from "hull:app";

app.manifest({
    modules: ["hull/http-server@1"],
    limits: { vmThread: true, vmThreadGraceMs: 400, vmThreadCeilingMs: 30000 },
});

app.get("/ping", (_req, res) => res.json({ ok: true }));
app.get("/stuck", (req, res) => {
    hull._testNativeSleep(Number(req.query.ms) || 3000);
    res.json({ done: true });
});
