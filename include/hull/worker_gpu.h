/*
 * worker_gpu.h - Runtime-agnostic GPU worker capability
 *
 * Offloads gpu.async.dispatch to the thread pool.
 * Follows the same pattern as worker_wasm.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_WORKER_GPU_H
#define HL_WORKER_GPU_H


#include "hull/cap/gpu.h"
#include "hull/limits/core.h"  /* HL_WORKER_ERR_SIZE (HL_GPU_NAME_MAX via cap/gpu.h) */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
typedef struct HlAsyncCtx HlAsyncCtx;
typedef struct KlServer KlServer;
typedef struct HlAsyncBackendPool HlAsyncBackendPool;
typedef struct KlAsyncOp KlAsyncOp;

/* ── Worker GPU operation ──────────────────────────────────────────── */

typedef struct HlWorkerGpuOp {
    HlAsyncCtx    *async_ctx;
    KlServer      *server;

    /* Input */
    HlGpuCtx      *gpu_ctx;          /* GPU context (borrowed) */
    int             is_pipeline;      /* 0 = dispatch, 1 = pipeline */

    /* Dispatch mode (is_pipeline == 0) */
    char            shader_name[HL_GPU_NAME_MAX];
    HlGpuDispatchOpts opts;           /* value-copied options */

    /* Pipeline mode (is_pipeline == 1) */
    HlGpuPipelineOpts pipe_opts;      /* value-copied */
    HlGpuPipelineStage *stages;       /* deep-copied stage array */
    int              stage_count;
    HlGpuPipelineOutput *pipe_outputs;/* deep-copied output specs */
    void           **stage_uniforms;  /* deep-copied per-stage uniforms */

    /* Shared: inline buffer descriptors (deep-copied) */
    HlGpuBufferDesc *buffers;         /* deep-copied buffer descs */
    void           **buffer_data;     /* deep-copied inline data pointers */
    int              buffer_count;
    void            *uniforms;        /* deep-copied uniform data (dispatch only) */
    size_t           uniforms_len;

    /* Output (set by worker thread) */
    void          *output;
    size_t         output_len;
    HlGpuPipelineResult pipe_result;  /* pipeline multi-output */
    int            error;
    int            error_code;
    char           error_msg[HL_WORKER_ERR_SIZE];
    atomic_int     cancelled;
} HlWorkerGpuOp;

/* Submit a gpu.async.dispatch operation to the thread pool.
 * Returns 0 on success, -1 on error (pool full or NULL). */
int hl_worker_gpu_submit(HlAsyncBackendPool *pool, HlWorkerGpuOp *op);

/* Free all resources owned by an HlWorkerGpuOp. */
void hl_worker_gpu_op_free(HlWorkerGpuOp *op);

/* Free HlWorkerGpuOp struct and all owned data (for use as free_driver). */
void hl_worker_gpu_op_free_all(void *ptr);

/* KlAsyncOp on_cancel handler for gpu.async operations. */
void hl_worker_gpu_async_cancel(KlAsyncOp *op, void *user_data);

#endif /* HL_WORKER_GPU_H */
