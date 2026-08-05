// examples/jobs (JS) - durable background jobs with the in-process poller.
//
// Enqueue work from a request handler; process it out-of-band with retries,
// backoff, and a dead-letter path. This app uses the SINGLE-PROCESS model: an
// `app.every` timer drives `jobs.work` on the event-loop thread. For a
// dedicated worker process instead, see worker.js + the README.
//
// Try it:
//   hull examples/jobs/app.js -d ./jobs.db
//   curl -X POST localhost:3000/jobs -d '{"type":"send_email","data":{"to":"a@b.c"}}'
//   curl -X POST localhost:3000/jobs -d '{"type":"flaky"}'
//   curl localhost:3000/jobs/stats
//   curl localhost:3000/jobs/dead
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { app } from "hull:app";
import { jobs } from "hull:jobs";
import { log } from "hull:log";

app.manifest({
    modules: [
        "hull/jobs@1",
        "hull/http-server@1",
        "hull/timers@1",
        "hull/log@1",
    ],
});

// Create _hull_jobs + indexes (idempotent - safe on every boot).
jobs.init();

// A normal handler: return nothing -> the job is done.
jobs.handler("send_email", (job) => {
    log.info("send_email -> " + (job.data && job.data.to));
});

// A flaky handler: fails the first two attempts, succeeds on the third. Throwing
// reschedules with exponential backoff until maxAttempts, then the job
// dead-letters. `job.attempts` is the count of the attempt now running.
jobs.handler("flaky", (job) => {
    if ((job.attempts || 0) < 3) {
        throw new Error("transient failure (attempt " + job.attempts + ")");
    }
    log.info("flaky succeeded on attempt " + job.attempts);
});

// Catch-all for unregistered types.
jobs.default((job) => {
    log.warn("no handler for job type '" + job.type + "'");
    return jobs.DISCARD;
});

// In-process execution model: drain a batch every 500ms on the event loop.
// Fire-and-forget (the timer re-arms immediately); jobs.work catches per-job
// errors internally, so a failing handler retries rather than rejecting here.
app.every(500, () => { jobs.work({ batch: 20 }); });

// Nightly retention sweep so done/dead rows don't accumulate forever.
app.daily("03:00", () => { jobs.cleanup({ olderThan: 7 * 86400 }); });

// ── Routes ───────────────────────────────────────────────────────────────────

// Enqueue a job. Body: { type, data?, opts? } where opts can carry
// queue / priority / delay / runAt / maxAttempts / dedupKey.
app.post("/jobs", (req, res) => {
    const body = JSON.parse(req.body || "{}");
    const id = jobs.enqueue(body.type || "send_email", body.data || {}, body.opts || {});
    res.json({ enqueued: id });   // id is null when a dedupKey collapsed it
});

// Status counts (pending / running / done / dead).
app.get("/jobs/stats", (req, res) => { res.json(jobs.stats()); });

// Inspect the dead-letter queue.
app.get("/jobs/dead", (req, res) => { res.json({ dead: jobs.dead({ limit: 50 }) }); });

// Requeue a dead job for another run (fresh attempt budget).
app.post("/jobs/retry/:id", (req, res) => {
    res.json({ requeued: jobs.retry(parseInt(req.params.id, 10)) });
});
