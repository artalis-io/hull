# Slice 2c plan: SMTP transport runtime scheduling and integration

Status: PLAN ONLY. No implementation until this PR is approved.
Branch: `feat/smtp-keel-slice-2c` off `main` @ 76bc29bf (Slice 2b merged).
Revision: v4 (freezes all five architectural-gate items: cap formula, deadline
relationship, audit schema, verified backend-shutdown ownership, and the exact
zero-Keel link seam).

## 0. Scope and frozen invariants

Four workstreams: (1) runtime/worker scheduling; (2) off-thread resolution; (3)
the post-resolution operation deadline; (4) deterministic coverage for the four
deferred behaviors. The reviewed Slice 2b protocol, TLS, parser, teardown, and
audit contracts are preserved UNCHANGED except the runtime API shape (section 2)
and the audit/execution split (section 4). PostgreSQL / MySQL are later
consumers. Native-Windows IOCP is a non-goal; "Windows" is the Cosmopolitan APE
(poll).

FROZEN: existing user-visible error strings are preserved verbatim; cancellation,
deadline, and scheduling failures all map to `connect_failed`, with distinctions
ONLY in structured audit metadata (section 3). No Keel string is ever leaked.

## 1. Today vs target

Model 1 today: `hl_cap_smtp_send` runs synchronously on the calling thread
(the event-loop thread in a handler), authorizes + audits inline, drives a
private `KlEventCtx` to terminal, returns 0/-1; `getaddrinfo` blocks that thread.
Model 2 target (design section 6): transport execution runs on a bounded worker;
the event loop suspends the runtime and resumes on the worker's published
terminal; the private `KlEventCtx` is pumped on the worker.

## 2. Public API contract and FROZEN fallback

- `hl_cap_smtp_send()` (C): SYNCHRONOUS, current signature + 0/-1; direct-C /
  unit-test entry and the no-loop executor.
- Lua `smtp.send(msg)`: yields when submitted; returns the same table on resume.
- JS `smtp.send(msg)`: ALWAYS returns a `Promise` resolving to `{ ok, error }`,
  including immediately-resolved validation/admission failures; rejects only on a
  programming error (bad argument type), never on an SMTP failure.

FROZEN fallback (never fall back to synchronous execution from an ACTIVE server
event loop; that reintroduces the loop-blocking defect):
- No active event loop (`app.main` CLI, in-process test harness): synchronous
  model-1 on the calling thread. Lua returns the table; JS returns an
  already-resolved Promise. This is the ONLY synchronous path.
- Active event loop: attempt admission (section 9). Admitted -> SUBMIT (Lua
  yields; JS pending Promise). Not admitted -> return an existing failure token
  WITHOUT running on the loop thread (Lua failure table; JS immediately-resolved
  Promise carrying it).

Consumers updated in lockstep: `email.lua`, `email.js` (JS must `await`), the
SMTP examples, `e2e_smtp` / `test_smtp_e2e` fixtures, and the docs. No public
`smtp.async.send`.

## 3. FROZEN audit schema (item 3)

Public error stays `connect_failed`. Distinctions live only in stable, readable
audit fields, EMITTED ONLY WHEN APPLICABLE, pinned by tests asserting exact JSON:

- `"schedule"`: `"pool_unavailable"` | `"cap_reached"` | `"queue_full"` |
  `"suspend_failed"` (present only on a scheduling failure; section 9).
- `"terminal"`: `"cancelled"` | `"post_resolution_deadline"` (present only when
  the op ended by cancellation or by the section 8 deadline).
- `"teardown"`: `"leaked"` (preserved from Slice 2b; present only on a
  non-detaching teardown).

A normal success/failure emits none of these three. The exact field names and
enum values above are frozen; only these metadata fields may be added.

## 4. Prepare/authorize vs worker execution split

```
EVENT LOOP (submit):  parse -> validate -> authorize host (declared hostname,
                      existing exact/glob/CIDR policy) -> deep-copy into HlSmtpOp
                      -> admission (section 9) -> submit
WORKER (execute):     resolve -> [post-DNS cancel check] -> connect (Happy
                      Eyeballs) -> optional TLS -> SMTP -> confirmed teardown
                      (detach ACKs) -> publish stable terminal payload
                      (NO audit, NO runtime objects, NO shared TLS context)
EVENT LOOP (complete): emit audit (section 3) -> build runtime result -> resume
```

Authorization is entirely on the submit side; the worker does NOT re-authorize.
The crossing payload carries only a stable token + the section-3 metadata; no
borrowed pointers, no Keel objects, no fd. The same execute-phase function serves
the worker and the no-loop inline path; the caller does audit + result in both.

## 5. Worker ownership and two-phase teardown

One `HlSmtpOp` owns the `HlSmtpTransport` + the private `KlEventCtx` for its whole
lifetime; nothing on the event loop dereferences transport storage. Two-phase
teardown: a cancel REQUEST is a signal, never a synchronous free; the runtime
releases suspension ONLY after the worker publishes a terminal. A per-request
cancel marks the continuation NON-RESUMABLE but RETAINS its runtime-side
ownership/ref until terminal publication (or the post-`pool_free()` shutdown
sweep, section 10); it must never release the last runtime-side ownership before
the worker publishes terminal. Transport-owned
resolver result, timers, stream, and TLS session remain alive until detach/close
ACKs (`kl_connect_op_is_detached`; `t->closed` via `tp_stream_on_close`; timers
cancelled; TLS destroyed after stream detach); the abortive
cancel-then-pump-to-detachment runs ON THE WORKER; fail-closed leaks + reports
`teardown:leaked` rather than freeing into a UAF.

## 6. One linearizable op state machine

```
QUEUED ---> RUNNING ---> COMPLETING ---> DONE
   \                                    /
    \--------> CANCEL_REQUESTED --------/
```

Single `_Atomic(int) state`, all moves by CAS. Worker dequeue: CAS
`QUEUED->RUNNING` (fail if `CANCEL_REQUESTED` -> cancelled-teardown). Cancel: CAS
`QUEUED->CANCEL_REQUESTED` or `RUNNING->CANCEL_REQUESTED` (no-op if already
`COMPLETING`/`DONE`). Normal finish: CAS `RUNNING->COMPLETING` (if it loses to
`CANCEL_REQUESTED`, honor the cancel). The CAS out of `RUNNING` is the
LINEARIZATION POINT: exactly one of complete/cancel wins. Memory ordering: the
worker writes the terminal payload, then publishes it with the state transition
under `memory_order_release`; the resume/complete side reads state with
`memory_order_acquire`, so the payload is fully visible before use; `DONE` is
observed only after a published payload. Refcount = 2 at submit (runtime-side +
worker-side); the worker frees transport storage after detachment then drops its
ref; the LAST ref frees the op shell + copied inputs + cross-boundary payload.

## 7. Resolver-result lifetime + post-DNS cancel check

`getaddrinfo` runs on the worker; its result (or copied sockaddrs) is op-owned,
outlives every connect attempt (incl. the staggered second), freed only after
connect-op detachment, on the worker, never while an attempt fd is armed; no
resolver result/sockaddr/index crosses to the event loop; no `KlResolver` is
retained. IMMEDIATELY after `getaddrinfo` returns and BEFORE opening a socket,
the worker checks for `CANCEL_REQUESTED` and, if set, goes to cancelled-teardown
without connecting. A cancel arriving DURING the blocking `getaddrinfo` cannot
interrupt it (section 8); this post-DNS check is the earliest honest cancel point.

## 8. FROZEN deadline relationship (item 2)

`getaddrinfo` cannot be interrupted (Keel's cancellable async resolver needs
`resolv.conf` + direct UDP the sandbox forbids, the reason `http_async` forces
`system_dns`), so the ceiling is POST-RESOLUTION, not a hard total. Frozen:

```
Dop    = monotonic_now_after_resolution + configured_timeout
Dstage = min(Dop, monotonic_now + stage_budget)
```

- Every post-resolution stage pumps against `Dstage` (via `pump_until_abs`), so
  no stage or retry can extend `Dop`.
- The connect-deadline timer receives `Dop - now`, NEVER a fresh full timeout.
- Graceful close is additionally bounded by `min(Dop, now + SMTP_CLOSE_GRACE_MS)`.
- On `Dop` expiry: issue a cancel request (not a stage hard-kill); public token
  `connect_failed`; audit `terminal:post_resolution_deadline`.
- DNS is explicitly outside the ceiling; naming/docs/audit say "post-resolution."
  A blocking `getaddrinfo` may occupy a worker after a cancel until the OS
  resolver returns (bounded by `resolv.conf` timeout x attempts); the SMTP cap
  (section 9) mitigates pool occupancy. A bounded sandbox-safe resolver is a
  separate future item.

## 9. FROZEN SMTP cap, admission, and fairness (item 1)

`kl_thread_pool_submit` returns -1 when the shared queue (default 64) is full;
the pool is shared with db.async / gpu.async / compute.

Frozen cap formula, over W = worker count known at serve wiring:

```
W <= 1: async SMTP admission DISABLED
W >= 2: smtp_max_inflight = max(1, floor(W / 2))
```

- The async pool does not expose W at run time, so `smtp_max_inflight` is
  COMPUTED AT WIRING (W is known there) and stored as a constant. SMTP maintains
  its OWN atomic counter of QUEUED-plus-RUNNING SMTP jobs (incremented at
  admission, decremented at terminal), NOT a query of active workers. This
  guarantees at least `W - floor(W/2) >= 1` workers always remain OUTSIDE SMTP
  admission for db/compute (W>=2), and a W<=1 pool admits no async SMTP at all
  (no false headroom claim).
- Admission (active loop): admit iff `counter < smtp_max_inflight` AND
  `kl_thread_pool_submit` succeeds; else a scheduling failure.
- FROZEN scheduling-failure mapping (public token `connect_failed`, section-3
  metadata): pool unavailable -> `schedule:pool_unavailable`; cap reached ->
  `schedule:cap_reached`; queue full -> `schedule:queue_full`; suspension setup
  failed -> `schedule:suspend_failed`.
- Queued-job cancellation via the section 6 `QUEUED->CANCEL_REQUESTED` path.
- Tests: parameterize the cap over W in {1,2,3,4,5,8}, asserting
  `smtp_max_inflight` = {disabled,1,1,2,2,4} and that at least one worker remains
  outside SMTP admission for W>=2. A SATURATION test floods slow SMTP (delayed
  peers) up to the cap and proves a db.async / compute op still completes within
  a bounded window.

## 10. VERIFIED backend-shutdown ownership (item 4)

Investigated both backends (not assumed):

- Keel (`src/hull/async/keel.c` -> `kl_thread_pool_free`, vendor/keel
  `thread_pool.c:240`): signals shutdown, JOINS workers, removes the wakeup
  watcher, then DRAINS the done queue calling `done_fn` for every completed item,
  then drains the work queue calling `cancel_fn` for never-started items. So on
  Keel, `done_fn` for work finishing during shutdown DOES fire at free.
- Poll (`src/hull/async/poll.c` -> `poll_pool_free:702`): signals shutdown,
  snapshots + clears QUEUED items, JOINS workers, then fires `cancel_fn` for the
  never-started items. It does NOT drain the completion queue, so a `done_fn`
  enqueued by work finishing during shutdown is DROPPED (same as the explicit
  OOM-drop at `poll.c:618`).
- Neither backend's `free()` fires `on_cancel` for a still-suspended op
  ("graceful shutdown is the caller's job"). So SMTP MUST drive its own
  cancellation of in-flight ops BEFORE pool/backend free; `on_cancel`-on-free is
  not a safety net.

Design consequence: `done_fn` is RESUME-ONLY and may be dropped (poll shutdown);
it NEVER owns a free or a ref-drop. `work_fn` (worker) always runs to completion
because `pool_free` joins, and it owns transport teardown + storage free + the
worker-ref drop on BOTH backends. The runtime-side ref (the FROZEN contract,
section 5) is released ONLY at or after terminal publication: by `on_resume` on
the normal path, or by the post-`pool_free()` shutdown sweep during teardown
(guaranteed, since `free()` will not fire `on_cancel`). A per-request cancel does
NOT drop the runtime ref; it marks the continuation NON-RESUMABLE (so a later
`on_resume` becomes a no-op that then drops the ref) but retains ownership until
terminal. No path releases the last runtime-side ownership before the worker
publishes terminal.

Ownership table (who owns queued/running/notified items at shutdown):

| Case | Keel | Poll | Owner / cleanup |
|---|---|---|---|
| Shutdown starts before work runs (QUEUED) | `cancel_fn` fires at free | `cancel_fn` fires at free | `cancel_fn` (loop thread) transitions `->CANCEL_REQUESTED`, no transport opened, drops the worker ref; runtime ref dropped by the shutdown sweep; last ref frees shell |
| Work completes while loop stopping (RUNNING, finishes in join) | `done_fn` drained at free | `done_fn` DROPPED | `work_fn` (worker, always runs) tore down transport + freed storage + dropped worker ref; runtime ref dropped by the shutdown sweep (not relied on `done_fn`); resume is best-effort |
| Completion notification queued but never dispatched | n/a (drained) | DROPPED | identical to the running case: storage already freed by `work_fn`; no free lives in `done_fn`; no leak |

Shutdown order (frozen), built around the real primitive - `pool_free()` is a
single call that signals shutdown, joins workers, dispatches/drops completions
per the table, runs `cancel_fn` for queued work, and destroys the pool (there is
no separate "join now, destroy later" step):

```
1. stop new SMTP submissions
2. mark shutdown + request SMTP cancellation of all in-flight ops
3. pool_free()   (backend-specific join / completion / cancel, per the table)
4. after it returns, verify every SMTP WORKER ref is gone (all work_fn ran to
   terminal + detachment during the join)
5. release the retained runtime refs / continuations (the shutdown sweep, since
   resume was best-effort and never targets a released runtime)
6. free the CA material and the server allocator
```

Allocator lifetime: the per-worker pthread-TLS `KlTlsCtx` destructors run AS
WORKERS EXIT during step 3 (`pool_free()`'s join), and a `KlTlsCtx` needs its
ALLOCATOR at destruction, so the allocator MUST stay alive THROUGH `pool_free()`
and is freed only in step 6. A `KlTlsCtx` does NOT dereference the original CA
buffer at destroy (mbedTLS parsed/copied the certificates into the context at
creation); the CA buffer must merely outlive any worker that could still CREATE a
context, i.e. through `pool_free()`, and freeing it in step 6 is a reasonable
server-lifetime policy, not a destroy-time dependency.

Backend-parity TEST REQUIREMENT: the shutdown-with-op-in-flight suite runs under
BOTH backends (keel and poll) and asserts, in all three cases, no leak / no UAF /
exactly-once free / exactly-once runtime-ref drop, DESPITE the documented
`done_fn`-dispatch divergence. (Aligning poll to drain `done_fn` at free for
strict parity is noted as an optional backend follow-up; the SMTP design is
correct without it.)

### 10a. The sweep mechanism (Keel-verified, from the implementation)

Step 5's sweep is CONCRETE, not abstract. Keel v3 exposes a public, idempotent
`kl_async_cancel(KlHttpServer *s, KlAsyncOp *op)` (`vendor/keel/include/keel/async.h`):
the exactly-one-terminal `_terminal` guard means a cancel racing a completion (or a
second cancel) never double-fires `on_cancel`, double-releases, or UAFs. It fires
`op->on_cancel` (Hull's `hl_async_on_cancel` -> `cont->cancel` + `free_driver` +
`cont->destroy` + free the `HlAsyncCtx`), removes the op from the server's
`async_ops` list, and does NOT re-arm the fd or drive the state machine. Keel's own
`kl_http_server_free()` already loops `while (s->async_ops) kl_async_cancel(...)`.

The CRITICAL ordering fact (verified in `serve.c`): `kl_http_server_free()` runs in
the FINAL `hl_serve_cleanup()`, i.e. AFTER `hl_serve_teardown_after_serve()` has
already run `hl_app_context_free()` (the runtime is GONE). So SMTP must NOT lean on
Keel's server-free cancel loop - by then `cont->cancel` would touch a freed
coroutine/Promise. Instead SMTP drives retirement itself, in TWO distinct registry
passes around `pool_free()` (step 3). Both are exposed as separate, idempotent
server operations so the SAME lifecycle applies to `hl_serve_teardown_after_serve()`
AND the partial-init `hl_serve_cleanup()`.

PASS 1 - `hl_smtp_server_request_cancel_all()`, BEFORE `pool_free()` (step 2). Sets
`shutting_down` (stop new submissions), then walks the registry WITHOUT unlinking
(`hl_smtp_inflight_for_each`) and calls `hl_smtp_submit_ctx_cancel()` on every op.
That marks each continuation NON-RESUMABLE and flips the worker op to
`CANCEL_REQUESTED`, so a RUNNING transport observes the shutdown (its cancel poll)
and detaches + terminates promptly instead of blocking the join for its full network
deadline. Registry-preserving + idempotent: the ops stay tracked for pass 2, and a
second call is a no-op. This pass is what makes shutdown BOUNDED; without it,
cancellation would begin only after the join and a slow peer could stall teardown.

PASS 2 - `hl_smtp_server_sweep()`, AFTER `pool_free()` (steps 4-5). Workers are
joined and every terminal is published. For each still-registered op:

```
unlink its registry node FIRST (single-owner transfer), then
ATTACHED (bound to a conn): hl_net_op_cancel(net_ctx, &actx->op)  [seam -> kl_async_cancel]
    -> on_cancel = hl_async_on_cancel -> cont->cancel + free_driver + destroy + free actx
DETACHED (timer, no conn):  hl_async_ctx_cancel(actx)             [the same body, no net op]
```

`free_driver` is the SMTP driver teardown: `registry_remove(node)` (a no-op - the
sweep already unlinked) + `hl_smtp_submit_ctx_release(sctx)` (drops the ONE retained
runtime ref) + free the driver/op shell. Because the sweep unlinks each node before
triggering its release, and `hl_net_op_cancel` / `kl_async_cancel` is idempotent, the
retained runtime ref is released EXACTLY ONCE - by normal completion (`on_resume` ->
`free_driver`, which `registry_remove`s first) OR by this sweep, never both. After
the sweep every SMTP op is retired and off Keel's `async_ops` list, so
`kl_http_server_free()`'s own loop finds nothing SMTP-owned to cancel. (This is
strictly more correct than `db.async`, which has no sweep and would fire `on_cancel`
post-runtime-free on a poll-backend shutdown with a still-suspended op; SMTP closes
that window.)

In `hl_serve_cleanup()` (partial init / error path) the same two-pass retirement runs
while the runtime + net/server contexts are still valid, BEFORE `hl_app_context_free()`
there too, regardless of whether Keel's server-free cancellation would also run.

## 11. TLS trust-material ownership

`MBEDTLS_THREADING` is OFF, so the shared `KlTlsCtx` (RNG/`ssl_config`) is not
concurrency-safe. At serve wiring, BEFORE sandbox activation, resolve the CA
material into an IMMUTABLE server-retained buffer: the embedded Mozilla bundle is
already a static in-binary buffer (`hl_embedded_ca_bundle`); a `--ca-bundle` FILE
is read ONCE into a server-owned buffer here (no CA file re-read after the
sandbox seals the fs). Each WORKER THREAD lazily builds and caches (pthread TLS,
`worker_db` pattern) its OWN `KlTlsCtx` from that buffer via
`hl_tls_client_ctx_create_from_buf` / `kl_tls_mbedtls_client_ctx_create_from_buf`
(no shared context, no RNG race, no lock, no file read on the worker). The
per-worker contexts are destroyed by their pthread-TLS destructors as workers
exit during `pool_free()`, and the CA buffer + allocator are freed only after it
returns (section 10 handles the lifetime + ordering; the allocator, not the CA
buffer, is the destroy-time dependency). The no-loop synchronous fallback keeps
the existing single-threaded shared context.

## 12. Exact zero-Keel link seam (item 5)

There are TWO distinct seams; the SMTP glue uses both and references neither Keel
symbol directly.

Seam A - worker EXECUTION (the thread pool), via `hl_async_backend()`:
- WEAK default: `const HlAsyncBackend hl_async_backend_poll` + the WEAK
  `hl_async_backend()` in `src/hull/async/poll.c` (0 `kl_` references,
  self-contained pthreads); what a Keel-less base links.
- STRONG anchor: `const HlAsyncBackend hl_async_backend_keel` + the STRONG
  `hl_async_backend()` override in `src/hull/async/keel.c` (22 `kl_` references;
  the SOLE referencer of `kl_thread_pool_*` / `libkeel.a`), composed via
  `libhull_feature-keel.a` (the `HL_KEEL_FEATURE` axis), pulled only for
  HTTP/Keel apps. The SMTP glue calls only `hl_async_backend()->pool_submit`.

Seam B - active-request SUSPENSION/RESUMPTION, via `hl_net_op_*` (NOT the
`HlAsyncBackend` vtable): connection-bound suspend/resume follows the existing
db/compute pattern through `hl_net_op_suspend()` / `hl_net_op_complete()` on an
`HlAsyncCtx` bound to the request's `net_ctx` (exactly as `mod_db.c:787` does for
`db.async`). These are weak stubs (Keel-free, in the base) versus the strong
`src/hull/net/keel.c`. The SMTP runtime binding suspends/resumes through these,
never through `HlAsyncBackend::op_suspend`.

So the SMTP submit/execute glue (`worker_smtp.o`, mirroring `worker_db.o`)
references ONLY `hl_async_backend()` (seam A) and the binding references
`hl_net_op_suspend` / `hl_net_op_complete` (seam B) - NEVER `kl_thread_pool_*` or
any `kl_` symbol directly.

Why compute-only linking cannot pull Keel: (a) SMTP is an HTTP-client-family
feature; a compute-only app declares no `hull/smtp` / `hull/email`, so the smtp
objects + `worker_smtp.o` are not linked at all; (b) even when linked, both seams'
weak defaults are Keel-free (poll backend, `hl_net_op_*` stubs), so the strong
keel anchors are never pulled. TEST: keep the existing zero-Keel `nm` assertion on
a compute app (`nm app | grep kl_thread_pool` -> empty), PLUS a NEGATIVE test that
`worker_smtp.o` has NO undefined `kl_` symbols (`nm -u worker_smtp.o | grep '
kl_'` -> empty) while PERMITTING the two `hl_net_op_*` symbols; a deliberate
variant that calls `kl_thread_pool_submit` directly (bypassing seam A) MUST make
that assertion fail, proving the guard bites.

## 13. Deterministic coverage for the four deferred behaviors

Each test BITES (fails on the reverted code, proven by an explicit revert), via
an `HL_SMTP_TEST_HOOKS` seam providing an injected deterministic address list
(bypassing the OS resolver) and attempt-observation (address index per attempt;
connect pending/failed/succeeded; which timer fired), plus bounded watchdogs.

1. Happy Eyeballs stagger: inject a two-address list whose FIRST address stays in
   connect-PENDING (not immediately refused), so the stagger timer fires and the
   SECOND wins; assert via the seam that the staggered second connected within
   the window and the injected set outlived the race. Revert proof: forcing
   sequential attempts changes the observed winner/timing.
2. Connect-deadline vs operation-deadline, ISOLATED: (a) keep CONNECT genuinely
   PENDING with `Dop` set LARGE -> only the connect timer can fire; assert the
   CONNECT timer fired. (b) an accepted-TCP peer that withholds its greeting with
   the connect deadline LARGE -> only `Dop` can fire; assert
   `terminal:post_resolution_deadline`. Each proves which timer fired. Revert
   proof: removing the respective arm hangs past the watchdog.
3. Live TLS-failure branches: a real in-process mbedTLS peer (ported from the
   Slice 2a spike, not a mock vtable): hostname mismatch, unknown CA, handshake
   timeout, rejected STARTTLS; assert fail-closed with the correct existing
   token, credentials never sent, no plaintext downgrade. Revert proof: each
   verify/no-downgrade check, reverted, flips an asserted outcome.
4. Cancellation / teardown with an op in flight (sections 5, 6, 10): tear down
   the runtime/request while a worker op is live at each stage (queued, resolving,
   racing connect, read, write, TLS handshake, DATA, graceful close), under BOTH
   backends. Assert: suspension released only after terminal published; storage
   detached exactly once; no UAF/double-free; fd/timer/TLS retire once; a
   non-detaching case reports `teardown:leaked`. Cancellation-DURING-RESOLUTION
   asserts the REAL behavior (blocking `getaddrinfo` not interruptible): the
   cancel is recorded, the worker stays occupied until `getaddrinfo` returns, THEN
   the post-DNS check (section 7) goes to cancelled-teardown before any socket.

Sanitizer / backend matrix (realistic): ASan+UBSan on Linux and macOS; MSan on
Linux/clang only (Keel instrumented, Slice 2b); TSan on Linux for the worker-pool
+ saturation + shutdown-parity paths; fd-leak + failure-injection where those
suites run. Backends: epoll (Linux), kqueue (macOS), poll (Cosmo APE); the APE
runs no sanitizers.

## 14. Preserved contracts (must not change)

Protocol (sequencing, incremental reply parser with exact terminators +
whole-response bound, dot handling, CRLF-injection rejection); TLS (implicit +
in-place STARTTLS, CA + hostname verify, no-downgrade, ClientHello socket-handoff
invariant, persistent-allocator-outlives-KlTls invariant); teardown (fail-closed
confirmed detachment, on the worker); audit (metadata + stable result only, never
credentials or bodies; section-3 fields internal); error tokens (every existing
user-visible string at its site; Keel errors mapped at the boundary).

## 15. Acceptance and residual implementation-review items

Acceptance (design section 10): all Slice 2b tests green; the four deferred
behaviors bite; Lua/JS parity + server-responsive-during-a-delayed-peer +
teardown-with-op-in-flight (both backends) pass; the cap parameterization +
saturation test pass; the zero-Keel `nm` + negative seam test pass; the
sanitizer/backend matrix (section 13) is clean; native + composed + all-flavor
builds pass and an SMTP-free compute app links zero unwanted subsystems; and the
exact-head cross-platform matrix is fully green. Stop for full acceptance review.

The five gate items are now frozen (sections 3, 8, 9, 10, 11, 12). The only
residual, converted into a test-gated implementation requirement rather than a
guess: VERIFY at implementation time that the section-10 backend-shutdown
ownership table holds under both backends exactly as documented, via the
backend-parity test; if a backend's real behavior differs from this record, fix
the code or the record before merging, do not weaken the test.
