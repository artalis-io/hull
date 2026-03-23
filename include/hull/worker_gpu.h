/*
 * worker_gpu.h — Runtime-agnostic GPU worker capability
 *
 * Offloads gpu.async.dispatch to the thread pool.
 * Follows the same pattern as worker_wasm.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_WORKER_GPU_H
#define HL_WORKER_GPU_H

#ifdef HL_ENABLE_GPU

#include "hull/cap/gpu.h"
#include "hull/limits.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlAsyncCtx HlAsyncCtx;
typedef struct KlServer KlServer;
typedef struct KlThreadPool KlThreadPool;
typedef struct KlAsyncOp KlAsyncOp;

/* ── Worker GPU operation ──────────────────────────────────────────── */

typedef struct HlWorkerGpuOp {
    HlAsyncCtx    *async_ctx;
    KlServer      *server;

    /* Input */
    HlGpuCtx      *gpu_ctx;          /* GPU context (borrowed) */
    char            shader_name[HL_GPU_NAME_MAX];
    HlGpuDispatchOpts opts;           /* value-copied options */

    /* Inline buffer descriptors (deep-copied) */
    HlGpuBufferDesc *buffers;         /* deep-copied buffer descs */
    void           **buffer_data;     /* deep-copied inline data pointers */
    int              buffer_count;
    void            *uniforms;        /* deep-copied uniform data */
    size_t           uniforms_len;

    /* Output (set by worker thread) */
    void          *output;
    size_t         output_len;
    int            error;
    int            error_code;
    char           error_msg[HL_WORKER_ERR_SIZE];
    atomic_int     cancelled;
} HlWorkerGpuOp;

/* Submit a gpu.async.dispatch operation to the thread pool.
 * Returns 0 on success, -1 on error (pool full or NULL). */
int hl_worker_gpu_submit(KlThreadPool *pool, HlWorkerGpuOp *op);

/* Free all resources owned by an HlWorkerGpuOp. */
void hl_worker_gpu_op_free(HlWorkerGpuOp *op);

/* Free HlWorkerGpuOp struct and all owned data (for use as free_driver). */
void hl_worker_gpu_op_free_all(void *ptr);

/* KlAsyncOp on_cancel handler for gpu.async operations. */
void hl_worker_gpu_async_cancel(KlAsyncOp *op, void *user_data);

#endif /* HL_ENABLE_GPU */
#endif /* HL_WORKER_GPU_H */
