// examples/workflows (JS) - durable workflow-as-code + observability.
//
// A workflow is a job whose body is ordinary code: `await ctx.step(name, fn)`
// runs a step ONCE and memoizes its result, so a crash or retry resumes past
// completed steps instead of re-charging a card. `ctx.waitSignal` parks the
// workflow durably until an external event arrives; saga `compensate` callbacks
// roll back completed steps on terminal failure; `ctx.now/uuid` stay stable
// across replays.
//
// This models order fulfillment: charge -> reserve stock -> WAIT for a shipping
// confirmation (could be minutes or days) -> notify. The workflow instance IS a
// job, so it survives restarts, retries with backoff, and reports progress.
//
// Try it:
//   hull examples/workflows/app.js -d ./wf.db
//   ID=$(curl -s -X POST localhost:3000/orders -d '{"sku":"A1","card":"tok_ok"}' | tr -dc 0-9)
//   curl localhost:3000/orders/$ID                 // {status:"waiting", stepsDone:["charge","reserve"]}
//   curl -X POST localhost:3000/orders/$ID/ship -d '{"tracking":"1Z999"}'
//   curl localhost:3000/orders/$ID                 // {status:"done", result:{...}}
//   curl localhost:3000/metrics                    // gauges + latency percentiles
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { app } from "hull:app";
import { jobs } from "hull:jobs";
import { json } from "hull:json";
import { log } from "hull:log";

app.manifest({
    modules: [
        "hull/jobs@1",
        "hull/json@1",
        "hull/http-server@1",
        "hull/timers@1",
        "hull/log@1",
    ],
});

// history: true turns on the opt-in attempt-history table, which powers the
// latency / throughput fields of jobs.metrics (gauges work without it).
jobs.init({ history: true });

// A tiny in-memory "inventory" so the saga compensation is observable. Real code
// would use db.* here; the point is the ROLLBACK, not the store.
const reserved = {};

// ── The workflow ─────────────────────────────────────────────────────────────
// Register once at startup. `ctx.input` is the payload from jobs.start.
jobs.workflow("fulfill_order", async (ctx) => {
    const order = ctx.input;

    // Step 1: charge the card. Memoized: if the workflow resumes after this
    // point, the charge is NOT repeated. A declined card returns ok=false (no
    // charge); the body then dead-letters below. ctx.uuid()/ctx.now() are
    // memoized (byte-identical on every replay), so the receipt id is stable.
    const charge = await ctx.step("charge", async () => {
        if (order.card === "tok_declined") return { ok: false };
        // In JS the deterministic primitives are async (they memoize via the step
        // store) - await them, like ctx.step itself.
        return { ok: true, receipt: await ctx.uuid(), amount: order.amount || 4200, at: await ctx.now() };
    });
    if (!charge.ok) {
        return jobs.DEAD;   // non-retryable; nothing reserved yet, nothing to roll back
    }

    // Step 2: reserve inventory, WITH a compensation. If the workflow fails
    // terminally after this, `compensate` runs (reverse order) to release stock.
    await ctx.step("reserve", () => {
        reserved[ctx.id] = order.sku || "A1";
        log.info(`reserved stock for order ${ctx.id}`);
        return true;
    }, {
        compensate: () => {
            delete reserved[ctx.id];
            log.warn(`compensated: released stock for order ${ctx.id}`);
        },
    });

    // A fraud flag fails the order AFTER reserving: returning jobs.DEAD from the
    // body dead-letters it and runs the saga compensations (releasing the stock).
    if (order.card === "tok_fraud") {
        return jobs.DEAD;
    }

    // Step 3: park until the warehouse confirms shipment. The workflow yields to
    // the durable 'waiting' status here - no worker CPU is held while it waits.
    // jobs.signal(id, "shipped", payload) wakes it; the payload is the return.
    const ship = await ctx.waitSignal("shipped");

    // Step 4: notify the customer (memoized, so a resume won't double-send).
    await ctx.step("notify", () => {
        log.info(`order ${ctx.id} shipped, tracking ${ship?.tracking}`);
        return true;
    });

    // The return value becomes the workflow result (jobs.result / jobs.await).
    return {
        order_id: ctx.id,
        receipt: charge.receipt,
        amount: charge.amount,
        tracking: ship?.tracking,
    };
});

// In-process execution model: an app.every timer drives jobs.work, which claims
// and advances workflows (and fires the reaper). For a dedicated worker process
// instead, see worker.js.
app.every(500, () => { jobs.work({ batch: 10 }); });

// ── Routes ───────────────────────────────────────────────────────────────────

// Start an order-fulfillment workflow. Returns the workflow (job) id.
app.post("/orders", (req, res) => {
    const body = json.decode(req.body || "{}") || {};
    const id = jobs.start("fulfill_order", body);
    res.json({ order: id });
});

// Inspect a workflow instance: status + which steps have run + the result.
app.get("/orders/:id", (req, res) => {
    const st = jobs.workflowStatus(Number(req.params.id));
    if (!st) { res.status(404).json({ error: "no such order" }); return; }
    res.json(st);
});

// Deliver the "shipped" signal, waking a workflow parked on waitSignal.
app.post("/orders/:id/ship", (req, res) => {
    const body = json.decode(req.body || "{}") || {};
    jobs.signal(Number(req.params.id), "shipped", { tracking: body.tracking });
    res.json({ signalled: true });
});

// Observability: DB-derived fleet-correct gauges + (with history on) latency
// percentiles and throughput. Point a dashboard at this.
app.get("/metrics", (_req, res) => {
    res.json(jobs.metrics());
});

// Live saga state: which orders currently hold reserved stock. A successful
// order keeps its entry; a compensated (fraud-dead-lettered) order has its
// entry released - so the rollback is observable right here.
app.get("/inventory", (_req, res) => {
    const held = {};
    for (const id of Object.keys(reserved)) held[id] = reserved[id];
    res.json({ reserved: held });
});
