# workflows - durable workflow-as-code + observability

A worked example of the durable-execution and observability features of
`hull/jobs@1`. A **workflow** is a job whose body is ordinary code; each
`ctx.step(name, fn)` runs once and **memoizes** its result, so a crash or retry
resumes past completed steps instead of repeating side effects (like charging a
card twice). The instance IS a job, so it inherits the atomic claim, retries with
backoff, the dead-letter path, and the visibility-timeout reaper - and it runs
unchanged on SQLite, PostgreSQL, or MySQL.

The example models **order fulfillment**:

```
charge  ─▶  reserve stock  ─▶  (wait for shipping confirmation)  ─▶  notify
                │                          ▲
                └── saga compensation ──────┘  (release stock if the order fails)
```

| Feature | Where |
|---|---|
| **Memoized steps** | `ctx.step("charge", …)` / `ctx.step("reserve", …)` - re-running the body skips completed steps. |
| **Deterministic replay** | `ctx.uuid()` / `ctx.now()` - the receipt id and timestamp stay identical across resumes. |
| **Durable wait** | `ctx.wait_signal("shipped")` parks the workflow in the non-terminal `waiting` status (no CPU held) until `jobs.signal` wakes it - could be seconds or days later. |
| **Saga compensation** | `ctx.step("reserve", fn, { compensate = … })` - if the workflow dead-letters after reserving, the compensation releases the stock (reverse order). |
| **Observability** | `jobs.metrics()` - DB-derived gauges + latency percentiles + throughput. |

## Run it

```sh
hull examples/workflows/app.lua -d ./wf.db        # or app.js
```

```sh
# start an order -> returns the workflow id
ID=$(curl -s -X POST localhost:3000/orders -d '{"sku":"A1","card":"tok_ok"}' | tr -dc 0-9)

# it charges + reserves, then parks waiting for shipment
curl localhost:3000/orders/$ID
# {"status":"waiting","steps_done":["charge","reserve"],"waiting_for":"signal", ...}

# the warehouse confirms shipment -> the signal wakes the workflow
curl -X POST localhost:3000/orders/$ID/ship -d '{"tracking":"1Z999"}'

# now it's done, with the memoized receipt and the tracking number
curl localhost:3000/orders/$ID
# {"status":"done","result":{"receipt":"…","tracking":"1Z999", …}, ...}

# fleet-wide observability
curl localhost:3000/metrics
# {"totals":{"pending":0,"running":0,"waiting":1,…},
#  "latency":{"wait_ms":{"p50":…},"run_ms":{…}},"throughput":{…}}
```

Try the failure paths:

```sh
# declined card -> dead-lettered before reserving (nothing to roll back)
curl -X POST localhost:3000/orders -d '{"card":"tok_declined"}'

# fraud -> charges + reserves, then dead-letters; the saga compensation
# releases the reserved stock (watch the "compensated: released stock" log line)
curl -X POST localhost:3000/orders -d '{"sku":"B2","card":"tok_fraud"}'
```

## Crash safety

Kill the process at any point and restart it against the same `-d` database. A
workflow parked on `wait_signal` is durable (it lives in `_hull_jobs`), and one
mid-flight resumes from its last completed step - the memoized `charge` /
`reserve` results are read back, so the card is not re-charged. This is the whole
point of workflow-as-code: the happy path reads like a straight-line function,
but every step is a durable checkpoint.

## Execution model

This example uses the **in-process poller** (`app.every` drives `jobs.work`
inside the web app) so a single `hull app.lua` is self-contained. For higher
throughput, run the same workflow under a **dedicated worker** process instead -
register the workflow with `jobs.workflow(...)` inside an `app.main` that calls
`jobs.run_worker()`, and launch it as its own process (see
[`examples/jobs/worker.lua`](../jobs/worker.lua) for the worker pattern). The two
models share one atomic claim, so you can add worker processes with no change to
the workflow code.

## See also

- [`docs/jobs.md`](../../docs/jobs.md) - the full `hull/jobs@1` guide (workflows,
  signals, sagas, deterministic replay, `ctx.patched` versioning, metrics).
- [`examples/jobs`](../jobs) - the simpler queue example (handlers, retries,
  dead-letter, the poller vs dedicated-worker models).
