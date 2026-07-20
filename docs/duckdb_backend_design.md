# DuckDB backend + `HlDbDialect` design

Status: design, pre-implementation. Adds DuckDB as a first-class OLAP SQL
backend alongside SQLite / PostgreSQL / MySQL / MariaDB, and formalizes a single
per-backend dialect descriptor (`HlDbDialect`) that the coming query / schema
builders will consume. This is the keystone step of the plan in
[docs/db_api_review.md](db_api_review.md); the query builder and schema builder
follow on top of the descriptor landed here.

## 1. Goals and non-goals

Goals:
- DuckDB (`duckdb://`) as an embedded SQL backend behind the existing
  `HlDbBackend` vtable, coherent with SQLite / PG / MySQL. No bespoke DuckDB
  methods on the core interface.
- Formalize `HlDbDialect`: one per-backend descriptor that is the single source
  of dialect truth (identifier quoting, placeholder style, upsert grammar,
  RETURNING support, identity DDL). Resolves finding #1 of the API review
  (dialect leaking into app space).
- Keep Hull's capability model intact: DuckDB's SQL-driven file/network I/O is
  fail-closed by default and only widened by the app's existing manifest
  capabilities.

Non-goals (v1):
- OLAP fast paths (columnar result fetch, Appender bulk insert). These land
  later as *optional* vtable methods; the abstraction does not bend for one
  backend.
- DuckDB network I/O (httpfs / S3 / https). Disabled in v1.
- DuckDB UDFs and nested-type round-tripping beyond JSON text.
- MariaDB-vs-MySQL runtime dialect refinement (see 2.3).

## 2. `HlDbDialect`

### 2.1 The descriptor

Only fields where backends actually differ AND a consumer (query builder,
schema builder, or a raw-SQL author) needs them. Each is an independent axis;
no redundancy.

```c
typedef struct HlDbDialect {
    char        identifier_quote;              /* '"' | '`'                          */
    const char *placeholder;                   /* "?" | "$n"                         */
    const char *upsert_style;                  /* "on_conflict" | "on_duplicate_key" */
    unsigned char supports_returning;          /* 0 => builder .returning() falls    */
                                               /*   back to last_id (single-row)     */
    unsigned char supports_index_if_not_exists;/* 0 = MySQL (backend shim covers it) */
    const char *identity_column;               /* inline PK column DDL               */
    const char *identity_sequence;             /* "CREATE SEQUENCE %s" companion for */
                                               /*   engines needing it (DuckDB); else*/
                                               /*   NULL                             */
} HlDbDialect;
```

`identifier_quote` and the former `autoincrement_id_ddl` (now `identity_column`)
move into this struct; the connection object exposes it as `conn.dialect`, and
`conn.quote_identifier` / `conn.autoincrement_id_ddl` become thin readers over
it, so nothing breaks and dialect has exactly one home.

Deliberately omitted (orthogonality): `limit_offset` (all four backends agree on
`LIMIT .. OFFSET`), `bool_literal` (booleans arrive as bound params, never
literals), `concat_op` (no v1 consumer). Each is added the day a backend or a
builder feature actually differs, not speculatively.

### 2.2 The four dialects

| axis | SQLite | PostgreSQL | MySQL / MariaDB | DuckDB |
|---|---|---|---|---|
| `identifier_quote` | `"` | `"` | `` ` `` | `"` |
| `placeholder` | `?` | `$n` | `?` | `?` |
| `upsert_style` | `on_conflict` | `on_conflict` | `on_duplicate_key` | `on_conflict` |
| `supports_returning` | 1 (>= 3.35) | 1 | 0 (MariaDB is 1) | 1 |
| `supports_index_if_not_exists` | 1 | 1 | 0 (shim) | 1 |
| `identity_column` | `INTEGER PRIMARY KEY` | `BIGSERIAL PRIMARY KEY` | `BIGINT AUTO_INCREMENT PRIMARY KEY` | `BIGINT DEFAULT nextval('%s') PRIMARY KEY` |
| `identity_sequence` | NULL | NULL | NULL | `CREATE SEQUENCE %s` |

The builder emits `?` placeholders uniformly; the PG backend already rewrites
`?` -> `$n` in C, so the builder never needs `$n`. `placeholder` is exposed only
so a raw-SQL author who bypasses the builder knows the native style.

DuckDB is exactly the backend that justifies the two-part identity: it has no
`AUTO_INCREMENT` / `SERIAL`, only sequences, which a single fragment cannot
express. The schema builder generates one sequence name per identity column and
threads it through both `identity_sequence` and `identity_column`.

### 2.3 MySQL / MariaDB caveat

One backend struct serves both `mysql://` and `mariadb://`, but MariaDB supports
`RETURNING` and native `CREATE INDEX IF NOT EXISTS`. The static per-backend
dialect cannot distinguish them, so v1 is conservative (treat as MySQL): the
backend's index shim covers `IF NOT EXISTS`, and `.returning()` falls back to
`last_id`. A later refinement could detect "MariaDB" from the handshake
`server_version` and hand out a per-connection dialect override, but that breaks
the pure-`.rodata` descriptor model, so it is a deliberate future decision.

### 2.4 Integration and back-compat

- `HlDbBackend` gains `HlDbDialect dialect;` (by value, `.rodata`). The former
  `identifier_quote` and `autoincrement_id_ddl` fields are removed; every
  consumer reads `be->dialect.*`.
- `hl_db_quote_ident` (inline) reads `be->dialect.identifier_quote`.
- The connection object exposes a read-only `conn.dialect` sub-table (Lua
  snake_case, JS camelCase). `conn.quote_identifier` and
  `conn.autoincrement_id_ddl` remain as aliases sourced from it.
- No behaviour change for existing apps: the aliases return the same values.

## 3. DuckDB backend

### 3.1 Shape

- `cap/db_duckdb.c` wraps the DuckDB C API (pin DuckDB >= 1.1; the security
  settings in 3.2 are version-gated). `duckdb://` scheme (already reserved in
  `db_select.c`) plus a file path or `:memory:`.
- Drops into `HlDbBackend` unchanged: `open`, `close`, `query`, `exec`,
  `exec_script`, `begin`/`commit`/`rollback`, `last_id`, `errmsg`,
  `insert_if_absent`, `upsert`, `table_columns`. It fills the DuckDB dialect row.
- Result rows adapt DuckDB's columnar result chunks to the existing
  `HlRowCallback` (row-at-a-time) in v1.
- Types: scalar DuckDB types -> `HlValue`; `LIST` / `STRUCT` / `MAP` -> JSON
  text, or a clear "unsupported nested type" error. `supports_udf = 0` in v1.
- `native_tag = HL_DB_NATIVE_DUCKDB` (new tag); `native_handle` optional.

### 3.2 Security model (locked-down by default)

The concern: DuckDB SQL can do its own file/network I/O (`read_parquet`,
`read_csv`, `COPY`, httpfs), which would bypass Hull's capability model. DuckDB
>= 1.1 exposes config knobs that let us enforce a policy; we apply it at every
connection open and then lock it so app SQL cannot undo it.

```sql
-- always, at config time (before open):
SET autoinstall_known_extensions = false;
SET autoload_known_extensions    = false;
SET allow_unsigned_extensions    = false;

-- (A) no fs.read / fs.write in the manifest  ->  full lockdown (SQLite-equivalent):
SET enable_external_access = false;      -- DB file + :memory: only

-- (B) fs.read / fs.write declared  ->  bounded LOCAL access, still no network:
SET enable_external_access = true;
SET allowed_directories = ['<absolute dirs from fs.read/fs.write>'];  -- post-connect

-- finally, in BOTH cases (post-connect):
SET lock_configuration = true;           -- app SQL can no longer re-enable anything
```

- **`lock_configuration = true` is the keystone.** Without it a handler could
  run `SET enable_external_access = true` and escape; with it the posture is
  fixed for the connection's life.
- **Network is off structurally**: httpfs / S3 are never linked or loaded, so
  there is no outbound path even before config. (No `disabled_filesystems` SET
  is needed — the filesystems simply do not exist in the binary.)
- **File access reuses `fs.read` / `fs.write`** (no new capability concept):
  serve.c resolves each declared fs path to its absolute containing directory
  (glob tail stripped; the path itself if it is a directory, else its parent),
  dedups, and installs the set as DuckDB's `allowed_directories`. A
  `read_parquet('/x')` on an ungranted path fails closed exactly like
  `fs.read('/x')`; the same fs paths are also unveiled/seatbelt'd, so the kernel
  sandbox and DuckDB agree. The DB *file* itself (the DSN path) is gated by the
  existing DB-path sandbox, same as a SQLite file. DuckDB introduces zero new
  capability surface. (Directory granularity, not per-file `allowed_paths`, in
  this pass: a declared file grants its containing directory.)

**Implementation (status: done) — mode B is a named/dynamic-connection feature.**
The policy is computed once at boot from the sealed manifest
(`hl_serve_install_duckdb_fs_policy` in serve.c, into the sealed policy arena)
and read by `duck_open` via `hl_db_duckdb_set_fs_policy`. A connection opened
**after** the policy is installed picks up mode B; one opened **before** it gets
mode A. In practice:

- **Named (`db.connect`) and dynamic (`db.open`) connections** open lazily from
  `app.main` / a handler — after the manifest is sealed and the policy is
  installed — so they get **mode B**.
- **The default `-d` connection** is opened during app-context init, *before*
  the app (and thus the manifest) loads, so app top-level code (e.g.
  `session.init()` creating tables) has a live DB. It therefore stays in
  **locked-down mode A**: normal SQL works, but SQL-driven external file access
  is off. For DuckDB file access, declare a named connection.

This split keeps the default connection's lockdown a hard guarantee (locked at
open, no unlocked window) and keeps top-level DB usage working, while giving
OLAP apps mode B on the connection they load data through. Making the *default*
connection mode B would require either opening it before the manifest is known
(impossible) or an unlocked-until-finalized window during the app's own load (a
capability weakening we chose not to take); a mid-load manifest hook is a
possible future refinement.

**Sandbox interaction (Linux) — the real blocker was `rseq`, in the pledge
layer, not the unveil layer.** A mode-B named connection opens lazily from
`app.main`, i.e. **after** the sandbox is sealed, and DuckDB spawns its worker
pool there. glibc >= 2.35 registers `rseq` (restartable sequences) per thread;
Hull's main thread registers it *before* the sandbox is applied, so it succeeds.
But Hull's vendored `pledge` polyfill historically ENOSYS-stubbed `rseq` (via
`SECCOMP_RET_ERRNO`, a compat shim shared with `openat2`/`statx`/`clone3`/
`memfd_create`), and glibc treats a thread whose `rseq` registration returns
`ENOSYS` *after* an earlier thread succeeded as **fatal**
(`Fatal glibc error: rseq registration failed`). So the process aborted at
worker-pool spawn, **before `read_csv` ran** — which for a long time was
misread as a file-I/O / unveil incompatibility (there was no pledge SIGSYS log,
because `SECCOMP_RET_ERRNO` does not trap). The fix is a one-syscall
`vendor/pledge` patch: stop ENOSYS-stubbing `rseq` and add it to the always-
allowed set (`kPledgeDefault`), so a post-sandbox thread registers it against
the kernel like the main thread did. `rseq` is a benign performance syscall with
no fs/net/exec reach; the patch also fixes the latent bug for *any* Hull app that
first creates a thread after the sandbox seals. Verified on Linux CI: with the
patch (and **no** `GLIBC_TUNABLES` workaround), a mode-B `read_csv` of a
declared, unveiled `fs.read` file returns rows under the full pledge/unveil
sandbox; that CI gate doubles as the regression test for the pledge patch.

Mode A works without the patch only because the default `-d` connection opens in
phase 6, *before* the sandbox, so its worker pool's `rseq` registration precedes
the seccomp filter.

Two belt-and-braces measures from the earlier (misdiagnosed) investigation are
retained but flagged for review: `sandbox.c` unveils a small static set of
read-only system-info paths (`/dev/urandom`, `/etc/localtime`,
`/sys/devices/system/cpu`, `/sys/fs/cgroup`, `/proc/self/cgroup`,
`/proc/sys/vm/overcommit_memory`, `/proc/meminfo`), and `duck_open` pins
`memory_limit` (2 GB) + `threads` (4) to skip DuckDB's CPU/memory auto-detection.
These were added while chasing the wrong cause; now that `rseq` is understood to
be the actual blocker, a follow-up should test whether they can be dropped (the
unveils are harmless read-only info paths; the pinned limits are otherwise a
real ceiling for large OLAP, so a manifest option to tune them is tracked
regardless). A large scan that spills to a temp dir still needs that dir in the
sandbox's write set — separate from the read path proven here.

**The fs bound is enforced by the kernel unveil.** DuckDB may only open files
under the unveiled `fs.read` / `fs.write` directories, so an undeclared
`read_csv('/x')` is blocked at the syscall. DuckDB's `SET allowed_directories`
is set post-connect as intended defense-in-depth, but it is runtime-SET-only
(not accepted by `duckdb_set_config` at config time) and does **not** enforce on
Linux (it does on macOS — a DuckDB platform quirk), so the kernel unveil is the
authoritative bound.

**Known limitation — apps must only read declared paths.** Because
`allowed_directories` does not pre-empt the open on Linux, an undeclared read
reaches the kernel, gets `EACCES`, and DuckDB **aborts the process** (NULL-deref
on the denied open) instead of returning a catchable error. Security holds (no
data leaks — the read is blocked), but a read of an undeclared path crashes the
DuckDB process rather than erroring. App SQL must therefore reference only
paths the manifest declares; a path derived from untrusted input is an app bug
(and a crash, not a leak). Making an undeclared read a clean error would need
`allowed_directories` to enforce on Linux (an upstream DuckDB fix) — tracked.

### 3.3 Packaging (side-loaded, on-demand)

> **Re-scoped (2026-07-18).** The "signed release variant / distinct
> `hull-duckdb` binary" sketch below is superseded: DuckDB ships as the **first
> composable feature** (`libhull_feature-duckdb.a`, `hull feature install
> duckdb`, composed at `hull build`), not a `full-duckdb` flavor or a runtime
> variant. Rationale (avoiding the 2^N flavor matrix for orthogonal large libs)
> and the concrete plan are in
> [features_and_flavors.md](features_and_flavors.md) §6. The size / static-link /
> trust-chain facts below still hold.

DuckDB's amalgamation is ~tens of MB versus Hull's ~5 MB. Dynamic linking is
ruled out (Hull's hardening bans `dlopen` / lazy binding, and a
`DT_NEEDED libduckdb.so` breaks the single-static-binary / reproducible / cosmo
story). So DuckDB is statically linked when present, and the default `hull`
stays lean and keeps rejecting `duckdb://` with the reserved-scheme hint -- now
pointing at the install command.

DuckDB ships as a signed **composable feature**, reusing the exact Ed25519
release-signature trust chain that `hull update` / `hull tools install` /
`hull flavor install` already use (`hl_release_io_*`): download -> verify
signature + SHA-256 against the signed manifest -> install. No rebuild from
source, no new key or verifier.

**Resolved** (the "distinct `hull-duckdb` binary vs in-place swap" sub-decision
below is superseded): neither. DuckDB is **not a separate binary** — it's a
per-feature static archive `libhull_feature-duckdb.a` that `hull feature install
duckdb` fetches into `~/.hull/feature/`, and `hull build --with=duckdb` composes
into the *app* binary at build time. The lean default `hull` is never disturbed;
the app that opts in carries DuckDB. See
[features_and_flavors.md](features_and_flavors.md) for the full model.

### 3.4 Link-time symbol collision with Hull's mbedTLS (resolved via isolation)

DuckDB's prebuilt `libduckdb_static.a` **embeds its own copy of mbedTLS**
(e.g. `cipher_wrap.cpp.o`), a *different version* than Hull's vendored mbedTLS
(the linker reports a size mismatch on `mbedtls_cipher_supported`, 84 vs 52
bytes). Linking both raw is a duplicate-symbol error AND an unsafe version mix;
omitting the separate `libduckdb_mbedtls.a` is not enough because the collision
is inside the core archive. Hull links its own mbedTLS whenever `HL_LINK_TLS=1`
(any HTTP half, or Postgres, or MySQL).

**Resolution — symbol isolation in `fetch-duckdb`.** After unzip, the fetch
target runs `objcopy --redefine-syms` (GNU `objcopy` on Linux, `llvm-objcopy`
on macOS) over `libduckdb_static.a`, renaming every `mbedtls_*` / `psa_*`
symbol (227 of them) to a DuckDB-private `hlduck_` prefix. Because
`--redefine-syms` rewrites definitions **and** references consistently within
each object, DuckDB's internal mbedTLS calls still resolve to its own renamed
copy, while Hull keeps the un-prefixed `mbedtls_*`. The two mbedTLS versions
then coexist in one binary, and DuckDB works alongside the full HTTP/TLS stack
(`make HL_ENABLE_DUCKDB=1` on the default flavor). The map is generated from
`nm` so it is correct on both ELF (no leading underscore) and Mach-O; re-running
is idempotent (an already-isolated archive yields an empty map). `miniz` needs
no isolation: DuckDB's is C++-namespaced (`duckdb_miniz::`), so its symbols
never clash with Hull's C `mz_*`.

**GNU-ld archive ordering.** The DuckDB archives reference each other
circularly (generated loader → extensions → core, and back). GNU ld resolves
that only inside a `-Wl,--start-group ... -Wl,--end-group`; macOS ld64 resolves
regardless of order. The Makefile wraps the archive set in a group on Linux
(plus `-lstdc++ -ldl`); macOS links them directly with `-lc++`.

## 4. Why this stays orthogonal

- One dialect home: DuckDB is a row in `HlDbDialect`; the query / schema builders
  and every backend read the same descriptor.
- One file-allowlist: DuckDB file access is `fs.read` / `fs.write`, not a new
  concept.
- No new I/O path: network off in v1; the DB never becomes a second outbound
  channel parallel to `http.fetch`.
- Same trust chain: side-loading reuses release signing.
- Same vtable: no bespoke DuckDB methods; OLAP power (columnar fetch, Appender)
  lands later as optional methods.

## 5. Sequencing

1. `HlDbDialect` -- introduce the struct, migrate `identifier_quote` /
   `autoincrement_id_ddl` into it across SQLite / PG / MySQL, expose
   `conn.dialect`, keep the old props as aliases. Backend-independent, low-risk,
   CI-testable on its own. (This step first.)
2. Query builder + schema builder on the descriptor (see the API review doc).
3. DuckDB backend: `cap/db_duckdb.c`, the DuckDB dialect row, the security-config
   mapping (3.2), and the side-load packaging (3.3). Landed as a **thin vertical
   slice** first (open/query/exec/txn on :memory:/file, prepared-statement param
   binding, columnar chunk decode, the full-lockdown security config, the dialect
   row, `duckdb://` selection, unit tests). The mbedTLS symbol isolation (3.4)
   that lets DuckDB coexist with the full HTTP/TLS stack, the manifest-driven
   `fs.read`/`fs.write` -> `allowed_directories` mode B (3.2; needs the
   vendor/pledge rseq fix so a post-sandbox worker thread can register rseq), and
   the `insert_if_absent`/`upsert`/`table_columns` dialect helpers (DuckDB speaks
   the Postgres-family `ON CONFLICT ... DO NOTHING / DO UPDATE SET
   col = excluded.col` grammar and exposes `information_schema.columns`, so they
   mirror the Postgres backend, with `?` placeholders instead of `$n`) all landed
   as follow-ups. Still deferred: temporal / decimal / nested type decoding, and
   the signed side-load packaging (3.3).
4. OLAP optional vtable methods (columnar fetch, Appender) as a follow-on.

## 6. Open items

- **DuckDB under Hull's Linux sandbox — resolved.** The Linux non-zero exit at
  teardown was not a DuckDB-teardown bug: the sandbox was handed the raw DSN as
  the DB path, so `duckdb://:memory:` was mangled (`strrchr('/')` lands inside
  `://`), the unveil failed with a warning, and the resulting bogus fs gating
  broke DuckDB's cleanup. `sandbox_db_path()` (src/hull/serve.c) now normalizes
  a DSN before the sandbox gates it — a scheme-qualified in-memory or network
  DSN gates nothing, a file DSN gates the real path — so the `:memory:` engine
  runs and exits 0 under the full pledge/unveil sandbox (confirmed on Linux CI).
  Remaining smaller item: a large DuckDB query that spills to a temp directory
  needs that directory inside the sandbox's write set (fine for `:memory:` +
  in-core work; revisit when wiring `fs.write` -> DuckDB `temp_directory`).
- **Manifest-driven `fs.read`/`fs.write` -> `allowed_directories` (mode B) —
  descoped.** The intent was to let a named / dynamic DuckDB connection read the
  directories an app declares (via `read_csv` / `read_parquet` / `COPY`) while
  everything else stayed locked down. It does not work under Hull's Linux
  sandbox: DuckDB's C++ runtime aborts (internal NULL-deref) on any file read
  under pledge/unveil even for a **declared, unveiled** path — not a bounded
  allowlist issue but a fundamental DuckDB-runtime vs restrictive-unveil
  incompatibility. Widening unveil to DuckDB's full probed set (`/dev/urandom`,
  `/etc/localtime`, `/sys/devices/system/cpu`, `/sys/fs/cgroup`,
  `/proc/self/cgroup`, `/proc/sys/vm/overcommit_memory`, `/proc/meminfo`) and
  pinning `memory_limit` + `threads` did not fix it. File I/O works only on
  macOS (seatbelt) and with `--no-sandbox`. A future mode B needs a different
  architecture (DuckDB work in a separate, less-restricted process, or upstream
  DuckDB sandbox support); the attempt is preserved on branch
  `feat/duckdb-fs-access`. The shipped backend is full-lockdown mode A
  everywhere.
- DuckDB rich-type mapping (JSON-encode-on-read vs extending `HlValue`).
- Side-load install UX (distinct `hull-duckdb` binary vs in-place swap).
- MariaDB per-connection dialect refinement (2.3).
- Whether `enable_external_access` + `allowed_directories` should also govern
  DuckDB's own DB-file directory, or only SQL-driven external access (they are
  separate in DuckDB; the DB file is gated by the existing DB-path sandbox).
