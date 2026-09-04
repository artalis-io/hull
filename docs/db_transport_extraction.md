# Shared DB blocking-transport extraction (HlDbTransport) - decisions record

Slices 3 (#452) and 4 (#453) landed the PostgreSQL and MySQL/MariaDB client
transports on Keel v3 as two files, `cap/pg_transport.{c,h}` and
`cap/mysql_transport.{c,h}`. Slice 4 checkpoint 2 proved they are byte-identical
modulo a `pg -> my` rename (the normalized diff was empty). This slice extracts
the single proven contract into one shared unit and deletes both copies. It is a
behavior-neutral refactor: no wire codec, auth, TLS negotiation, error-token, or
scheduling change.

## D1. Full merge into one `HlDbTransport` - no shims

One `cap/db_transport.{c,h}` defines `HlDbTransport` +
`hl_db_transport_{connect,adopt,attach_tls,send,recv,send_all,fd,close}` and the
`HL_DB_TRANSPORT_TEST_HOOKS` provider seam. `cap/pg_transport.{c,h}` and
`cap/mysql_transport.{c,h}` are deleted in the same atomic cutover; there are no
`PgTransport`/`MyTransport` typedefs or forwarding shims. Both are already
identical, so a shim layer would be permanent forwarding boilerplate with no
ownership or behavioral boundary.

`pg_conn.c` / `mysql_conn.c` hold an `HlDbTransport *` and call
`hl_db_transport_*` directly; `hl_pg_ssl_negotiate` takes `HlDbTransport *`.

## D2. Diagnostic backend label - first parameter, logging only

The only backend-specific content in the two transports was two `log_error`
prefixes (`"pg: ..."` vs `"mysql: ..."`) on the rare connect-op non-detachment
paths. These are diagnostics, never public error tokens (those are set by
`pg_conn.c` / `mysql_conn.c` and are unchanged). The merged transport carries a
`const char *tag` label used ONLY in those `log_error` calls.

`tag` is the FIRST parameter of `hl_db_transport_connect` / `_adopt`
(`hl_db_transport_connect("pg", host, port, timeout_ms, sp, err, errlen)`). First
position is deliberate: it makes caller-site insertion a single reliable textual
substitution rather than a positional edit into each call's variable argument
tail, and it reads like a labeled constructor. The label never affects behavior.

## D3. Composition - both feature archives, pull-by-symbol (no new archive)

The `postgres`/`mysql` feature archives are pull-by-symbol, NOT whole-archived
(only `tui` sets `whole_archive`; the DB backends are reached through the
generated collector's reference to `hl_db_backend_postgres` / `_mysql`). Under
pull-by-symbol linking a member present in both archives is extracted exactly
once - once `hl_db_transport_*` is defined, the second archive's copy is never
pulled, even inside the `base_group` `--start-group`. So `cap_db_transport.o` is
placed in BOTH `libhull_feature-postgres.a` and `libhull_feature-mysql.a`; no new
archive, no embedding, no release-pipeline or `hull feature install` change. The
existing composed-feature signature attestation covers it (each archive's hash
just changes).

Monolithic builds compile it once into the base, gated on
`HL_ENABLE_POSTGRES` OR `HL_ENABLE_MYSQL`; a neither-build drops it entirely.

Acceptance (verified in the link matrix): PostgreSQL-only and MySQL-only links;
both features linked in both archive orders; exactly one resolved
`hl_db_transport_*` implementation in a combined binary; a neither-build contains
zero `hl_db_transport_*` symbols and no new Keel references; `cap_db_transport.o`
is a byte-identical build in both archives; reproducibility and feature-install
tests stay green.

## D4. Tests - retarget both provider-seam suites, keep both

`test_pg_transport.c` and `test_mysql_transport.c` are retargeted to the shared
API + shared `HL_DB_TRANSPORT_TEST_HOOKS` seam and RETAINED as two independent
parity guards. `test_pg_conn.c` gets the mechanical transport-type rename where
its `negotiate_with` helper constructs a transport (`hl_pg_transport_adopt` ->
`hl_db_transport_adopt`, `PgTransport` -> `HlDbTransport`); its test cases and
coverage are unchanged. `test_mysql_conn.c` drives `hl_my_conn_start` only and
does not touch the transport type, so it is unchanged. The PG/MySQL connection,
codec-only (`test_mysqlwire` under `HL_MY_NO_TLS`), fuzzer, and e2e suites keep
their coverage and Keel-free guards.

## Non-goals

No change to `pg_conn.c` / `mysql_conn.c` wire behavior, auth, TLS negotiation,
dialect, or async model beyond the mechanical transport-type/function rename plus
the `"pg"` / `"mysql"` tag argument. No shared abstraction beyond the byte
transport (the SMTP transport uses a different model - `KlStream` + a post-connect
event loop - and is out of scope). No new `--with` feature or install concept.
