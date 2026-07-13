/*
 * worker_db.h — Runtime-agnostic DB worker capability
 *
 * Manages per-worker database connections (thread-local) and provides
 * HlWorkerDbOp for offloading db.async.query/exec to the thread pool.
 * Backend-agnostic: each worker thread opens its own connection through
 * the HlDbBackend vtable (SQLite file or PostgreSQL DSN), so the async
 * and per-thread-runtime paths work transparently on either backend.
 * Zero Lua/JS knowledge: purely C.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_WORKER_DB_H
#define HL_WORKER_DB_H

#include "hull/cap/types.h"
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"  /* HlDbHandle */
#include "hull/limits/core.h"  /* HL_WORKER_ERR_SIZE */
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlAsyncCtx HlAsyncCtx;
typedef struct HlAllocator HlAllocator;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;
typedef struct KlServer KlServer;
typedef struct KlAsyncOp KlAsyncOp;

/* ── Per-worker DB context (thread-local, lazy init) ───────────────── */

typedef struct HlWorkerDb {
    HlDbHandle handle;   /* vtable-opened, owned by this worker thread */
} HlWorkerDb;

/* Initialize the worker DB capability. Call once before workers spawn.
 * Stores the DSN (a SQLite file path or a postgres:// URL); must remain
 * valid for the process lifetime. */
void hl_worker_db_init(const char *dsn);

/* Get current worker thread's DB connection (lazy open on first call).
 * Returns NULL on error. Thread-safe: each thread gets its own connection. */
HlWorkerDb *hl_worker_db_get(void);

/*
 * Shared "get worker DB + check internal-table namespace" helper for the
 * per-thread runtime bindings (runtime/{lua,js}/worker_db.c). Returns the
 * worker's backend handle on success, or NULL with *out_err set to a static
 * message on failure (no connection, or the SQL touches a reserved _hull_*
 * table). Callers then run the query/exec through the HlDbBackend vtable
 * (hl_db_query / hl_db_exec) so the path is backend-agnostic.
 */
HlDbHandle *hl_worker_db_handle_checked(const char *sql, const char **out_err);

/* ── Materialized query result (deep-copied, worker-thread safe) ──── */

typedef struct HlDbValue {
    HlValueType  type;
    int64_t      i;
    double       d;
    char        *s;       /* heap copy, owned */
    size_t       len;
} HlDbValue;

typedef struct HlDbResult {
    char       **col_names;   /* ncols heap-copied strings */
    int          ncols;
    HlDbValue   *values;      /* flat array: nrows * ncols */
    int          nrows;
    int          capacity;    /* allocated rows */
} HlDbResult;

void hl_db_result_free(HlDbResult *r);

/* ── Worker DB operation ───────────────────────────────────────────── */

typedef enum { HL_WORK_DB_QUERY, HL_WORK_DB_EXEC } HlWorkerDbKind;

typedef struct HlWorkerDbOp {
    HlWorkerDbKind kind;
    HlAsyncCtx    *async_ctx;
    HlAllocator   *alloc;
    KlServer      *server;

    /* Input (deep-copied, owned) */
    char          *sql;
    HlValue       *params;
    int            nparams;

    /* Output (set by worker thread) */
    HlDbResult     result;        /* DB_QUERY */
    int            exec_changes;  /* DB_EXEC */
    int64_t        last_id;       /* DB_EXEC */
    int            error;
    char           error_msg[HL_WORKER_ERR_SIZE];
    int            cancelled;     /* set by async cancel, checked by done_fn */
} HlWorkerDbOp;

/* Submit a db.async operation to the thread pool.
 * Returns 0 on success, -1 on error (pool full or NULL). */
int hl_worker_db_submit(HlAsyncBackendPool *pool, HlWorkerDbOp *op);

/* Free all resources owned by an HlWorkerDbOp (sql, params, result). */
void hl_worker_db_op_free(HlWorkerDbOp *op);

/* Free HlWorkerDbOp struct and all owned data (for use as free_driver). */
void hl_worker_db_op_free_all(void *ptr);

/* KlAsyncOp on_cancel handler for db.async operations.
 * Marks op as cancelled so done_fn skips kl_async_complete.
 * Wire as ctx->op.on_cancel after hl_async_ctx_create. */
void hl_worker_db_async_cancel(KlAsyncOp *op, void *user_data);

/* Deep-copy HlValue params array. String/blob values get heap copies.
 * Returns NULL on failure or if n <= 0. Caller frees strings + array. */
HlValue *hl_deep_copy_params(const HlValue *src, int n);

#endif /* HL_WORKER_DB_H */
