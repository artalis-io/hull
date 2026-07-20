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


#include "hull/worker_gpu.h"
#include "hull/utils/alloc.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/shared/thread_affinity.h"
#include "hull/net_backend.h"
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
    int rc;

    if (op->is_pipeline) {
        /* Wire deep-copied stages and buffers into pipeline opts */
        op->pipe_opts.stages = op->stages;
        op->pipe_opts.stage_count = op->stage_count;
        if (op->pipe_outputs)
            op->pipe_opts.outputs = op->pipe_outputs;

        /* Wire per-stage buffers and uniforms */
        int buf_off = 0;
        for (int s = 0; s < op->stage_count; s++) {
            if (op->stages[s].buffer_count > 0) {
                op->stages[s].buffers = &op->buffers[buf_off];
                buf_off += op->stages[s].buffer_count;
            }
            if (op->stage_uniforms && op->stage_uniforms[s]) {
                op->stages[s].uniforms = op->stage_uniforms[s];
            }
        }

        rc = hl_cap_gpu_pipeline(op->gpu_ctx, &op->pipe_opts,
                                  &op->pipe_result, &err_msg);
        /* For single-output pipelines, copy to output/output_len for
         * the standard push_result callback */
        if (rc == HL_GPU_OK && op->pipe_result.count == 1) {
            op->output = op->pipe_result.data[0];
            op->output_len = op->pipe_result.len[0];
            op->pipe_result.data[0] = NULL; /* transfer ownership */
            op->pipe_result.count = 0;
        }
    } else {
        /* Wire the deep-copied buffer descs into opts */
        op->opts.buffers = op->buffers;
        op->opts.buffer_count = op->buffer_count;
        op->opts.uniforms = op->uniforms;
        op->opts.uniforms_len = op->uniforms_len;

        /* Fire-and-forget: pass NULL output pointers */
        void **out_ptr = (op->opts.output_buffer >= 0) ? &op->output : NULL;
        size_t *out_len_ptr = (op->opts.output_buffer >= 0) ? &op->output_len : NULL;

        rc = hl_cap_gpu_dispatch(op->gpu_ctx, op->shader_name, &op->opts,
                                 out_ptr, out_len_ptr, &err_msg);
    }

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

    HlAsyncCtx *ctx = op->async_ctx;
    if (ctx->detached)
        hl_async_ctx_resume_detached(ctx);
    else
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
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

int hl_worker_gpu_submit(HlAsyncBackendPool *pool, HlWorkerGpuOp *op)
{
    if (!pool || !op) return -1;
    /* gpu.async submit is event-loop-only; workers only dispatch
     * pre-compiled pipelines, never compile or mutate caches. */
    hl_assert_on_event_loop("hl_worker_gpu_submit (gpu.async)");
    const HlAsyncBackend *be = hl_async_backend();
    return be->pool_submit(pool, gpu_work_fn, gpu_done_fn,
                           gpu_cancel_fn, op);
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
            hl_free_const(op->buffers[i].name);
    }
    free(op->buffers);
    op->buffers = NULL;

    free(op->uniforms);
    op->uniforms = NULL;

    /* Pipeline-specific cleanup */
    if (op->stage_uniforms) {
        for (int s = 0; s < op->stage_count; s++)
            free(op->stage_uniforms[s]);
        free(op->stage_uniforms);
        op->stage_uniforms = NULL;
    }
    if (op->stages) {
        for (int s = 0; s < op->stage_count; s++)
            hl_free_const(op->stages[s].shader);
        free(op->stages);
        op->stages = NULL;
    }
    free(op->pipe_outputs);
    op->pipe_outputs = NULL;
    hl_cap_gpu_pipeline_result_free(&op->pipe_result);

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

