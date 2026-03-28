# Database Backend Vtable Roadmap

Status: **Planned** | Created: 2026-03-28

## Goal

Decouple the query engine from SQLite via a `HlDbBackend` vtable, enabling:
- Pure compute applications (no DB at all)
- Future alternative backends (PostgreSQL, DuckDB)
- Hull internals always on embedded SQLite regardless of app backend

## Design

### Vtable

```c
typedef struct HlDbBackend {
    const char *name;   /* "sqlite", "none" */
    int    (*open)(void **ctx, const char *dsn, HlAllocator *alloc);
    void   (*close)(void *ctx);
    int    (*query)(void *ctx, const char *sql,
                    const HlValue *params, int nparams,
                    HlRowCallback cb, void *cb_ctx, HlAllocator *alloc);
    int    (*exec)(void *ctx, const char *sql,
                   const HlValue *params, int nparams);
    int    (*begin)(void *ctx);
    int    (*commit)(void *ctx);
    int    (*rollback)(void *ctx);
    int64_t (*last_id)(void *ctx);
    const char *(*errmsg)(void *ctx);
    void   (*guard_stale_txn)(void *ctx);  /* NULL = no-op */
} HlDbBackend;

typedef struct HlDbHandle {
    const HlDbBackend *backend;
    void              *ctx;
} HlDbHandle;
```

### Dual-handle architecture

```
HlRuntime
  ├── db_handle       → HlDbHandle* (app queries via vtable)
  ├── hull_db_handle  → HlDbHandle* (hull internal, always SQLite)
```

Default: both point to the same SQLite handle (zero overhead).
Future: app uses Postgres, hull internals stay on embedded SQLite.

### Namespace routing

```c
HlDbHandle *h = is_stdlib_caller ? rt->hull_db_handle : rt->db_handle;
```

App code goes through the vtable. Stdlib code (`_hull_*` tables) always
routes to the hull SQLite handle. Namespace check still prevents app
code from touching `_hull_*` tables.

## Implementation Phases

### Phase 1: Introduce vtable without changing behavior

1. Create `include/hull/cap/db_backend.h` — vtable + HlDbHandle + inline wrappers
2. Create `src/hull/cap/db_sqlite.c` — wraps existing `hl_cap_db_*` functions
3. Modify `include/hull/runtime.h` — add `db_handle`/`hull_db_handle`, keep deprecated `db`/`stmt_cache` temporarily
4. Modify `src/hull/app_context.c` — use `hl_db_backend_sqlite.open()`, wire both handles
5. All tests pass unchanged

### Phase 2: Migrate consumers to vtable

6. `src/hull/runtime/lua/mod_db.c` — replace `stmt_cache`/`sqlite3_errmsg` with `db_handle`/`hl_db_errmsg`
7. `src/hull/runtime/js/mod_db.c` — same
8. `src/hull/runtime/{lua,js}/runtime.c` — guard_stale_txn
9. `src/hull/runtime/{lua,js}/modules.c` — availability check
10. `src/hull/worker_db.c` — use backend open/close
11. `src/hull/migrate.c` — `hl_db_sqlite_raw()` for migration runner
12. UDF registration — use `hl_db_sqlite_raw()` for raw sqlite3*

### Phase 3: Remove deprecated fields

13. Remove `sqlite3 *db` and `HlStmtCache *stmt_cache` from `HlRuntime`
14. Remove `#include <sqlite3.h>` from runtime binding files

### Phase 4: Pure compute mode

15. Allow `HlAppContext` to skip DB init when no `db_path` provided
16. Both handles NULL, `db` global not registered

## Key Decisions

- **Thin vtable, no dialect normalization.** App author writes backend-specific SQL.
- **Hull internals always use embedded SQLite.** Session, outbox, inbox, idempotency, RBAC, migrations use `_hull_*` tables on SQLite regardless of app backend.
- **Statement cache is backend-internal.** SQLite backend wraps `HlStmtCache` in its ctx. Other backends implement their own or none.
- **UDFs are SQLite-specific.** `db.udf.register` uses raw `sqlite3*` via `hl_db_sqlite_raw()`. Unavailable for non-SQLite app backends.
- **Worker DB stays SQLite.** `db.async.*` opens per-thread SQLite connections. Future Postgres would use libpq async, not worker threads.
- **Existing apps work unchanged.** SQLite is the default. The vtable is internal.

## Files

**New (2):** `include/hull/cap/db_backend.h`, `src/hull/cap/db_sqlite.c`

**Modified (~15):** `runtime.h`, `db.h`, `mod_db.c` (Lua+JS), `modules.c` (Lua+JS), `runtime.c` (Lua+JS), `app_context.c`, `main.c`, `worker_db.c`, `migrate.c`, `Makefile`

## Risk

Low. SQLite backend wraps existing functions 1:1. Inline wrappers compile to the same code. One extra function pointer dereference per DB call (~1ns). All existing tests exercise the same code paths. Missed callsites cause compile errors (type mismatch `sqlite3*` vs `HlDbHandle*`).
