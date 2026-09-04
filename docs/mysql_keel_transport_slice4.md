# MySQL/MariaDB client transport onto Keel v3 (Slice 4) - decisions record

Status: design record, frozen at Checkpoint 1. Branch
`feat/mysql-keel-transport-slice4` (off `main`). Slice 4 follows the proven
PostgreSQL cadence ([`pg_keel_transport_slice3.md`](pg_keel_transport_slice3.md)):
route the MySQL/MariaDB client's transport onto the same Keel v3 primitives, keep
the connection synchronous + blocking, and leave the wire codec untouched.

## D1. Reuse the PG transport PATTERN - as an independent MySQL copy

`cap/pg_transport.c` is already backend-neutral: it holds zero PG-specific code -
it is a generic blocking byte transport (`KlConnectOp` resolve + Happy-Eyeballs
racing on a private operation-local `KlEventCtx`, then `set_blocking()` the winner;
raw `KlSocketProvider` I/O; heap-opaque handle; `adopt` consumes-fd-on-failure;
`io_status` classification; provider validation + `KL_SOCK_CAP_NATIVE_FD`;
truly-unbounded connect; one-shot fallible TLS attach; fallible teardown/close with
safe non-detachment preservation; a `-DHL_PG_TEST_HOOKS` provider seam).

**Decision:** build a NEW, INDEPENDENT `cap/mysql_transport.{c,h}` (`MyTransport`)
for this slice rather than sharing `PgTransport`. It will be essentially a copy of
the PG transport with `pg`->`my` / `Pg`->`My` renames and a `-DHL_MY_TEST_HOOKS`
seam. This is acknowledged, intentional, temporary near-verbatim duplication
(~840 lines): it keeps the PostgreSQL implementation untouched and lets a later,
SEPARATE extraction be based on two proven concrete implementations.

**Deferred follow-up (explicitly NOT this slice):** once both backends are green,
reconcile the duplication - rename `PgTransport` to a neutral shared blocking
transport both backends own, or dedupe - based on the two proven implementations.
No shared abstraction is introduced in Slice 4.

## D2. TLS ownership - identical to PG

TLS stays the shared `hl_tls_client_*` layered over the provider-created blocking
descriptor and ATTACHED to the transport (`hl_my_transport_attach_tls`), so
post-upgrade bytes tunnel through it. The transport runs no handshake;
`mysql_conn.c` owns the negotiation and hands the session over. The only raw-fd
use is `hl_my_transport_fd` handed to `hl_tls_client_handshake(int fd, ...)`.

## D3. Timeout semantics - identical to PG

`timeout_ms` bounds TCP establishment ONLY (deadline armed after resolution;
Happy-Eyeballs stagger never refreshes it; `<= 0` is truly unbounded, no hidden
ceiling). DNS: IP literals via `kl_sockaddr_parse` (no DNS), hostnames via a
blocking `getaddrinfo` adapter kept in `mysql_transport.c`; at most
`KL_CONNECT_MAX_ADDRS` ordered addresses.

## D4. The MySQL-specific difference: interleaved TLS

MySQL has NO separate `hl_pg_ssl_negotiate`-style pre-handshake probe. The TLS
upgrade is inline in the connect/handshake path: read the server handshake (which
advertises `CLIENT_SSL`) -> if `sslmode != disable && server_ssl`, send the
SSLRequest PACKET (`hl_my_build_ssl_request`, seq `f.seq + 1`) via `conn_send` ->
`hl_tls_client_handshake` -> send the credentialed `HandshakeResponse41` over TLS
(seq `f.seq + 2`); `else if sslmode >= require` -> fail (no downgrade).

The credentialed response after the TLS upgrade is sent through the SAME
`conn_send`. So once `conn_send` rides the transport, that response cannot work
unless the completed `HlTlsClient` has already been attached to the transport.
This makes the TLS wiring INTRINSIC to the cutover, not deferrable:

- **Checkpoint 3** performs the mechanical `hl_my_transport_fd` -> handshake ->
  `hl_my_transport_attach_tls` wiring as part of the cutover, requiring only
  PLAINTEXT ACCEPTANCE (plaintext MySQL/MariaDB green; TLS wired but not yet
  security-verified).
- **Checkpoint 4** is the focused SECURITY verification of that already-wired TLS
  path: the full sslmode matrix - no plaintext downgrade (`require` vs a server
  without `CLIENT_SSL` fails closed), verification failure (`verify` vs an
  untrusted cert rejects), and encrypted authentication + query evidence
  (e.g. `SHOW STATUS LIKE 'Ssl_cipher'` / `Ssl_version`). `verify` SUCCESS against a
  private CA is not e2e-testable (Hull's TLS verify trusts only the embedded
  Mozilla bundle); the chain/hostname verifier is the shared `shared/tls_client.c`,
  already covered by the live-mbedTLS-peer SMTP unit suite that this path reuses
  unchanged.

## D5. Build split - Amendment-2 shape, MySQL specifics

The Keel/transport/TLS-dependent connection layer is gated so the pure-parser
harnesses stay Keel-free. The exact split (corrected at review):

- **`test_mysql_conn`** DROPS `-DHL_MY_NO_TLS`: it now drives `hl_my_conn_start`'s
  adopt path through the transport, so it relinks with `mysql_transport.c` + Keel +
  `tls_client` + mbedTLS, becomes `HL_ENABLE_MYSQL`-gated, and joins a standing
  `mysql-transport-sanitize` CI job (ASan/LSan, `db-mysql` group, registered in
  `classify_changes.py` + `job_plan.py` + `ci-success.needs`) so it is not orphaned
  from the base `make test`.
- **`test_mysqlwire`** KEEPS `-DHL_MY_NO_TLS`: it tests the wire codec + DSN/auth
  helpers and does NOT exercise the connection I/O, so it needs no transport and
  stays Keel-free.
- **`fuzz_mysqlwire`** compiles ONLY `mysqlwire.c` (never `mysql_conn.c`) -
  unaffected.
- **`fuzz_mysql_dsn`** stays Keel-free through `-DHL_MY_NO_AUTH` (which omits the
  whole auth/connection layer), NOT `HL_MY_NO_TLS`.

Consequence for the checkpoint-3 `#ifdef` restructure: the transport-dependent
connection I/O + handshake must be omitted when EITHER `HL_MY_NO_AUTH` (fuzz DSN)
OR `HL_MY_NO_TLS` (test_mysqlwire) is set, while the pure auth helpers stay under
`HL_MY_NO_AUTH` only (present for test_mysqlwire, absent for fuzz_mysql_dsn). The
DSN parser + wire helpers stay outside both.

## D6. Checkpoint 4 verification split - tests only, no production change

The TLS security branches all exist and were approved at checkpoint 3
(`my_start_over_transport`: `sslmode` parse-reject, `verify` flag on
`HL_MY_SSL_VERIFY`, the fail-closed `"server does not support TLS but sslmode
requires it"` branch). Checkpoint 4 VERIFIES them; it adds no production logic.
Coverage mirrors PG across both levels but places each property where it is
provable most cheaply:

- **No-downgrade + parse-reject: unit level** (`test_mysql_conn`, now real-TLS
  linked). The default socketpair handshake advertises no `CLIENT_SSL`, so
  `sslmode=require` / `verify-full` deterministically hit the fail-closed branch,
  and `sslmode=bogus` is rejected before any credential reaches the wire. This is
  a stronger, deterministic proof than an e2e phase and needs no server.
- **Verification-failure + encrypted-auth/query evidence: e2e** (`e2e_mysql`,
  mysql:8 TLS leg). `sslmode=require` succeeds (encrypted, `caching_sha2` full
  auth, non-empty `Ssl_cipher`); `sslmode=verify-ca` / `verify-full` must REJECT
  MySQL 8's self-signed auto-cert (not in the embedded Mozilla bundle) via the
  ported `assert_connect_refused` helper.

PG's e2e no-downgrade phase was free because its container starts TLS-less;
MySQL 8 ships TLS-on, so an e2e no-downgrade would need a second `--skip-ssl`
container. The unit-level no-downgrade covers that property deterministically, so
the fragile second container is deliberately not added. `verify-*` SUCCESS against
a private CA stays untestable here (Hull's mysql TLS verify trusts only the
embedded bundle); the chain + hostname logic is covered by the shared
`tls_client` live-peer unit suite.

## Non-goals

No wire-codec change (framing, `mysql_native_password` SHA-1 +
`caching_sha2_password` SHA-256 auth, the binary prepared-statement protocol,
dialect DDL). No async-model change (stays synchronous blocking; `db.async`
unchanged). No new app-facing authority. Every public error token preserved
byte-for-byte, including the MySQL-specific `"could not connect to %s:%s"` (NOT
"cannot connect"), plus `"unknown sslmode: %s"`, `"failed to send SSLRequest"`,
`"server does not support TLS but sslmode requires it"`,
`"TLS handshake with %s failed"`. No shared PG/MySQL abstraction extraction.

## Checkpoints

1. This decisions record. Approved.
2. `cap/mysql_transport.{c,h}` + provider test seam + unit tests, wire codec
   untouched.
3. Cut `hl_my_conn_open`/`_start`/`_close` + I/O onto the transport, INCLUDING the
   `hl_my_transport_fd` -> handshake -> `attach_tls` wiring; plaintext
   MySQL/MariaDB green (`test_mysql_conn` + non-TLS `e2e_mysql`).
4. Focused security verification of the wired TLS path: full sslmode / fail-closed
   matrix + encrypted auth/query evidence.
5. Sanitizer + link/flavor + e2e + fresh exact-head CI; stop for review before
   the squash-merge.

The shared-transport extraction is a separate follow-up after both PostgreSQL and
MySQL are proven.
