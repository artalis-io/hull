# Design amendment: audit convergence §6.3 phase 1 - the app VM on its own thread

Amends the audit-convergence design (`docs/design/audit_convergence.md` on branch
`design/audit-convergence-phase1`),
§6.3 "Preemption of long native calls", phase 1, and answers its open question 4. Ships
behind `--vm-thread` / manifest `limits.vm_thread`, off by default.

## 1. Goal

One slow builtin degrades one request, not the server. The run watchdog (audit 12) raises a
stop flag at a run's deadline, and the engines poll it between instructions / at calls. A
native call that does not poll - an uncharged engine builtin, a long synchronous capability
call - runs to its end, and today it does so on the event-loop thread, holding every
connection of the server.

## 2. Decision: a baton, not a concurrent VM

The VM thread runs the app VM's entries; the loop thread runs Keel. They never run Hull code
at the same time. The loop thread hands an entry to the VM thread and waits for it to finish
(`hl_vmt_run`, `src/hull/cap/vm_thread.c`); the hand-off is one mutex in each direction, so
everything the VM thread wrote is visible to the loop thread afterwards, and vice versa.

Alternatives considered and rejected for phase 1:

- **Marshal every Keel touch (response writes, op arming, timers, WebSocket / SSE sends)
  into messages the loop thread applies.** The task statement's first suggestion. It is the
  design a phase 2 (a pool of VM threads, real concurrency) needs, but it touches every
  binding that reaches Keel in both runtimes (bindings_response, http_async, ws / ws-client,
  sse, net_stream, the async backends) and every park path. Done in one PR it would be the
  riskiest change in Hull, and every missed site a data race.
- **A private response object per run, copied to Keel's on completion.** Covers responses but
  not the async / park machinery, which suspends connections, arms Keel timers and starts the
  Keel HTTP client from inside a run.
- **Let the loop thread keep running the full Keel loop once a run is abandoned.** Keel's
  callbacks would run Hull code (timers, completions, other requests) concurrently with the
  stuck run, which resumes at an unknown moment and touches the same runtime state.

The baton keeps every existing data structure single-threaded - Keel, connections, responses,
the continuation machinery, the runtime, the database connections - only not always used from
the same thread. Requests stay serialized, as before. The cost is that the loop thread does
no I/O while a run is in progress (exactly today's behaviour); the gain, abandonment, is in
section 3.

## 3. Abandonment

The loop thread waits for a run until the run's deadline (read from its run watch with
`hl_run_watch_peek`; a sequence number tells whether the run has armed it yet, else
`job start + the configured limit` is used) plus a grace period (default 1 s). A run still
going then is inside a native call that ignores the stop flag. The loop thread goes into
STANDBY, which touches nothing the VM thread may be using:

1. The request that started the run - on a plaintext HTTP/1.x connection whose response has
   not started - is answered with a raw `503 Service Unavailable` (`Retry-After: 1`,
   `Connection: close`) written straight to its socket, whose write side is then shut. The
   descriptor was captured before the hand-off; nothing a handler or middleware entry runs
   writes that socket (only an SSE entry streams on its own socket, and it is never answered
   early). Whatever the run later leaves in the response goes to a socket that refuses it, and
   Keel closes the connection.
2. Every new connection on the plaintext listener is accepted and answered the same 503. The
   listener is the only Keel object standby touches, with `accept(2)` only; Keel is not
   running (the loop thread is the one that runs it).
3. Connections Keel already holds, timers and async completions wait: the Keel loop is not run
   in standby.

When the native call returns, the run trips at its next poll (the stop flag is still raised),
unwinds, and its entry ends on the VM thread exactly as any failed run does. The loop thread
sees the job done, finishes the entry (an HTTP entry answers 503 and closes - on a TLS or
HTTP/2 connection this is when the 503 goes out) and resumes the Keel loop.

If the run is still stuck after the hard ceiling (default 60 s from abandonment), or the
server was asked to stop (SIGTERM / SIGINT) and the drain timeout has passed, the process
exits with one log line and `_exit` - status 75 (EX_TEMPFAIL) for the ceiling, 1 for a stop.
No teardown can run while a thread is inside the VM; a supervisor restarts the process.

Policy for new requests while stuck: **refused** (503) on new plaintext connections; requests
on connections Keel already holds are **queued** in the socket buffers (bounded by the kernel
and Keel's connection pool) and served once the run returns, or dropped when the ceiling
exits the process.

## 4. What runs on the VM thread

Every entry that runs app code from a Keel callback in serve mode, in both runtimes:

| Entry | Lua | JS |
|---|---|---|
| HTTP handler | `hl_lua_keel_handler` | `hl_js_keel_handler` |
| middleware (pre and post) | `hl_lua_keel_middleware` | `hl_js_keel_middleware` |
| SSE handler | `hl_lua_sse_handler` | `hl_js_sse_handler` |
| WebSocket server callbacks | `hl_lua_ws_on_{open,message,close}` | `hl_js_ws_on_{open,message,close}` |
| WebSocket client callbacks | `lua_ws_client_on_{open,message,close,error}` | `js_ws_client_on_{open,message,close,error}` |
| timers (`app.every` / `app.daily`) | `hl_lua_timer_trampoline` | `hl_js_timer_trampoline` |
| async resumes (every parked op) | `hl_lua_async_resume` | `hl_js_async_resume`, `mp_js_pump` (multipart) |
| detached tasks / sleeps | `lua_task_fire` | `js_task_fire`, `js_sleep_timer_fire` |

Each is a thin wrapper (`NAME` -> `NAME_impl`) that hands the call to `hl_vmt_run` when a VM
thread exists and the caller is not it; from the VM thread itself (an entry started inside a
run: a nested task, a callback Keel calls synchronously from a send) it runs inline. Abandonment
is generic: it needs no per-entry knowledge beyond the HTTP early 503.

Not offloaded (they run no app code, so a native call cannot get stuck in them): the async
cancel paths, `request_done`, the JS hold op's resume. Not offloaded by design: app load,
`app.main` (CLI mode and the startup hook run before the serve loop), `hull test`, `hull agent`,
the `app.main` runner (`serve_cli.c`), and teardown - the VM thread is joined first.

## 5. Correctness notes

- **Thread-local state.** Every `_Thread_local` / `pthread_key` in Hull was audited:
  - bound per run, by the run's own arm, on the thread it runs on: the db budget binding
    (`tl_budget_fn/ud`), the fs charge scope (`tl_charge`), the SQLite namespace trust
    (`g_db_ns_trusted`), the audit buffer, the WASM host-call context, the wasm watchdog's
    pending watch, `db_rename_state`, the stb caps and live total, the stbiw jump buffer,
    wgpu's last error, the Lua worker's row-loop counter, the db registry's scratch buffer -
    correct on any thread;
  - per worker thread (`worker_db_key`, `js_worker_key`, smtp TLS): pool threads only;
  - **persisting between runs: `tl_txn_gen`** (the transaction generation the Lua coroutine
    checks compare across resumes). It is handed across with the baton
    (`hl_db_txn_gen_set`): the loop thread's value goes to the VM thread with each job and comes
    back with the result, and `hl_vmt_destroy` hands the last one to the thread that tears down.
    The VM therefore sees one counter, as before.
  - The dispatcher counters of PR #981 (per-run charging) are bound at arm like the db budget;
    after the rebase they must be (re)bound by the arm, never only at thread start.
- **QuickJS stack bounds.** QuickJS measures its stack from the C stack it was last told about,
  so the runtime's `thread_switch` hook (`JS_UpdateStackTop`) runs whenever the VM changes
  threads. The VM thread's stack is 64 MiB (virtual), well above QuickJS's own limit and Lua's
  C-call limit.
- **Database connections.** SQLite is built `SQLITE_THREADSAFE=1` (serialized); every
  connection of the event-loop VM - registry, `db.open`, the stale-transaction guard, the
  teardown close - is used by one thread at a time under the baton. Postgres / MySQL / DuckDB
  connections are plain sockets / handles used the same way.
- **Kernel sandbox.** A thread inherits the seccomp filter and the Landlock domain only from
  the thread that creates it (`prctl(PR_SET_SECCOMP)` and `landlock_restrict_self` are
  per-thread), so the VM thread is created after phase 2 of the sandbox, right before the loop.
  The Linux pledge polyfill allows `clone` with thread flags under `stdio` and lets a late
  thread register `rseq` (existing HULL PATCH); Seatbelt and OpenBSD pledge are per-process.
  *Observation (not changed here):* Keel's thread-pool workers are created in serve phase 8,
  before the phase-1 pledge, so on Linux they run without the seccomp filter or the Landlock
  domain; they run `worker.dispatch` VMs and `db.async` queries.
- **Signals.** The VM thread starts with the asynchronous signals blocked (SIGINT, SIGTERM,
  SIGHUP, SIGQUIT, SIGPIPE, SIGUSR1/2, SIGCHLD, SIGALRM), so Keel's SIGTERM handler runs on the
  loop thread and no run's system call is interrupted; SIGBUS (the mmap guard) and the other
  synchronous signals stay deliverable.
- **Thread-affinity assertions** (debug builds) accept the VM thread while it holds the baton
  (`hl_event_loop_baton_enter/leave`).
- **Teardown.** `hl_serve_teardown_after_serve` joins the VM thread first (step 0), with no run
  in progress (the loop has returned); everything after it - the pool's done callbacks,
  `stop_async`, the server free and its WebSocket `on_close`s, the runtime free and its
  finalizers - runs on the main thread exactly as without the flag.

## 6. Coverage and limits of phase 1

Covered: a native call stuck in any offloaded entry (section 4), both runtimes; the request
that started it is answered 503 at deadline + grace on plaintext HTTP/1.x (on TLS / HTTP/2 when
the run returns or never); new plaintext connections are answered 503 while it is stuck; the
server serves normally once it returns; the hard ceiling and a stop during standby end the
process.

Not covered:

- TLS listeners get no standby answers (standby cannot speak TLS without touching Keel's TLS
  state); clients wait in the backlog until the run returns or the ceiling exits the process.
- Keel's own work (keep-alive connections, timers, completions) is frozen during standby;
  static files are not served then either (every new connection gets the 503).
- A run abandoned in an async resume (a handler that parked, resumed, then got stuck) gets no
  early 503 on its own request: the resume path runs in the Keel-free base runtime object and
  cannot look up the connection's response there. That request is answered (500) when the run
  returns; new connections still get their 503 at once.
- No concurrency: one VM, serialized runs (phase 2).
- `--max-run-ms 0` (no deadline) disables abandonment, and with it the ceiling.

## 7. Open question 4: `hull.ssh` / `hull.kv` network calls

Keep them where they are - started from the run, driven by the loop. Under the baton a stream's
socket is armed on the loop from the VM thread while the loop thread waits, and its readiness
callbacks resume the run through the (offloaded) async resume; nothing about them needs to move
for phase 1. A synchronous `kv` command blocks inside the run (it already asks the run's budget
every slice, audit 19), so a stuck one is abandoned like any other native call. Moving the
stream I/O itself off the loop belongs to phase 2, together with the message-passing design of
section 2.

## 8. Flipping the default

1. Ship off by default (this change). `make e2e-vm-thread-all` runs the server suites with
   `HULL_VM_THREAD=1` in CI on every push, next to the same suites without it.
2. After PRs #980 / #981 land and are rebased under it, run the full e2e matrix (Postgres,
   MySQL, Valkey, SMTP, the WebSocket and SSE suites, the playwright suites) with
   `HULL_VM_THREAD=1` and a soak test (sustained load with injected stuck runs) under TSan on
   Linux.
3. Cut the hand-off cost before flipping. Measured on this change (macOS arm64, `ab -c 8 -k`
   against a trivial Lua `/ping`): 74k req/s without the thread, 33k with it - two condvar
   wake-ups per entry, about 15 us. That is noise next to a handler doing real work, but not
   next to a trivial one. A short spin on the job / done flags before the condvar sleep (both
   threads are usually hot) should recover most of it; require under 10 % on the HTTP
   benchmarks.
4. Turn it on by default in the next minor release, with `--no-vm-thread` /
   `limits.vm_thread = false` to opt out, and say so in the release notes.
