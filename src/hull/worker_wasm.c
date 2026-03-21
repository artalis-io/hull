/*
 * worker_wasm.c — WASM compute worker for thread pool dispatch
 *
 * Follows the same three-callback pattern as worker_db.c:
 *   work_fn  — runs on worker thread (instantiate + call + cleanup)
 *   done_fn  — runs on event loop (resume or cleanup if cancelled)
 *   cancel_fn — cleanup for items that never ran
 *
 * The wasm_module_t is borrowed from HlWasmCache (immutable after load).
 * Each call creates a fresh wasm_module_inst_t with isolated linear memory.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/worker_wasm.h"
#include "hull/async.h"
#include "hull/limits.h"
#include "log.h"
#include "wasm_export.h"
#include "keel/thread_pool.h"
#include "keel/async.h"
#include "keel/server.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── work_fn: runs on worker thread ────────────────────────────────── */

static void wasm_work_fn(void *ud)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ud;

    uint32_t max_input  = op->opts.max_input  ? op->opts.max_input  : HL_WASM_DEFAULT_MAX_INPUT;
    uint32_t max_output = op->opts.max_output ? op->opts.max_output : HL_WASM_DEFAULT_MAX_OUTPUT;
    uint32_t heap_size  = op->opts.heap_size  ? op->opts.heap_size  : HL_WASM_DEFAULT_HEAP;
    uint32_t stack_size = op->opts.stack_size ? op->opts.stack_size : HL_WASM_DEFAULT_STACK;
    int64_t  gas        = op->opts.gas        ? op->opts.gas        : HL_WASM_DEFAULT_GAS;

    if (max_input > HL_WASM_MAX_IO_SIZE)   max_input = HL_WASM_MAX_IO_SIZE;
    if (max_output > HL_WASM_MAX_IO_SIZE)  max_output = HL_WASM_MAX_IO_SIZE;
    if (heap_size > (uint32_t)HL_WASM_MAX_HEAP) heap_size = (uint32_t)HL_WASM_MAX_HEAP;
    if (stack_size > (uint32_t)HL_WASM_MAX_STACK) stack_size = (uint32_t)HL_WASM_MAX_STACK;
    if (gas > HL_WASM_MAX_GAS) gas = HL_WASM_MAX_GAS;

    if (op->input_len > max_input) {
        op->error = 1;
        op->error_code = HL_WASM_ERR_INPUT;
        snprintf(op->error_msg, sizeof(op->error_msg), "input_too_large");
        return;
    }

    /* Try to acquire from instance pool */
    HlWasmCache *cache = (HlWasmCache *)op->wasm_cache;
    wasm_module_inst_t inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t process_fn = NULL;
    int from_pool = 0;
    HlWasmModule *cached_mod = NULL;

    if (cache) {
        /* Find the module entry for pool operations */
        for (int i = 0; i < cache->count; i++) {
            if (cache->modules[i].module == op->module) {
                cached_mod = &cache->modules[i];
                break;
            }
        }
        if (cached_mod) {
            pthread_mutex_lock(&cache->pool_mutex);
            HlWasmPool *pool = &cached_mod->pool;
            for (int i = 0; i < pool->count; i++) {
                HlWasmPoolEntry *e = &pool->entries[i];
                if (e->heap_size == heap_size && e->stack_size == stack_size) {
                    inst = (wasm_module_inst_t)e->instance;
                    exec_env = (wasm_exec_env_t)e->exec_env;
                    process_fn = (wasm_function_inst_t)e->process_fn;
                    pool->entries[i] = pool->entries[pool->count - 1];
                    pool->count--;
                    from_pool = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&cache->pool_mutex);
        }
    }

    if (!from_pool) {
        char error_buf[256];
        inst = wasm_runtime_instantiate(
            (wasm_module_t)op->module, stack_size, heap_size,
            error_buf, sizeof(error_buf));
        if (!inst) {
            op->error = 1;
            op->error_code = HL_WASM_ERR_INTERNAL;
            snprintf(op->error_msg, sizeof(op->error_msg), "internal_error");
            log_error("[wasm-worker] instantiate '%s' failed: %s", op->name, error_buf);
            return;
        }

        process_fn = wasm_runtime_lookup_function(inst, "hull_process");
        if (!process_fn) {
            op->error = 1;
            op->error_code = HL_WASM_ERR_NOT_FOUND;
            snprintf(op->error_msg, sizeof(op->error_msg), "no_hull_process_export");
            wasm_runtime_deinstantiate(inst);
            return;
        }

        exec_env = wasm_runtime_create_exec_env(inst, stack_size);
        if (!exec_env) {
            op->error = 1;
            op->error_code = HL_WASM_ERR_INTERNAL;
            snprintf(op->error_msg, sizeof(op->error_msg), "internal_error");
            wasm_runtime_deinstantiate(inst);
            return;
        }
    }

    /* Gas metering */
    if (gas > 0) {
        int gas_int = (gas > INT_MAX) ? INT_MAX : (int)gas;
        wasm_runtime_set_instruction_count_limit(exec_env, gas_int);
    }

    /* Allocate input in WASM linear memory */
    void *native_in = NULL;
    uint64_t wasm_in_ptr = 0;
    if (op->input_len > 0) {
        wasm_in_ptr = wasm_runtime_module_malloc(inst, (uint64_t)op->input_len, &native_in);
        if (!wasm_in_ptr || !native_in) {
            op->error = 1;
            op->error_code = HL_WASM_ERR_INTERNAL;
            snprintf(op->error_msg, sizeof(op->error_msg), "internal_error");
            wasm_runtime_destroy_exec_env(exec_env);
            wasm_runtime_deinstantiate(inst);
            return;
        }
        memcpy(native_in, op->input, op->input_len);
    }

    /* Allocate output in WASM linear memory */
    void *native_out = NULL;
    uint64_t wasm_out_ptr = 0;
    if (max_output > 0) {
        wasm_out_ptr = wasm_runtime_module_malloc(inst, (uint64_t)max_output, &native_out);
        if (!wasm_out_ptr || !native_out) {
            op->error = 1;
            op->error_code = HL_WASM_ERR_INTERNAL;
            snprintf(op->error_msg, sizeof(op->error_msg), "internal_error");
            if (wasm_in_ptr) wasm_runtime_module_free(inst, wasm_in_ptr);
            wasm_runtime_destroy_exec_env(exec_env);
            wasm_runtime_deinstantiate(inst);
            return;
        }
    }

    /* Call hull_process */
    uint32_t argv[4] = {
        (uint32_t)wasm_in_ptr,
        (uint32_t)op->input_len,
        (uint32_t)wasm_out_ptr,
        max_output,
    };

    if (!wasm_runtime_call_wasm(exec_env, process_fn, 4, argv)) {
        const char *exception = wasm_runtime_get_exception(inst);
        if (exception && strstr(exception, "instruction count")) {
            op->error_code = HL_WASM_ERR_GAS;
            snprintf(op->error_msg, sizeof(op->error_msg), "gas_exhausted");
        } else {
            op->error_code = HL_WASM_ERR_INTERNAL;
            snprintf(op->error_msg, sizeof(op->error_msg), "call_failed");
        }
        op->error = 1;
        goto cleanup;
    }

    /* Process return value */
    int32_t result = (int32_t)argv[0];
    if (result < 0) {
        op->error = 1;
        op->error_code = (result == -2) ? HL_WASM_ERR_OUTPUT : HL_WASM_ERR_INTERNAL;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 (result == -2) ? "output_too_small" : "call_failed");
        goto cleanup;
    }

    /* Copy output */
    if (result > 0 && (uint32_t)result <= max_output) {
        op->output = malloc((size_t)result);
        if (op->output) {
            memcpy(op->output, native_out, (size_t)result);
            op->output_len = (size_t)result;
        } else {
            op->error = 1;
            op->error_code = HL_WASM_ERR_INTERNAL;
            snprintf(op->error_msg, sizeof(op->error_msg), "internal_error");
        }
    }

cleanup:
    if (wasm_in_ptr) wasm_runtime_module_free(inst, wasm_in_ptr);
    if (wasm_out_ptr) wasm_runtime_module_free(inst, wasm_out_ptr);

    /* Return to pool on success, destroy on error */
    if (cache && cached_mod && !op->error &&
        heap_size <= HL_WASM_POOL_HEAP_THRESHOLD) {
        pthread_mutex_lock(&cache->pool_mutex);
        HlWasmPool *pool = &cached_mod->pool;
        if (pool->count < HL_WASM_POOL_MAX) {
            wasm_runtime_clear_exception(inst);
            HlWasmPoolEntry *e = &pool->entries[pool->count++];
            e->instance   = inst;
            e->exec_env   = exec_env;
            e->process_fn = process_fn;
            e->heap_size  = heap_size;
            e->stack_size = stack_size;
            pthread_mutex_unlock(&cache->pool_mutex);
            return;
        }
        pthread_mutex_unlock(&cache->pool_mutex);
    }
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(inst);
}

/* ── done_fn: runs on event loop after work completes ──────────────── */

static void wasm_done_fn(void *ud)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ud;

    if (op->cancelled) {
        HlAsyncCtx *ctx = op->async_ctx;
        hl_worker_wasm_op_free(op);
        free(op);
        if (ctx) hl_async_ctx_free(ctx);
        return;
    }

    kl_async_complete(op->server, &op->async_ctx->op);
}

/* ── cancel_fn: cleanup for items that never ran ───────────────────── */

static void wasm_cancel_fn(void *ud)
{
    HlWorkerWasmOp *op = (HlWorkerWasmOp *)ud;
    HlAsyncCtx *ctx = op->async_ctx;
    hl_worker_wasm_op_free(op);
    free(op);
    if (ctx) hl_async_ctx_free(ctx);
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_worker_wasm_submit(KlThreadPool *pool, HlWorkerWasmOp *op)
{
    if (!pool || !op) return -1;

    KlWorkItem item = {
        .work_fn   = wasm_work_fn,
        .done_fn   = wasm_done_fn,
        .cancel_fn = wasm_cancel_fn,
        .user_data = op,
    };
    return kl_thread_pool_submit(pool, &item);
}

void hl_worker_wasm_op_free(HlWorkerWasmOp *op)
{
    if (!op) return;
    free(op->input);
    free(op->output);
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

    op->cancelled = 1;

    if (ctx->cont) {
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
    }
}

#endif /* HL_ENABLE_WASM */
