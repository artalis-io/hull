# Hull — C Public API Reference

Per-function reference for Hull's C public headers (`include/hull/*.h`).
Audience: embedders, contributors, runtime authors.

For prose / patterns see [`../../CLAUDE.md`](../../CLAUDE.md) and
[`../agent_guide.md`](../agent_guide.md). For stability tiers see
[`../stability.md`](../stability.md).

## Table of contents

- [Capability layer](#capability-layer) — `hl_cap_*`
  - [Database (`cap/db.h`)](#database-capdbh)
  - [Filesystem (`cap/fs.h`)](#filesystem-capfsh)
  - [Crypto (`cap/crypto.h`)](#crypto-capcryptoh)
  - [HTTP client (`cap/http.h`)](#http-client-caphttph)
  - [Environment (`cap/env.h`)](#environment-capenvh)
  - [Time (`cap/time.h`)](#time-captimeh)
  - [Audit (`cap/audit.h`)](#audit-capaudith)
  - WebSocket, WASM, GPU, Image, SMTP, Body — _coming next_
- [Runtime layer](#runtime-layer)
  - [`runtime.h`](#runtimeh) — `HlRuntime` + vtable
  - [`runtime/factory.h`](#runtimefactoryh) — factory registry
  - [`app_context.h`](#app_contexth) — embedder context
- [Manifest (`manifest.h`)](#manifest-manifesth)
- [VFS (`vfs.h`)](#vfs-vfsh)
- [Buffer protocol (`buffer.h`)](#buffer-bufferh) — `HlBufferView`
- [Signature (`signature.h`, `release.h`)](#signature-signatureh-releaseh)
- [Agent library (`agent_lib.h`, `agent_api.h`)](#agent-library)
- [Command dispatch (`commands/*.h`)](#command-dispatch)

---

## Capability layer

The shared C boundary that mediates every system access from Lua/JS
runtimes. Each capability module enforces its own manifest allowlist and
input validation. See [`security.md`](../security.md) § "Capability
enforcement invariants" for the cross-cutting guarantees.

### Database (`cap/db.h`)

#### Error codes

```c
typedef enum {
    HL_DB_OK           =  0,
    HL_DB_ERR_PREPARE  = -1,
    HL_DB_ERR_BIND     = -2,
    HL_DB_ERR_EXEC     = -3,
    HL_DB_ERR_BUSY     = -4,
    HL_DB_ERR_DENIED   = -5,
} HlDbError;
```

#### `hl_stmt_cache_init(cache, db, alloc)`

Initialize a prepared-statement LRU cache attached to a `sqlite3` handle.

| Param   | Type             | Description |
|---------|------------------|-------------|
| `cache` | `HlStmtCache *`  | Caller-owned cache struct to populate. Memory uninitialised on entry; left zero+populated on exit. |
| `db`    | `sqlite3 *`      | Live SQLite handle. Borrowed; not retained on cache destroy. |
| `alloc` | `HlAllocator *`  | Allocator for any cache-internal allocations. `NULL` = raw `malloc`/`free`. |

**Returns:** `void`. Cannot fail.

**Notes:** the cache has a fixed capacity of `HL_STMT_CACHE_SIZE = 32`
entries. Inserting a 33rd evicts the LRU entry (with `sqlite3_finalize`).
The cache struct is **Tier 4 — internal**; its layout will change post-v0.1.0.
Always use the entry points, never field access.

---

#### `hl_stmt_cache_destroy(cache)`

Finalize every prepared statement in the cache and zero the struct.

| Param   | Type            | Description |
|---------|-----------------|-------------|
| `cache` | `HlStmtCache *` | Cache to destroy. Safe on a zeroed struct (no-op). |

**Returns:** `void`.

---

#### `hl_cap_db_init(db)`

Apply Hull's standard PRAGMA configuration to a fresh `sqlite3` handle:
WAL mode, foreign keys on, busy timeout, etc.

| Param | Type        | Description |
|-------|-------------|-------------|
| `db`  | `sqlite3 *` | Live handle from `sqlite3_open*`. |

**Returns:** `int` — `0` on success, `-1` if any PRAGMA failed (handle should be closed by caller).

**Example:**

```c
sqlite3 *db;
if (sqlite3_open(":memory:", &db) != SQLITE_OK) return -1;
if (hl_cap_db_init(db) != 0) { sqlite3_close(db); return -1; }
```

---

#### `hl_cap_db_shutdown(db)`

Flush any pending writes and finalize cached statements before close.
Optional — `sqlite3_close` works without it — but recommended for clean shutdown logs.

| Param | Type        | Description |
|-------|-------------|-------------|
| `db`  | `sqlite3 *` | Handle to shut down. Caller still calls `sqlite3_close` after. |

**Returns:** `void`.

---

#### `hl_cap_db_query(cache, sql, params, nparams, cb, ctx, alloc)`

Run a SELECT with parameterised bindings, invoking `cb` once per row.

| Param     | Type                | Description |
|-----------|---------------------|-------------|
| `cache`   | `HlStmtCache *`     | Prepared-statement cache (lookup-or-prepare). |
| `sql`     | `const char *`      | Literal SQL with `?` placeholders. **MUST NOT** contain interpolated user input. |
| `params`  | `const HlValue *`   | Array of `nparams` parameter values, bound to the `?` placeholders in order. May be `NULL` iff `nparams == 0`. |
| `nparams` | `int`               | Length of `params`. |
| `cb`      | `HlRowCallback`     | `int (*)(void *ctx, HlColumn *cols, int ncols)` — called once per row. Return non-zero to abort iteration. |
| `ctx`     | `void *`            | Opaque pointer passed back to `cb`. |
| `alloc`   | `HlAllocator *`     | Allocator for the per-row `HlColumn` array. `NULL` = raw malloc. |

**Returns:** `int` — `0` on full iteration completion, an `HL_DB_ERR_*` code on prepare/bind/step failure, or the value the callback returned to abort.

**Errors:**
- `HL_DB_ERR_PREPARE` — invalid SQL.
- `HL_DB_ERR_BIND` — too many or wrong-typed params.
- `HL_DB_ERR_EXEC` — runtime error during step.
- `HL_DB_ERR_DENIED` — SQL references a reserved `_hull_*` table (call-stack check; stdlib bypasses this).

**Example:**

```c
static int print_row(void *ctx, HlColumn *cols, int ncols) {
    for (int i = 0; i < ncols; i++)
        printf("%s=%s ", cols[i].name, cols[i].value.s);
    printf("\n");
    return 0;
}
HlValue params[] = { { .type = HL_TYPE_INT, .i = 42 } };
hl_cap_db_query(cache, "SELECT id, name FROM users WHERE id = ?",
                params, 1, print_row, NULL, NULL);
```

---

#### `hl_cap_db_exec(cache, sql, params, nparams)`

Run INSERT/UPDATE/DELETE/DDL with parameterised bindings. No row callback.

| Param     | Type              | Description |
|-----------|-------------------|-------------|
| `cache`   | `HlStmtCache *`   | Prepared-statement cache. |
| `sql`     | `const char *`    | Literal SQL with `?` placeholders. |
| `params`  | `const HlValue *` | Bindings (may be `NULL` iff `nparams == 0`). |
| `nparams` | `int`             | Length of `params`. |

**Returns:** `int` — number of affected rows on success (≥ 0), or `HL_DB_ERR_*` on failure.

**See also:** [`hl_cap_db_last_id`](#hl_cap_db_last_iddb).

---

#### `hl_cap_db_last_id(db)`

Return the `ROWID` of the most-recently-inserted row on this connection.

| Param | Type        | Description |
|-------|-------------|-------------|
| `db`  | `sqlite3 *` | Connection. |

**Returns:** `int64_t` — SQLite's `last_insert_rowid()`. `0` if no INSERT has occurred on this connection.

---

#### `hl_cap_db_begin(db)` / `hl_cap_db_commit(db)` / `hl_cap_db_rollback(db)`

Transaction control. `begin` issues `BEGIN IMMEDIATE`; `commit` / `rollback` finalize.

| Param | Type        | Description |
|-------|-------------|-------------|
| `db`  | `sqlite3 *` | Connection. |

**Returns:** `int` — `HL_DB_OK` (0) on success, `HL_DB_ERR_*` on failure.

**Notes:** nested transactions are not supported (SQLite doesn't have them). Use the stdlib `db.batch(fn)` wrapper from app code; this C-level API is for embedders.

---

#### `hl_cap_db_guard_stale_txn(db)`

Roll back any stale transaction left by a crashed handler. Safe to call
unconditionally before each request dispatch — no-op if no transaction is
open.

| Param | Type        | Description |
|-------|-------------|-------------|
| `db`  | `sqlite3 *` | Connection. |

**Returns:** `void`.

---

#### `hl_cap_db_check_namespace(sql)`

Reject SQL referencing the internal `_hull_*` namespace. Used by `hl_cap_db_query`/`exec` to enforce the stdlib/user-code boundary.

| Param | Type           | Description |
|-------|----------------|-------------|
| `sql` | `const char *` | SQL string to scan. |

**Returns:** `int` — `0` if safe, `HL_DB_ERR_DENIED` if the SQL touches `_hull_*` tables.

**Notes:** the actual gate uses call-stack inspection (Lua `ar.source`, JS module name) so stdlib modules transparently bypass this check via their normal `db.exec`/`db.query` calls. Direct calls from C have no caller-source so callers MUST check the namespace themselves before invoking with stdlib-internal SQL.

---

### Filesystem (`cap/fs.h`)

#### `hl_cap_fs_validate(cfg, path, mode)`

Reject a path that would escape the manifest's declared allowlist.

| Param  | Type                 | Description |
|--------|----------------------|-------------|
| `cfg`  | `const HlFsConfig *` | Allowlist of `fs.read` / `fs.write` patterns. |
| `path` | `const char *`       | Relative path to validate (no leading `/`, no `..` components). |
| `mode` | `HlFsMode`           | `HL_FS_READ` or `HL_FS_WRITE`. |

**Returns:** `int` — `0` if the path is allowed under `mode`, `-1` otherwise (with `errno` set on filesystem failures).

**Rejection rules:**
- Absolute paths (starts with `/` or `\`).
- Any `..` component.
- Embedded NUL.
- Symlink targets that resolve outside the app dir (via `realpath` ancestor check).

**Notes:** every `hl_cap_fs_*` mutating function calls this first. Linux/Cosmo additionally `unveil`s the same allowlist so a kernel-level deny applies even if validation is bypassed.

---

#### `hl_cap_fs_read(cfg, path, app_dir, out_buf, out_len)`

Read a file's full contents into a Hull-owned buffer.

| Param      | Type                 | Description |
|------------|----------------------|-------------|
| `cfg`      | `const HlFsConfig *` | Allowlist; `path` is validated against `cfg->read`. |
| `path`     | `const char *`       | Relative path under `app_dir`. |
| `app_dir`  | `const char *`       | App root to join against `path`. |
| `out_buf`  | `char **`            | Receives a malloc'd buffer; caller must `free`. NUL-terminated. |
| `out_len`  | `size_t *`           | Receives the byte length (excludes terminator). |

**Returns:** `int` — `0` ok, `-1` on validate/open/read failure.

**Capacity:** the read is capped at `HL_MODULE_MAX_SIZE` (10 MiB by default).

---

#### `hl_cap_fs_write(cfg, path, app_dir, data, len)`

Write bytes to a file. Creates parent directories if absent. Overwrites existing files.

| Param     | Type                 | Description |
|-----------|----------------------|-------------|
| `cfg`     | `const HlFsConfig *` | Allowlist; path checked against `cfg->write`. |
| `path`    | `const char *`       | Relative path under `app_dir`. |
| `app_dir` | `const char *`       | App root. |
| `data`    | `const char *`       | Bytes to write (may contain NULs). |
| `len`     | `size_t`             | Byte count. |

**Returns:** `int` — `0` ok, `-1` on failure (validate / mkdir / fopen / fwrite).

---

(Remaining `cap/fs.h` entries — `hl_cap_fs_exists`, `_delete`, `_mmap`, `_list_dir` — are documented in the same format. Continuing through the rest of the C surface in the next pass.)

---

## Status

Initial slice complete for `cap/db.h` (full) and `cap/fs.h` (3 of 6
functions). The format is now concrete — please review and confirm
before I batch-produce the rest of the C surface. The remaining work
in this file:

- `cap/fs.h` (3 functions left)
- `cap/crypto.h` (~30 functions: SHA, HMAC, PBKDF2, Ed25519, NaCl, random, base64url, password hash/verify)
- `cap/http.h` (`hl_cap_http_request` + async variants)
- `cap/env.h`, `cap/time.h`, `cap/audit.h`, `cap/body.h`, `cap/ws.h`, `cap/wasm.h`, `cap/gpu.h`, `cap/image.h`, `cap/smtp.h`, `cap/tool.h`, `cap/test.h`
- `runtime.h`, `runtime/factory.h`, `app_context.h`
- `manifest.h`, `vfs.h`, `buffer.h`
- `signature.h`, `release.h`
- `agent_lib.h`, `agent_api.h`
- `commands/*.h`

Estimate: ~250 functions total. About 60 documented after this initial slice (24%).
