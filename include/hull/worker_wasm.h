/*
 * worker_wasm.h — Runtime-agnostic WASM worker capability
 *
 * Offloads WASM compute.async.call to the thread pool.
 * Follows the same pattern as worker_db.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_WORKER_WASM_H
#define HL_WORKER_WASM_H

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/limits/core.h"  /* HL_WORKER_ERR_SIZE */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlAsyncCtx HlAsyncCtx;
typedef struct HlAllocator HlAllocator;
struct HlVfs;
typedef struct KlServer KlServer;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;
typedef struct KlAsyncOp KlAsyncOp;

/* ── Worker WASM operation ─────────────────────────────────────────── */

typedef struct HlWorkerWasmOp {
    HlAsyncCtx    *async_ctx;
    KlServer      *server;

    /* Input (deep-copied, owned by this struct) */
    void          *wasm_cache;    /* HlWasmCache* for pool operations */
    const struct HlVfs *app_vfs;  /* app VFS for module loading (borrowed) */
    const char    *app_dir;       /* app directory for filesystem fallback (borrowed) */
    char           name[256];     /* module name (for logging) */
    void          *input;         /* deep-copied input bytes */
    size_t         input_len;
    HlWasmCallOpts opts;          /* value-copied options */
    HlAllocator   *alloc;         /* tracked allocator for output (NULL = raw malloc) */

    /* Persistent instance mode (NULL = use pooled call) */
    HlWasmInstance *persistent_inst;

    /* Buffer mode */
    int            want_buffer;   /* 1 = return HlWasmBuffer instead of raw bytes */

    /* Output (set by worker thread) */
    void          *output;        /* malloc'd result buffer (when !want_buffer) */
    size_t         output_len;
    HlWasmBuffer  *output_buf;    /* non-NULL when buffer mode requested */
    int            error;         /* 0 = success */
    int            error_code;    /* HL_WASM_ERR_* */
    char           error_msg[HL_WORKER_ERR_SIZE];
    atomic_int     cancelled;
} HlWorkerWasmOp;

/* Submit a compute.async.call operation to the thread pool.
 * Returns 0 on success, -1 on error (pool full or NULL). */
int hl_worker_wasm_submit(HlAsyncBackendPool *pool, HlWorkerWasmOp *op);

/* Free all resources owned by an HlWorkerWasmOp. */
void hl_worker_wasm_op_free(HlWorkerWasmOp *op);

/* Free HlWorkerWasmOp struct and all owned data (for use as free_driver). */
void hl_worker_wasm_op_free_all(void *ptr);

/* KlAsyncOp on_cancel handler for compute.async operations. */
void hl_worker_wasm_async_cancel(KlAsyncOp *op, void *user_data);

#endif /* HL_ENABLE_WASM */
#endif /* HL_WORKER_WASM_H */
