# jobs - durable background job queue

A worked example of `hull/jobs@1`: enqueue a unit of work from a request, then
process it out-of-band with retries, exponential backoff, and a dead-letter
path. The queue is **DB-backed** (SQLite here, but the same code runs on
PostgreSQL / MySQL), so `jobs.enqueue` is a plain `INSERT` that commits with the
business row it depends on - no external broker, no Redis.

Two execution models share one atomic claim, so you can start with the first and
add the second later with **no code change**:

| File | Model | When |
|------|-------|------|
| `app.lua` / `app.js` | **In-process poller** - an `app.every` timer drives `jobs.work` inside the web app. | One process, simplest deployment. |
| `worker.lua` / `worker.js` | **Dedicated worker** - `app.main` runs the blocking `jobs.run_worker` loop as its own process. | Decouple job throughput from request serving; run K copies to scale. |

## Run the poller (single process)

```sh
hull examples/jobs/app.lua -d ./jobs.db        # or app.js

# enqueue work
curl -X POST localhost:3000/jobs -d '{"type":"send_email","data":{"to":"a@b.c"}}'
curl -X POST localhost:3000/jobs -d '{"type":"flaky"}'   # fails twice, then succeeds

# inspect
curl localhost:3000/jobs/stats                 # {"pending":..,"running":..,"done":..,"dead":..}
curl localhost:3000/jobs/dead                  # dead-letter queue
curl -X POST localhost:3000/jobs/retry/7       # requeue dead job #7
```

## Run a dedicated worker (scale out)

Point a worker at the **same** database the web app enqueues into:

```sh
hull examples/jobs/app.lua    -d ./jobs.db     # web app: enqueues only (drop the app.every line)
hull examples/jobs/worker.lua -d ./jobs.db     # worker 1
hull examples/jobs/worker.lua -d ./jobs.db     # worker 2 ...
```

`hull jobs worker examples/jobs/worker.lua -d ./jobs.db` is the discoverable CLI
form (identical effect - it just resolves the entry and runs its `app.main`).

Each worker's claim is atomic, so a job runs on **exactly one** worker even under
heavy contention. On SQLite that's the WAL single-writer serialization; on
Postgres/MySQL it's `SELECT ... FOR UPDATE SKIP LOCKED`.

## Semantics worth knowing

- **At-least-once.** A handler that returns normally marks the job `done`. If it
  raises, the job retries with `2^n·10s` backoff (capped at 1h) until
  `max_attempts` (default 25), then dead-letters. Handlers **must be idempotent**
  - a worker that crashes after the side effect but before marking `done` will
  re-run the job.
- **Outcome control.** Return nothing / `true` -> done. Raise (or return
  `jobs.RETRY`) -> retry. Return `jobs.DEAD` -> dead-letter immediately
  (non-retryable). Return `jobs.DISCARD` -> done without effect.
- **Visibility-timeout reaper.** A job stuck in `running` past
  `visibility_timeout` (default 5 min) is presumed orphaned and reclaimed. The
  worker loop runs the reaper each iteration.
- **v1 enqueue options** (`opts` on `jobs.enqueue`): `queue`, `priority`,
  `delay` / `run_at`, `max_attempts`, `dedup_key` (idempotent enqueue - a
  duplicate un-run key is a no-op).

Full API and design: [`docs/jobs.md`](../../docs/jobs.md).
