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
- **Missed ticks** (all workers were down) advance to the next future occurrence
  - fire-once, no backfill storm.

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
    run_at       = 1735689600, -- absolute unix ts (overrides delay)
    max_attempts = 5,          -- override the module default (25)
    dedup_key    = "order-42", -- idempotent enqueue: a duplicate un-run key is a no-op
})
```
Returns the new job id, or `nil` when a `dedup_key` collapsed it. JS keys are
camelCase (`runAt`, `maxAttempts`, `dedupKey`).

## Ops surface

```lua
jobs.stats()                  -- { pending, running, done, dead } (opts.queue to scope)
jobs.dead({ limit = 50 })     -- list dead-lettered jobs (newest first) for inspection
jobs.retry(id)                -- requeue a dead job with a fresh attempt budget; false if not dead
jobs.cancel(id)               -- delete a still-pending (e.g. delayed) job; false if not pending
jobs.cleanup({ older_than = 7 * 86400 })   -- purge terminal (done + dead) rows past a retention age
```
`jobs.cancel` only removes a `pending` job (a delayed/scheduled one that hasn't
started); a `running` job is mid-flight and is left to finish or dead-letter.

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
- **Mind the visibility timeout.** A compute job that runs longer than
  `visibility_timeout` (default 300s) is presumed orphaned and re-run. For
  multi-minute WASM/GPU work, raise `visibility_timeout` accordingly (a per-job
  heartbeat is a tracked follow-up).

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
