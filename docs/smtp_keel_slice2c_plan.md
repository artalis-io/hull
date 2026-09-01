# Slice 2c plan: SMTP transport runtime scheduling and integration

Status: PLAN ONLY. No implementation until this PR is approved.
Branch: `feat/smtp-keel-slice-2c` off `main` @ 76bc29bf (Slice 2b merged).
Revision: v3 (freezes fallback, the op state machine, scheduling-failure tokens,
TLS trust-material ownership, the SMTP cap, and shutdown ordering).

## 0. Scope (fixed by review)

Four workstreams, nothing else:

1. runtime/worker scheduling (move SMTP execution off the event-loop thread);
2. off-thread name resolution;
3. the post-resolution operation deadline (section 8);
4. deterministic coverage for the four deferred behaviors.

The reviewed SMTP protocol, TLS, parser, teardown, and audit contracts from
Slice 2b are preserved UNCHANGED except the runtime API shape (section 2) and the
audit/execution split (section 4). PostgreSQL / MySQL are later independent
consumers. Native-Windows IOCP stays a non-goal; "Windows" is the shipped
Cosmopolitan APE (readiness/poll).

FROZEN PUBLIC ERROR MAPPING: existing user-visible strings are preserved
verbatim. Cancellation, deadline, and every scheduling failure map to EXISTING
tokens (sections 2, 8, 9); their distinctions live only in structured audit
metadata. No Keel string is ever leaked as a user-visible token.

## 1. Today vs target

Today (model 1): `hl_cap_smtp_send` runs synchronously on the calling thread
(the event-loop thread in a handler), authorizes and writes audit inline, drives
a private `KlEventCtx` to terminal, returns 0/-1. Resolution is a blocking
`getaddrinfo` on that thread. A slow peer blocks the whole loop.

Target (model 2, design section 6): transport execution runs on a bounded worker
thread; the event loop suspends the runtime and resumes on the worker's
published terminal result; the private `KlEventCtx` is pumped on the worker.

## 2. Public API contract and FROZEN fallback (correction 1)

Surfaces differ by runtime, one result shape (`{ ok }` / `{ ok, error }`):

- `hl_cap_smtp_send()` (C): SYNCHRONOUS, current signature + 0/-1. Direct-C /
  unit-test entry and the no-loop executor.
- Lua `smtp.send(msg)`: yields when submitted; returns the same table on resume.
- JS `smtp.send(msg)`: ALWAYS returns a `Promise` resolving to that object,
  including immediately-resolved failures (validation, admission). It rejects
  only on a programming error (bad argument type), never on an SMTP failure.

FROZEN fallback rule (a synchronous fall-back from an ACTIVE server event loop is
forbidden, since it recreates the original loop-blocking defect):

- No active event loop (a `app.main` CLI run, the in-process test harness):
  synchronous model-1 execution on the calling thread. Lua returns the table; JS
  returns an already-resolved Promise carrying it. This is the ONLY synchronous
  path.
- Active event loop: attempt admission (section 9). If admitted, SUBMIT to a
  worker (Lua yields; JS returns a pending Promise). If NOT admitted (no pool /
  pool cannot accept / SMTP cap reached / queue full), return an EXISTING failure
  token WITHOUT running on the loop thread (Lua returns the failure table; JS
  returns an immediately-resolved Promise carrying it). Never run the operation
  synchronously on the loop thread.

Consumers updated in lockstep: `stdlib/.../email.lua` and `email.js` (JS must
`await`), the SMTP examples, `e2e_smtp` / `test_smtp_e2e` fixtures, and the docs.
A `smtp.async.send` public API is OUT of scope.

## 3. Operation-input ownership (correction 2, 7)

The bindings borrow Lua/QuickJS storage; after suspension the worker must not
reference any of it. `HlSmtpOp` deep-copies and solely owns, at submit time on
the event-loop thread: `host`, `port`, `use_tls`; credentials (`username`,
`password`); `from`, `to`, `reply_to`, `subject`, `body`, `content_type`; every
`cc` element as an owned vector of owned strings; and the run-time config values
(timeout). (No `bcc`: the current `HlSmtpMessage` has only `cc` / `cc_count`; a
BCC field is not in scope.)

Scrubbing/free on EVERY exit path (submission failure, queued-cancel,
running-cancel, success, each worker failure): credentials and the copied body
are volatile-memset scrubbed before free (extends Slice 2b's `smtp_secure_zero`
from stack buffers to the owned heap copies); all owned storage is freed exactly
once by the owning side per the section 6 refcount.

## 4. Prepare/authorize vs worker execution split (correction 3-of-v2)

The worker never touches audit writers or runtime objects. `hl_cap_smtp_send` is
decomposed into three phases with a hard thread boundary:

```
EVENT LOOP (submit):  parse -> validate -> authorize host (declared hostname,
                      existing exact/glob/CIDR policy) -> deep-copy into HlSmtpOp
                      -> admission (section 9) -> submit
WORKER (execute):     resolve -> [cancel check] -> connect (Happy Eyeballs) ->
                      optional TLS -> SMTP -> confirmed teardown (detach ACKs) ->
                      write a stable terminal payload   (NO audit, NO runtime
                      objects, NO shared TLS context)
EVENT LOOP (complete): emit audit (metadata + stable result + internal
                      cancel/deadline/scheduling distinction) -> build runtime
                      result (Lua table / resolve JS promise) -> resume/reject
```

Authorization is entirely on the submit side against the declared hostname; an
unauthorized host never crosses the boundary and the worker does NOT
re-authorize. The crossing payload carries only a stable token (an existing
user-visible string) plus internal audit fields; no borrowed pointers, no Keel
objects, no fd. The same execute-phase function serves both the worker and the
no-loop inline path; the caller does audit + result construction in both.

## 5. Worker ownership and two-phase teardown

One `HlSmtpOp` owns the `HlSmtpTransport` (connect op, stream, TLS session,
timers, write queue, reply accumulator) and the private `KlEventCtx` for its
whole lifetime; nothing on the event loop dereferences transport storage. The
private `KlEventCtx` is created and destroyed on the worker.

Two-phase teardown (cancellation REQUEST vs confirmed DETACHMENT):
- a cancel REQUEST is a signal, never a synchronous free;
- the runtime releases its suspension/completion state ONLY after the worker
  publishes a terminal;
- transport-owned resolver result, timers, stream, and TLS session remain alive
  until their detach/close ACKs complete (connect op `kl_connect_op_is_detached`;
  stream `t->closed` via `tp_stream_on_close`; timers cancelled; TLS destroyed
  only after stream detach); the abortive cancel-then-pump-to-detachment runs ON
  THE WORKER;
- fail-closed: if detachment is not confirmed within the bound, storage is
  intentionally leaked and reported (`teardown_leaked` -> audit), never freed
  into a use-after-free.

Ordering: cancel request -> worker observes -> worker abortive-detaches ->
detach/close ACKs -> worker frees transport storage -> worker publishes terminal
-> event loop emits audit + resumes -> runtime releases suspension.

## 6. One linearizable op state machine (correction 2)

A separate `cancel_requested` flag plus an independent state CAS cannot guarantee
"cancel-set-first is honored," so Slice 2c uses ONE atomic state; cancellation
and the RUNNING->COMPLETING transition CONTEND ON THE SAME atomic:

```
QUEUED ---> RUNNING ---> COMPLETING ---> DONE
   \                                    /
    \--------> CANCEL_REQUESTED --------/
```

- `_Atomic(int) state`, initialized `QUEUED`. All moves are CAS.
- Worker dequeue: CAS `QUEUED -> RUNNING`. If that fails because the state is
  `CANCEL_REQUESTED` (a cancel beat the dequeue), the worker takes the
  cancelled-teardown path.
- Cancel: CAS `QUEUED -> CANCEL_REQUESTED` or `RUNNING -> CANCEL_REQUESTED`. If
  the CAS fails because the state is already `COMPLETING`/`DONE`, cancel is a
  no-op (the worker won).
- Normal finish: CAS `RUNNING -> COMPLETING`. If that fails because the state is
  `CANCEL_REQUESTED` (cancel won), the worker honors the cancel and takes the
  cancelled-teardown path. This CAS out of `RUNNING` (to `COMPLETING` or losing
  to `CANCEL_REQUESTED`) is the LINEARIZATION POINT: exactly one of
  complete/cancel wins.
- Memory ordering: the worker writes the terminal payload, then performs the
  state transition to publish it with `memory_order_release`; the completing/
  resuming side reads state with `memory_order_acquire`, so the payload is
  fully visible before it is consumed. `DONE` is set after the payload is
  published; the resume/audit side only ever observes a published payload.
- Refcount = 2 at submit (one runtime-side ref held while suspended, one
  worker-side ref while owning the op). The worker frees transport-derived
  storage after detachment ACKs, then drops its ref. The LAST ref to drop frees
  the op shell + the copied inputs (section 3) and the cross-boundary payload;
  which side is last is decided by the refcount, never freed by both.

Cases:
- Queued-but-not-started cancel: `QUEUED -> CANCEL_REQUESTED`; the worker on
  dequeue takes cancelled-teardown with NO transport opened (nothing to detach),
  drops its ref.
- Running cancel: `RUNNING -> CANCEL_REQUESTED`; the worker observes it at
  defined safe points (between stage pumps and inside the pump predicate),
  abortive-detaches to ACKs, publishes the cancelled terminal, drops its ref;
  the runtime ref drops only at resume.
- Completion racing cancel: resolved by the single CAS out of `RUNNING`.
- Pool shutdown: `kl_thread_pool_free` runs the cancel_fn for never-started
  (`QUEUED`) items (-> queued-cancel) and JOINS running threads; see section 10.
- Submit ok but runtime-suspension setup then failed: CAS toward
  `CANCEL_REQUESTED`, drop the runtime ref immediately, mark "no runtime to
  resume"; the worker still runs to terminal + detachment and self-frees via the
  refcount; the complete-side resume is guarded by runtime liveness and is a
  no-op (audit still emitted). No resume-after-free.

## 7. Resolver-result lifetime + post-DNS cancel check (correction 2, 5, 7)

- `getaddrinfo` runs on the worker; its result (or the copied sockaddrs) is
  op-owned, lives on the worker, and outlives every connect attempt including the
  staggered second; freed only after connect-op detachment, on the worker, never
  while an attempt fd is armed. No resolver result/sockaddr/index crosses to the
  event loop. No `KlResolver` object is retained.
- IMMEDIATELY after `getaddrinfo` returns, and BEFORE opening any socket, the
  worker checks the op state for `CANCEL_REQUESTED` and, if set, transitions to
  cancelled-teardown without connecting. (A cancel that arrives DURING the
  blocking `getaddrinfo` cannot interrupt it; see section 8. This post-DNS check
  is the earliest honest cancellation point.)

## 8. Post-resolution operation deadline: honest (correction 5)

A blocking `getaddrinfo` cannot be interrupted, and Keel's cancellable async
resolver needs `resolv.conf` + direct UDP that Hull's sandbox forbids (the reason
`http_async` forces `system_dns`). So DNS is NOT bounded by the ceiling.

Decision (honest naming): Slice 2c introduces a POST-RESOLUTION operation
deadline, computed AFTER resolution returns (`kl_monotonic_ms() + budget`) and
threaded to every subsequent stage pump via `pump_until_abs` as the min-bounding
ceiling (per-stage budgets remain sub-bounds). Naming, docs, the audit field, and
tests all say "post-resolution operation deadline"; nothing claims to bound DNS.
On expiry it issues a cancel request (not a hard stage-kill) and tears down via
confirmed detachment. Public token on deadline: `connect_failed` (frozen), with
the deadline distinction in audit metadata.

Pool-capacity consequence: a blocking `getaddrinfo` can occupy a worker after a
cancel request until the OS resolver returns (bounded in practice by
`resolv.conf` timeout x attempts). This is acknowledged; the mitigation is the
SMTP cap (section 9). A bounded sandbox-compatible resolver is a separate future
item.

## 9. Pool admission, the SMTP cap, and fairness (correction 6-of-v2, 5)

`kl_thread_pool_submit` returns -1 when the shared queue (default capacity 64) is
full; the pool is shared with db.async / gpu.async / compute.

- Worker count: the async pool does not expose its size at run time, so the SMTP
  cap is COMPUTED AT SERVE WIRING, where the pool size is known, and stored as a
  constant `smtp_max_inflight = floor(pool_size * FRACTION)` (FRACTION a
  compile-time constant, default 1/2). SMTP maintains its OWN atomic counter of
  QUEUED-plus-RUNNING SMTP jobs (incremented at admission, decremented at
  terminal), NOT a query of active workers.
- Admission (active loop): admit iff `counter < smtp_max_inflight` AND
  `kl_thread_pool_submit` succeeds. Otherwise it is a scheduling failure.
- One-worker (or tiny) pool: `floor(1 * 1/2) = 0`, so `smtp_max_inflight == 0`
  DISABLES asynchronous SMTP admission entirely. Under an active loop that means
  SMTP always takes the scheduling-failure path (below); we do NOT pretend
  DB/compute headroom exists, and we do NOT block the loop. (No-loop runs are
  unaffected: they use the synchronous path, section 2.)
- FROZEN scheduling-failure token: all four scheduling failures return the
  existing public token `connect_failed`, with the distinction ONLY in audit
  metadata:
  - pool unavailable (no pool on an active loop) -> `connect_failed`
    (meta: `sched=pool_unavailable`);
  - SMTP admission cap reached -> `connect_failed` (meta: `sched=cap_reached`);
  - shared queue full (`kl_thread_pool_submit` == -1) -> `connect_failed`
    (meta: `sched=queue_full`);
  - suspension setup failure -> `connect_failed` (meta: `sched=suspend_failed`).
- Queued-job cancellation: the section 6 `QUEUED -> CANCEL_REQUESTED` path.
- Saturation test: flood the pool with concurrent slow SMTP operations (delayed
  peers) and assert a `db.async` (or compute) op still completes within a bounded
  window, proving SMTP cannot indefinitely starve other subsystems.

## 10. Shutdown and runtime ownership ordering (correction 6)

Explicit shutdown order (server stop / runtime teardown):

```
1. stop accepting NEW SMTP submissions
2. request cancellation of all in-flight SMTP ops (section 6)
3. drain queued + running SMTP jobs to a terminal state (join; running ops run
   to detachment ACKs on their worker before their thread exits)
4. deliver-or-DISCARD completion callbacks under a defined rule (below)
5. release continuations / runtime state
6. destroy the per-worker-thread TLS contexts (section 3.TLS)
7. destroy the pool, then the server-retained CA buffer, then the persistent
   allocator
```

Completion-delivery rule: a completion normally resumes the runtime on the event
loop. `kl_thread_pool_free` "drains the queue, joins threads, removes the
watcher, and frees"; it runs `cancel_fn` for items that NEVER STARTED, but it is
NOT assumed to deliver already-finished done-callbacks after the server loop has
stopped. Therefore any terminal that becomes ready after the loop stops is
DISCARDED for resume purposes: the worker/last-ref still frees all storage
(transport on the worker; shell + inputs + payload via the refcount), and audit
is best-effort (skipped if the audit sink is already torn down). No resume ever
targets a released runtime. The exact backend guarantee here (queued done-
callback delivery vs discard after loop stop) is a MUST-VERIFY item at
implementation time against both async backends; the plan assumes discard and
does not rely on post-stop delivery.

Ordering invariants: per-worker TLS contexts are destroyed only AFTER all
running SMTP jobs have joined (step 3), and the CA buffer + allocator are freed
only AFTER the per-worker contexts are destroyed (step 6 before 7), because a
KlTlsCtx dereferences both at destroy.

## 11. TLS trust-material ownership (correction 4)

A worker cannot build a private context from the shared `KlTlsConfig` alone (that
only points at the unsafe shared `KlTlsCtx`; `MBEDTLS_THREADING` is off, so the
shared context's RNG/`ssl_config` are not safe for concurrent use). Concrete
ownership:

- At serve wiring, BEFORE sandbox activation, resolve the effective CA material
  into an IMMUTABLE server-retained buffer: the embedded Mozilla bundle is
  already a static in-binary buffer (`hl_embedded_ca_bundle`); a `--ca-bundle`
  FILE is read ONCE into a server-owned buffer here (so no CA file is re-read
  after the sandbox seals the filesystem).
- Each WORKER THREAD lazily builds and caches (pthread TLS, `worker_db` pattern)
  its OWN `KlTlsCtx` from that buffer via `hl_tls_client_ctx_create_from_buf` /
  `kl_tls_mbedtls_client_ctx_create_from_buf`. No shared context, no RNG race, no
  lock, no file read on the worker.
- The CA buffer stays alive until the worker pool is fully drained. Per-worker
  contexts are destroyed before the CA buffer + allocator are freed (section 10
  ordering).
- The event-loop-thread synchronous fallback (section 2) keeps using the existing
  single-threaded shared context (safe, one thread).

## 12. Deterministic coverage for the four deferred behaviors (correction 7)

Each test BITES (round-4 discipline: fails on the reverted code, proven by an
explicit revert), via injected/observable seams and bounded watchdogs, never OS
timing or resolver ordering. A test-only `HL_SMTP_TEST_HOOKS` seam provides an
injected deterministic address list (bypassing the OS resolver) and
attempt-observation (address index per attempt; connect pending/failed/
succeeded; which timer fired).

1. Happy Eyeballs stagger: inject a two-address list whose FIRST address stays
   in connect-PENDING (not immediately refused) so the stagger timer fires and
   the SECOND address wins; assert via the observation seam that the staggered
   second attempt connected within the stagger window and the injected set
   outlived the race. Revert proof: forcing sequential attempts (no stagger)
   changes the observed winner/timing.
2. Connect-deadline timer: use a provider/attempt seam that keeps CONNECT
   genuinely PENDING (never completes) with the post-resolution OPERATION
   deadline set LARGE, so the connect-deadline timer is the only one that can
   fire; assert via the observation seam that the CONNECT timer fired. The mirror
   case (an accepted-TCP peer that withholds its greeting, with the connect
   deadline set large) proves the OPERATION deadline fired. The two deadlines are
   isolated per test so each proves which timer fired. Revert proof: removing the
   respective arm hangs past the watchdog.
3. Live TLS-failure branches: a real in-process mbedTLS peer (ported from the
   Slice 2a spike, not a mock vtable) for hostname mismatch, unknown CA,
   handshake timeout, and rejected STARTTLS; assert fail-closed with the correct
   existing token, credentials never sent, no plaintext downgrade. Revert proof:
   each verify/no-downgrade check, reverted, flips an asserted outcome.
4. Cancellation / teardown with an op in flight (sections 5, 6): tear down the
   runtime/request while a worker op is live at each stage (queued, resolving,
   racing connect, read, write, TLS handshake, DATA, graceful close). Assert:
   suspension released only after terminal published; storage detached exactly
   once; no UAF/double-free; fd/timer/TLS retire once; a non-detaching case
   reports `teardown_leaked`. Cancellation-DURING-RESOLUTION asserts the REAL
   behavior (blocking `getaddrinfo` is not interruptible): the cancel is
   recorded, the worker stays occupied until `getaddrinfo` returns, THEN the
   post-DNS cancel check (section 7) transitions to cancelled-teardown before any
   socket opens. The test does not pretend resolve cancels instantly.

Sanitizer / backend matrix (realistic): ASan+UBSan on Linux and macOS; MSan on
Linux/clang only (Keel instrumented, Slice 2b); TSan on Linux for the worker-pool
+ saturation paths; fd-leak + failure-injection where those suites run. Backends:
epoll (Linux), kqueue (macOS), poll (Cosmo APE); the APE runs no sanitizers.

## 13. Preserved contracts (must not change)

Protocol (sequencing, incremental reply parser with exact terminators +
whole-response bound, dot handling, CRLF-injection rejection); TLS (implicit +
in-place STARTTLS, CA + hostname verify, no-downgrade, ClientHello
socket-handoff invariant, persistent-allocator-outlives-KlTls invariant);
teardown (fail-closed confirmed detachment, now on the worker); audit (metadata +
stable result only, never credentials or bodies; cancel/deadline/scheduling
distinctions are internal structured metadata); error tokens (every existing
user-visible string at its existing site; Keel errors mapped at the boundary).

## 14. Acceptance (maps to design section 10)

Done when: all Slice 2b tests stay green; the four deferred behaviors bite;
Lua/JS result-shape parity + server-stays-responsive-during-a-delayed-peer +
teardown-with-op-in-flight pass; the saturation test proves no cross-subsystem
starvation; the sanitizer/backend matrix (section 12) is clean; native +
composed + all-flavor builds pass and an SMTP-free compute app links zero
unwanted subsystems; and the exact-head cross-platform matrix is fully green.
Stop for full acceptance review before any follow-on.

## 15. Open decisions for implementation review

- The exact SMTP-cap FRACTION and the per-stage-vs-post-resolution budget
  relationship (locked by tests).
- The precise internal audit-metadata schema for the cancel / deadline /
  scheduling distinctions (public strings stay frozen; only metadata gains
  fields).
- VERIFY the async backends' post-loop-stop completion behavior (section 10)
  and confirm the discard assumption holds for both.
- Confirm no Keel symbol leaks into a compute-only base through the new glue
  (section 10/`nm` in `e2e_build_flavor`).
