# jobs durable execution - workflow-as-code (Bet #2)

**Status:** PROPOSED / design RFC. Not scheduled. The flagship differentiator.
**Target:** `hull/jobs@1` (a multi-phase epic, not a single release).
**Decisions (from review):**
- **Phased execution model:** Phase 1 ships **step-memoization** (DBOS/Restate
  style); Phase 2 adds an opt-in **deterministic-WASM-replay** "strict mode"
  (Temporal style) for workflows that need bulletproof determinism.
- **Phase 1 surface:** `ctx.step` + **durable timers** (`ctx.sleep`) +
  **signals** (`ctx.wait_signal` / `jobs.signal`) + **queries**
  (`jobs.workflow_status`). Saga **compensation** included. Child workflows
  deferred.
- Full parity Lua + JS. Rides the existing job infrastructure (claim / reap /
  retry / worker / result / await) - a durable workflow is **"just a job that
  yields."** Minimal/zero new C.

> **Why Hull, uniquely.** Durable execution (Temporal / DBOS / Restate) is the
> current frontier for orchestration, and Hull is uniquely positioned: it already
> has a **transactional queue** fused with the app's data AND a **deterministic,
> gas-metered, sandboxed compute engine** (WASM) next to it. Deterministic replay
> is the hard part of Temporal - and the WASM sandbox is exactly the deterministic
> executor that makes Phase 2 credible. No other job runner has both halves.

---

## 1. The model

A **durable workflow** is a long-lived process expressed as a normal Lua/JS
function `fn(ctx)`. It can loop, branch, sleep for days, and wait for external
signals, and it **survives crashes and restarts**: on recovery it resumes where
it left off, without re-doing completed work.

The mechanism is **step-memoization**, not full replay:

- The workflow function is **re-entered from the top** on every resume.
- Each `ctx.step(name, fn)` checks a durable store: if this step already
  completed, it **returns the memoized result** without re-running `fn`; else it
  runs `fn`, persists the result, and returns it.
- So the code *between* steps re-runs on each resume (must be cheap and
  side-effect-free); all side effects live inside steps and run **once**
  (see §4 for the exactly-once vs at-least-once contract).

This is weaker than Temporal's whole-body determinism but far more ergonomic for
Lua/JS: only the **sequence of `ctx.step` calls** must be stable across resumes,
not the entire body. Phase 2 (§11) tightens this to full determinism for WASM
workflows.

A workflow instance **is a row in `_hull_jobs`** (a job with a workflow marker),
so it is claimed, reaped, retried, and drained by the exact same worker
infrastructure. "Yielding" (sleep / wait) is just rescheduling that job.

## 2. API

```lua
-- Define a workflow (like jobs.handler, but the fn receives a durable ctx):
jobs.workflow("order_fulfillment", function(ctx)
  local charge = ctx.step("charge", function()
    return payments.charge(ctx.input.order_id, ctx.input.amount)   -- runs once
  end)

  ctx.sleep(24 * 3600)                          -- durable: survives restarts/redeploys

  local approval = ctx.wait_signal("manager_approval")   -- pause until signalled

  ctx.step("ship", function()
    return shipping.dispatch(ctx.input.order_id, approval.by)
  end, { compensate = function() shipping.recall(ctx.input.order_id) end })

  return { shipped = true, charge = charge.id }
end)

-- Start an instance (returns a workflow id = a job id):
local wf = jobs.start("order_fulfillment", { order_id = 42, amount = 100 })

-- Deliver a signal from anywhere (e.g. an HTTP approval handler):
jobs.signal(wf, "manager_approval", { by = "alice" })

-- Query progress (DB-derived; no live process needed):
local st = jobs.workflow_status(wf)
-- { status="running", steps_done={"charge"}, waiting_for="signal:manager_approval", started_at=.. }

-- Await / fetch the final result (reuses the v1.5 result backend):
local r = jobs.await(wf)          -- { status="done", result={ shipped=true, ... } }
```

```javascript
jobs.workflow("order_fulfillment", async (ctx) => {
  const charge = await ctx.step("charge", () => payments.charge(ctx.input.orderId, ctx.input.amount));
  await ctx.sleep(24 * 3600);
  const approval = await ctx.waitSignal("manager_approval");
  await ctx.step("ship", () => shipping.dispatch(ctx.input.orderId, approval.by),
                 { compensate: () => shipping.recall(ctx.input.orderId) });
  return { shipped: true, charge: charge.id };
});
const wf = jobs.start("order_fulfillment", { orderId: 42, amount: 100 });
jobs.signal(wf, "manager_approval", { by: "alice" });
const st = jobs.workflowStatus(wf);
const r = await jobs.await(wf);
```

**The `ctx` object:**
- `ctx.input` - the payload passed to `jobs.start`.
- `ctx.id` - the workflow id (job id).
- `ctx.step(name, fn, opts?)` - run-once-and-memoize; `opts.compensate` registers
  a saga rollback (§8). `name` is unique per workflow instance.
- `ctx.sleep(seconds)` - durable timer (§6).
- `ctx.wait_signal(name, opts?)` / `ctx.waitSignal` - block until a signal (§7);
  `opts.timeout` optional.
- `ctx.trace` - the propagated trace context (Bet #1, pillar C).

**Module functions:**
- `jobs.workflow(name, fn)` - register a definition (a specialized handler).
- `jobs.start(name, input, opts?)` - start an instance -> workflow id. `opts` are
  the usual enqueue opts (queue, priority, dedup_key for idempotent start, trace).
- `jobs.signal(id, name, payload)` - deliver a signal; unblocks a waiting
  workflow.
- `jobs.workflow_status(id)` / `jobs.workflowStatus` - query state.
- `jobs.await(id)` / `jobs.result(id)` - reuse the v1.5 result backend for the
  final return value.

## 3. How a workflow rides the job loop

`jobs.workflow(name, fn)` registers, under the hood, a normal job **handler** for
a reserved type (`__wf:<name>`). `jobs.start` enqueues a job of that type with a
`workflow_name` marker and the input as payload. `jobs.work` claims and dispatches
it exactly like any job; the handler is the **workflow runner** that builds `ctx`
and calls `fn(ctx)`. The runner interprets the three yield points:

- `fn` returns normally -> the runner returns its value -> `mark_done` stores it
  as the workflow result (reuses `_hull_job_results` + `jobs.await`).
- `fn` raises -> retry/backoff/dead exactly like any job; on dead, saga
  compensations run (§8).
- `fn` hits an unsatisfied `ctx.sleep` / `ctx.wait_signal` -> the runner **yields**
  (§5): the job is rescheduled/blocked and re-run later; **no terminal event**.

So durable workflows inherit at-least-once dispatch, the visibility-timeout
reaper (a crashed workflow mid-step is reclaimed and resumed), backoff, priority,
queues, concurrency, and `run_worker` - for free.

## 4. Execution contract

- **Steps are at-least-once by default.** A step's `fn` runs, then its result is
  persisted. If the worker crashes *between* the side effect and the persist, the
  step re-runs on resume. So a step with an external side effect (an HTTP POST)
  must be **idempotent**, same rule as a job handler.
- **Steps can be exactly-once when the effect is DB-local.** A step that does its
  work and records its result in the **same `db.batch`** commits atomically -
  either both or neither - giving exactly-once for DB-transactional steps. The
  runner exposes this: `ctx.step(name, fn, { transactional = true })` wraps the
  fn + the result-write in one transaction.
- **The step-call sequence must be stable across resumes.** Branching on
  `ctx.input` or on prior step *results* is fine (those are stable); branching on
  ambient non-determinism (wall-clock, RNG, external reads done *outside* a step)
  can change which steps are reached and break memo-key matching. Rule:
  **all non-determinism goes inside a `ctx.step`.** (`ctx.sleep` uses the clock
  safely on the runner's behalf.) Phase 2 enforces this structurally for WASM.
- **Between-step code re-runs on every resume** - keep it pure and cheap. Heavy
  or effectful work belongs in a step.

## 5. The yield mechanism

`work()` gains one internal outcome beyond done/retry/dead: **yield**. The
workflow runner signals it (a private sentinel, not part of the public
handler-return contract) carrying either a wake time (sleep) or a block reason
(signal). On yield, `work()`:

- for a **sleep**: sets the job `pending` with `run_at = wake_at` (reuses the
  `mark_retry` reschedule path; no attempt increment, no backoff) - the durable
  timer is just a future-dated pending job;
- for a **signal wait**: sets the job `status = 'waiting'` (a new non-terminal
  status, invisible to the claim query) - it is re-activated to `pending` only by
  `jobs.signal`;

and emits **no** `completed`/`dead` event (those fire only on true termination).

## 6. Durable timers - `ctx.sleep`

`ctx.sleep(seconds)` is a memoized step whose stored value is its **wake time**:

1. First encounter: record `sleep:<n>` with `wake_at = now + seconds`; **yield**
   (reschedule the job to `run_at = wake_at`).
2. On resume, the body re-runs; `ctx.sleep` finds the recorded `wake_at`. If
   `now >= wake_at`, it returns (continue past it); else it yields again (a
   spurious early wake just re-sleeps).

Because the sleeping workflow is only a future-dated pending job, sleeps of
arbitrary length survive process restarts, redeploys, and crashes at zero cost
(no held thread, no timer in memory).

## 7. Signals - `ctx.wait_signal` + `jobs.signal`

The human-in-the-loop / wait-for-webhook / approval primitive.

- `ctx.wait_signal(name)`: if a signal row `(workflow_id, name)` exists, return
  its payload (memoized - consumed once); else **yield** with `status='waiting'`.
- `jobs.signal(id, name, payload)`: INSERT the signal row, then flip the workflow
  job `waiting -> pending` so a worker re-runs it; the `wait_signal` now finds the
  signal and proceeds. Idempotent-ish: a duplicate signal name is either ignored
  or last-wins (design choice; default: first delivery wins, extras ignored).
- `opts.timeout`: a `wait_signal` with a timeout also arms a `ctx.sleep`-style
  wake, returning `nil` / a timeout marker if no signal arrives in time.
- Signals can be delivered **before** the workflow reaches the wait (stored and
  consumed when it gets there) - no lost-signal race.

## 8. Saga / compensation

`ctx.step(name, fn, { compensate = cfn })` registers `cfn` as the rollback for a
successful step. If the workflow later **fails terminally** (dead), the runner
runs the registered compensations of **completed** steps in **reverse order**
before finalizing - the saga pattern (undo the charge if shipping can't be
arranged). Compensations are themselves at-least-once (idempotent) and their
execution is recorded so a crash mid-compensation resumes correctly.

## 9. Queries - `jobs.workflow_status`

Because all progress is persisted (step results, current status, pending waits),
querying a workflow is a **DB read**, not a message to a live process (an
advantage over Temporal's in-memory queries - it works even when no worker is
currently running the instance):

```lua
jobs.workflow_status(id) -> {
  status      = "running" | "waiting" | "sleeping" | "done" | "dead",
  name        = "order_fulfillment",
  steps_done  = { "charge" },              -- completed step names, in order
  waiting_for = "signal:manager_approval", -- or "sleep:<wake_at>" or nil
  started_at  = <ts>, updated_at = <ts>,
  result      = <value>,                   -- when done (via _hull_job_results)
  error       = <string>,                  -- when dead
}
```

## 10. Storage

Two new tables + one marker column; the workflow instance itself reuses
`_hull_jobs`.

```
_hull_workflow_steps(
  workflow_id  INTEGER NOT NULL,
  step_key     VARCHAR(255) NOT NULL,   -- ctx.step name / "sleep:<n>" / "signal:<name>"
  result       TEXT,                    -- JSON step result (or wake_at for sleeps)
  status       VARCHAR(16) NOT NULL,    -- 'done' | 'compensated'
  created_at   INTEGER NOT NULL,
  PRIMARY KEY (workflow_id, step_key)
)
_hull_workflow_signals(
  workflow_id  INTEGER NOT NULL,
  name         VARCHAR(255) NOT NULL,
  payload      TEXT,
  created_at   INTEGER NOT NULL,
  consumed_at  INTEGER,
  PRIMARY KEY (workflow_id, name)
)
-- _hull_jobs gains: workflow_name VARCHAR(255)  (nullable marker; appended via ensure_column)
-- _hull_jobs.status gains a non-terminal value: 'waiting' (excluded by the claim query)
```

Cleanup: `jobs.cleanup` sweeps step/signal rows whose workflow job is gone
(mirrors the existing `_hull_job_results` orphan sweep).

## 11. Phase 2 - deterministic WASM replay (strict mode)

Phase 1's contract ("keep non-determinism in steps") is a discipline the
developer must follow. Phase 2 makes it **structural** for workflows that opt in
by running the workflow body as a **WASM compute module**:

- The body executes in the gas-metered WASM sandbox with **no ambient I/O, clock,
  or RNG** - every effect is a host-call that maps to a `ctx.step` (memoized) or a
  yield. Determinism is enforced by the sandbox, not by convention.
- Re-execution is true **deterministic replay**: the same inputs + the memoized
  step log reproduce the exact same execution, so the body *cannot* drift.
- This is the Temporal guarantee, achieved with the compute engine Hull already
  ships. `jobs.workflow(name, fn, { strict = true })` (or a `.wasm` workflow
  module) selects it.

Deferred: the host-call ABI for the WASM<->step bridge, the workflow-to-WASM
build path, and replay tooling are their own design pass.

## 12. Relationship to existing `depends_on` workflows (v1.4)

Two complementary orchestration models, both first-class:

| | Declarative DAG (`depends_on`, v1.4) | Durable execution (this doc) |
|---|---|---|
| Shape | static graph of independent jobs | imperative code-as-workflow |
| Best for | known fan-out/fan-in pipelines | dynamic control flow, long-running, human-in-the-loop |
| Time | each node runs to completion | can sleep days, wait for signals |
| Result flow | injected as `job.deps` | `ctx.step` return values in-line |

They interoperate: a durable workflow's step may enqueue DAG jobs (and
`ctx.step` await their results via `jobs.await`), and a DAG node may `jobs.start`
a workflow.

## 13. Observability integration (Bet #1)

- Each `ctx.step` records to the attempt-history timeline (when history is on), so
  `jobs.history(workflow_id)` shows the step-by-step execution with durations.
- `jobs.metrics` counts running/waiting/sleeping workflows and step latencies.
- Trace context (`ctx.trace`) flows from `jobs.start` through steps to any child
  jobs - one trace per business process.

## 14. Reused infrastructure (what we do NOT rebuild)

Claim / atomic dispatch, the visibility-timeout reaper (resumes a crashed
in-step workflow), retry + backoff, priority, named + multi queues, concurrency,
`run_worker` + `hull jobs worker`, rate limiting, pause/resume, the result
backend (`await`/`result`), and cleanup - all apply to workflows unchanged,
because a workflow instance *is* a job.

## 15. Testing

- **Step memoization:** a workflow with 3 steps; kill it after step 2 (simulate
  by throwing post-step-2 and re-claiming via the reaper); on resume steps 1-2
  return memoized results (their `fn`s do not re-run - assert via a side-effect
  counter), step 3 runs; final result correct. Both runtimes.
- **Durable timer:** `ctx.sleep(2)` -> the job is future-dated pending, not
  running; after the delay it resumes and completes. Survives a process restart
  (run in one process, resume in another against a file DB).
- **Signals:** `ctx.wait_signal` yields to `waiting`; `jobs.signal` before AND
  after the wait both deliver; timeout path returns the timeout marker.
- **Saga:** a workflow that fails after a compensated step runs the compensation
  in reverse (assert order + idempotency on re-run).
- **Queries:** `jobs.workflow_status` reports steps_done / waiting_for / result
  across the lifecycle.
- **Exactly-once DB step:** a `transactional` step + a crash between effect and
  persist does not double-apply (the effect + result commit atomically).
- **Interop:** a workflow step enqueues a DAG job and awaits it.

## 16. Phasing

1. **Phase 1a - steps + result:** `jobs.workflow`/`start`, `ctx.step` +
   memoization, the yield outcome, `_hull_workflow_steps`, `jobs.workflow_status`,
   `jobs.await` on a workflow. The core.
2. **Phase 1b - durable timers:** `ctx.sleep` (reschedule via `run_at`).
3. **Phase 1c - signals + queries:** `ctx.wait_signal` / `jobs.signal`, the
   `waiting` status, `_hull_workflow_signals`, timeouts.
4. **Phase 1d - saga:** `ctx.step` compensation on terminal failure.
5. **Phase 2 - strict mode:** deterministic WASM-replay workflows (separate
   design pass).
6. (Later) child/sub-workflows.

## 17. Scope discipline

**Do:** durable steps, timers, signals, queries, sagas, the WASM strict-mode path
(Phase 2). **Don't** (for now): a visual workflow designer, child-workflow trees
(deferred), cross-workflow transactions, or a bespoke workflow DSL - workflows are
plain Lua/JS functions. Keep the core dependency-free and riding the existing job
loop.
