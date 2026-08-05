# `hull/jobs@1` - durable background job queue (design)

Status: **design** (2026-08-05). Implementation phased below.

`hull/jobs` is a durable, DB-backed background job queue: enqueue a unit of
work, process it later with retries, backoff, and a dead-letter path. It is the
missing primitive between the transactional `outbox` (reliable *outbound
delivery*) and `app.every` / `app.daily` (simple *schedules*): durable queueing
of *user-triggered* work - image resizing, CSV import, PDF rendering, a batch of
emails after a plan upgrade, a scheduled report.

## Design principles

1. **DB-backend agnostic.** Runs unchanged on every `hull/db` backend (SQLite,
   PostgreSQL, MySQL/MariaDB; DuckDB is OLAP and not a target). All dialect
   variance is resolved through the existing `HlDbBackend` capability surface
   (`placeholder`, `identifier_quote`, `supports_returning`,
   `identity_column` / `autoincrement_id_ddl`, `supports_index_if_not_exists`)
   plus one small additive flag, `supports_skip_locked`.
2. **Orthogonal.** Depends only on `hull/db`, `hull/time`, `hull/json`. No
   coupling to HTTP, session, auth, or any web module. It composes *with* those
   (a request handler can enqueue inside its transaction) but assumes none of
   them. A pure `app.main` CLI tool can use it.
3. **Transactionally coupled, not a separate broker.** `enqueue` is a plain
   `INSERT`, so it participates in the caller's `db.batch()` / transaction: the
   job commits atomically with the business row, or not at all. This is the
   whole reason it is DB-backed rather than Redis - it closes the dual-write
   gap (business write commits, enqueue fails, or vice versa) that an external
   queue reintroduces. Same argument as the transactional `outbox`.
4. **Performant + configurable.** A real atomic claim (`SELECT ... FOR UPDATE
   SKIP LOCKED` on Postgres/MySQL, serialized on SQLite), batch claiming,
   configurable concurrency, poll interval, priority, delay, per-queue
   isolation, backoff, and visibility timeout. Both an in-process poller and a
   dedicated multi-process worker share one concurrency-safe claim.

## Non-goals

- **Not a message broker / pub-sub.** No fan-out topics, no cross-service
  routing. One producer-DB, N workers on the same DB.
- **Not Redis/Valkey, and doesn't need one.** A KV layer would break the
  transactional coupling (§principle 3) and reintroduce an external dependency
  against Hull's single-binary identity. A table + atomic claim is the correct,
  mainstream design (GoodJob, Solid Queue, River, pg-boss).
- **Not exactly-once.** Delivery is **at-least-once**; handlers must be
  idempotent (see §Semantics). A crashed worker's in-flight job is reclaimed
  and re-run.
- **Not cron (v1).** Recurring schedules compose from `app.every` /
  `app.daily` calling `jobs.enqueue`; a first-class `jobs.cron` is a deferred
  follow-up.

## Module

- Name: `hull/jobs@1` (Lua `require("hull.jobs")`, JS `import { jobs } from
  "hull:jobs"`).
- Registry deps (auto-admitted): `hull/db`, `hull/time`, `hull/json`.
- Owns `_hull_jobs` in the protected `_hull_*` namespace (user code cannot read
  or write it directly; the module bypasses the namespace guard the same way
  `outbox` / `session` do).

## Data model

`_hull_jobs`, built with the portable DDL helpers (identity column via
`autoincrement_id_ddl`, `CREATE INDEX IF NOT EXISTS` gated on
`supports_index_if_not_exists`, `VARCHAR(255)` for every keyed/indexed text
column so MySQL can index it):

| column         | type              | notes |
|----------------|-------------------|-------|
| `id`           | identity          | dialect-portable autoincrement PK |
| `queue`        | VARCHAR(255)      | named queue, default `'default'` |
| `type`         | VARCHAR(255)      | handler dispatch key |
| `payload`      | TEXT              | JSON-encoded job data |
| `status`       | VARCHAR(255)      | `pending` / `running` / `done` / `failed` / `dead` |
| `priority`     | INTEGER           | higher runs first; default `0` |
| `attempts`     | INTEGER           | incremented on each claim |
| `max_attempts` | INTEGER           | dead-letter threshold |
| `run_at`       | INTEGER           | unix ts; not claimable until `run_at <= now` (delay/schedule) |
| `claim_token`  | VARCHAR(255)      | per-claim nonce; disambiguates which worker won a row |
| `claimed_at`   | INTEGER           | unix ts of the claim (visibility-timeout reaper) |
| `dedup_key`    | VARCHAR(255)      | optional; unique per queue for idempotent enqueue |
| `last_error`   | TEXT              | trimmed failure message |
| `created_at`   | INTEGER           | |
| `updated_at`   | INTEGER           | |

Indexes:
- `(queue, status, run_at, priority, id)` - the claim's scan path.
- unique `(queue, dedup_key)` where `dedup_key` is non-null - idempotent enqueue.
- `(status, claimed_at)` - the reaper's scan.

## The atomic claim (the technical heart)

`outbox` is single-flusher: it `SELECT`s pending rows then `UPDATE`s per row, so
two concurrent flushers double-deliver. `jobs` must be safe for N concurrent
workers (in-process + separate processes), so it needs a real atomic claim. The
claim SQL is composed per connection from the dialect flags.

**Vtable addition** (`include/hull/cap/db_backend.h`, one `unsigned char`,
mirrors `supports_returning`):

```c
/* 1 if the backend supports SELECT ... FOR UPDATE SKIP LOCKED (Postgres 9.5+,
 * MySQL 8+); 0 for SQLite (single-writer - claims are serialized, so no lock
 * skipping is needed or available). Read by the hull/jobs claim to pick the
 * concurrency-safe claim shape. */
unsigned char supports_skip_locked;
```
Set: SQLite `0`, Postgres `1`, MySQL `1`. (DuckDB not a jobs target.)

**Postgres** (`supports_skip_locked` + `supports_returning`) - one statement:
```sql
UPDATE _hull_jobs SET status='running', claim_token=$1, claimed_at=$2,
                      attempts=attempts+1, updated_at=$2
 WHERE id IN (SELECT id FROM _hull_jobs
              WHERE queue=$3 AND status='pending' AND run_at<=$2
              ORDER BY priority DESC, id
              LIMIT $4
              FOR UPDATE SKIP LOCKED)
RETURNING id, type, payload, attempts, max_attempts;
```
Uncontended: workers skip each other's locked rows, no waiting.

**MySQL 8** (`supports_skip_locked`, no `RETURNING`) - claim-token, three steps
in one transaction:
```sql
START TRANSACTION;
SELECT id FROM _hull_jobs
  WHERE queue=? AND status='pending' AND run_at<=?
  ORDER BY priority DESC, id LIMIT ? FOR UPDATE SKIP LOCKED;   -- id list
UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?,
                      attempts=attempts+1, updated_at=?
  WHERE id IN (<ids>);
COMMIT;
SELECT id, type, payload, attempts, max_attempts
  FROM _hull_jobs WHERE claim_token=?;
```

**SQLite** (single-writer, `supports_returning` on the vendored 3.35+) - the
serialized writer makes the claim atomic without row locks:
```sql
UPDATE _hull_jobs SET status='running', claim_token=?, claimed_at=?,
                      attempts=attempts+1, updated_at=?
 WHERE id IN (SELECT id FROM _hull_jobs
              WHERE queue=? AND status='pending' AND run_at<=?
              ORDER BY priority DESC, id LIMIT ?)
RETURNING id, type, payload, attempts, max_attempts;
```

The `claim_token` (a `crypto.random` hex nonce) is the portable disambiguator:
even on backends without `RETURNING`, the claimant re-reads exactly its rows by
token. The `AND status='pending'` inside the writable `UPDATE ... WHERE` (PG/
SQLite) closes the last-writer race; MySQL's `FOR UPDATE SKIP LOCKED` does the
same. All three: a row is claimed by exactly one worker.

## Execution models (both ship in v1)

Both drive the *same* `jobs.work()` claim, so they interoperate on one DB.

1. **In-process poller.** `app.every(interval_ms, function() jobs.work(opts)
   end)` - the simplest deployment, single process, zero extra moving parts.
   `jobs.work()` claims a batch and runs the handlers as async coroutines on the
   event loop (like SSE / timers), so a handler doing `http.fetch` /
   `db.async.query` yields and other work proceeds. Heavy CPU jobs dispatch to
   `compute.async` / `gpu.async`.
2. **Dedicated worker process.** `hull jobs worker [app_dir] [--queue q]
   [--concurrency N]` (a new CLI subcommand) OR `jobs.run_worker(opts)` (a
   blocking loop callable from `app.main`) runs the claim loop as its own OS
   process. Run K of them (containers, systemd units, `SO_REUSEPORT`-style
   horizontal scale); each claims disjoint jobs via the atomic claim. This is
   the "performant" path - decouples job throughput from request-serving
   capacity. The worker uses the per-connection `db.async` pool for concurrency
   within the process, and the atomic claim for safety across processes.

The claim is identical either way, so you can start in-process and add worker
processes later with no code change.

## Handler registration (both styles)

```lua
local jobs = require("hull.jobs")
jobs.init()                                   -- create table + indexes

-- Per-type handlers (primary API): structured, individually testable.
jobs.handler("send_email", function(job)      -- job = { id, type, data, attempts, max_attempts }
    email.send(job.data.to, job.data.subject, job.data.html)
end)
jobs.handler("resize_image", function(job) ... end)

-- Optional catch-all for unregistered types (fallback dispatcher).
jobs.default(function(job)
    log.warn("no handler for job type " .. job.type)
    return jobs.DISCARD           -- or raise to retry, or jobs.DEAD to dead-letter
end)
```
`jobs.work()` claims a batch and dispatches each job to its registered handler,
else the `default`, else dead-letters it. A handler signals outcome by return /
raise (see §Semantics).

## Lifecycle & semantics

- **At-least-once.** A handler that completes without error marks the job
  `done`. An error (raise / rejected promise) increments `attempts` and either
  reschedules with backoff (`run_at = now + backoff(attempts)`, reusing outbox's
  `2^n · 10s` capped at 1h, overridable) or, at `attempts >= max_attempts`,
  moves it to `dead` (dead-letter). Handlers **must be idempotent**: a worker
  that crashes *after* the side effect but *before* marking `done` will re-run
  the job.
- **Visibility timeout / reaper.** A job in `running` whose `claimed_at` is
  older than `visibility_timeout` (default 5 min, configurable) is presumed
  orphaned (worker died) and reset to `pending` for reclaim. `jobs.work()` runs
  the reaper opportunistically; a dedicated worker runs it each loop.
- **Return-value contract.** Handler returns nil / true → `done`. Returns
  `jobs.RETRY` (or raises) → retry with backoff. Returns `jobs.DEAD` → straight
  to dead-letter (non-retryable failure). Returns `jobs.DISCARD` → `done`
  without side effect (drop).
- **Dead-letter.** `status='dead'` rows stay in the table (queryable via
  `jobs.dead(opts)`); `jobs.retry(id)` requeues one, `jobs.cleanup(opts)` purges
  old `done`/`dead` past a retention age.

## Configuration (all optional, sane defaults)

`jobs.init({...})` / `jobs.work({...})` / `jobs.run_worker({...})`:

| option | default | meaning |
|--------|---------|---------|
| `max_attempts` | 25 | dead-letter threshold |
| `backoff(attempt)` | `2^n·10s` cap 1h | retry delay fn |
| `visibility_timeout` | 300 | seconds before an orphaned `running` job is reclaimed |
| `batch` | 10 | jobs claimed per `work()` call |
| `concurrency` | 1 (poller) / cpu (worker) | in-flight handlers |
| `queue` | `"default"` | which queue this worker/poller drains |
| `poll_ms` | 1000 | dedicated-worker sleep between empty claims |

## API surface (Lua; JS is the camelCase mirror)

```
jobs.init(opts?)                         -- create _hull_jobs + indexes
jobs.enqueue(type, data, opts?) -> id    -- INSERT (join the caller's db.batch); opts: queue, priority, delay|run_at, max_attempts, dedup_key
jobs.handler(type, fn)                   -- register a per-type handler
jobs.default(fn)                         -- optional catch-all
jobs.work(opts?) -> processed_count      -- claim a batch + dispatch + reap (drive from app.every)
jobs.run_worker(opts?)                   -- blocking claim loop (app.main / hull jobs worker)
jobs.stats(opts?) -> { pending, running, done, failed, dead }
jobs.dead(opts?) -> rows ; jobs.retry(id) ; jobs.cleanup(opts?)
-- outcome sentinels: jobs.RETRY, jobs.DEAD, jobs.DISCARD
```
JS: `jobs.enqueue`, `jobs.handler`, `jobs.default`, `jobs.work` (async),
`jobs.runWorker`, `jobs.stats`, `jobs.dead`, `jobs.retry`, `jobs.cleanup`.

## Orthogonality & composition

- **With `hull/web/middleware/transaction`**: enqueue inside the request's
  `db.batch` - the job and the row it depends on commit together.
- **With `app.every`**: the in-process execution model *is* a timer calling
  `jobs.work`. Recurring jobs = a timer that enqueues.
- **With `db.async` / `compute.async` / `gpu.async`**: handlers offload blocking
  or heavy work exactly as request handlers do; the worker pool is shared.
- **With `hull/outbox`**: complementary. Outbox = reliable outbound HTTP/email
  *delivery*; jobs = arbitrary deferred *work*. §5 of the roadmap notes email
  retry flows should schedule via jobs rather than each carrying backoff.

## Security

- `_hull_jobs` is in the protected namespace; app code reaches it only through
  the module (call-stack namespace guard, same as outbox/session).
- Every query is parameter-bound at the cap layer - `payload` and `dedup_key`
  are values, never concatenated (zero SQLi, by construction, on all backends).
- No new authority: jobs needs only `hull/db`, which the app already declares.
  A sandboxed DB-only app runs the worker with the DB connection it already has.

## Testing

- Unit (both runtimes): enqueue/claim/complete round-trip; retry + backoff;
  dead-letter at max_attempts; delay (`run_at`) not claimable early; priority
  ordering; dedup unique-collapse; visibility-timeout reclaim.
- **Concurrency (the critical one):** N parallel workers against one DB claim
  each job exactly once (no double-run) - on SQLite, Postgres, and MySQL. The
  Postgres/MySQL variants run in the existing `e2e_postgres` / `e2e_mysql`
  Docker jobs; SQLite via threads.
- `db_backend` test: `supports_skip_locked` set correctly per backend; the
  composed claim SQL matches the backend shape.

## Phased implementation

1. **Vtable + schema.** Add `supports_skip_locked` (sqlite 0 / pg 1 / mysql 1);
   `_hull_jobs` portable DDL + indexes; `jobs.init`. Backend test.
2. **Enqueue + claim + the three per-backend claim shapes.** `jobs.enqueue`
   (transactional), the composed claim, `claim_token`. Concurrency e2e on all
   three backends (the correctness gate).
3. **Handlers + work loop + semantics.** `jobs.handler` / `jobs.default` /
   `jobs.work`; retry/backoff, dead-letter, visibility-timeout reaper, outcome
   sentinels. In-process (`app.every`) path. JS parity.
4. **Dedicated worker.** `jobs.run_worker` blocking loop + the `hull jobs
   worker` CLI subcommand; `--queue` / `--concurrency`. Worker-mode e2e.
5. **v1 features polish + ops.** priority / delay / named-queue / dedup surfaced
   and tested; `jobs.stats` / `dead` / `retry` / `cleanup`; docs +
   `examples/jobs`.

## Deferred (compose or follow up)

- First-class recurring / cron (`jobs.cron("0 2 * * *", type, data)`) - v1 uses
  `app.daily` + `jobs.enqueue`.
- Per-queue rate limiting / concurrency caps beyond `--concurrency`.
- Job chaining / workflows (a job enqueues its successor - already possible by
  hand; a helper is future).
- A `supports_skip_locked` fast path for DuckDB (not a jobs target today).
