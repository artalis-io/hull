# Database API review + roadmap (multi-backend, query builder, DuckDB/OLAP)

Status: design review, no code changes. Captures an assessment of the `db`
capability's C / Lua / JS surface for consistency, ease-of-use, orthogonality,
DRY, and modular design, plus a forward plan for a backend-agnostic query
builder and a first-class DuckDB (OLAP) backend.

Backends today: SQLite (default, embedded), PostgreSQL (pure-C wire client),
MySQL / MariaDB (pure-C wire client). Planned: DuckDB (embedded, OLAP).

## 1. Current surface

### C (`include/hull/cap/db_backend.h`)

The `HlDbBackend` vtable is the abstraction seam:

- Methods: `open`, `close`, `query`, `exec`, `exec_script`, `begin`, `commit`,
  `rollback`, `last_id`, `errmsg`, `guard_stale_txn`, `insert_if_absent`,
  `upsert`, `table_columns`, `native_handle`.
- Fields: `name`, `schemes[]`, `identifier_quote`, `native_tag`,
  `supports_udf`, `autoincrement_id_ddl`.
- Registration: the single `BACKENDS[]` table in `src/hull/cap/db_select.c`;
  DSN-scheme selection via `hl_db_backend_select`. `duckdb://` is already a
  reserved scheme with a "not available in this build" hint
  (`db_select.c` `RESERVED[]`).
- Result rows flow through `HlRowCallback` (one row at a time).

### Lua (`src/hull/runtime/lua/mod_db.c`)

- Module: `db.default()`, `db.connect(name)`, `db.open(dsn)`.
- Connection object (`db_conn_methods`, ~line 1213): `query`, `exec`, `batch`,
  `last_id`, `insert_if_absent`, `upsert`, `table_columns`,
  `quote_identifier`; sub-objects `async.{query,exec}` and
  `udf.{register,unregister}` (SQLite-only); properties `backend_name`,
  `autoincrement_id_ddl`.
- `close()` exists only on `db.open` (caller-owned) connections;
  `db.default`/`db.connect` are registry-pooled and process-owned.
- `db.query(sql, params?)` -> array of row tables; `db.exec(sql, params?)` ->
  affected-row count; `db.last_id()` -> last insert rowid.

### JS (`src/hull/runtime/js/mod_db.c`)

Identical surface, camelCased: `lastId`, `insertIfAbsent`, `tableColumns`,
`quoteIdentifier`, `backendName`, `autoincrementIdDdl`, `async`, `udf`,
`close`.

### What is solid

- Clean vtable abstraction with a single registration point; adding a backend
  is a `.schemes` declaration + one `BACKENDS[]` line.
- Handles-only API (no global `db.*`); explicit connection objects.
- Sound ownership model: registry-pooled (`default`/`connect`, no `close`) vs
  caller-owned (`open`, has `close`, GC-finalized, double-close-safe).
- Capability gating: `udf` sub-object is absent on backends that do not support
  it, rather than a method that fails at call time; `native_handle` is reached
  via a generic tag, not per-backend symbols.
- Parameterized binding on every backend, so SQL injection has no surface.
- Real Lua/JS parity (snake_case vs camelCase, otherwise identical).

## 2. Findings

| # | Dimension | Finding | Recommendation |
|---|-----------|---------|----------------|
| 1 | DRY / modularity | Dialect leaks into app space. `autoincrement_id_ddl` (a raw SQL fragment apps interpolate) and `quote_identifier` are dialect exposed as connection properties; CLAUDE.md itself calls the former an "escape hatch." Dialect knowledge is scattered across the C vtable, the stdlib DDL strings, and app code. | Consolidate all dialect into one **dialect descriptor** (Section 4.1); let a builder own it. |
| 2 | Ease-of-use | `insert_if_absent` / `upsert` are positional parallel-array APIs: `upsert(table, conflict_cols, cols, values)` -- four arrays kept aligned by index, no key->value binding. The least ergonomic corner of the surface. | Keep as low-level primitives; the query builder replaces them with `.insert({...}).on_conflict("k").merge()`. |
| 3 | Orthogonality | `async` is a strict subset of sync: `conn.async` = `{query, exec}` only -- no `async.upsert`, `async.batch`, `async.insert_if_absent`. | Unify via a builder terminal (`:run()` vs `:run_async()`), or document the intentional subset. |
| 4 | Portability trap | `last_id()` is not portable. After an insert you call `last_id()`, but PostgreSQL returns `-1` (no rowid; needs `RETURNING`). The "insert then `last_id()`" pattern silently breaks on PG. | Builder offers portable `.returning("id")` (compiles to `RETURNING` on PG / SQLite >= 3.35 / DuckDB, `last_insert_id` on MySQL). Document the trap now. |
| 5 | Consistency | Two connection shapes: `db.open(...)` has `close()`, `db.connect/default` do not (correct by ownership, but a surprise for users). | A harmless idempotent no-op `close()` on pooled connections (friendlier than method-absent), or a clear "pooled; not closable" error. Minor. |
| 6 | DRY | The stdlib re-implements dialect bits (VARCHAR-vs-TEXT keys, the `CREATE INDEX IF NOT EXISTS` shim, `autoincrement_id_ddl` interpolation) spread across ~10 modules. | A **schema/migration builder** (Section 4.3) centralizes this; the exact portability work from the MySQL epic becomes one dialect table. |
| 7 | Composition | No safe fragment / `raw` composition at the script layer -- everything is a full SQL string + params. | Builder provides `.raw(sql, params)` + composable clauses. |

None of these are correctness or security defects; they are ergonomics and
maintainability improvements. The parameterized-binding safety property holds
throughout.

## 3. DuckDB / OLAP forward-compat

DuckDB is embedded (like SQLite), so a `cap/db_duckdb.c` wrapping the DuckDB C
API drops into the current vtable as a transactional SQL backend (it has
`ON CONFLICT` and transactions; the scheme is already reserved). Its OLAP
nature stresses four seams worth designing before the backend lands:

- **Row-callback result API.** `HlRowCallback` (one row at a time) is correct
  but poor for columnar / large analytical results. Add an **optional
  chunked/columnar fetch** vtable method; backends that support it use it, the
  row-callback path stays the default. Not required for a v1 DuckDB, but design
  the seam so it is additive.
- **Bulk insert / Appender.** DuckDB's fast load path is the Appender API, not
  row-by-row `INSERT`. An optional `bulk_insert` vtable method lets the query
  builder's `.insert([...many rows...])` route to it.
- **DDL dialect divergence.** DuckDB has no `AUTOINCREMENT` / `SERIAL`; it uses
  sequences (`CREATE SEQUENCE seq; ... DEFAULT nextval('seq')`). The
  single-fragment `autoincrement_id_ddl` cannot express that (it needs a
  companion statement) -- another push toward a schema builder that emits
  multi-statement DDL per dialect.
- **Capability model for file / URL attach.** DuckDB routinely reads
  Parquet / CSV / S3 / httpfs. That must integrate with Hull's fs sandbox and
  host allowlist (a DuckDB reading `s3://` / `https://` is an outbound
  capability, not just a local read). This is a security decision to settle
  **before** DuckDB ships, not after.
- **Rich types.** DuckDB `LIST` / `STRUCT` / `MAP` / `DECIMAL` do not map to the
  scalar `HlValue` (nil / int / double / text / blob / bool). Decide
  JSON-encode-on-read vs extending `HlValue`.

Net: the vtable is extensible enough for DuckDB as a SQL backend today; the
OLAP-specific value (columnar fetch, appender, file attach) argues for a few
**optional** vtable additions plus the capability-model decision above.

## 4. Query builder (knex-like, backend-agnostic)

### 4.0 Architecture

A pure **Lua / JS stdlib module** (`hull/query`), not C. It builds an immutable
AST and compiles to `(sql, params)`, then executes through the existing
`conn.query` / `exec` / `batch`. Pure orchestration -- matches Hull's "script
orchestrates, C computes" split, and is fully unit-testable without a database.
`conn.query`/`exec` stay the primitive; the builder is sugar that ultimately
calls them.

### 4.1 Dialect descriptor (the keystone)

The builder needs more dialect than is exposed today (only `identifier_quote`,
`backend_name`, `autoincrement_id_ddl`). Formalize one descriptor keyed by
`backend_name`:

```
{ quote           = '"' | '`',
  placeholder     = "qmark",         -- emit '?'; PG rewrites ?->$n in C already
  upsert          = "on_conflict" | "on_duplicate_key" | "insert_ignore",
  returning       = true | false,    -- PG / SQLite>=3.35 / DuckDB : MySQL false
  limit_offset    = "limit_offset" | ...,
  identity_ddl    = "...",            -- autoincrement column DDL
  sequence_ddl    = "..." | nil,      -- DuckDB sequences; nil where inline
  bool_literal    = "TRUE" | "1",
  string_concat   = "||" | "CONCAT" }
```

Adding DuckDB later is **one dialect entry**. This becomes the single source of
dialect truth that findings #1 and #6 ask for. Decision: emit `?` placeholders
uniformly and rely on each backend's existing rewrite (PG already rewrites
`?`->`$n` in C; SQLite / MySQL / DuckDB use `?`), so the builder stays
placeholder-agnostic and reuses the existing C plumbing.

### 4.2 API sketch (fluent, immutable, chainable)

```lua
-- read
db("users"):where("age", ">", 18):order_by("name"):limit(10):all()
db("users"):where("id", uid):first()
db("users"):join("orders", "users.id", "orders.user_id")
           :select("users.name", "orders.total"):all()

-- write (returning closes the last_id portability trap, finding #4)
db("users"):insert({ name = "a", email = "e" }):returning("id"):run()
db("users"):where("id", uid):update({ name = "b" }):run()
db("users"):where("id", uid):delete():run()

-- upsert subsumes insert_if_absent / upsert (finding #2)
db("events"):insert(rows):on_conflict("id"):merge():run()   -- ON CONFLICT/ON DUP KEY
db("events"):insert(rows):on_conflict("id"):ignore():run()  -- INSERT IGNORE / DO NOTHING

-- escape hatch + introspection
db.raw("select 1"):all()
local sql, params = q:to_sql()   -- no execution; debugging + passthrough
```

Terminals `:all()` / `:first()` / `:run()` (sync) and `:run_async()` etc.
(async) close the async-subset gap (finding #3). JS mirrors in camelCase
(`orderBy`, `onConflict`, `toSql`, `runAsync`). Transactions compose via the
existing `db.batch(fn)`; builder calls inside it share the connection.

### 4.3 Companion schema builder

`db.schema.create_table("users", function(t) ... end)` emits dialect-correct
DDL and **absorbs the exact portability work from the MySQL epic** -- VARCHAR
vs TEXT keys, the `CREATE INDEX IF NOT EXISTS` shim, autoincrement / sequence
DDL -- into the dialect table (finding #6). Makes DuckDB DDL just another
dialect entry.

## 5. Sequencing

1. **Dialect descriptor** -- expose the missing bits; low-risk, foundational.
2. **Query builder** (Lua + JS stdlib) on top, emitting `?`-placeholder SQL run
   via `conn.query` / `exec`. Pure + testable, no C change.
3. **Schema builder** -- subsumes DDL dialect + the `CREATE INDEX` shim +
   VARCHAR-key logic.
4. **DuckDB backend** -- embedded, reserved scheme; plus the OLAP-optional
   vtable methods (columnar fetch, appender) and the file/URL capability-model
   decision.
5. Document the `last_id` portability trap now; steer apps to `.returning()`.

## 6. Open decisions

- **DuckDB file/URL attach capability model** (Section 3) -- the one item that
  is a security decision, not ergonomics. Settle before DuckDB work starts.
- **DuckDB rich-type mapping** -- JSON-encode-on-read vs extending `HlValue`.
- **`close()` on pooled connections** (finding #5) -- no-op vs explicit error.
- **Builder home** -- confirm `hull/query` as the module name and whether the
  schema builder is `hull/query/schema` or a sibling `hull/schema`.
