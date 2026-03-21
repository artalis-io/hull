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
#include "hull/limits.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlAsyncCtx HlAsyncCtx;
typedef struct HlAllocator HlAllocator;
typedef struct KlServer KlServer;
typedef struct KlThreadPool KlThreadPool;
typedef struct KlAsyncOp KlAsyncOp;

/* ── Worker WASM operation ─────────────────────────────────────────── */

typedef struct HlWorkerWasmOp {
    HlAsyncCtx    *async_ctx;
    KlServer      *server;

    /* Input (deep-copied, owned by this struct) */
    void          *module;        /* wasm_module_t (borrowed from cache, immutable) */
    void          *wasm_cache;    /* HlWasmCache* for pool operations */
    char           name[256];     /* module name (for logging) */
    void          *input;         /* deep-copied input bytes */
    size_t         input_len;
    HlWasmCallOpts opts;          /* value-copied options */

    /* Output (set by worker thread) */
    void          *output;        /* malloc'd result buffer */
    size_t         output_len;
    int            error;         /* 0 = success */
    int            error_code;    /* HL_WASM_ERR_* */
    char           error_msg[HL_WORKER_ERR_SIZE];
    int            cancelled;
} HlWorkerWasmOp;

/* Submit a compute.async.call operation to the thread pool.
 * Returns 0 on success, -1 on error (pool full or NULL). */
int hl_worker_wasm_submit(KlThreadPool *pool, HlWorkerWasmOp *op);

/* Free all resources owned by an HlWorkerWasmOp. */
void hl_worker_wasm_op_free(HlWorkerWasmOp *op);

/* Free HlWorkerWasmOp struct and all owned data (for use as free_driver). */
void hl_worker_wasm_op_free_all(void *ptr);

/* KlAsyncOp on_cancel handler for compute.async operations. */
void hl_worker_wasm_async_cancel(KlAsyncOp *op, void *user_data);

#endif /* HL_ENABLE_WASM */
#endif /* HL_WORKER_WASM_H */
