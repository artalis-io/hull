# jobs events Phase 4 - low-latency wakeup via LISTEN/NOTIFY (#235)

**Status:** PROPOSED / design of record. Not yet scheduled.
**Depends on:** durable events Phase 1-3 (#262 / #263 / #265). This is the final,
**optional, latency-only** event phase.

Phases 1-3 made the event log durable, fleet-safe (leased cursor drain), and
resilient (poison skip + metrics). Delivery rides **polling**: `jobs.work` /
`jobs.run_worker` drain subscriptions each tick, and an idle `run_worker` sleeps
`poll_ms` (default 1000) before the next drain. Correctness is complete. The only
gap is **idle-wakeup latency** - a newly appended event (or a newly enqueued job)
waits up to `poll_ms` before an idle worker notices it.

Phase 4 closes that gap on **PostgreSQL** with `LISTEN`/`NOTIFY`: a producer signals
on append, and an idle drainer wakes *immediately* instead of at the next poll.

> **The one inviolable rule (from the events design §8, testability T8):**
> **NOTIFY is a latency optimization, never a correctness dependency.** Every wait
> is bounded by a `poll_ms` timeout and degrades to the Phase 2/3 polling path, so
> a missed / dropped / unsupported notification only costs latency, never an event.
> The whole test suite stays green on SQLite/MySQL with no NOTIFY at all.

---

## 1. Shape: a blocking LISTEN-wait on the existing worker pool

The pg connection is a **blocking** socket, and `db.async` already dispatches
blocking DB work to a **worker-thread pool** (each worker owns a per-DSN
connection; the calling coroutine yields to the event loop and resumes on
completion). Phase 4 reuses that infrastructure directly:

- A new **async op** - "LISTEN on a channel, then wait for a notification, bounded
  by `timeout_ms`" - is dispatched to a worker thread. The worker `poll(2)`s its
  connection's fd; the event loop stays free (the coroutine is parked, other
  requests are served). It returns `notified` or `timeout`.
- `run_worker`'s idle branch swaps `hull.sleep(poll_ms)` for this yielding wait
  (only when the backend supports it; otherwise unchanged).
- Producers issue a `NOTIFY` when they append an event (and, as a natural
  extension, when they enqueue a job - see §6).

**Why a worker thread, not Keel event-loop fd registration.** Blocking on the pg
fd must never block the event loop. Running the wait on a worker thread (proven
`db.async` path) keeps the loop free with no new event-loop plumbing. Registering
the pg fd directly with Keel would be lower-latency-per-wait but a much larger,
riskier C change; it is explicitly **out of scope** (a possible future
optimization, not needed for the win here).

**Why a dedicated LISTEN connection.** After `LISTEN`, `NotificationResponse` ('A')
frames can arrive at any time - including mid-result-stream if the same connection
also ran queries. Phase 4 uses a **dedicated** connection for the LISTEN-wait
(distinct from the query/claim connection), so 'A' only ever arrives on a
connection that is doing nothing but waiting. This sidesteps async-interleaving
entirely. The worker pool already gives isolated per-thread connections; the
wait op uses its own.

## 2. Layer C - the pg wire client

Today the receive loop skips an unknown frame via `default: break`, so a stray 'A'
is tolerated but lost. Phase 4 adds capture + a bounded wait:

- **`#define HL_PG_B_NOTIFY 'A'`** (`pgwire.h`) and an 'A' case that parses the
  `NotificationResponse` frame: `int32 pid`, `cstr channel`, `cstr payload`.
- **`int hl_pg_wait_notify(HlPgConn *conn, int timeout_ms)`** (`pg_conn.c`):
  `poll(conn->fd, POLLIN, timeout_ms)`; on readable, `recv` + frame-parse the
  accumulation buffer; return `1` if an 'A' frame was seen, `0` on timeout, `-1`
  on error/EOF (dead connection). `LISTEN <channel>` itself is a normal query
  issued once via the existing `hl_pg_exec_simple` before the first wait; the
  channel is a fixed literal (`hull_jobs`), not user input.
- Bounds/hardening already present carry over: the frame reader is length-checked
  over untrusted bytes (mirrors the rest of `pgwire.c`); a hostile 'A' payload is
  read as a bounded cstr and ignored (we only need the *fact* of a notification,
  not its contents).

**Unit test (socketpair, deterministic, no real PG):** queue an 'A' frame on one
end → `hl_pg_wait_notify` returns 1; queue nothing → returns 0 at the timeout;
close the peer → returns -1. Extends `test_pg_conn.c` exactly like the existing
`select_rows_and_affected` / `exec_affected_count` cases.

## 3. Layer db-cap - vtable + async op

- **Producer side is just SQL** - no new vtable method needed. `NOTIFY` takes an
  *identifier*, so to stay injection-safe the producer uses the function form
  **`SELECT pg_notify(?, ?)`** (channel, payload as bound params) through the
  normal `db.exec` path. On non-PG backends this is simply never called.
- **Consumer side is a new optional vtable method**
  `int (*wait_notify)(HlDbHandle *h, const char *channel, int timeout_ms)` -
  Postgres issues `LISTEN` once (idempotent per connection) then calls
  `hl_pg_wait_notify`; **SQLite/MySQL leave it `NULL`** (unsupported). A
  `db.dialect.supports_notify` flag (like `supports_returning`) lets stdlib probe
  it without a call.
- **Async dispatch:** a new `HlWorkerDbKind` (e.g. `WAIT_NOTIFY`) so the wait runs
  on the worker pool and the coroutine yields, mirroring `db.async.query`. Surface
  it as a yielding `conn.wait_notify(channel, timeout_ms)` (Lua) /
  `conn.waitNotify` (JS) that returns `true` (notified) or `false` (timeout).
  Absent the thread pool / event loop it throws (same contract as `db.async`).

## 4. Layer jobs - wiring

- **Emit:** when `_cfg.events` and `db.dialect.supports_notify`, `emit_durable`
  additionally runs `SELECT pg_notify('hull_jobs', '')` **inside the same
  transaction** as the event INSERT (PG delivers NOTIFY on commit, and coalesces
  duplicates within a transaction, so a busy commit sends at most one). Cost on
  non-PG: zero (never issued).
- **Idle wait:** `run_worker`'s empty-poll branch becomes:
  ```lua
  if db.dialect.supports_notify then
      conn.wait_notify("hull_jobs", poll_ms)   -- wakes on NOTIFY, else after poll_ms
  else
      hull.sleep(poll_ms)                        -- Phase 2/3 fallback, unchanged
  end
  ```
  The `poll_ms` timeout is the safety net: a missed notification just falls back to
  a poll. Everything else (the drain, the lease, retention) is unchanged.

## 5. Testability (T8 - latency, never correctness)

- **All existing correctness tests are untouched and backend-agnostic** - they use
  the synchronous `jobs._events_drain` seam and never depend on NOTIFY. The suite
  is green on SQLite/MySQL with zero NOTIFY.
- **C unit test** (§2): `hl_pg_wait_notify` over a socketpair - notify → 1, nothing
  → 0 (timeout), EOF → -1. Deterministic.
- **PG-only latency test** (`e2e_postgres.sh`): a drainer parks on
  `conn.wait_notify("hull_jobs", 5000)`; a second connection `NOTIFY`s; assert the
  waiter returns **notified** in well under the 5 s timeout (a tolerant latency
  assertion - "woke before the timeout", not a tight bound). This proves NOTIFY
  works *without* making any correctness assertion hinge on its timing.
- **Fallback test:** with no NOTIFY, `wait_notify` returns `false` at the timeout -
  same observable outcome as `hull.sleep`, so `run_worker` behaves identically.
- **Reconnect** (§ risks): kill the LISTEN connection mid-wait → the worker's
  connection cache reopens and re-`LISTEN`s on the next wait; asserted by a wait
  that survives a dropped connection (returns at the timeout, then works again).

## 6. Natural extension: low-latency job pickup

The same channel trivially covers **new jobs**, not just new events: have
`jobs.enqueue` also `pg_notify('hull_jobs', '')` (PG only), so an idle
`run_worker` wakes immediately on a freshly enqueued job instead of at the next
`poll_ms`. This is arguably the *bigger* practical win (dispatch latency), and it
falls out of the exact same primitive - one channel, producers NOTIFY on
enqueue-or-append, the idle worker waits on it. Recommended to include; gated the
same way (PG + events/enqueue), and correctness still rides the poll timeout.

## 7. Risks (named, with mitigations)

| # | Risk | Mitigation |
|---|------|-----------|
| R1 | Async 'A' interleaving with query results | **Dedicated** LISTEN connection (never runs app queries); stray 'A' elsewhere stays harmlessly skipped by `default: break`. |
| R2 | Long-idle LISTEN connection dropped by server/network | `wait_notify` returns -1 on EOF; the worker's per-DSN connection cache reopens and re-`LISTEN`s on the next wait. Correctness unaffected (poll fallback). |
| R3 | Notification missed in the drain→wait gap (delivered before LISTEN) | The `poll_ms` **timeout** bounds every wait, so a miss costs at most `poll_ms` of latency, never an event. This is the core safety property. |
| R4 | Blocking the event loop | The wait runs on a **worker thread** (existing `db.async` pool); the coroutine yields, the loop stays free. |
| R5 | NOTIFY storm on a busy system | PG coalesces duplicate NOTIFYs within a transaction; a payload-less `NOTIFY` is cheap; only idle waiters are woken (busy ones are already draining). |

## 8. Phasing

- **4.1 - C wire primitive.** `HL_PG_B_NOTIFY`, the 'A' capture, `hl_pg_wait_notify`,
  the socketpair unit test. No behavior change yet; pure capability.
- **4.2 - db-cap surface.** `db.dialect.supports_notify`, the optional
  `wait_notify` vtable method (PG only), the `WAIT_NOTIFY` async worker op, the
  yielding `conn.wait_notify`. Standalone-testable (a manual LISTEN/NOTIFY round
  trip on real PG).
- **4.3 - jobs wiring.** `emit_durable` NOTIFY (+ `enqueue` NOTIFY per §6),
  `run_worker` idle wait, the PG-only latency test + the fallback test. Ships the
  latency win.

## 9. Scope discipline

- **Postgres only.** SQLite (single process/file) and MySQL (no LISTEN/NOTIFY)
  keep polling, unchanged. `supports_notify` is false there and nothing is emitted
  or awaited.
- **Latency only.** No new delivery guarantee, no new event, no schema change. If
  Phase 4 were removed entirely, behavior would be identical modulo idle-wakeup
  latency.
- **No Keel fd registration.** The worker-thread wait is the whole mechanism;
  event-loop fd integration is a deliberate non-goal.
- **Not a general pub/sub.** `hull_jobs` is an internal wakeup channel, not an
  app-facing NOTIFY surface (that would be a separate `hull/pg` feature).
