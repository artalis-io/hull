/*
 * worker_wasm.h - Runtime-agnostic WASM worker capability
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
#include "hull/cap/wasm_spans.h" /* HL_WASM_MAX_SPANS */
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

    /* Module this op targets, resolved at submit time on the event loop
     * (pooled: looked up by name; persistent: persistent_inst->module).
     * Used to bump/clear mod->inflight_async so segment mutation can be
     * refused while this call is outstanding. NULL if the module was not
     * resolvable at submit (the worker then fails as before). */
    HlWasmModule  *mod;

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

    /* Per-invocation mapped spans (mapped-spans item D.4). Deep-copied +
     * submission-pinned at the binding (event loop) before submit: span_names
     * OWNS the name bytes, span_reqs[i].name points into span_names[i], and each
     * span_reqs[i].buf is borrow-pinned (span_pins counts how many). opts.spans
     * points at span_reqs (op-owned), so no Lua/JS-managed pointer crosses
     * submission. The pins are released in hl_worker_wasm_op_free -- AFTER the
     * worker call's own span-set teardown -- on success, trap, and cancellation. */
    HlWasmSpanReq span_reqs[HL_WASM_MAX_SPANS];
    char          span_names[HL_WASM_MAX_SPANS][64];
    int           span_pins;
} HlWorkerWasmOp;

/* Adopt validated span requests into the op: deep-copy each name into the op,
 * point span_reqs at the owned names + the caller's buffers, submission-pin every
 * buffer, and set opts.spans/opts.span_count. Runs on the event loop, after op
 * allocation, before submit. n == 0 leaves the op a plain (no-spans) call. The
 * pins are released by hl_worker_wasm_op_free. */
void hl_worker_wasm_adopt_spans(HlWorkerWasmOp *op, const HlWasmSpanReq *reqs, int n);

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
