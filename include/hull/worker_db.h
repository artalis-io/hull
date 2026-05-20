/*
 * worker_db.h — Runtime-agnostic DB worker capability
 *
 * Manages per-worker SQLite connections (thread-local) and provides
 * HlWorkerDbOp for offloading db.async.query/exec to the thread pool.
 * Zero Lua/JS knowledge — purely C + SQLite.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_WORKER_DB_H
#define HL_WORKER_DB_H

#include "hull/cap/types.h"
#include "hull/cap/db.h"
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
    sqlite3      *db;
    HlStmtCache   cache;
} HlWorkerDb;

/* Initialize the worker DB capability. Call once before workers spawn.
 * Stores db_path (must remain valid for process lifetime). */
void hl_worker_db_init(const char *db_path);

/* Get current worker thread's DB connection (lazy open on first call).
 * Returns NULL on error. Thread-safe: each thread gets its own connection. */
HlWorkerDb *hl_worker_db_get(void);

/*
 * Shared "get worker DB + check namespace + prepare statement" helper
 * for runtime bindings. Both Lua and JS worker_db.c bindings used to
 * duplicate this 6-line dance at the top of every query/exec function;
 * folded into one place (roadmap item L / M9).
 *
 * Returns 0 on success: *out_db and *out_stmt are filled, caller is
 * responsible for sqlite3_finalize(*out_stmt).
 * Returns -1 on failure: *out_err points to a static or sqlite3_errmsg
 * string that's valid until the next sqlite3 call on *out_db.
 */
int hl_worker_db_get_and_prepare(const char    *sql,
                                 sqlite3      **out_db,
                                 sqlite3_stmt **out_stmt,
                                 const char   **out_err);

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
