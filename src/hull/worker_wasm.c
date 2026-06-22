/*
 * worker_wasm.c — WASM compute worker for thread pool dispatch
 *
 * Follows the same three-callback pattern as worker_db.c:
 *   work_fn  — runs on worker thread (delegates to hl_cap_wasm_call[_buf])
 *   done_fn  — runs on event loop (resume or cleanup if cancelled)
 *   cancel_fn — cleanup for items that never ran
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/worker_wasm.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/utils/alloc.h"
#include "hull/shared/thread_affinity.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "log.h"
#include "keel/thread_pool.h"
#include "keel/async.h"
#include "keel/server.h"

#include <stdlib.h>
#include <string.h>

/* ── work_fn: runs on worker thread ────────────────────────────────── */

static void wasm_work_fn(void *ud)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ud;
    const char *err_msg = NULL;
    int rc;

    if (op->persistent_inst) {
        /* Persistent instance mode */
        if (op->want_buffer) {
            rc = hl_cap_wasm_instance_call_buf(op->persistent_inst,
                                                op->input, op->input_len,
                                                &op->output_buf, &op->opts,
                                                NULL, NULL,
                                                op->alloc, &err_msg);
        } else {
            rc = hl_cap_wasm_instance_call(op->persistent_inst,
                                            op->input, op->input_len,
                                            &op->output, &op->output_len,
                                            &op->opts, NULL, NULL,
                                            op->alloc, &err_msg);
        }
    } else {
        /* Pooled call mode */
        HlWasmCache *cache = (HlWasmCache *)op->wasm_cache;
        if (op->want_buffer) {
            rc = hl_cap_wasm_call_buf(cache, op->name,
                                       op->input, op->input_len,
                                       &op->output_buf, &op->opts,
                                       NULL, NULL,
                                       op->app_vfs, op->app_dir,
                                       op->alloc, &err_msg);
        } else {
            rc = hl_cap_wasm_call(cache, op->name,
                                   op->input, op->input_len,
                                   &op->output, &op->output_len,
                                   &op->opts, NULL, NULL,
                                   op->app_vfs, op->app_dir,
                                   op->alloc, &err_msg);
        }
    }

    if (rc != HL_WASM_OK) {
        op->error = 1;
        op->error_code = rc;
        snprintf(op->error_msg, sizeof(op->error_msg), "%s",
                 err_msg ? err_msg : "internal_error");
    }
}

/* ── done_fn: runs on event loop after work completes ──────────────── */

/* Drop this op's in-flight reservation on its module (event loop only).
 * Pairs with the bump in hl_worker_wasm_submit. Once it reaches zero,
 * hl_cap_wasm_data_load may mutate the module's shared segments again. */
static void wasm_inflight_release(HlWorkerWasmOp *op)
{
    if (op->mod && op->mod->inflight_async > 0)
        op->mod->inflight_async--;
    op->mod = NULL;
}

static void wasm_done_fn(void *ud)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ud;

    wasm_inflight_release(op);

    /* Clear busy flag for persistent instances (event loop thread) */
    if (op->persistent_inst)
        atomic_store(&op->persistent_inst->busy, 0);

    if (atomic_load(&op->cancelled)) {
        HlAsyncCtx *ctx = op->async_ctx;
        hl_worker_wasm_op_free(op);
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

static void wasm_cancel_fn(void *ud)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ud;

    wasm_inflight_release(op);

    /* Clear busy flag for persistent instances */
    if (op->persistent_inst)
        atomic_store(&op->persistent_inst->busy, 0);

    HlAsyncCtx *ctx = op->async_ctx;
    hl_worker_wasm_op_free(op);
    free(op);
    if (ctx) hl_async_ctx_free(ctx);
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_worker_wasm_submit(HlAsyncBackendPool *pool, HlWorkerWasmOp *op)
{
    if (!pool || !op) return -1;

    /* Async submit (and the inflight_async accounting below) is
     * event-loop-only — done/cancel clear the counter on the same thread. */
    hl_assert_on_event_loop("hl_worker_wasm_submit (compute.async)");

    /* Reserve an in-flight slot on the target module BEFORE the worker can
     * run, so a concurrent compute.segment() (hl_cap_wasm_data_load, event
     * loop) cannot free/rebuild segments this call is about to snapshot.
     * Runs on the event loop; the bindings already pre-load the module, so
     * the pooled lookup is a cache hit. Released in done/cancel. */
    if (op->persistent_inst)
        op->mod = op->persistent_inst->module;
    else if (op->wasm_cache)
        op->mod = hl_cap_wasm_module_lookup((HlWasmCache *)op->wasm_cache,
                                            op->name);
    if (op->mod)
        op->mod->inflight_async++;

    const HlAsyncBackend *be = hl_async_backend();
    int rc = be->pool_submit(pool, wasm_work_fn, wasm_done_fn,
                             wasm_cancel_fn, op);
    if (rc != 0)
        wasm_inflight_release(op);  /* op reaches neither done nor cancel */
    return rc;
}

void hl_worker_wasm_op_free(HlWorkerWasmOp *op)
{
    if (!op) return;
    free(op->input);
    hl_alloc_free(op->alloc, op->output, op->output_len);
    if (op->output_buf) {
        hl_wasm_buffer_destroy(op->output_buf);
        hl_alloc_free(op->alloc, op->output_buf, sizeof(HlWasmBuffer));
        op->output_buf = NULL;
    }
    op->input = NULL;
    op->output = NULL;
}

void hl_worker_wasm_op_free_all(void *ptr)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ptr;
    if (!op) return;
    hl_worker_wasm_op_free(op);
    free(op);
}

void hl_worker_wasm_async_cancel(KlAsyncOp *kl_op, void *user_data)
{
    (void)kl_op;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ctx->driver;

    atomic_store(&op->cancelled, 1);

    if (ctx->cont) {
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
    }
}

#endif /* HL_ENABLE_WASM */
