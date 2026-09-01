# SMTP transition to the Keel client transport

Status: design only. No SMTP, TLS, runtime, or build behavior changes are made
by this record.

## 1. Objective

Move Hull's SMTP connection lifecycle from direct POSIX socket, `poll`, and
blocking TLS calls onto Keel v3's client transport surface without changing the
public `hull/smtp@1` or `hull/email@1` behavior.

The transition has two distinct ownership layers:

- Hull continues to own SMTP policy and protocol: manifest host authorization,
  message validation, SMTP reply parsing, EHLO, STARTTLS, AUTH PLAIN, envelope
  commands, message formatting, dot stuffing, stable error tokens, and audit
  records.
- Keel owns the byte transport: resolution, Happy Eyeballs connection attempts,
  timeout and cancellation retirement, socket-provider portability, queued
  reads and writes, TLS attachment, and graceful or abortive close.

SMTP must not be expressed as HTTP, depend on Keel private headers, or copy the
HTTP client's internal connect machinery into Hull.

## 2. Current implementation

`src/hull/cap/smtp.c` is a synchronous, single-message client. One call to
`hl_cap_smtp_send` performs the entire lifecycle:

1. validate the message and enforce `manifest.hosts`;
2. call `getaddrinfo`, select the first address, create a POSIX socket, and run
   a non-blocking connect bounded by `poll`;
3. optionally perform implicit TLS before the greeting;
4. read the greeting and run EHLO;
5. optionally issue STARTTLS, attach TLS, and run EHLO again;
6. optionally authenticate with AUTH PLAIN, but only after TLS is active;
7. issue MAIL FROM, RCPT TO, DATA, the formatted message, and QUIT;
8. send TLS `close_notify`, close the descriptor, and emit the audit result.

Plain and TLS I/O are selected by local `io_read` and `io_write` helpers. The
TLS handshake and session are Hull's blocking `HlTlsClient` wrapper over
Keel's `KlTls` interface. The connection path directly assumes POSIX APIs and
an `int` descriptor.

The current test boundary is useful and must be retained:

- `tests/hull/cap/test_smtp.c` covers validation, authorization, reply parsing,
  Base64, and message formatting without a network;
- `tests/hull/cap/test_smtp_e2e.c` drives a real local SMTP peer, including
  plaintext, rejection, recipients, and message formatting;
- the TLS E2E cases cover STARTTLS and implicit TLS against real sockets;
- the Lua and JavaScript bindings deliberately expose the same synchronous
  `{ok,error}` result shape.

## 3. Keel v3 surface audit

Keel v3 provides the required lower-level machines:

- `KlConnectOp` owns a terminal-once, cancellation-safe Happy Eyeballs state
  machine;
- `KlStream` owns bounded queued writes, strict read pause/resume, cancellation,
  and confirmed close detachment across readiness and completion backends;
- `KlSocketProvider`, `KlResolver`, `KlEventCtx`, and `KlTls` provide portable
  socket, DNS, event, and TLS axes.

It does not expose a public, protocol-neutral object that combines those pieces
into a connected stream. Hull should not add one preemptively. SMTP is the
first non-HTTP consumer, while PostgreSQL and MySQL have different negotiation,
pooling, and lifetime requirements. A shared client facade before those uses
are implemented would freeze an abstraction based on one protocol.

Instead, an SMTP-owned `HlSmtpOp` composes the existing public Keel primitives
directly. It implements the `KlConnectOp` hooks, owns a `KlStream`, and binds
them to the configured resolver, event context, socket provider, timers, and
TLS session. This adapter remains private to Hull SMTP. It may use only public
Keel headers and opaque accessors.

The first design gate is a compile-and-lifecycle spike proving that this can be
done without an HTTP-private header. If one operation is unavailable publicly,
Keel should expose that operation as the narrowest reusable primitive or
accessor. It should not add a combined client facade for this transition.

## 4. Required primitive composition contract

The SMTP-owned adapter must establish these contracts from the existing Keel
primitives:

### 4.1 Open

- Inputs: event context, allocator, host, port, overall deadline, optional
  socket provider, resolver policy, connect-attempt delay, and callbacks.
- Resolution through the configured Keel resolver, with the sandbox-compatible
  system resolver selected by Hull unless a future manifest explicitly admits
  another resolver.
- IPv4 and IPv6 racing through `KlConnectOp`, not first-address-only behavior.
- A terminal callback exactly once with either the winning connected socket or
  a stable Keel error, followed by confirmed connect-operation detachment.
- Cancellation that does not return ownership until the connect operation and
  all racing descriptors and timers are detached.

### 4.2 Byte stream

- Ordered, bounded, all-or-none write admission using `KlStream`.
- Read delivery that handles fragmented and coalesced protocol records without
  assuming one callback equals one SMTP line.
- Backpressure notification so the SMTP machine resumes only after queued bytes
  have drained sufficiently.
- One overall operation deadline plus explicit per-stage rearming if Keel
  exposes it. Hull must not recreate unbounded `poll` loops around the stream.
- Readiness and completion backends, including epoll, kqueue, poll, IOCP, and
  Cosmopolitan's selected backend.

### 4.3 TLS upgrade

- Implicit TLS attachment before any application bytes are delivered.
- In-place STARTTLS upgrade only when the plaintext write queue is empty and no
  plaintext receive is outstanding or buffered beyond the accepted 220 reply.
- SNI and certificate/hostname verification from Hull's existing `KlTlsConfig`.
- No plaintext fallback after STARTTLS was requested or after TLS negotiation
  began.
- Best-effort TLS `close_notify` on graceful close; abortive close on timeout,
  cancellation, parser failure, or transport error.

### 4.4 Ownership

- Callback-safe cancellation, including cancellation from inside a callback.
- A documented point at which Hull may free the SMTP operation, message copy,
  credentials, TLS session, and allocator state.
- Secret buffers remain Hull-owned and are scrubbed after AUTH completes or the
  operation terminates. Keel must not log or retain SMTP application bytes.
- No direct access to `KlStream`, connect-operation, watcher, or socket-provider
  layouts. The opt-in layout headers (`connect_op_detail.h`, `stream_detail.h`)
  may be included solely to provide storage for an embedded opaque object (so
  `KlConnectOp` / `KlStream` can be a field of `HlSmtpOp` rather than a separate
  allocation); their struct fields must never be read or written. "Opaque
  accessors only" means the layout is used for sizing/embedding, never for field
  access.

The Hull spike must include a public-header compile gate and lifecycle tests
that open, write, read, upgrade TLS, cancel, and close without private Keel
headers. If it exposes a missing narrow Keel primitive, that primitive also
requires a Keel public-contract test.

## 5. Hull SMTP state machine

The implementation replaces the current linear function with an internal
operation object. Its protocol states are:

```text
CONNECTING
  -> TLS_IMPLICIT (optional)
  -> GREETING
  -> EHLO
  -> STARTTLS_COMMAND -> TLS_UPGRADE -> EHLO_AFTER_TLS (optional)
  -> AUTH (optional)
  -> MAIL_FROM
  -> RCPT_PRIMARY
  -> RCPT_CC zero or more
  -> DATA_COMMAND
  -> DATA_BODY
  -> DATA_RESULT
  -> QUIT
  -> CLOSING
  -> DONE | FAILED | CANCELLED
```

The reply parser is incremental. It must retain partial bytes across reads,
accept multiple complete lines in one read, bound both a line and a complete
multiline response, require one consistent three-digit code across continuation
lines, and reject malformed or over-limit replies.

Each state defines the only accepted reply class. The initial implementation
preserves today's exact behavior and error tokens. Any standards expansion,
including AUTH mechanisms other than PLAIN, PIPELINING, multiple primary
recipients, SMTPUTF8, DSN, or connection pooling, is out of scope.

## 6. Public API and scheduling decision

The first Hull slice preserves:

```c
int hl_cap_smtp_send(const HlSmtpConfig *, const HlSmtpMessage *,
                     const char **err_msg);
```

and the synchronous Lua and JavaScript `smtp.send` surfaces.

Internally, the function may drive a private Keel event context until the SMTP
operation reaches a terminal state. When called from a running Hull request,
it must not recursively drive the server's event context. The implementation
must therefore choose and test one of these explicit models:

1. a dedicated, operation-local client event context for the compatibility
   wrapper; or
2. a worker job that owns the client event context while the runtime suspends.

The recommended implementation is model 2. SMTP already performs blocking
network work, and the worker boundary prevents recursive event-loop entry and
stops a slow mail server from blocking unrelated requests. Lua and JavaScript
resume through their existing async completion seams, but retain their current
result shape.

A new public `smtp.async.send` API is not part of this transition. It requires
a separate Lua/JavaScript parity design covering promises/coroutines,
cancellation on request teardown, and concurrency limits.

## 7. Security invariants

The transition is acceptable only if all of these remain true:

- Host authorization occurs before DNS resolution or socket creation.
- Authorization remains against the declared hostname. Connected-address
  handling must preserve the existing exact/glob/CIDR policy semantics and
  cannot widen authority through redirects, aliases, or TLS names.
- CR and LF are rejected in every command/header field before transport use.
- Credentials are never sent unless TLS is active and verified under the
  configured policy.
- STARTTLS cannot be stripped by continuing in plaintext after a failed or
  malformed upgrade.
- TLS plaintext and ciphertext cannot cross the upgrade boundary or share an
  incorrectly retained parser buffer.
- Reply, message, write-queue, allocation, and operation deadlines are bounded.
- Audit output records metadata and the stable result only, never credentials
  or message bodies.
- Every failure and cancellation path closes or retires all sockets, timers,
  watchers, TLS objects, queues, and worker/runtime references exactly once.
- An SMTP-free or HTTP-free application retains its existing feature
  composition and zero-unwanted-subsystem link invariants.

## 8. Error compatibility

Existing Hull error tokens are public behavior and remain stable. Keel errors
are mapped at the capability boundary; they do not leak as backend-specific
strings. At minimum, the mapping preserves:

- `connect_failed` for resolution, connect, and connect-deadline failure;
- `tls_config_missing`, `tls_handshake_failed`, and `starttls_rejected`;
- the existing greeting, EHLO, AUTH, MAIL, RCPT, DATA, send, format, allocation,
  validation, and authorization tokens.

Cancellation and a total operation deadline need new internal distinctions for
tests and audit. Whether either becomes a new public token is an implementation
review decision; it must not be introduced accidentally by passing through a
Keel error string.

## 9. Implementation slices

### Slice 2a: public-primitives feasibility spike

- Build a Hull-local SMTP transport adapter from `KlConnectOp`, `KlStream`, the
  public resolver/event/timer/socket-provider APIs, and `KlTls`.
- Add no combined Keel client facade.
- Prove POSIX readiness, Cosmopolitan poll, and Windows completion behavior.
- Prove connect cancellation, deadline, partial I/O, backpressure, TLS upgrade,
  graceful close, and confirmed detachment.
- Keep the spike separate from the production SMTP path.

Stop for API and ownership review. If the adapter requires HTTP-private
headers, identify the exact missing primitive and expose only that narrow seam
in Keel before continuing.

### Slice 2b: Hull internal SMTP machine

- Introduce the internal SMTP operation and incremental reply parser.
- Preserve formatting, authorization, audit, TLS policy, result shape, and
  stable error mapping.
- Route it through the reviewed Hull-local primitive adapter.
- Delete direct socket, `fcntl`, `poll`, `getaddrinfo`, `read`, `write`, and
  descriptor-close logic from `cap/smtp.c`.

Stop for protocol/security review.

### Slice 2c: runtime scheduling and integration

- Move SMTP execution to the bounded worker/completion path.
- Prove Lua and JavaScript parity and runtime teardown cancellation.
- Preserve application-facing synchronous semantics unless a separately
  reviewed async API is approved.
- Reconfirm composable-feature and zero-unwanted-subsystem linkage.

Stop for full acceptance review.

PostgreSQL and MySQL are later independent consumers of Keel primitives, not
automatic consumers of the SMTP adapter. Common code is extracted only after
at least two real protocol implementations demonstrate the same contract.

## 10. Acceptance matrix

The final SMTP transition requires all existing tests plus these cases:

- IPv4, IPv6, first-address failure, delayed winning address, DNS failure, and
  connect deadline;
- fragmented greeting, coalesced multiline reply, inconsistent multiline code,
  oversized line/response, early EOF, reset, and timeout in every state;
- short writes, write backpressure, queue-full retry, and message larger than
  one transport write;
- plaintext delivery, STARTTLS, implicit TLS, hostname mismatch, unknown CA,
  handshake timeout, rejected STARTTLS, and no downgrade;
- AUTH withheld on plaintext, credentials scrubbed after success and each
  failure point;
- cancellation during resolve, racing connect, read, write, TLS handshake,
  DATA transfer, and graceful close;
- exactly-once completion and cleanup under synchronous callback reentrancy;
- Lua/JavaScript result parity, server responsiveness during a delayed SMTP
  peer, and teardown with an SMTP operation in flight;
- ASan, MSan, TSan, fd-leak, and failure-injection coverage;
- Linux, macOS, Windows, and Cosmopolitan APE execution;
- native/composed builds, all flavors, and proof that SMTP-free compute apps
  link no SMTP client transport, Keel event loop, or mbedTLS.

## 11. Decisions frozen by this record

- SMTP protocol and capability policy stay in Hull.
- Keel primitives own connect, stream, event, socket, and TLS lifecycle
  invariants; Hull SMTP owns their protocol-specific composition.
- Hull uses only public Keel headers and opaque accessors. The `*_detail.h`
  opt-in layout headers may be included for embedded-object storage only, never
  for field access (see section 4.4).
- The HTTP client is not repurposed for SMTP.
- No generic Keel client facade is introduced by this transition.
- The first transition preserves the current Lua/JavaScript API and error
  contract.
- SMTP transport work precedes PostgreSQL and MySQL adoption.
- External API expansion, SMTP feature expansion, and pooling are separate
  decisions.

## 12. Open review questions

1. Should the operation use one total deadline, independently bounded stage
   deadlines, or both? The recommendation is both, with the total deadline as
   the hard ceiling.
2. Is connection pooling explicitly forbidden for the first implementation or
   merely deferred? The recommendation is forbidden until credential and TLS
   identity partitioning are designed.
3. Should cancellation become a stable public SMTP error token? The
   recommendation is no for the compatibility surface; retain it as an
   internal/audit distinction until a public async API exists.
