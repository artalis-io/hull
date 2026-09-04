# Valkey/Redis client transport onto Keel v3 (Slice 5) - decisions record

The Valkey/Redis KV backend (`cap/valkey_conn.c`, the `--with=valkey` feature)
is the last first-party client still dialing raw Berkeley sockets: `getaddrinfo`
+ `socket` + non-blocking `connect` + `select` + `getsockopt(SO_ERROR)` +
`setsockopt(SO_RCVTIMEO/SNDTIMEO)` + raw `send`/`recv`. SMTP, PostgreSQL, and
MySQL are already on Keel; the byte transport is now the SHARED, protocol-agnostic
`cap/db_transport.c` (`HlDbTransport`, docs/db_transport_extraction.md). This slice
routes Valkey onto that same shared transport - NOT a new backend-specific copy.

Behavior-neutral: no RESP codec, DSN, AUTH/HELLO handshake, SELECT-db, or KV
backend change beyond the transport swap and the tokens below.

## D1. Route onto the shared HlDbTransport directly

`HlValkeyConn` drops `int fd` + `HlTlsClient *tls` for a single
`HlDbTransport *transport`. The call-site cutover mirrors pg_conn.c / mysql_conn.c:

- `vk_connect(host, port, timeout)` (raw socket) -> `hl_db_transport_connect("valkey", alloc, host, port, timeout, NULL, errbuf, errlen)` (see D6 for the `alloc` parameter; the trailing `NULL` is the default socket provider).
- `hl_valkey_conn_start(out, fd, dsn, ...)` (the adopt / test path) -> `hl_db_transport_adopt("valkey", alloc, fd, NULL, ...)`.
- `io_send` / `io_recv` -> `hl_db_transport_send` / `_recv`; `conn_send` / `conn_fill` unchanged above the io layer.
- `hl_valkey_conn_close` retires the transport via `hl_db_transport_close`.

`conn_new` takes the transport instead of an fd. The RESP writer/parser, the reply
arena, `resp3`, and every command path are untouched.

## D2. TLS - implicit (rediss), simpler than MySQL

Valkey TLS is IMPLICIT: for a `rediss://` / `valkeys://` DSN the client runs the
TLS handshake immediately after connect, before any RESP byte (no SSLRequest
probe, unlike MySQL's interleaved path). The wiring is the mechanical
`hl_db_transport_fd` -> `hl_tls_client_handshake` -> `hl_db_transport_attach_tls`
(then every RESP byte tunnels the attached session), gated `#ifndef
HL_VALKEY_NO_TLS`. The public token `"TLS handshake to %s failed"` is preserved.

## D3. Per-operation I/O timeout - preserve via an additive method (open-path only)

`vk_connect` sets `SO_RCVTIMEO` / `SO_SNDTIMEO` on the connected fd so a hung/slow
server cannot stall the (event-loop-thread) blocking recv/send forever.
`HlDbTransport` has NO per-op I/O timeout - pg_conn.c and mysql_conn.c dropped
theirs on migration. A direct swap would silently REMOVE Valkey's read/write
timeout, so it is preserved via an additive shared-transport method:

    void hl_db_transport_set_io_timeout(HlDbTransport *t, int ms);

Contract (reproduces the retired `setsockopt` exactly):
- **Best-effort, native-FD.** Sets `SO_RCVTIMEO` AND `SO_SNDTIMEO` on the winning
  descriptor; a setsockopt failure is IGNORED (the old `vk_connect` ignored them
  too). One setsockopt pair covers both the plaintext provider recv/send and the
  TLS `hl_tls_client_read/write`, which run on the same fd.
- **`ms <= 0` is a no-op** (no timeout installed), matching `vk_connect`'s
  `if (timeout_ms > 0)` guard and the transport's unbounded convention.
- **Intentional native-FD escape**, documented in the header: it `setsockopt`s the
  raw descriptor directly rather than through a provider op. This is sound because
  `HlDbTransport` already REQUIRES `KL_SOCK_CAP_NATIVE_FD` (validated in
  connect/adopt), so the winning handle is always a real POSIX fd.

**Applied ONLY on the connect path.** Valkey calls it from `hl_valkey_conn_open`
right after `hl_db_transport_connect`, with `dsn->connect_timeout_ms`, exactly
reproducing today's `vk_connect`. It is NOT called from `hl_valkey_conn_start`
(the adopt path): today `_start` adopts a caller-provided socket and sets no
timeouts, so calling it there would be a behavior change. pg/mysql never call the
method, so they are byte-for-byte unchanged (opt-in; the object re-hashes across
all three feature archives, automatic + covered by the composed-sig).

Deterministic tests (no live server): a socketpair-backed transport asserts (via
`getsockopt` readback) that BOTH `SO_RCVTIMEO` and `SO_SNDTIMEO` are installed
with the requested value, that `ms <= 0` installs neither, and that a `recv`
against a peer that never writes EXPIRES (returns the timeout error) within the
bound rather than blocking forever.

## D4. Build gating - mirror MySQL's HL_MY_NO_TLS split

`HL_VALKEY_NO_TLS` already exists but today gates only the TLS members; the raw
`vk_connect`/`io_send`/`io_recv` compile unconditionally (Keel-free). After the
cutover the whole transport-backed connection layer (connect/adopt, transport
I/O, TLS attach, the io-timeout method call) needs Keel, so it moves ENTIRELY
under `#ifndef HL_VALKEY_NO_TLS`, exactly like MySQL's connection layer under
`HL_MY_NO_TLS`. The DSN parser + RESP codec stay outside the guard (Keel-free).

- `test_valkey_dsn` and `fuzz_valkey_dsn`: KEEP `-DHL_VALKEY_NO_TLS` (DSN-only,
  no transport, no Keel).
- `test_respwire` / `fuzz_respwire`: unchanged (codec only, never compiled
  valkey_conn.c).
- `test_valkey_conn` and `test_valkey_backend`: DROP `-DHL_VALKEY_NO_TLS` and
  link Keel + mbedTLS + `db_transport.c` (they drive `hl_valkey_conn_start`'s
  adopt path over a socketpair, which now rides the transport - same change
  test_mysql_conn took). They join a sanitizer CI job (ASan/LSan) like
  pg/mysql_transport. The socketpair drives plaintext, so mbedTLS is linked but
  not exercised.
- `libhull_feature-valkey.a` adds `cap_db_transport.o` (its third pull-by-symbol
  carrier, alongside postgres/mysql). Because valkey fills a DIFFERENT base hook
  (`hl_kv_feature_backends`, not `hl_db_feature_backends`), `--with=valkey
  --with=postgres` is a valid combo; `tests/e2e_feature_db_shared.sh` is extended
  to compose valkey alongside a SQL backend and assert the shared transport is
  still resolved exactly once. Monolithic `HL_ENABLE_VALKEY=1` compiles
  `db_transport.c` once (the Makefile gate becomes
  `HL_ENABLE_POSTGRES || HL_ENABLE_MYSQL || HL_ENABLE_VALKEY`).

## D5. Public tokens - preserved, and one retired

Preserved verbatim: `"connect to %s:%s failed"` (Valkey-specific; NOT the pg/mysql
"could not connect to"), `"TLS handshake to %s failed"`, `"out of memory"`, and
every RESP/AUTH error message. No new authority; the KV sandbox
`network_outbound` grant is unchanged.

RETIRED: `"TLS not available in this build"`. Today it lives in
`hl_valkey_conn_open`'s `#else` (the `HL_VALKEY_NO_TLS` + `dsn->tls` path). Under
D4 the ENTIRE connection layer (`_open` / `_start`) moves under
`#ifndef HL_VALKEY_NO_TLS`, so that `#else` branch is deleted, not kept. This is
not a token regression: `HL_VALKEY_NO_TLS` is a parser/test-harness-only define
(no shipped Valkey build sets it), so in every shipped build TLS is always
compiled and the branch was already unreachable. A raw-socket fallback is NOT
retained merely to keep the token. This matches the MySQL precedent (checkpoint 3
removed MySQL's dead `"TLS not available in this build"` when its connection layer
moved wholly under `HL_MY_NO_TLS`).

## D6. Allocator contract - shared constructors take an optional HlAllocator

`include/hull/cap/valkey_conn.h:50` is a PUBLIC embedding promise: ALL connection
memory (the handle, the receive buffer, the reply arena) comes from the
caller-supplied `HlAllocator`, so an embedder's allocator + limits apply. Today
Valkey's "transport" is a bare `int fd` (no allocation). `HlDbTransport`, however,
is a heap block allocated with libc `calloc`/`free`. Routing Valkey through it
as-is would allocate the connection's transport OUTSIDE the embedder's allocator,
violating that contract.

Fix: the shared transport constructors gain an optional allocator:

    HlDbTransport *hl_db_transport_connect(const char *tag, HlAllocator *alloc, ...);
    HlDbTransport *hl_db_transport_adopt  (const char *tag, HlAllocator *alloc, int fd, ...);

- `alloc == NULL` -> libc `calloc`/`free` (PostgreSQL and MySQL pass NULL; they are
  byte-for-byte unchanged). Valkey passes its `HlAllocator`.
- The transport RETAINS the allocator pointer in `HlDbTransport` and uses it for
  EVERY allocation and free of the transport block, including the failure and the
  intentional non-detachment leak paths (a leaked block stays accounted to the
  embedder's allocator, correctly, since a live op still references it). The
  retained allocator therefore must outlive the transport - Valkey's connection
  allocator already does (it owns the handle).
- The private connect-time `KlEventCtx`'s `KlAllocator` is derived from the same
  `HlAllocator` when one is supplied (a thin `KlAllocator` shim delegating to it),
  falling back to `kl_allocator_default()` when NULL, so connect-time scratch also
  honors the embedder's allocator. NULL (pg/mysql) keeps `kl_allocator_default()`
  exactly as today.

`tag` stays the FIRST parameter; `alloc` follows it (both are transport-identity /
policy, ahead of the connection arguments), keeping caller-site insertion a simple
prefix edit for the pg/mysql call sites (they gain a `NULL` after the tag).

## Non-goals

No shared KV/SQL abstraction beyond the byte transport. No RESP / DSN / AUTH /
SELECT / KV-op change. No async model change (Valkey stays synchronous on the
calling thread). The SMTP transport keeps its own model (`KlStream` + post-connect
loop) and is out of scope.

## Checkpoints

1. This decisions record. STOP for review.
2. Shared-transport additions FIRST (behavior-neutral for pg/mysql): the optional
   `HlAllocator` parameter on connect/adopt (D6) + `hl_db_transport_set_io_timeout`
   (D3), with pg/mysql call sites gaining a `NULL` alloc arg and their suites
   re-run to prove no change; add the deterministic io-timeout + allocator tests.
   THEN cut `valkey_conn.c` onto `HlDbTransport` (struct + connect/adopt/io/TLS +
   the open-path io-timeout + its allocator) + the `HL_VALKEY_NO_TLS` whole-layer
   gate (retiring the dead TLS-unavailable branch, D5) + feature-archive +
   test/fuzzer gating; retarget `test_valkey_conn` / `test_valkey_backend`; local
   link matrix + tests. STOP for source review.
3. TLS/security verification (rediss e2e, io-timeout expiry evidence), the extended
   shared-transport compose (valkey + a SQL backend, one resolved impl), full
   exact-head CI, PR. STOP for merge approval.
