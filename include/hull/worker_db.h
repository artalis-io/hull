/*
 * worker_db.h - Runtime-agnostic DB worker capability
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
#include "hull/cap/db_budget.h"   /* HlDbOpBudget */
#include "hull/limits/core.h"  /* HL_WORKER_ERR_SIZE */
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlAsyncCtx HlAsyncCtx;
typedef struct HlAllocator HlAllocator;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;
typedef struct KlHttpServer KlHttpServer;
typedef struct KlAsyncOp KlAsyncOp;

/* ── Per-worker DB context (thread-local, lazy init) ───────────────── */

typedef struct HlWorkerDb {
    HlDbHandle handle;   /* vtable-opened, owned by this worker thread */
} HlWorkerDb;

/* Initialize the worker DB capability. Call once before workers spawn.
 * Stores the DSN (a SQLite file path or a postgres:// URL); must remain
 * valid for the process lifetime. */
void hl_worker_db_init(const char *dsn);

/* Get current worker thread's DB connection to the DEFAULT database (lazy
 * open on first call). Returns NULL on error. Thread-safe: each thread gets
 * its own connection. Equivalent to hl_worker_db_get_for(NULL). */
HlWorkerDb *hl_worker_db_get(void);

/*
 * Get this worker thread's connection to the database identified by @p dsn
 * (lazy open on first call, cached per-thread per-DSN). @p dsn NULL selects
 * the default database (the one passed to hl_worker_db_init). Lets db.async
 * on a named connection object target its own database rather than always the
 * default. Returns NULL on backend-select / open failure. Thread-safe: the
 * cache is thread-local, so no locking and no cross-thread connection sharing.
 */
HlWorkerDb *hl_worker_db_get_for(const char *dsn);

/*
 * Drop this thread's cached connection to @p dsn (NULL = default), closing it,
 * so the next hl_worker_db_get_for(@p dsn) reopens a fresh one. Used to recover
 * a connection the caller found dead (e.g. a LISTEN/NOTIFY wait that returned
 * "connection lost"): the next use reconnects and re-establishes any per-
 * connection state (LISTEN). A no-op if this thread has no such cached
 * connection. Thread-safe: touches only the calling thread's local cache.
 */
void hl_worker_db_invalidate(const char *dsn);

/*
 * End of a worker.dispatch job on this thread: roll back any transaction
 * the job left open on this thread's pooled connections (dropping one whose
 * rollback does not take), so it cannot hold locks or run later jobs and
 * db.async ops inside it. Returns 1 when one was open, with a message for
 * the failed job in @p err (may be NULL); 0 otherwise. Never opens a
 * connection.
 */
int hl_worker_db_end_job(char *err, size_t errsz);

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

/* Run @p sql on @p h and copy its rows into @p out - what a db.async query
 * does on its pool thread. The copy is bounded by @p max_bytes (0: only
 * HL_WORKER_DB_RESULT_MAX_BYTES) and the 100k-row cap, and charged to the
 * budget bound on this thread. When the copy fails (too large, too many rows,
 * rows of another width, out of memory) @p budget - the op's, bound on this
 * thread - is tripped, so a network backend closes the connection at its next
 * row instead of reading the rest of the reply (audit 16). Returns 0; -1 when
 * the query failed (@p err "query: ..."); -2 when the result was refused
 * (@p err says why, @p out is empty). */
int hl_worker_db_materialize(HlDbHandle *h, const char *sql,
                             const HlValue *params, int nparams,
                             size_t max_bytes, HlDbOpBudget *budget,
                             HlDbResult *out, char *err, size_t errsz);

/* ── Worker DB operation ───────────────────────────────────────────── */

typedef enum { HL_WORK_DB_QUERY, HL_WORK_DB_EXEC,
               HL_WORK_DB_WAIT_NOTIFY } HlWorkerDbKind;

/* The most a db.async query's materialized result may hold (column data plus
 * per-cell bookkeeping), whatever the submitting VM's heap limit. */
#define HL_WORKER_DB_RESULT_MAX_BYTES ((size_t)256 * 1024 * 1024)

/* conn.wait_notify / waitNotify: the timeout is clamped to 0..this many ms
 * (audit 14), so one call cannot hold a pool thread for days. */
#define HL_WORKER_DB_NOTIFY_MAX_MS 60000

/* A wait_notify timeout as asked for (any number), clamped to
 * 0..HL_WORKER_DB_NOTIFY_MAX_MS; NaN is 0. */
int hl_worker_db_notify_timeout(double ms);

typedef struct HlWorkerDbOp {
    HlWorkerDbKind kind;
    HlAsyncCtx    *async_ctx;
    HlAllocator   *alloc;
    KlHttpServer      *server;

    /* Input (deep-copied, owned) */
    char          *sql;
    char          *dsn;    /* owned; target database DSN, NULL = default. The
                            * worker opens its per-thread connection against
                            * this so db.async on a named connection hits the
                            * right database. */
    HlValue       *params;
    int            nparams;
    /* A db.open (dynamic) DSN: the worker closes the connection it opened
     * for the op instead of caching it. Cached, each worker thread kept up to
     * 32 per-tenant connections past the app's close(), well beyond the
     * process-wide cap of 16 dynamic connections. A connection the thread
     * already had for the same DSN (a named / default connection's) is left
     * alone. A WAIT_NOTIFY keeps its LISTEN connection, tagged with
     * @p dyn_id (hl_db_dynamic_id), only while that handle stays open: once
     * the app closes it, the worker closes the connection (audit 6 L7). */
    int            no_cache;
    uint64_t       dyn_id;
    /* The submitting VM's instruction limit (0 = none): the op's SQL is
     * charged to an HlDbOpBudget of its own (cap/db_budget.h), so a runaway
     * statement does not hold a pool thread for good. */
    int64_t        max_instructions;
    /* The submitting VM's heap limit (0 = none): a DB_QUERY result is copied
     * into plain malloc on the worker thread before the VM converts it, so
     * the copy is bounded by this (and HL_WORKER_DB_RESULT_MAX_BYTES) and
     * charged to the op budget at 1 unit per HL_DB_ALLOC_UNIT_BYTES - an
     * unbounded result exhausted the process's memory (audit 15). */
    size_t         max_result_bytes;

    /* WAIT_NOTIFY input: LISTEN channel (owned) + wait bound in ms. */
    char          *channel;
    int            timeout_ms;

    /* Output (set by worker thread) */
    HlDbResult     result;        /* DB_QUERY */
    int            exec_changes;  /* DB_EXEC */
    int64_t        last_id;       /* DB_EXEC */
    int            notified;      /* DB_WAIT_NOTIFY: 1 notified, 0 timeout */
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
