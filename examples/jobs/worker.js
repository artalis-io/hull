// examples/jobs (JS) - the DEDICATED WORKER model.
//
// Same handlers as app.js, but instead of an `app.every` poller inside a web
// app, this is a standalone process whose app.main runs the blocking claim
// loop. Run K copies for horizontal scale; each claims disjoint jobs via the
// atomic claim (SKIP LOCKED on Postgres/MySQL, serialized on SQLite).
//
// Run it against the SAME database the web app enqueues into:
//   hull examples/jobs/worker.js -d ./jobs.db
//   # or, discoverable via the CLI (identical effect):
//   hull jobs worker examples/jobs/worker.js -d ./jobs.db
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { app } from "hull:app";
import { jobs } from "hull:jobs";
import { log } from "hull:log";

app.manifest({ modules: ["hull/jobs@1", "hull/log@1"] });

app.main(async () => {
    jobs.init();

    jobs.handler("send_email", (job) => {
        log.info(`send_email -> ${job.data?.to}`);
    });
    jobs.handler("flaky", (job) => {
        if ((job.attempts || 0) < 3) {
            throw new Error(`transient failure (attempt ${job.attempts})`);
        }
    });

    log.info("worker started - draining the 'default' queue");
    // Blocks: claim a batch, dispatch, reap; sleep 500ms when idle. Runs until
    // the process is signalled; an in-flight job is then reclaimed by the
    // visibility-timeout reaper (handlers are at-least-once).
    await jobs.runWorker({ batch: 20, pollMs: 500 });
    return 0;
});
