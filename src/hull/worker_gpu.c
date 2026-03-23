/*
 * worker_gpu.c — GPU compute worker for thread pool dispatch
 *
 * Follows the same three-callback pattern as worker_wasm.c:
 *   work_fn  — runs on worker thread (delegates to hl_cap_gpu_dispatch)
 *   done_fn  — runs on event loop (resume or cleanup if cancelled)
 *   cancel_fn — cleanup for items that never ran
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_GPU

#include "hull/worker_gpu.h"
#include "hull/async.h"
#include "hull/limits.h"
#include "log.h"
#include "keel/thread_pool.h"
#include "keel/async.h"
#include "keel/server.h"

#include <stdlib.h>
#include <string.h>

/* ── work_fn: runs on worker thread ────────────────────────────────── */

static void gpu_work_fn(void *ud)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)ud;
    const char *err_msg = NULL;

    /* Wire the deep-copied buffer descs into opts */
    op->opts.buffers = op->buffers;
    op->opts.buffer_count = op->buffer_count;
    op->opts.uniforms = op->uniforms;
    op->opts.uniforms_len = op->uniforms_len;

    int rc = hl_cap_gpu_dispatch(op->gpu_ctx, op->shader_name, &op->opts,
                                 &op->output, &op->output_len, &err_msg);

    if (rc != HL_GPU_OK) {
        op->error = 1;
        op->error_code = rc;
        snprintf(op->error_msg, sizeof(op->error_msg), "%s",
                 err_msg ? err_msg : "internal_error");
    }
}

/* ── done_fn: runs on event loop after work completes ──────────────── */

static void gpu_done_fn(void *ud)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)ud;

    if (atomic_load(&op->cancelled)) {
        HlAsyncCtx *ctx = op->async_ctx;
        hl_worker_gpu_op_free(op);
        free(op);
        if (ctx) hl_async_ctx_free(ctx);
        return;
    }

    kl_async_complete(op->server, &op->async_ctx->op);
}

/* ── cancel_fn: cleanup for items that never ran ───────────────────── */

static void gpu_cancel_fn(void *ud)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)ud;
    HlAsyncCtx *ctx = op->async_ctx;
    hl_worker_gpu_op_free(op);
    free(op);
    if (ctx) hl_async_ctx_free(ctx);
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_worker_gpu_submit(KlThreadPool *pool, HlWorkerGpuOp *op)
{
    if (!pool || !op) return -1;

    KlWorkItem item = {
        .work_fn   = gpu_work_fn,
        .done_fn   = gpu_done_fn,
        .cancel_fn = gpu_cancel_fn,
        .user_data = op,
    };
    return kl_thread_pool_submit(pool, &item);
}

void hl_worker_gpu_op_free(HlWorkerGpuOp *op)
{
    if (!op) return;

    /* Free deep-copied inline buffer data and names */
    if (op->buffer_data) {
        for (int i = 0; i < op->buffer_count; i++)
            free(op->buffer_data[i]);
        free(op->buffer_data);
        op->buffer_data = NULL;
    }
    if (op->buffers) {
        for (int i = 0; i < op->buffer_count; i++)
            free((void *)op->buffers[i].name);
    }
    free(op->buffers);
    op->buffers = NULL;

    free(op->uniforms);
    op->uniforms = NULL;

    free(op->output);
    op->output = NULL;
}

void hl_worker_gpu_op_free_all(void *ptr)
{
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)ptr;
    if (!op) return;
    hl_worker_gpu_op_free(op);
    free(op);
}

void hl_worker_gpu_async_cancel(KlAsyncOp *kl_op, void *user_data)
{
    (void)kl_op;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlWorkerGpuOp *op = (HlWorkerGpuOp *)ctx->driver;

    atomic_store(&op->cancelled, 1);

    if (ctx->cont) {
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
    }
}

#endif /* HL_ENABLE_GPU */
