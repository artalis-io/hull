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
-- always, on open:
SET autoinstall_known_extensions = false;
SET autoload_known_extensions    = false;
SET allow_unsigned_extensions    = false;

-- (A) no fs.read / fs.write in the manifest  ->  full lockdown (SQLite-equivalent):
SET enable_external_access = false;      -- DB file + :memory: only

-- (B) fs.read / fs.write declared  ->  bounded LOCAL access, still no network:
SET enable_external_access = true;
SET allowed_directories = ['<mapped fs.read/fs.write dirs>'];
SET allowed_paths       = ['<mapped specific files>'];
SET disabled_filesystems = 'HTTPFileSystem,S3FileSystem,...';

-- finally, in BOTH cases:
SET lock_configuration = true;           -- app SQL can no longer re-enable anything
```

- **`lock_configuration = true` is the keystone.** Without it a handler could
  run `SET enable_external_access = true` and escape; with it the posture is
  fixed for the connection's life.
- **Network is off structurally in v1**: httpfs / S3 are never linked or loaded,
  so there is no outbound path even before config.
- **File access reuses `fs.read` / `fs.write`** (no new capability concept): the
  only thing that widens DuckDB's file reach is the app's existing manifest fs
  paths, mapped to `allowed_directories` / `allowed_paths`. A `read_parquet('/x')`
  on an ungranted path fails closed exactly like `fs.read('/x')`. The DB *file*
  itself (the DSN path) is already gated by the existing DB-path sandbox, same
  as a SQLite file. DuckDB therefore introduces zero new capability surface.

### 3.3 Packaging (side-loaded, on-demand)

DuckDB's amalgamation is ~tens of MB versus Hull's ~5 MB. Dynamic linking is
ruled out (Hull's hardening bans `dlopen` / lazy binding, and a
`DT_NEEDED libduckdb.so` breaks the single-static-binary / reproducible / cosmo
story). So DuckDB is statically linked when present, and the default `hull`
stays lean and keeps rejecting `duckdb://` with the reserved-scheme hint -- now
pointing at the install command.

DuckDB ships as a signed release variant fetched on demand, reusing the exact
Ed25519 release-signature trust chain that `hull update` / `hull tools install` /
`hull platform install` already use (`hl_release_io_*`): download -> verify
signature + SHA-256 against the signed manifest -> install. No rebuild from
source, no new key or verifier. Open sub-decision for implementation time:
whether the variant installs alongside as a distinct `hull-duckdb` binary
(preferred, so the lean default is never disturbed) or swaps the running binary
in place like `hull update`.

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
   that lets DuckDB coexist with the full HTTP/TLS stack landed as the
   immediate follow-up. Still deferred: the manifest-driven
   `fs.read`/`fs.write` -> `allowed_directories` mode, the
   `insert_if_absent`/`upsert`/`table_columns` dialect helpers, temporal /
   decimal / nested type decoding, and the signed side-load packaging (3.3).
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
- DuckDB rich-type mapping (JSON-encode-on-read vs extending `HlValue`).
- Side-load install UX (distinct `hull-duckdb` binary vs in-place swap).
- MariaDB per-connection dialect refinement (2.3).
- Whether `enable_external_access` + `allowed_directories` should also govern
  DuckDB's own DB-file directory, or only SQL-driven external access (they are
  separate in DuckDB; the DB file is gated by the existing DB-path sandbox).
