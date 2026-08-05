# jobs observability - metrics, attempt history, trace propagation (Bet #1)

**Status:** PROPOSED / design RFC. Not scheduled.
**Target:** `hull/jobs@1` v1.6-ish. Builds on the v1.5 surface (docs/jobs.md).
**Decisions (from review):** attempt history is **opt-in** via config; the
metrics/trace surface is **pull-based `jobs.metrics()` + W3C trace-context
propagation** - stdlib-heavy, **no C**, no built-in Prometheus/OTLP exporter
(those can arrive later as pure-stdlib helpers). Full parity Lua + JS.

Observability is where a job runner is judged in production ("did it run?" ->
"here is exactly what happened, traced end to end"). DB-backed makes it cheap:
history is rows, metrics are queries. Three pillars.

---

## Pillar A - pull metrics: `jobs.metrics(opts?)`

A single DB-derived snapshot the app exposes however it wants (log line, a
`/metrics` route, a dashboard). No ambient exporter, no new dependency.

```lua
jobs.metrics({ queue = "emails", window = 300 })   -- opts both optional
```
```javascript
jobs.metrics({ queue: "emails", window: 300 });
```

Return shape (Lua; JS camelCase mirror):

```lua
{
  queues = {
    default = { pending=10, running=2, blocked=0, dead=1, oldest_pending_age=42 },
    emails  = { pending=3,  running=1, blocked=0, dead=0, oldest_pending_age=5  },
  },
  totals  = { pending=13, running=3, blocked=0, dead=1,
              done_window=812, dead_window=4, failure_rate=0.0049 },
  -- latency + throughput only when attempt history (Pillar B) is enabled:
  latency = {
    wait_ms = { p50=12, p95=340, p99=1200 },   -- enqueue -> first start
    run_ms  = { p50=8,  p95=95,  p99=410  },   -- start -> finish
  },
  throughput = { done_per_sec=2.7, dead_per_sec=0.01 },   -- over `window`
}
```

- **Gauges** (`pending`/`running`/`blocked`/`dead` counts, `oldest_pending_age`
  = `now - min(created_at)` over pending) come straight from `_hull_jobs` with
  the existing claim index - cheap, always available.
- **Windowed counts / failure rate / latency / throughput** need durable
  completion records. `_hull_jobs` terminal rows are transient (`jobs.cleanup`
  deletes them), so these are computed from the **attempt-history table**
  (Pillar B) over the last `window` seconds - and are simply **absent** when
  history is off (documented; `jobs.metrics()` still returns gauges).
- **Percentiles**: portable SQL has no `PERCENTILE`, so `jobs.metrics` pulls a
  bounded sample of recent attempt durations (last `window` or a cap, e.g.
  2000 rows) and computes p50/p95/p99 in Lua/JS (sort + index). Bounded work.
- No metric is a monotonic process counter (that would reset per process); every
  value is a DB-derived snapshot, so it is correct across a fleet of workers.

**Prometheus / OTLP:** intentionally NOT built in. A later pure-stdlib
`hull.jobs.prometheus` helper can format `jobs.metrics()` as text; the app owns
the scrape route. Keeps the core dependency-free and the taxonomy honest.

## Pillar B - attempt history / timeline (opt-in)

A durable, queryable record of every attempt - the Temporal-grade "what happened
to this job" timeline, and the source for the latency/throughput metrics above.
**Opt-in** because it is one write per attempt (amplification on high volume).

Enable globally or per queue:

```lua
jobs.init({ history = true })                 -- record every attempt
jobs.init({ history = { queues = { "critical" } } })   -- only these queues
```

New table (created by `init` only when history is ever enabled):

```
_hull_job_attempts(
  attempt_id   <autoincrement PK>,
  job_id       INTEGER NOT NULL,      -- the _hull_jobs.id (not an enforced FK, for portability)
  queue        VARCHAR(255) NOT NULL,
  type         VARCHAR(255) NOT NULL,
  attempt_no   INTEGER NOT NULL,      -- job.attempts at run time
  started_ms   INTEGER NOT NULL,      -- time.now_ms() captured when work() begins the handler
  finished_ms  INTEGER NOT NULL,      -- time.now_ms() after the outcome
  duration_ms  INTEGER NOT NULL,      -- finished_ms - started_ms
  outcome      VARCHAR(16) NOT NULL,  -- 'done' | 'retried' | 'dead'
  error        TEXT,                  -- on retried/dead
  worker       VARCHAR(255),          -- optional worker id (from run_worker opts.id)
  trace_id     VARCHAR(255)           -- Pillar C correlation, if present
)
-- index: (job_id, attempt_no)  for jobs.history(id)
-- index: (queue, finished_ms)  for windowed metrics
```

Recorded in `work()` at each outcome, right where the v1.5 lifecycle events fire
(one INSERT, only when history is enabled for that queue). Millisecond timing is
captured in `work()` via `time.now_ms()` around the handler call - accurate
execution duration independent of the second-resolution `claimed_at`.

Read the timeline:

```lua
jobs.history(id)   -- array of attempts, oldest first:
-- { { attempt_no=1, started_ms=.., finished_ms=.., duration_ms=8, outcome="retried", error="timeout" },
--   { attempt_no=2, started_ms=.., finished_ms=.., duration_ms=6, outcome="done" } }
```

**Retention:** `jobs.cleanup` gains an attempt sweep (default same `older_than`;
an explicit `history_retention` overrides). A hard cap can bound the table under
sustained load. Attempts outlive the job row deliberately (a `done` job pruned by
cleanup can still have its history for a retention window, for post-hoc metrics).

## Pillar C - trace-context propagation

Carry a W3C `traceparent` from enqueue into the handler so an app's spans link
across the async boundary. Hull has no ambient tracer, so this is **explicit and
minimal**: jobs stores and hands back the context; the app creates the spans.

- New nullable column `trace_context VARCHAR(255)` on `_hull_jobs` (appended;
  idempotent `ensure_column` migration, the v1.5 pattern).
- `jobs.enqueue(type, data, { trace = "<traceparent>" })` stores it. (A helper
  can default it from a per-request convention, e.g. `req.ctx.traceparent`.)
- The claimed `job.trace` is exposed to the handler; the handler starts its span
  as a child of that context and/or stamps it into log lines (pairs naturally
  with the logger middleware's `request_id`).
- **Workflow inheritance:** a durable workflow (Bet #2) and any jobs it enqueues
  inherit the parent's `trace_context`, so a whole process is one trace.
- Copied into `_hull_job_attempts.trace_id` for correlation between the timeline
  and the trace.

No span creation or OTLP export in core - propagation only. A later stdlib
`hull.jobs.otel` could turn `job.trace` + attempt rows into real spans.

## Config + API summary

```lua
jobs.init({ history = true|false|{ queues={...} }, history_retention = <sec> })
jobs.metrics(opts?)      -- { queue?, window? } -> snapshot (gauges always; latency/throughput when history on)
jobs.history(id)         -- attempt timeline for one job ([] if history off / none)
jobs.enqueue(t, d, { trace = "<traceparent>" })   -- + job.trace in the handler
```
JS: `jobs.metrics` / `jobs.history` (camelCase fields), `enqueue(t,d,{ trace })`.

## Storage / build

- One opt-in table `_hull_job_attempts` + one nullable column
  `_hull_jobs.trace_context`, both via the portable `ensure_column` /
  create-if-enabled path. **Zero C.** Pure stdlib, both runtimes.
- `time.now_ms()` (already bound) for ms timing; percentiles computed in Lua/JS
  over a bounded sample.

## Testing

- `jobs.metrics` gauges (counts/age) on a seeded DB, both runtimes.
- History off -> no `_hull_job_attempts` table, `jobs.history` returns empty,
  metrics omit latency/throughput.
- History on -> a job that retries-then-dies records N attempt rows with correct
  `attempt_no`/`outcome`/`duration_ms`; `jobs.history` returns the timeline;
  `jobs.metrics` latency percentiles computed.
- Trace: `enqueue({ trace })` -> `job.trace` in the handler -> `trace_id` on the
  attempt row.
- Retention: `jobs.cleanup` prunes old attempts.

## Phasing

1. **Pillar A + C** (metrics gauges + trace propagation) - tiny, no new table
   (trace_context column only). Immediate value.
2. **Pillar B** (opt-in attempt history) + wire latency/throughput into
   `jobs.metrics` + `jobs.history` + retention.
3. (Later, stdlib) Prometheus text + OTel span exporters as optional helpers.

## Integration

- **Bet #2 (durable execution):** each `ctx.step` can record as an attempt-like
  row (or a step timeline), so a workflow's internal timeline shows in
  `jobs.history` / metrics. Trace context flows parent -> steps -> child jobs.
- **v1.5 events:** `jobs.on` hooks and attempt history are complementary - hooks
  are live/in-process, history is durable/queryable.
