# hull/jobs - durable background jobs

`hull/jobs@1` is a durable, DB-backed background job queue. Enqueue a unit of
work, process it later with retries, exponential backoff, and a dead-letter
path. It is:

- **DB-backend agnostic** - runs on SQLite, PostgreSQL, or MySQL through the
  `hull/db` capability surface; no code change to switch.
- **Transactionally coupled** - `jobs.enqueue` is a plain `INSERT`, so calling it
  inside a `db.batch()` commits the job atomically with the business row it
  depends on. This is the reason jobs is DB-backed rather than an external broker
  (Redis/SQS): there's no window where the row commits but the job is lost, or
  the job fires but the row rolled back.
- **Orthogonal** - depends only on `hull/db` + `hull/time` + `hull/json` +
  `hull/crypto`. No HTTP or session coupling.

It is **at-least-once**, not exactly-once: handlers must be idempotent. It is not
a broker and not a cron scheduler (compose `app.every` / `app.daily` for
recurring work).

Worked example: [`examples/jobs`](../examples/jobs). Design and rationale:
[`docs/jobs_design.md`](jobs_design.md).

## Quick start

```lua
local jobs = require("hull.jobs")

app.manifest({ modules = { "hull/jobs@1" } })

jobs.init()                                   -- create _hull_jobs + indexes (idempotent)

jobs.handler("send_email", function(job)      -- job = { id, type, data, attempts, max_attempts }
    email.send(job.data.to, job.data.subject, job.data.html)
end)

-- Enqueue from a request handler (ideally inside the request's db.batch):
jobs.enqueue("send_email", { to = "a@b.c", subject = "Hi" })

-- Process it: in-process poller (simplest) ...
app.every(1000, function() jobs.work() end)
```
```javascript
import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/jobs@1"] });
jobs.init();
jobs.handler("send_email", (job) => email.send(job.data.to, job.data.subject, job.data.html));
jobs.enqueue("send_email", { to: "a@b.c", subject: "Hi" });
app.every(1000, () => { jobs.work(); });      // fire-and-forget; the timer re-arms immediately
```

## Two execution models (one atomic claim)

Both drive the same concurrency-safe claim, so you can start with the poller and
add worker processes later with no code change.

**1. In-process poller.** An `app.every` timer calls `jobs.work` on the event
loop. One process, zero extra moving parts.

```lua
app.every(1000, function() jobs.work({ batch = 20 }) end)
```

**2. Dedicated worker process.** `jobs.run_worker` is a blocking claim loop; run
it from `app.main` and launch the app as its own process. Run K copies for
horizontal scale - each claims disjoint jobs.

```lua
app.main(function() jobs.init(); jobs.run_worker() end)
```
```sh
hull worker.lua -d ./app.db              # or, discoverable:
hull jobs worker worker.lua -d ./app.db  # resolves the entry, runs its app.main
```

`hull jobs worker [entry|dir] [-d DSN] [args...]` resolves the entry (a file
as-is; a directory -> `app.lua`|`app.js`) and forwards the rest to the app.

**Intra-process concurrency.** `run_worker({ concurrency = N })` runs N
independent claim-loops in one process, so up to N handlers are in flight at
once - real parallelism for I/O-bound handlers (each loop claims disjoint jobs
via the atomic claim). It's orthogonal to running K processes; the two multiply
(K processes x N concurrency). Leave it at 1 for CPU-bound handlers (more
processes scale those better).

```lua
app.main(function() jobs.init(); jobs.run_worker({ concurrency = 8 }) end)
```

**Multiple queues.** A worker can drain several named queues via `opts.queues`
(on `claim`, `work`, or `run_worker`). A **list** is strict priority - each queue
tried in order, claiming from the first with ready work (higher queues drain
first). A **map** is weighted fairness - a weighted-random draw with every queue
still a fallback, so none starves and a homogeneous fleet is fair fleet-wide.

```lua
jobs.run_worker({ queues = { "critical", "default", "low" } })  -- strict priority
jobs.run_worker({ queues = { critical = 3, default = 2, low = 1 } })  -- weighted
```

## Lifecycle hooks

`jobs.on(event, fn)` registers an **in-process** listener fired synchronously by
`jobs.work`, in the worker that processed the job - for observability, metrics,
and side-effects. Events: `completed` (`info.result`), `retried`
(`info = { error, attempt }`, a failed attempt that will retry), `dead`
(`info.error`; exhausted / `jobs.DEAD` / no handler). Listeners are isolated (a
throwing hook can't derail the loop); keep them fast and synchronous. These are
per-worker (not fleet-wide - durable cross-process eventing is a separate epic).

```lua
jobs.on("completed", function(job, info) metrics.inc("jobs.done", { type = job.type }) end)
jobs.on("dead",      function(job, info) log.error("job died: " .. info.error) end)
```

## Recurring jobs (cron)

`jobs.cron(name, spec, data?, opts?)` registers a **durable** recurring
schedule: on each matching minute, exactly one worker enqueues a job. Schedules
live in the DB (`_hull_cron`), so they survive restarts and coordinate across
the fleet (a compare-and-set on the schedule row - no double-fire even with many
workers). A worker (an `app.every` poller or `run_worker`) must be running to
fire them.

```lua
jobs.init()
jobs.handler("nightly_report", function(job) build_report(job.data) end)

jobs.cron("nightly_report", "0 2 * * *", { region = "us" })   -- 02:00 UTC daily
jobs.cron("heartbeat", "*/5 * * * *")                          -- every 5 minutes
jobs.uncron("heartbeat")                                       -- remove a schedule
```
```javascript
jobs.cron("nightly_report", "0 2 * * *", { region: "us" });
jobs.cron("heartbeat", "*/5 * * * *");
jobs.uncron("heartbeat");
```

- **`name`** is the schedule id and, by default, the enqueued **job type**
  (override with `opts.type`). Re-registering the same `name` updates it.
- **`spec`** is standard 5-field cron - `minute hour day-of-month month
  day-of-week` - in **UTC**. Supports `*`, `n`, `a-b`, `*/step`, `a-b/step`, and
  comma lists; `day-of-week` is `0`/`7` = Sunday. When both day-of-month and
  day-of-week are restricted, a match on **either** fires (standard cron).
- **`opts`**: `type`, `queue`, `priority`, `max_attempts` for the enqueued jobs.
- **`opts.tz`**: evaluate the spec in a **fixed offset** east of UTC -
  `"+02:00"`, `"-0530"`, or minutes as a number (`120`). IANA zone names
  (`"Europe/Budapest"`) are rejected: a DST-correct named zone needs a tz
  database Hull can't read inside the sandbox. Default is UTC.
- **Missed ticks** (all workers were down) advance to the next future occurrence
  - fire-once, no backfill storm.

## Rate limiting

`jobs.limit(queue, { rate, per })` caps how fast a queue is dispatched:
**at most `rate` jobs per `per`-second window across the whole fleet** (default
`per = 1`). The window counter lives in the DB (`_hull_ratelimit`), so K worker
processes share one budget - unlike a per-process limiter that would let K
workers each run `rate`. Use it to respect a downstream's limit (an email
provider, a third-party API quota).

```lua
jobs.limit("emails", { rate = 10 })            -- <=10 jobs/sec, fleet-wide
jobs.limit("reports", { rate = 100, per = 60 })-- <=100/min
jobs.limit("emails", nil)                       -- remove the limit
```
```javascript
jobs.limit("emails", { rate: 10 });
jobs.limit("reports", { rate: 100, per: 60 });
```

- The limit is **per queue** (the `queue` arg). Enqueue rate-limited work onto
  that queue: `jobs.enqueue("send", data, { queue = "emails" })`.
- Register the limit at startup **on every worker** (the limit config lives in
  code; only the counter is shared). A homogeneous fleet is assumed.
- Enforcement is **claim-then-reconcile**: a claim keeps the highest-priority
  jobs the window budget allows and requeues the excess (undoing the claim, so a
  rate-deferred job keeps its full retry budget). Deferred jobs run in the next
  window - throughput is shaped, nothing is dropped.
- Costs one extra small DB write per claim **only on limited queues**; unlimited
  queues are unaffected.

## Workflows (job dependencies)

`jobs.enqueue(type, data, { depends_on = { id1, id2 } })` makes a job **wait
until those jobs complete**. It starts `blocked` (not claimed) and becomes
`pending` only once every dependency is `done`. Each dependency's result is
passed to the handler as **`job.deps`** — an array in declaration order.

```lua
-- chain: extract -> transform -> load, passing results forward
local a = jobs.enqueue("extract", { src = "..." })
local b = jobs.enqueue("transform", {}, { depends_on = { a } })
           jobs.enqueue("load", {}, { depends_on = { b } })

jobs.handler("extract",   function(job) return fetch(job.data.src) end)     -- return = result
jobs.handler("transform", function(job) return massage(job.deps[1]) end)    -- deps[1] = extract's result
jobs.handler("load",      function(job) save(job.deps[1]) end)              -- deps[1] = transform's result

-- fan-in: run after BOTH parts, with both results
local p1 = jobs.enqueue("part1", d); local p2 = jobs.enqueue("part2", d)
jobs.enqueue("merge", {}, { depends_on = { p1, p2 } })   -- job.deps = { r1, r2 }
```
`depends_on` with N parents expresses **chains** (depend on 1), **fan-in**
(depend on N), and **fan-out** (enqueue N children). JS uses `dependsOn` and
`job.deps[0]`, `[1]`, …

- **Results.** A handler's **return value** is stored as the job's result (JSON)
  and injected into dependents as `job.deps`. Returning nothing / `true` /
  `jobs.DISCARD` stores no result. (Results live in `_hull_job_results` and are
  swept by `jobs.cleanup`.)
- **Failure cascades.** If a dependency **dead-letters**, the dependent
  cascade-fails too (dead-lettered, transitively down the graph) - you don't run
  `load` if `transform` died. Opt a job out with `on_dep_failure = "run"`
  (`onDepFailure` in JS) to run it regardless (e.g. a cleanup / notify step).
- Depend on job **ids** returned from `enqueue`; enqueue parents first. Unknown /
  already-cleaned-up dependency ids are treated as satisfied.

## Durable workflows (workflow-as-code)

The `depends_on` DAG above is a *static* graph of independent jobs. For a
*dynamic*, long-running process - loops, conditionals, resumable across crashes -
use a **durable workflow**: a normal function whose steps are memoized, so a
crashed or retried workflow resumes past the work it already did. It can sleep for
days, wait for external signals, and roll back on failure. (See
[docs/jobs_durable_execution_design.md](jobs_durable_execution_design.md);
deterministic WASM-replay strict mode is the remaining Phase 2.)

```lua
jobs.workflow("checkout", function(ctx)
  local charge = ctx.step("charge", function()   -- runs once, result persisted
      return payments.charge(ctx.input.order_id, ctx.input.amount)
  end)
  ctx.step("ship", function() return shipping.dispatch(ctx.input.order_id) end)
  return { charged = charge.id }
end)

local wf = jobs.start("checkout", { order_id = 42, amount = 100 })   -- returns a job id
```
```javascript
jobs.workflow("checkout", async (ctx) => {
  const charge = await ctx.step("charge", () => payments.charge(ctx.input.orderId, ctx.input.amount));
  await ctx.step("ship", () => shipping.dispatch(ctx.input.orderId));
  return { charged: charge.id };
});
const wf = jobs.start("checkout", { orderId: 42, amount: 100 });
```

- **A workflow instance is a job** (reserved type `__wf:<name>`), so it rides the
  same claim / visibility-reaper / retry / worker / result machinery. If a
  worker crashes mid-workflow, the reaper reclaims it and it resumes.
- **`ctx.step(name, fn)`** runs `fn` once and stores the result in
  `_hull_workflow_steps`; on any re-run (retry or crash-resume) the step returns
  the stored result **without** re-executing `fn`. Steps are **at-least-once**
  (idempotent, same contract as a handler): a crash between the side effect and
  the memo write re-runs the step.
- **`ctx.input`** is the payload from `jobs.start`; **`ctx.id`** is the workflow
  id. The body between steps re-runs on each resume, so keep it cheap and put all
  side effects inside steps.
- **`ctx.sleep(seconds)`** is a **durable timer**: the workflow yields and is
  rescheduled to wake later (it becomes a future-dated `pending` job), so a sleep
  survives restarts, redeploys, and crashes at zero cost - no held thread, no
  in-memory timer. A worker must be running when it is due. A sleep does not
  consume the retry budget. `workflow_status` reports `waiting_for = "sleep:<ts>"`
  while it waits.
- **`ctx.wait_signal(name, opts?)`** (`waitSignal`) pauses the workflow until
  `jobs.signal(id, name, payload)` (`workflowStatus` shows `waiting_for =
  "signal"`); it returns the delivered payload. The workflow parks in a
  non-terminal `waiting` status (claimed by nobody) and is re-activated on
  delivery - the human-in-the-loop / wait-for-webhook / approval primitive. A
  signal delivered **before** the workflow reaches the wait is stored and
  consumed when it gets there (no lost-signal race). `opts.timeout` (seconds)
  makes the wait return `nil` if no signal arrives in time.
- **Saga compensation:** `ctx.step(name, fn, { compensate = cfn })` registers a
  rollback. If the workflow **fails terminally** (dead-letters), the completed
  steps' `compensate` functions run in **reverse order** (undo the charge if
  shipping can't be arranged). Compensations are at-least-once (idempotent) and
  recorded, so a crash mid-rollback resumes.
- **`jobs.workflow_status(id)`** (`workflowStatus`) → `{ status, name,
  steps_done, result?, error? }`, DB-derived (works even when no worker is
  running it). Fetch the final value with `jobs.await(id)` / `jobs.result(id)`.

## Handler contract

`jobs.work` dispatches each claimed job to its registered handler, else the
`jobs.default` catch-all, else dead-letters it. The handler signals its outcome
by **return value** (or by raising):

| Handler does | Outcome |
|--------------|---------|
| returns `nil` / `true` / any value | **done** |
| raises an error / returns `jobs.RETRY` | **retry** with backoff (until `max_attempts`, then dead-letter) |
| returns `jobs.DEAD` | **dead-letter** immediately (non-retryable) |
| returns `jobs.DISCARD` | **done** without effect (drop) |

`job.attempts` is the count of the attempt currently running (incremented by the
claim). Handlers that do blocking or heavy work offload via `db.async` /
`compute.async` / `gpu.async`, exactly as request handlers do. JS `jobs.work` is
`async` and awaits the handler, so sync and async handlers both work.

## Enqueue options

```lua
jobs.enqueue(type, data, {
    queue        = "emails",   -- named queue (default "default")
    priority     = 10,         -- higher runs first (default 0)
    delay        = 60,         -- seconds until claimable (run_at = now + delay)
    at           = 1735689600, -- absolute unix ts to run at (wins over run_at/delay)
    run_at       = 1735689600, -- absolute unix ts (overrides delay)
    max_attempts = 5,          -- override the module default (25)
    dedup_key    = "order-42", -- idempotent enqueue: a duplicate un-run key is a no-op
    throttle     = 60,         -- windowed: skip if a (queue,type) job was made in the last N s
    trace        = traceparent,-- W3C trace context; surfaced to the handler as job.trace
})
```
Returns the new job id, or `nil` when a `dedup_key` collapsed it (or a
`throttle` window suppressed it). JS keys are camelCase (`runAt`, `maxAttempts`,
`dedupKey`). `throttle` is best-effort (keyed by `(queue, type)`, no unique
constraint - use `dedup_key` for exact-once).

**Bulk enqueue.** `jobs.enqueue_many(items)` (`enqueueMany` in JS) inserts a list
of `{ type, data, opts }` in **one transaction** - a single commit/fsync instead
of one per job. Returns ids in input order (`nil` for a deduped item). Every
`opts` field works except `depends_on` (bulk is for independent jobs; build
graphs with `enqueue`).

```lua
local ids = jobs.enqueue_many({
    { type = "email", data = { to = "a@b.c" } },
    { type = "email", data = { to = "d@e.f" }, opts = { priority = 5 } },
})
```

## Ops surface

```lua
jobs.get(id)                  -- one job's full status view, or nil if it doesn't exist
jobs.result(id)               -- terminal result: { status, result?, error? }, or nil if unknown
jobs.await(id, { timeout = 5000 })   -- yield until the job is terminal, then return jobs.result
jobs.progress(id, 42)         -- set a running job's progress 0-100 (surfaced by get(id).progress)
jobs.stats()                  -- { pending, running, done, dead } (opts.queue to scope)
jobs.metrics()                -- DB-derived dashboard snapshot: per-queue gauges + backlog age + totals
jobs.dead({ limit = 50 })     -- list dead-lettered jobs (newest first) for inspection
jobs.retry(id)                -- requeue a dead job with a fresh attempt budget; false if not dead
jobs.cancel(id)               -- delete a still-pending (e.g. delayed) job; false if not pending
jobs.cleanup({ older_than = 7 * 86400 })   -- purge terminal (done + dead) rows past a retention age
jobs.pause("emails") / jobs.resume("emails")   -- stop / resume dispatching a queue (durable, fleet-wide)
jobs.purge("emails")          -- delete pending jobs in a queue ("clear the backlog"); returns count
```
`jobs.pause(queue)` stops workers claiming from that queue (in-flight jobs
finish, and `enqueue` still works) - durable and fleet-wide, taking effect within
~1s on other workers. `jobs.purge(queue)` deletes `pending` jobs by default; pass
`{ statuses = { "pending", "running", "done", "dead" } }` to widen.
`jobs.get(id)` is the "did my job run?" primitive: after `local id =
jobs.enqueue(...)`, poll `jobs.get(id).status` (`pending` → `running` → `done` /
`dead`). It's the only way to inspect an individual job from app code -
`_hull_jobs` is namespace-protected, so a direct query is blocked.

**Result backend.** A handler's non-nil return value is persisted, so
`jobs.result(id)` returns `{ status, result?, error? }` for any job (a `done`
job carries its return value, a `dead` job its error) - `nil` if the id is
unknown or already purged. `jobs.await(id, { timeout, interval })` yields to the
event loop and polls until the job is terminal (or the timeout elapses), then
returns the same shape - the "enqueue then wait for the value" pattern. Results
live as long as the job row (governed by `jobs.cleanup`), so read before purge.
`jobs.progress(id, pct)` lets a long handler publish 0-100 progress, surfaced on
`jobs.get(id).progress`.

`jobs.cancel` only removes a `pending` job (a delayed/scheduled one that hasn't
started); a `running` job is mid-flight and is left to finish or dead-letter.

**Metrics & tracing.** `jobs.metrics()` returns a **DB-derived** snapshot for a
dashboard or a `/metrics` route - `{ queues = { <q> = { pending, running,
waiting, blocked, dead, oldest_pending_age }, ... }, totals = {...} }`. It's
pull-based (no process counters), so it's correct across a whole fleet;
`oldest_pending_age` is the backlog age of the oldest ready pending job.
`enqueue(..., { trace })` carries a W3C `traceparent` through to the handler as
`job.trace` (and into a durable workflow's `ctx.trace`), so app-created spans
link across the async boundary.

**Attempt history (opt-in).** `jobs.init({ history = true })` (or
`{ history = { queues = {...} } }` for specific queues) records one row per
attempt. `jobs.history(id)` then returns the **timeline** -
`{ attempt_no, started_ms, finished_ms, duration_ms, wait_ms, outcome, error }`
per attempt, oldest first - and `jobs.metrics()` gains `latency`
(`wait_ms` = queue latency, `run_ms` = execution, each as `{ p50, p95, p99 }`)
and `throughput` (`done_per_sec` / `dead_per_sec`) over the last `opts.window`
seconds (default 300). It costs one write per attempt (hence opt-in); retention
is governed by `jobs.cleanup` (or `history_retention`). Design:
[docs/jobs_observability_design.md](jobs_observability_design.md).

## Compute-heavy jobs (WASM / GPU)

A handler is ordinary app code, so it has full capability access - `compute.*`
(WASM), `gpu.*`, `db.async.*`, `http.fetch`, everything the app's manifest
grants. Offloading heavy work to a background job is a natural fit. Five things
to get right (these are general compute-orchestration rules, not jobs-specific,
but the visibility timeout makes the last one sharper):

- **Use the async variants.** `compute.async.call` / `gpu.async.dispatch` yield
  to the event loop; `compute.call` / `gpu.dispatch` **block** the single
  event-loop thread for the whole computation, stalling every other concurrent
  loop, timer, and (if the process also serves) HTTP request. With
  `run_worker({ concurrency = N })` this matters more - one sync compute call
  freezes the other N-1 loops.
- **Parallelism is bounded by the thread pool, not by `concurrency`.**
  `compute.async` / `gpu.async` / `db.async` all dispatch to one shared worker
  pool; `concurrency = 32` firing async compute still only runs pool-size jobs on
  hardware at once.
- **Reference large inputs, don't inline them.** Payloads are JSON text, so a big
  binary compute input would be base64-bloated. Store it (fs / blob / db) and put
  a path/key in the payload; the handler loads it and feeds compute via the
  zero-copy buffer protocol (`fs.mmap`, `WasmBuffer`).
- **Jobs are fire-and-forget - persist the result.** There is no result backend;
  a compute job that produces output must write it somewhere (db / blob) for the
  enqueuer to read later. If you need the result inline, call `compute.async` in
  the request handler instead of enqueuing a job.
- **Heartbeat long jobs.** A job that runs longer than `visibility_timeout`
  (default 300s) is presumed orphaned and re-run. A long WASM/GPU handler should
  call `jobs.heartbeat(job)` periodically (at least every `visibility_timeout/2`
  s) to extend its claim. It returns `false` once the claim has been lost (the
  reaper already reclaimed it, or another worker re-claimed it) - the handler's
  signal to stop and let the other runner win, avoiding a double-run:

  ```lua
  jobs.handler("transcode", function(job)
      for _, chunk in ipairs(chunks(job.data)) do
          process(chunk)                       -- long, per-chunk work
          if not jobs.heartbeat(job) then      -- lost the claim -> abort
              return jobs.DEAD
          end
      end
  end)
  ```
  (Or just raise `visibility_timeout` at `jobs.init` if the handler can't
  checkpoint.)

Declare the modules (`hull/compute` + ship `compute/*.wasm`; `hull/gpu` +
`manifest.gpu = true` for the sandbox grants), and `require`/`import` them -
`compute` and `gpu` are modules, not globals.
Run `jobs.cleanup` from `app.daily` to bound table growth; it only touches
terminal rows, so it never races a live job. JS: `jobs.cleanup({ olderThan: ... })`.

## Semantics

- **At-least-once.** A worker that crashes after a side effect but before marking
  `done` re-runs the job. Make handlers idempotent (dedup on a natural key, use
  `insert_if_absent`, check-then-act inside a transaction).
- **Retry backoff.** `2^attempt · 10s`, capped at 1h (overridable via
  `jobs.init({ backoff = fn })`).
- **Visibility-timeout reaper.** A job stuck in `running` past
  `visibility_timeout` (default 300s) is presumed orphaned (its worker died) and
  reset to `pending`. `jobs.work` runs the reaper each call.
- **Atomic claim.** One job is claimed by exactly one worker even under heavy
  concurrency: `SELECT ... FOR UPDATE SKIP LOCKED` on Postgres/MySQL, WAL
  single-writer serialization on SQLite. A `claim_token` nonce disambiguates.

## Configuration

`jobs.init({ ... })` (all optional):

| option | default | meaning |
|--------|---------|---------|
| `max_attempts` | 25 | dead-letter threshold |
| `visibility_timeout` | 300 | seconds before an orphaned `running` job is reclaimed |
| `reap_interval` | 30 | min seconds between reaper sweeps (a no-op sweep still takes the write lock, so `work` throttles it; `0` = every call) |
| `backoff(attempt)` | `2^n·10s` cap 1h | retry-delay function |

`jobs.work` / `jobs.run_worker` take `{ queue, batch, visibility_timeout, poll_ms }`;
`jobs.run_worker` also accepts `{ concurrency, drain, max_empty_polls }` (N in-flight
handlers; bounded / batch-drain runs) and `jobs.stop()` for graceful shutdown.

## Backend notes

- **SQLite** (default): concurrency relies on WAL mode + `busy_timeout`, both set
  by Hull's DB cap layer. For a worker pool, use a **file** DSN (not `:memory:`,
  which is connection-private).
- **PostgreSQL / MySQL**: `SKIP LOCKED` gives lock-free, contention-free claims
  across many worker processes. Selected by DSN scheme on the connection; the
  same jobs code runs unchanged.
- **`SKIP LOCKED` version floor.** It needs PostgreSQL 9.5+, MySQL 8+, or
  MariaDB 10.6+. `jobs.init` probes the server once; on an older one it
  transparently falls back to plain `FOR UPDATE` - still exactly-once (one job,
  one worker), but concurrent claimants **block** on locked rows instead of
  skipping them (lower throughput under heavy contention). No configuration
  needed; upgrade the server to regain skip-based claims.
