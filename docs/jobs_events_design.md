# jobs fleet-wide durable events (#246)

**Status:** PROPOSED / design of record. Not yet scheduled.
**Provenance:** the "what makes hull/jobs world-class" gap analysis. Today
`jobs.on(event, fn)` is **in-process and per-worker** - a listener only sees the
jobs *that same worker* processed, the callback is lost on a crash, and no other
process (a dashboard, an analytics sink, a reacting service) can subscribe. Every
serious peer that offers cross-process job events (BullMQ `QueueEvents`, Sidekiq
Pro, Oban) makes them **durable and fleet-wide**. This is also the substrate a
live dashboard and low-latency reactions need.

> **Design ethos (unchanged from the rest of jobs):** DB-backed, backend-agnostic
> (SQLite / Postgres / MySQL via the `HlDbBackend` vtable), **transactionally
> coupled** (an event commits atomically with the state change that produced it),
> at-least-once + idempotent consumers, no new external broker. Pure stdlib Lua/JS
> over the caps we already ship; zero new C. **Testable by construction** (§10) -
> the mechanism is a synchronous, clock-injectable function; scheduling is a thin
> driver on top, exactly like `process_cron` / `jobs._tick`.

---

## 1. The model: an append-only log + per-subscription cursors

Two moving parts, both durable rows:

1. **The event log** `_hull_job_events` - an append-only, monotonically-id'd
   record of lifecycle events (`enqueued`, `started`, `completed`, `retried`,
   `dead`, `cancelled`, and later workflow events). Written by whichever worker
   applies the transition.
2. **Durable subscriptions** `_hull_job_subscriptions` - a named cursor per
   consumer (`cursor` = the id of the last event that consumer has processed).

A subscription is a **cursor over the log**, not a queue that consumes-and-
deletes. So N independent subscribers (dashboard + webhook-reactor + analytics)
each see **every** event at-least-once - fan-out, not steal. This is the
transactional-outbox / consumer-group shape, not a work queue (jobs already IS
the work queue; events are the *observation* plane).

`jobs.on` (the fast in-process hook) stays as-is for cheap local side-effects;
durable events are a **new, opt-in** plane (`jobs.init({ events = true })`) because
they cost one INSERT per lifecycle transition.

## 2. Emission: transactional coupling (the correctness keystone)

The event row is INSERTed in the **same transaction** as the state transition
that produced it. `mark_done` / `mark_dead` / `mark_retry` / `enqueue` / `claim`
already run under a write (SQLite) or an explicit `db.batch` (the MySQL claim);
the event INSERT joins that unit of work. Therefore:

- a `completed` event exists **iff** the job actually committed `done` - no lost
  events (crash after commit, before an external publish) and no phantom events
  (publish, then the transaction rolls back). This is *the* reason events live in
  the same DB as the jobs, mirroring why `enqueue` is a plain INSERT.

Implementation: the existing local `emit(event, job, info)` gains a second
responsibility - when `events` is enabled, append a durable row - and the emit
call sites already sit inside the transition. No new emission points; we harden
the ones that exist.

## 3. Consumption: leased cursor drain (fleet-safe, at-least-once)

A worker advances each subscription by draining new events:

1. **Lease** the subscription (CAS a `lease_token` + `lease_until` onto its row,
   `WHERE name=? AND (lease_until IS NULL OR lease_until <= now)`), so exactly one
   worker drains a given subscription at a time - the same claim-token + visibility
   idea the job claim uses. Other workers skip a leased subscription.
2. **Fetch** events `WHERE id > cursor ORDER BY id LIMIT batch`.
3. **Deliver** each to the handler, in id order.
4. **Advance** the cursor to the last delivered id and **release** the lease, in
   one write.

At-least-once: a crash between "handler succeeded" and "cursor advanced" re-
delivers those events on the next drain (the lease expires, another worker
resumes from the unchanged cursor) - so **handlers must be idempotent**, the same
contract as job handlers. Ordering is guaranteed **per subscription** (by event
id); there is no global cross-subscription ordering promise.

Draining is driven from the same places the reaper/cron already run - each
`jobs.work` tick (throttled), and inside `jobs.run_worker` - or from a dedicated
`jobs.events_worker()` for deployments that want event delivery on its own
process. No new scheduler.

## 4. API surface

```lua
jobs.init({ events = true })                 -- turn on durable emission

-- Durable named subscription (persisted; survives restarts). handler is
-- at-least-once + idempotent. from = "now" (default) | "beginning".
jobs.subscribe("fulfillment", function(ev)   -- ev = { id, ts, type, job_id,
    if ev.type == "completed" and ev.job_type == "charge" then --   job_type, queue,
        webhook.notify(ev)                    --   data, trace }
    end
end, { events = { "completed", "dead" }, from = "now" })

jobs.unsubscribe("fulfillment")

-- Ad-hoc read-only tail for a dashboard (no cursor, no delivery guarantee).
local recent = jobs.events({ since = ts, types = { "dead" }, limit = 100 })
```

JS mirrors with camelCase (`jobs.subscribe`, `jobs.unsubscribe`, `jobs.events`,
`jobType`). The handler filter (`events = {...}`) is applied in SQL where cheap
(a `type IN (...)` on the fetch) so a narrow subscriber doesn't wake for every
event.

## 5. Storage

```
_hull_job_events(
    id         INTEGER PRIMARY KEY (autoincrement / identity - the cursor space),
    ts         INTEGER NOT NULL,          -- emit time (host clock)
    type       VARCHAR(32) NOT NULL,      -- enqueued|started|completed|retried|dead|cancelled|...
    job_id     INTEGER,
    job_type   VARCHAR(255),
    queue      VARCHAR(255),
    data       TEXT,                       -- JSON: outcome-specific (error, attempt, result-ref, trace)
    INDEX(type, id)                        -- filtered drain
)
_hull_job_subscriptions(
    name        VARCHAR(255) PRIMARY KEY,
    cursor      INTEGER NOT NULL DEFAULT 0,
    types       VARCHAR(255),              -- optional filter (comma list) or NULL = all
    lease_token VARCHAR(255),
    lease_until INTEGER,
    failures    INTEGER NOT NULL DEFAULT 0,-- consecutive handler failures (reliability policy)
    updated_at  INTEGER NOT NULL
)
```

The `id` is the total order and the cursor space - the same monotonic-id trick
the claim relies on. Portable identity DDL (`db.autoincrement_id_ddl`), `VARCHAR`
keyed columns for MySQL indexability, exactly like `_hull_jobs`.

## 6. Retention (safe truncation)

The log grows; `jobs.cleanup` truncates it, but **never past an unconsumed
event**. The safe watermark is `min(cursor)` across all subscriptions (or, with no
subscriptions, a time window). Delete `WHERE id <= min_cursor AND ts < before`.
So a slow/paused subscriber holds the log open for its own unseen events but not
for events everyone has passed. `jobs.metrics` gains an `events` block (log depth,
per-subscription lag = `max(id) - cursor`) so a stuck subscriber is visible.

## 7. Reliability policy (a failing / absent subscriber)

A handler that keeps throwing must not wedge retention forever. Each subscription
carries `failures`: on a handler error the drain stops at that event, bumps
`failures`, and backs off (re-lease later). After `max_failures` (opt-in), the
event is **skipped** (cursor advances past it) and recorded as a per-subscription
dead event (`jobs.events({ types = {"subscription_skipped"} })`), so one poison
event can't block the subscription or retention. A subscription with no live
drainer simply lags (visible in metrics) until one runs.

## 8. Low latency - LISTEN/NOTIFY (#235), additive and separable

Correctness rides polling (works on every backend). As an **optional** latency
optimization, on Postgres the emit path issues `NOTIFY hull_jobs_events` and a
drainer `LISTEN`s, waking immediately instead of at the next poll. This needs the
async `NotificationResponse` support #235 tracks in the pg wire client; it is a
**separate phase** and the design does **not** depend on it. SQLite/MySQL keep
polling. Crucially (see §10) NOTIFY is never on the correctness path in a test -
only on a latency assertion.

## 9. Reused infrastructure (what we do NOT rebuild)

- The monotonic-id + cursor idea = the claim's `id` ordering.
- The lease-token + visibility-timeout reclaim = `jobs.reap`.
- The CAS-advance-under-contention = `process_cron`'s `next_run_at` compare-and-set.
- The transactional coupling = `enqueue`'s plain-INSERT-in-a-batch.
- The clock-injectable synchronous seam = `process_cron(now)` / `jobs._tick(now)`.
- Emission points = the existing `emit()` call sites.

## 10. Testability (designed in, not bolted on)

The whole feature is async, durable, multi-process, and time-dependent - the four
things that make tests flaky. The design neutralizes each **by construction**.
(The v0.11.0 durable-timer e2e flake - `sleep(1)` + second-resolution `now` racing
a `run_at > now` boundary on a loaded CI runner - is the anti-pattern this section
exists to prevent: **no wall-clock sleeps drive correctness assertions**.)

**T1. A synchronous, side-effect-free drain seam.** The core mechanism is
`jobs._events_drain(name, { now, batch })` - it fetches, delivers, advances the
cursor, and returns a summary `{ delivered, cursor, leased }` - with **no timers,
no sleeps, no async**. Scheduling (the `jobs.work` tick, `run_worker`) is a thin
driver that calls it. Every correctness test calls `_events_drain` directly and
asserts on the return + DB state. (Exactly how `jobs._tick(now)` makes cron
testable without waiting for a real minute.)

**T2. Injectable clock.** Every time input - lease expiry, retention `before`,
`ts` - is a parameter, never a bare `time.now()` inside the mechanism. Tests pass
explicit `now`, so lease-expiry and retention are asserted at exact instants with
zero sleeping. Emit records `ts`; drain/retention take `now`/`before`.

**T3. Deterministic capture harness.** A test subscription whose handler appends
each `ev` to a Lua table / JS array (or a scratch table). Because drain is
cursor-ordered and synchronous, the delivered sequence is **deterministic**:
after `enqueue -> work -> _events_drain`, assert the capture equals exactly
`[enqueued, started, completed]` in order, with the right `job_id`/`job_type`.
Both runtimes assert the identical sequence (parity).

**T4. In-process fleet simulation + one real fleet gate.** "Fleet-wide" is tested
at two levels:
- *Deterministic (unit):* interleave several `_events_drain` calls against one
  shared SQLite DB to simulate N workers, asserting the lease serializes them -
  every event is delivered, none is delivered twice **in the same drain window**,
  and a second concurrent drainer sees the subscription leased and no-ops.
- *Real (e2e):* K `hull` processes each running a drainer against one DB (a
  `hull jobs events-worker`), mirroring the existing claim/rate-limit/strict-
  concurrency **fleet gates** in `e2e_jobs.sh` - assert the union of deliveries
  covers every event and (under idempotent handlers) the end state is exactly one
  effect per event.

**T5. Fault-injection seams for the at-least-once contract.** The hard guarantees
get explicit, deterministic tests via seams that model a crash:
- *Duplicate delivery / crash-before-cursor:* `jobs._events_drain(name, { commit_cursor = false })` delivers but does **not** advance the cursor (models a
  process death between handler success and the cursor write). Re-drain -> assert
  the same events are re-delivered (at-least-once) and that an **idempotent**
  handler (dedup on `ev.id`) produces one net effect.
- *Lease expiry / worker death:* drain to take the lease, don't release, advance
  `now` past `lease_until`, drain from a "second worker" -> assert it reclaims and
  resumes from the unchanged cursor (no loss, no gap). Mirrors the reaper test.
- *Producer atomicity:* force the state transition to roll back (a `db.batch`
  whose body raises after the `mark_*` + event INSERT) -> assert **no** orphan
  event row exists (event and state commit or abort together). And the converse:
  a committed transition always has its event. This is the transactional-coupling
  proof and it is fully deterministic (no timing).

**T6. Retention safety.** Emit a spread of events; advance subscription A's cursor
but not B's; run `jobs.cleanup({ before })` -> assert events at/below `min(cursorA,
cursorB)` and older than `before` are gone, and events B has not yet seen are
**retained**. Pure cursor + explicit `before`, no clock race.

**T7. Backend parity (the this-session lesson).** The drain fetch, the CAS/lease
UPDATE, and the retention delete must be portable-first and validated on **all
three** backends locally (SQLite via `e2e_jobs`, plus MySQL 8 + Postgres 16 in
Docker) before pushing - the exact discipline that caught the `LIKE ... ESCAPE`
break and vetted the correlated-subquery reconcile this session. No `LIKE` on a
`_`-containing literal without a portable escape; no UPDATE whose subquery targets
its own table on MySQL.

**T8. NOTIFY is latency, never correctness (when Phase 4 lands).** The
LISTEN/NOTIFY tests assert only that a waiter **wakes sooner** than the poll
interval (a tolerant, Postgres-only latency check). Every correctness assertion
runs on the polling drain path, so the test suite is green on SQLite/MySQL with no
NOTIFY at all.

**Coverage map:** unit (`tests/hull/...` not needed - pure stdlib) + `e2e_jobs.sh`
new phases: emit-atomicity, deterministic capture (both runtimes), the drain seam,
duplicate/lease/rollback fault injection, retention safety, the multi-process
fleet gate; plus the durable-exec/observability event types exercised through the
same seam. `e2e_postgres.sh` / `e2e_mysql.sh` gain an events phase (the parity
guard), like every other jobs feature.

## 11. Phasing

- **Phase 1 - durable event log + emission.** `_hull_job_events`, transactional
  emit at the existing `emit()` sites, `jobs.init({ events = true })`,
  `jobs.events(opts)` read-only tail. Tests: T5-atomicity, T3-capture (tail form),
  T7-parity. *Ships value alone:* a dashboard can poll `jobs.events`.
- **Phase 2 - durable subscriptions.** `_hull_job_subscriptions`, `jobs.subscribe`
  / `unsubscribe`, the `jobs._events_drain(name, {now})` seam, drain from
  `jobs.work` + `jobs.run_worker`, retention-safe `cleanup`. Tests: T1-T7 in full
  (the fleet gate, fault injection, retention).
- **Phase 3 - reliability policy.** Per-subscription failure backoff + skip-after-N
  + `subscription_skipped` events + the `events`/lag block in `jobs.metrics`.
- **Phase 4 - low latency (#235).** Postgres LISTEN/NOTIFY wake, polling fallback.
  Tests: T8 (latency-only, PG-only).

## 12. Scope discipline (non-goals)

- **Not a message bus.** No cross-cluster fan-out, topics, or partitions - it is
  the observation/reaction plane for *this* jobs DB. Kafka is a different product.
- **Not exactly-once.** At-least-once + idempotent handlers, the same honest
  contract as job handlers; we do not pretend otherwise.
- **Not a replacement for `jobs.on`.** The in-process hook stays for cheap local
  side-effects; durable events are the opt-in, cross-process, survives-restart
  plane. Both coexist (the durable path is `emit()` doing one extra INSERT).
- **Emission is opt-in.** `events = true` is off by default - an app that doesn't
  subscribe pays nothing (no log writes), preserving the lean default.
