# PostgreSQL Backend Design (roadmap §1)

Status: **design approved, implementation not started** (2026-07-13).

Hull ships an embedded SQLite backend behind the `HlDbBackend` vtable
(`include/hull/cap/db_backend.h`). This document designs a second,
optional PostgreSQL backend so an app can target a networked Postgres
for multi-instance / cloud deployments where state cannot live in a
single-node file. The vtable was built with this in mind (its comments
already cite Postgres dialect forms and `open()` already takes a DSN),
so the abstraction is done; the work is a new backend plus the
network / TLS / untrusted-input concerns SQLite never had.

## Principles

1. **Optional and independent.** SQLite and Postgres are separate
   compile flags. A build can enable SQLite only (today's default),
   Postgres only, both, or neither.
2. **Reuse the existing vtable.** No new abstraction. The Postgres
   backend is one more `const HlDbBackend` instance.
3. **Pure-C, Hull-owned wire client.** No vendored libpq (rationale
   below). The Postgres v3 wire protocol is implemented in
   `src/hull/cap/`, compiled under Hull's `-Werror`/`-fsanitize`
   hardening, and fully auditable.
4. **c-audit throughout.** const vtable in `.rodata`, capability
   boundary, credential zeroing, a bounds-checked parser for untrusted
   server input, and parameterized-only queries.

## Why a pure-C wire client, not libpq

libpq is battle-tested but a poor fit for Hull: it is a large vendored
library with its own build system, it depends on OpenSSL (which
conflicts with Hull's mbedTLS-only TLS stack), it compiles under
relaxed `-w` warnings, and it fights Hull's all-vendored / reproducible
/ no-external-dependency model. The Postgres v3 frontend/backend
protocol is well-documented and the subset Hull needs is small
(~2-3k LOC). Hull already has every crypto primitive the protocol's
auth needs (SHA-256, HMAC-SHA-256, PBKDF2 in `cap/crypto`), and mbedTLS
for the TLS handshake. Owning the code keeps it inside Hull's warning /
sanitizer / fuzz regime, which is exactly where the one real risk (a
parser over untrusted network bytes) belongs.

## Flag matrix

Mirrors the proven `HL_ENABLE_HTTP` server/client split (see the
Makefile HTTP-flag reference block). Two granular flags plus an umbrella:

```
HL_ENABLE_SQLITE   ?= 1     # the current embedded backend (default on)
HL_ENABLE_POSTGRES ?= 0     # new, opt-in
```

`HL_ENABLE_DB` becomes a **derived umbrella**, `-D`-defined iff either
granular flag is on:

```
HL_ENABLE_DB = 1  when (HL_ENABLE_SQLITE=1 OR HL_ENABLE_POSTGRES=1)
```

Every existing `#if defined(HL_ENABLE_DB)` guard keeps its current
meaning ("any DB backend is present") unchanged. `cap/db.c` (the
runtime-agnostic query surface), `worker_db.c`, `migrate.c`,
`mod_db.c`, and the `HL_MOD_CAP_DB` module gate all stay keyed to the
umbrella. Only backend-specific translation units are keyed to a
granular flag:

| Flag | Gates |
|------|-------|
| `HL_ENABLE_SQLITE` | `vendor/sqlite/sqlite3.o`, `cap/db_sqlite.c`, `cap/db_udf.c` (UDFs are SQLite-only), the FTS5 define |
| `HL_ENABLE_POSTGRES` | `cap/db_postgres.c`, `cap/pgwire.c`; forces mbedTLS to link (see TLS) |
| `HL_ENABLE_DB` (umbrella) | `cap/db.c`, `worker_db.c`, `migrate.c`, `mod_db.c`, `agent/db.c`, `HL_MOD_CAP_DB` |

`HL_ENABLE_DB=0` stays a valid back-compat spelling that pins both
granular flags off (pure-compute), exactly like `HL_ENABLE_HTTP=0`.

Build-flavor note: a Postgres-enabled build links mbedTLS regardless of
the HTTP flags, so `pure-compute + Postgres` is a real (if unusual)
combination. The `flavors` CI matrix gains a `postgres` link check.

## Backend selection

Three sites hardwire `&hl_db_backend_sqlite` today
(`app_context.c:168`, `tool_orchestration.c:322`, `migrate.c:48`).
Replace each with a selector:

```c
/* Returns the backend for a DSN, or NULL with *err set if the DSN
 * names a backend this binary was not built with. */
const HlDbBackend *hl_db_backend_select(const char *dsn, const char **err);
```

Rule: a DSN beginning `postgres://` or `postgresql://` selects the
Postgres backend; anything else (a file path, `:memory:`, `file:`)
selects SQLite. A DSN for a backend that was not compiled fails closed
with a clear message ("this hull was built without HL_ENABLE_POSTGRES").
DSN sources are unchanged: default `data.db` / `:memory:`, CLI `-d`,
env `HULL_DB`.

## Connection and concurrency model

Postgres maps onto Hull's existing per-worker model without a new
concurrency design:

- **Main handle.** `HlAppContext.db_handle` holds one `HlDbHandle`
  whose `backend` is chosen by the selector. `HlAppContext.db` (the
  cached raw `sqlite3*`) is **NULL** under Postgres; every consumer of
  the raw pointer already tolerates NULL or will be routed through the
  vtable (see Migrations).
- **Per-worker connections.** `worker_db.c` opens a fresh connection
  lazily per thread in TLS. For Postgres this is a per-worker network
  connection (Postgres connections are not shareable across threads
  anyway), opened through the same `hl_worker_db_*` path generalized
  off the vtable instead of `sqlite3_open`.
- **Async.** `db.async.*` deep-copies SQL + params, submits to the
  thread pool, runs the (blocking) backend call on a worker thread, and
  resumes the event-loop continuation with a materialized result. The
  Postgres backend is blocking wire I/O on that worker thread. This is
  byte-for-byte the same lifecycle SQLite async uses today.

## The pgwire client (`cap/pgwire.c` + `cap/db_postgres.c`)

- **Protocol.** Postgres v3 frontend/backend. Startup, auth,
  Parse/Bind/Describe/Execute/Sync (extended query), Simple query for
  DDL, row description + data row decoding, error/notice handling,
  transaction status.
- **Auth.** SCRAM-SHA-256 (the PG 14+ default) built from Hull's own
  `hl_cap_crypto` SHA-256 / HMAC-SHA-256 / PBKDF2; `md5` and
  `password` (cleartext, TLS-only) as fallbacks. No new crypto.
- **TLS.** The `SSLRequest` probe then a standard mbedTLS handshake,
  verifying the server cert against the **embedded Mozilla CA bundle**
  (the same trust path as HTTPS). An `sslmode` DSN parameter controls
  policy: `verify-full` (default), `require`, and `disable` (dev / unix
  socket only, mirroring the `--no-ca-bundle` escape).
- **Parameters.** The vtable's `query`/`exec` already take
  `HlValue *params`. The Postgres backend binds them through the
  extended protocol (`$1`, `$2`, typed Bind), never string
  interpolation, so "SQL injection impossible" holds. Because the
  stdlib writes `?` placeholders (SQLite style), the backend runs a
  **literal-aware `?` -> `$n` rewriter** that does not touch `?` inside
  string / dollar-quoted literals or `--` / `/* */` comments.
- **Types.** Decode the common OIDs (int2/4/8, float4/8, bool, text,
  bytea, numeric-as-text, timestamptz-as-text) into `HlValue`; encode
  `HlValue` params symmetrically. Unknown OIDs fall back to text.
- **Dialect helpers** (already in the vtable): `autoincrement_id_ddl`
  = `BIGSERIAL PRIMARY KEY`; `insert_if_absent` = `ON CONFLICT DO
  NOTHING`; `upsert` = `ON CONFLICT (...) DO UPDATE SET ...`;
  `last_id` via `RETURNING`; `table_columns` via `information_schema`.

## Capability and security (c-audit)

The wire parser is the one genuinely new risk surface: it reads
length-prefixed messages from a server that must be treated as
untrusted. Treatment:

- **Bounds + overflow.** Every message length is validated against the
  bytes actually available and against a hard cap before any allocation
  or copy; all size arithmetic is guarded (`SIZE_MAX/2`), no
  `strcpy`/`strcat`/`sprintf` on server bytes. Field counts and column
  counts are range-checked before iterating.
- **Fuzzing.** A libFuzzer harness feeds arbitrary bytes to the message
  decoder (Hull already runs a `fuzz` CI lane). This is a hard
  requirement, not optional.
- **const vtable.** `const HlDbBackend hl_db_backend_postgres` lands in
  `.rodata` (RO-mapped, CFI-clean), identical to the SQLite instance.
- **Credential zeroing.** The DSN password is secret material:
  `hull_secure_zero` after the handshake, never logged, never placed in
  an error string.
- **Host allowlist.** The Postgres host must appear in
  `manifest.hosts`, checked at connect time, consistent with
  `http.fetch` / `ws`. Fails closed against an empty allowlist.
- **Parameterized only.** As above: Bind, never interpolation.
- **No raw-handle leak.** `hl_db_sqlite_raw` returns NULL for a
  Postgres handle (already its contract); no Postgres-specific raw
  accessor is exposed to runtimes.

## Feature scope for v1

**Backend-agnostic (works on Postgres via the vtable):** core
`query` / `exec` / transactions, `last_id`, the dialect helpers, every
`_hull_*` stdlib table (session, outbox, inbox, idempotency, rbac,
audit-log, transaction), and schema migrations.

**SQLite-only in v1 (clear error under Postgres):**
- `db.udf` (`db.udf.register`) is 100% `sqlite3_create_function` +
  `sqlite3_value_*`/`sqlite3_result_*`; there is no clean Postgres
  equivalent for embedded WASM/Lua UDFs. Calling it on a Postgres
  handle raises "db.udf is not supported on the postgres backend".
- `hull/search` is 100% FTS5 virtual tables. Under Postgres it raises a
  clear error pointing at the SQLite-only limitation; a native
  `tsvector`/`tsquery` port is future work (see below).

The module resolver keeps `HL_MOD_CAP_DB` on the umbrella flag (so
`hull/db` and its 15 dependents resolve on any DB build). `hull/search`
gains a note that it needs the SQLite backend at runtime; the failure
is a clear runtime error rather than a resolver rejection, because the
same binary may carry both backends and pick per-DSN.

## Migrations and internal tables

`migrate.c` uses `hl_db_sqlite_raw` today but already NULL-guards it.
Generalize the migration runner to drive the `_hull_migrations`
tracking table and apply each migration through the vtable
(`hl_db_exec` / `hl_db_begin`/`commit`), keeping the raw-sqlite path as
a SQLite-only fast path with a vtable fallback. Migration SQL authored
by apps is their responsibility to keep dialect-portable; Hull's own
`_hull_*` DDL uses the dialect helpers so it runs on both.

## Phased implementation

Each phase is independently reviewable and mergeable.

1. **Flag split, no behavior change.** Introduce `HL_ENABLE_SQLITE` /
   `HL_ENABLE_POSTGRES`, derive the `HL_ENABLE_DB` umbrella, add
   `hl_db_backend_select` (Postgres branch returns "not built"), route
   the three hardwired sites through it. Default build is byte-identical.
2. **pgwire core + backend skeleton.** `cap/pgwire.c` (protocol framing,
   simple query, extended query, type decode) and `cap/db_postgres.c`
   (the vtable), behind `HL_ENABLE_POSTGRES`, plaintext + `md5` auth
   first. Unit tests + fuzz harness for the parser.
3. **TLS + SCRAM-SHA-256.** mbedTLS handshake, CA-bundle verification,
   `sslmode`, SCRAM auth via `cap/crypto`.
4. **Per-worker + async wiring.** Generalize `worker_db.c` off the
   vtable; `db.async.*` on Postgres.
5. **Migrations + stdlib on Postgres.** Vtable-driven migration runner;
   `_hull_*` tables green on Postgres; the SQLite-only degradation
   messages for `db.udf` / `hull/search`.
6. **CI + docs.** A Postgres service in an e2e job (`docker` Postgres),
   `test_db_backend` parity run against both backends, the `flavors`
   link check, and user docs.

## Testing

- **Unit.** `cap/pgwire.c` message framing + type decode, the
  `?`->`$n` rewriter (literal/comment awareness), DSN parsing.
- **Fuzz.** libFuzzer over the message decoder (untrusted-input gate).
- **Backend parity.** `test_db_backend` (22 cases today) parameterized
  to run against both backends where the DSN is available.
- **E2E.** A `postgres:16` service container; run the `_hull_*`
  stdlib suites (session / outbox / idempotency / audit-log) against
  Postgres to prove the dialect helpers.
- **Local check.** `make CC=cosmocc` after any platform-gated change;
  `make check` (ASan/UBSan) for the parser.

## Future (out of v1 scope)

- `hull/search` on Postgres native full-text (`tsvector`/`tsquery`).
- `db.udf` has no clean Postgres analogue for embedded WASM/Lua; likely
  stays SQLite-only indefinitely.
- Connection-pool tuning (pool size, idle reaping) beyond per-worker.
- `LISTEN`/`NOTIFY` as a possible push primitive for SSE/WS fan-out.
