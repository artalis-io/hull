# PostgreSQL client transport onto Keel v3 (Slice 3) - decisions record

Status: design record, frozen at Checkpoint 1. Branch
`feat/pg-keel-transport-slice3` (off `main`). This is the sibling of the SMTP
Slice 2c work ([`smtp_keel_slice2c_plan.md`](smtp_keel_slice2c_plan.md)); it
routes the PostgreSQL client's transport onto the same public Keel v3 primitives
while keeping the connection synchronous and blocking and the wire codec
untouched.

## Objective

Replace `src/hull/cap/pg_conn.c`'s hand-rolled POSIX socket layer
(`getaddrinfo` / `socket` / `connect` / `select` / `send` / `recv` / `close`)
with Keel v3 transport primitives, reusing the pattern proven by
`cap/smtp_transport.c`. The Postgres wire codec (framing, SCRAM auth, `$n`
rewrite, typed decode, migrations) and the connection's synchronous blocking
semantics do not change. The win is one transport stack (a `KlSocketProvider`),
RFC 8305 Happy-Eyeballs racing, and a provider test seam for the DB client.

## D1. Racing via `KlConnectOp`; blocking I/O via raw `KlSocketProvider` ops

Multi-address connection racing uses `KlConnectOp`. Socket creation, connect
completion, blocking read/write, and close use the raw provider ops
(`sp->ops->{socket,connect,close,send,recv,set_blocking,set_nonblocking,get_so_error}`,
all in `keel/socket.h`), plus best-effort `set_nosigpipe` where the platform
offers it. `set_cloexec` is deliberately NOT used: neither the current
`pg_conn.c` nor the SMTP transport sets close-on-exec (the kernel sandbox blocks
`exec` outright), so the transport does not require or call it and it is not part
of the required provider surface. `KlStream` is not used either: its queued-write,
watcher, and lifecycle machinery serve an asynchronous client, and a synchronous
PostgreSQL connection needs none of it.

`KlConnectOp` is asynchronous and needs an event loop to drive its attempts plus
the stagger and deadline timers. The transport therefore owns a private
`KlEventCtx` that is used ONLY during connect establishment. The transport pumps
that loop to a terminal state, receives the winning descriptor via
`kl_connect_op_on_attempt_connected`, and then calls `sp->ops->set_blocking(fd)`.
After that point the connection is pure synchronous blocking I/O with no event
loop: startup, SCRAM auth, the TLS handshake, and every query use plain blocking
`recv` / `send`.

File split: all provider and Keel interaction lives in a new
`src/hull/cap/pg_transport.c` (plus `include/hull/cap/pg_transport.h`).
`pg_conn.c` keeps only the wire protocol and calls the transport, mirroring the
`cap/smtp.c` vs `cap/smtp_transport.c` division (the codec translation unit
contains no `getaddrinfo` / `socket` / `poll` / raw I/O).

## D2. TLS stays `hl_tls_client_*` on the provider-created blocking descriptor

Keep `hl_tls_client_handshake` / `_read` / `_write` / `_free` / `_shutdown` and
hand it the blocking descriptor the provider created. This preserves the whole
existing `sslmode=disable|prefer|require|verify-ca|verify-full` behavior (chain
plus hostname verification, fail-closed with no plaintext fallback) with the
smallest security-sensitive diff. `KlTls` is deliberately not adopted: it would
couple a transport migration to TLS lifecycle and buffering changes for no gain
on a synchronous contract. The `hl_tls_client_*` helpers take the raw integer
fd, which under the POSIX provider is exactly the `KlSocketHandle` value the
transport holds.

## D3. Connect-timeout mapping bounds TCP establishment only

`timeout_ms` bounds TCP connection establishment only. It does not bound DNS, and
it does not bound the PostgreSQL startup or TLS handshake as a single global
operation (unchanged meaning). After resolution the transport computes one
absolute connect deadline and gives its remaining budget to `KlConnectOp`'s
`arm_deadline` hook (`keel/connect_op.h`). Happy-Eyeballs `arm_delay` stagger
timers must not refresh that deadline. When `timeout_ms <= 0` the transport arms
no deadline, preserving today's unbounded-connect behavior. On terminal failure
or timeout the transport calls `kl_connect_op_cancel` and pumps the private loop
until confirmed detachment before freeing storage, honoring the "no reuse until
confirmed detachment" invariant in `keel/connect_op_detail.h`. Detachment is
observed by POLLING `kl_connect_op_is_detached` in the teardown pump; the
transport installs no `on_detach` callback (the `KlConnectOpHooks.on_detach` slot
is left NULL) and keeps no `connect_detached` field. The teardown detach pump is
iteration-bounded and non-blocking, so a pathological op that will not detach is
declared promptly (see Amendment 4 / Checkpoint 2 review).

## DNS: no async resolver in this slice (recorded honestly)

Keel's async resolver is not used. It requires event-driven DNS authority that
Hull's sandbox deliberately avoids, and adopting it would be an async-model
change. This slice does not buy async-capable DNS.

- IP-literal hosts are parsed directly with `kl_sockaddr_parse` (Keel's
  documented numeric-literal, no-DNS parser). This matches today's
  IP-literal-only `databases.dynamic` CIDR gate.
- Hostnames are resolved through a sandbox-compatible blocking `getaddrinfo`
  adapter kept in `pg_transport.c` (never in `pg_conn.c`), exactly as
  `cap/smtp_transport.c` does. The adapter feeds results into `KlConnectOp` via
  `kl_connect_op_on_resolved(op, naddrs)` and `_on_resolve_failed`.
- Resolution returns at most `KL_CONNECT_MAX_ADDRS` (8) ordered addresses. Their
  storage is owned by the transport and remains valid through confirmed
  detachment of the connect operation.

## Amendment 1: transport lifetime and ownership

`PgTransport` lives for the entire PostgreSQL connection, not only during
connect. It is embedded in `HlPgConn`, or `HlPgConn` owns an opaque transport
allocation. It retains the borrowed provider reference (see Amendment 4) and
owns:

- the private connect-time event context,
- the `KlConnectOp` storage and its timers,
- the resolved addresses during connection racing,
- the winning descriptor,
- the optional attached `HlTlsClient`.

This yields one close path: TLS shutdown, then TLS free, then provider close,
performed exactly once in that order.

## Amendment 2: preserve the existing connected-fd test API

`hl_pg_conn_start(conn, fd, dsn)` and its socketpair tests are preserved through
an explicit transport-adopt operation. Adoption:

- takes ownership of the supplied descriptor exactly once,
- associates it with the default provider,
- skips resolution, connection racing, and event-context creation,
- routes subsequent I/O and close through the same transport API as an ordinary
  connection.

`hl_pg_conn_start`'s public contract is that it takes ownership of the fd and
closes it on every failure. To keep that intact, `hl_pg_transport_adopt` CONSUMES
the descriptor (closes it exactly once through the provider) on every failure
outcome where the provider can close it - i.e. an allocation failure after a valid
provider is resolved. The only non-consuming failures are an invalid fd (< 0) or
an invalid provider (no way to close the fd); with the default provider (what
`hl_pg_conn_start` uses) neither is reachable, so the fd is never leaked. Covered
by a deterministic test that forces the allocation failure and asserts the
descriptor is closed exactly once (`pg_transport_adopt.alloc_failure_consumes_fd_once`).

No second legacy raw-fd I/O path remains in `pg_conn.c`; the adopted descriptor
and a raced-and-won descriptor converge on one send / receive / close
implementation. The pre-TLS SSLRequest probe (`hl_pg_ssl_negotiate`) now also
rides the transport: it takes a `PgTransport *` and uses
`hl_pg_transport_send_all` / `hl_pg_transport_recv` (plaintext, before any TLS is
attached), so every production PG I/O path is behind the transport. The only raw
descriptor use left is handing `hl_pg_transport_fd` to `hl_tls_client_handshake`
(which takes an int fd) for the TLS handshake itself.

## sslmode matrix (verified)

The complete DSN `sslmode` policy is exercised end to end (`tests/e2e_postgres.sh`,
real Postgres 16 in Docker) plus the negotiation-decision unit matrix
(`pg_ssl.negotiation_matrix`):

- `disable` - plaintext, no SSLRequest sent.
- `require` against a TLS server - SSLRequest -> mbedTLS handshake -> SCRAM over
  TLS, asserted encrypted via `pg_stat_ssl`.
- `require` against a NON-TLS server - the server answers `N`; the connection
  HARD-FAILS with no silent plaintext downgrade.
- `verify-full` against an untrusted self-signed cert - chain verification against
  the embedded CA bundle REJECTS it (no connection).
- `prefer`/`require` server-`S`/`N` decisions - covered by the unit matrix.

`verify-full` SUCCESS against a private CA is not e2e-testable here because Hull's
PG TLS verify trusts only the embedded Mozilla bundle (a private PG cert is never
in it); the chain + hostname verification logic itself is the shared
`shared/tls_client.c`, covered by the live-mbedTLS-peer unit suite
(`test_smtp_tls`), which the PG path reuses unchanged.

## Amendment 3: blocking I/O contract

The transport's send and receive wrappers preserve the existing behavior exactly:
EINTR retry, partial-write completion, the existing error text, and SIGPIPE
suppression (via the provider's `set_nosigpipe` and the existing send flags).
Provider `send` / `recv` callbacks may return short operations; the full-write
loop semantics of `conn_send` remain. No new post-connect read or write timeout
is introduced.

## Amendment 4: precise provider wording

`PgTransport` does not own the provider itself. It retains a borrowed, immutable
`KlSocketProvider` reference whose lifetime exceeds the connection (the default
provider, or a test-supplied one). It owns every descriptor created through that
provider and disposes every winner, loser, and failure-path descriptor through
the same provider (`sp->ops->close`), never a bare `close(2)`.

## Frozen ownership rules

1. `PgTransport` retains the borrowed immutable `KlSocketProvider` reference and
   owns the private `KlEventCtx`, the `KlConnectOp` (embedded via
   `connect_op_detail.h`), the resolved address set, the winning descriptor, and
   the optional attached `HlTlsClient`. It lives for the whole connection.
2. Resolver results outlive every racing attempt and remain valid through
   confirmed detachment; at most `KL_CONNECT_MAX_ADDRS` ordered addresses.
3. Every losing descriptor is disposed through the provider, never a bare
   `close(2)`.
4. A failed or timed-out connect reaches confirmed detachment before any
   transport storage is freed.
5. The provider test seam (`pg_test_socket_provider`) and any
   attempt-observation or pump hooks compile out of production (nm-verified,
   gated by `-DHL_PG_TEST_HOOKS`, mirroring SMTP's `-DHL_SMTP_TEST_HOOKS`).
6. No PostgreSQL or Keel transport objects enter a non-Postgres app.
   `cap_pg_transport.o` joins `libhull_feature-postgres.a` (today
   `cap_db_postgres.o` plus `cap_pg_conn.o` plus `cap_pgwire.o`,
   `mk/features/postgres.mk`); a base or non-Postgres app links zero of it.

## Non-goals

No codec change. No async-model change: the connection stays synchronous and
blocking, and `db.async` (worker-pool connections) is unchanged. No new
app-facing manifest or sandbox authority beyond the existing `network_outbound`
grant for a declared network DB. Identical rows, errors, and timeouts.

## Grounding (verified symbols)

- `keel/socket.h`: provider ops including `set_blocking`, `set_nosigpipe`, and
  `get_so_error`.
- `keel/connect_op.h`: `kl_connect_op_init` / `_start` / `_cancel` plus
  `_on_resolved` / `_on_attempt_connected` / `_on_attempt_failed` / `_on_delay` /
  `_on_deadline`; terminal-once with detach-gated reuse; `KL_CONNECT_MAX_ADDRS`
  is 8.
- `keel/sockaddr.h`: `kl_sockaddr_parse` (numeric literal, no DNS).
- `hull/shared/tls_client.h`: `HlTlsClient` and the raw-fd `hl_tls_client_*`
  helpers.
- Template: `src/hull/cap/smtp_transport.c` (blocking `getaddrinfo` adapter, a
  private `KlEventCtx` connect pump, confirmed detachment). PG diverges on the
  last point: it confirms detachment by polling `kl_connect_op_is_detached` in the
  teardown pump rather than installing the template's `on_detach` callback.

## Checkpoint 2 review refinements

Source review of the first Checkpoint 2 cut froze six additional correctness
rules, now implemented:

1. **Truly unbounded connect.** `timeout_ms <= 0` imposes NO total deadline and no
   hidden ceiling: the connect pump loops in bounded `PG_PUMP_STEP_MS` increments
   until the op completes (the `select(..., NULL)` contract). Only `timeout_ms > 0`
   arms a deadline.
2. **Provider error classification via `io_status`.** Connect / send / recv
   classify would-block / pending / interrupted through the provider's
   `ops->io_status` when it supplies one, falling back to hosted errno only when it
   is absent. A mock that reports `io_status` while leaving errno irrelevant is
   covered.
3. **Required provider subset + capability, validated up front.** Before creating
   anything, `validate_provider` requires
   `socket`/`connect`/`close`/`send`/`recv`/`get_so_error`/`set_nonblocking`/`set_blocking`
   and `KL_SOCK_CAP_NATIVE_FD` (the private event loop watches provider handles);
   a missing op or capability fails closed. `set_blocking` is never assumed
   present (a missing one no longer silently leaves the winner non-blocking).
4. **Non-detachment is represented safely.** `connect_teardown` and
   `hl_pg_transport_close` return status. When confirmed detachment fails the
   ENTIRE heap allocation is PRESERVED (never freed under a live op) and reported:
   `close` returns -1 without marking anything closed, so the owner may retry or
   intentionally leak the whole block. This is why the transport is a
   heap-allocated opaque handle (Amendment 1), not an embedded value.
5. **One-shot fallible TLS attach.** `hl_pg_transport_attach_tls` requires a live
   descriptor and a non-NULL session and rejects a second attachment, returning
   status; an owned session is never silently dropped.
6. **Tests.** The suite adds deterministic coverage for the connect deadline +
   detachment, truly-unbounded mode via a controlled completion (no 30 s wait),
   immediate `connect()` success (rc == 0), `io_status` with an irrelevant errno,
   missing provider ops + missing native-FD capability, `set_blocking` failure,
   partial receive, provider-observed close exactly once, TLS attach
   rejection/replacement, and forced non-detachment proving the allocation is
   preserved and close is retryable. Run under ASan/UBSan + TSan locally and
   ASan/LSan in CI; the production-hook nm proof is retained.

## Checkpoints

1. Decisions record (this document). Approved.
2. Transport plus test seam only, with `pg_conn.c` codec behavior untouched.
3. Cut `hl_pg_conn_open` / `_start` / `_close` over to the transport (non-TLS);
   `test_pg_conn` and non-TLS `e2e_postgres` green.
4. The `sslmode` matrix over the new transport; full `e2e_postgres` green.
5. Fresh full matrix on the exact head, then squash-merge on approval.

Slice 4 repeats this against `cap/mysql_conn.c` (identical raw-socket shape),
reusing the Slice 3 transport, as a separate PR under the same checkpoint
discipline.
