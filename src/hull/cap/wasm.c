/*
 * hull_cap_wasm.c — WASM compute capability (WAMR)
 *
 * Module cache + WAMR lifecycle + call dispatch + host_call native.
 * Compute-only: NO WASI, NO I/O — pure functions only.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/alloc.h"
#include "hull/cap/audit.h"
#include "hull/limits.h"
#include "hull/vfs.h"
#include "log.h"
#include "wasm_export.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

_Static_assert(sizeof(uint32_t) == 4, "WASM32 ABI requires 4-byte uint32_t");
_Static_assert(HL_WASM_CALLBACK_BUF_SIZE <= 65536,
               "callback buffer must fit on stack (max 64 KB)");

/* ── Architecture suffix for AOT lookup ────────────────────────────── */

static const char *wasm_arch_suffix(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#else
    return NULL;
#endif
}

/* ── Thread-local callback context for host_call ───────────────────── */

typedef struct {
    HlWasmCallbackFn fn;
    void            *ctx;
    /* Shared data segments visible to current call */
    const HlWasmSharedData *shared_data;  /* NULL = no shared data */
} HlHostCallCtx;

static _Thread_local HlHostCallCtx tl_host_ctx;

/* ── host_call native implementation ───────────────────────────────── */

static int32_t host_call_handler(wasm_exec_env_t exec_env,
                                 int32_t opcode, int32_t ptr, int32_t len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    if (opcode == HL_WASM_OP_LOG) {
        if (len > 0 && len < 4096) {
            if (!wasm_runtime_validate_app_addr(inst, (uint64_t)ptr, (uint64_t)len))
                return -1;
            char *msg = wasm_runtime_addr_app_to_native(inst, (uint64_t)ptr);
            if (msg)
                log_info("[wasm] %.*s", len, msg);
        }
        return 0;
    }

    if (opcode == HL_WASM_OP_CALLBACK) {
        if (!tl_host_ctx.fn)
            return -1;

        /* Extract callback ID from first 4 bytes of ptr data */
        if (len < 4)
            return -1;
        if (!wasm_runtime_validate_app_addr(inst, (uint64_t)ptr, (uint64_t)len))
            return -1;

        uint8_t *data = wasm_runtime_addr_app_to_native(inst, (uint64_t)ptr);
        if (!data)
            return -1;

        uint32_t cb_id_u = (uint32_t)data[0] | ((uint32_t)data[1] << 8)
                         | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        if (cb_id_u > (uint32_t)INT_MAX)
            return -1;
        int cb_id = (int)cb_id_u;
        const void *cb_in = data + 4;
        size_t cb_in_len = (size_t)(len - 4);

        /* Callback receives input from WASM and returns a status code.
         * out_buf is provided for future use (host→guest data return)
         * but is not currently copied back to WASM linear memory —
         * only the integer return value is visible to the guest. */
        char out_buf[HL_WASM_CALLBACK_BUF_SIZE];
        int rc = tl_host_ctx.fn(cb_id, cb_in, cb_in_len,
                                out_buf, sizeof(out_buf), tl_host_ctx.ctx);
        if (rc < 0 || (uint32_t)rc > HL_WASM_CALLBACK_BUF_SIZE)
            return -1;
        return (int32_t)rc;
    }

    if (opcode == HL_WASM_OP_DATA_INFO) {
        const HlWasmSharedData *sd = tl_host_ctx.shared_data;
        if (!sd)
            return 0;
        /* ptr == -1: return segment count */
        if (ptr == -1)
            return (int32_t)sd->count;
        /* ptr >= 0: segment index */
        if (ptr < 0 || ptr >= sd->count)
            return 0;
        if (len == 0)
            return (int32_t)sd->segments[ptr].wasm_addr;
        if (len == 1)
            return (int32_t)sd->segments[ptr].size;
        return -1;
    }

    return -1; /* unknown opcode */
}

static NativeSymbol host_symbols[] = {
    { "host_call", (void *)host_call_handler, "(iii)i", NULL },
};

/* ── Module name validation ────────────────────────────────────────── */

static int validate_module_name(const char *name)
{
    if (!name || !name[0])
        return -1;
    if (strlen(name) > 255)
        return -1;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\')
            return -1;
    }
    if (name[0] == '.')
        return -1;
    return 0;
}

/* ── Cache lookup ──────────────────────────────────────────────────── */

static HlWasmModule *cache_find(HlWasmCache *cache, const char *name)
{
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->modules[i].name, name) == 0)
            return &cache->modules[i];
    }
    return NULL;
}

/* ── Instance pool ─────────────────────────────────────────────────── */

/* Drain all pooled instances for a module. Caller must hold pool_mutex. */
static void pool_drain(HlWasmPool *pool)
{
    for (int i = 0; i < pool->count; i++) {
        HlWasmPoolEntry *e = &pool->entries[i];
        wasm_runtime_destroy_exec_env((wasm_exec_env_t)e->exec_env);
        wasm_runtime_deinstantiate((wasm_module_inst_t)e->instance);
    }
    pool->count = 0;
}

/* Try to acquire a pooled instance matching (heap_size, stack_size).
 * Returns 1 and fills out_inst/out_env/out_fn on hit, 0 on miss.
 * Always snapshots shared data pointers under the mutex for thread safety. */
static int pool_acquire(HlWasmCache *cache, HlWasmModule *mod,
                        uint32_t heap_size, uint32_t stack_size,
                        wasm_module_inst_t *out_inst,
                        wasm_exec_env_t *out_env,
                        wasm_function_inst_t *out_fn,
                        const HlWasmSharedData **out_sd,
                        void **out_chain_head)
{
    pthread_mutex_lock(&cache->pool_mutex);
    /* Snapshot shared data under the lock */
    *out_sd = mod->shared_data;
    *out_chain_head = mod->shared_data ? mod->shared_data->chain_head : NULL;
    HlWasmPool *pool = &mod->pool;
    for (int i = 0; i < pool->count; i++) {
        HlWasmPoolEntry *e = &pool->entries[i];
        if (e->heap_size == heap_size && e->stack_size == stack_size) {
            *out_inst = (wasm_module_inst_t)e->instance;
            *out_env  = (wasm_exec_env_t)e->exec_env;
            *out_fn   = (wasm_function_inst_t)e->process_fn;
            /* Swap-remove */
            pool->entries[i] = pool->entries[pool->count - 1];
            pool->count--;
            pthread_mutex_unlock(&cache->pool_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&cache->pool_mutex);
    return 0;
}

/* Return an instance to the pool, or destroy it.
 * Only pools on success, small heaps, and when pool isn't full. */
void hl_wasm_pool_release(HlWasmCache *cache, HlWasmModule *mod,
                          void *inst_v, void *exec_env_v, void *process_fn_v,
                          uint32_t heap_size, uint32_t stack_size,
                          int success)
{
    wasm_module_inst_t inst = (wasm_module_inst_t)inst_v;
    wasm_exec_env_t exec_env = (wasm_exec_env_t)exec_env_v;

    if (success && heap_size <= HL_WASM_POOL_HEAP_THRESHOLD) {
        pthread_mutex_lock(&cache->pool_mutex);
        HlWasmPool *pool = &mod->pool;
        if (pool->count < HL_WASM_POOL_MAX) {
            /* Clear any stale exception before returning to pool */
            wasm_runtime_clear_exception(inst);
            HlWasmPoolEntry *e = &pool->entries[pool->count++];
            e->instance   = inst_v;
            e->exec_env   = exec_env_v;
            e->process_fn = process_fn_v;
            e->heap_size  = heap_size;
            e->stack_size = stack_size;
            pthread_mutex_unlock(&cache->pool_mutex);
            return;
        }
        pthread_mutex_unlock(&cache->pool_mutex);
    }
    /* process_fn is owned by the instance — no separate cleanup needed */
    (void)process_fn_v;
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(inst);
}

/* ── Shared data helpers ───────────────────────────────────────────── */

/* Rebuild the WAMR shared heap chain from segment array.
 * Must be called under pool_mutex. Drains pool. */
static int rebuild_chain(HlWasmModule *mod)
{
    HlWasmSharedData *sd = mod->shared_data;
    if (!sd || sd->count == 0) {
        sd->chain_head = NULL;
        return 0;
    }

    /* For a single segment, no chaining needed — just create the heap */
    if (sd->count == 1) {
        sd->segments[0].wasm_addr =
            (uint32_t)(UINT32_MAX - sd->segments[0].alloc_size + 1);
        sd->chain_head = sd->segments[0].shared_heap;
        return 0;
    }

    /* Chain from right to left: last segment is highest address.
     * chain(head, body) puts body at higher addresses. */
    wasm_shared_heap_t chain = (wasm_shared_heap_t)sd->segments[sd->count - 1].shared_heap;

    for (int i = sd->count - 2; i >= 0; i--) {
        chain = wasm_runtime_chain_shared_heaps(
            (wasm_shared_heap_t)sd->segments[i].shared_heap, chain);
        if (!chain) {
            log_error("[wasm] failed to chain shared heaps");
            sd->chain_head = NULL;
            return -1;
        }
    }

    sd->chain_head = chain;

    /* Read back WASM addresses from the chain.
     * After chaining, each heap's start_off_mem32 is set by WAMR.
     * We can read it back via a temporary attach to a dummy instance,
     * but it's simpler to compute: last segment ends at UINT32_MAX,
     * and each prior segment is placed contiguously before it. */
    /* WAMR uses alloc_size (page-aligned) for heap mapping, so addresses
     * must be computed with alloc_size, not logical data size. */
    uint64_t addr = (uint64_t)UINT32_MAX + 1;
    for (int i = sd->count - 1; i >= 0; i--) {
        addr -= sd->segments[i].alloc_size;
        sd->segments[i].wasm_addr = (uint32_t)addr;
    }

    return 0;
}

/* Free a single segment's resources (heap + backing if owned). */
static void free_segment(HlWasmDataSegment *seg)
{
    /* shared_heap is destroyed by wasm_runtime_destroy() globally,
     * but we need to unchain first. For individual removal, we just
     * NULL out the pointer — WAMR cleans up on runtime destroy. */
    if (!seg->is_mmap && seg->host_addr) {
        munmap(seg->host_addr, seg->alloc_size);
        seg->host_addr = NULL;
    }
    seg->shared_heap = NULL;
    seg->size = 0;
    seg->wasm_addr = 0;
    seg->name[0] = '\0';
}

/* Free all shared data for a module. Caller holds pool_mutex. */
static void free_shared_data(HlWasmModule *mod)
{
    if (!mod->shared_data)
        return;

    HlWasmSharedData *sd = mod->shared_data;

    /* Unchain all heaps first */
    if (sd->chain_head && sd->count > 1) {
        wasm_runtime_unchain_shared_heaps(
            (wasm_shared_heap_t)sd->chain_head, true);
    }

    for (int i = 0; i < sd->count; i++)
        free_segment(&sd->segments[i]);

    sd->count = 0;
    sd->chain_head = NULL;
    free(sd);
    mod->shared_data = NULL;
}

/* Attach shared data chain to a WASM instance.
 * sd and chain_head must be snapshotted under pool_mutex. */
static void attach_shared_heap(wasm_module_inst_t inst, void *chain_head)
{
    if (chain_head) {
        if (!wasm_runtime_attach_shared_heap(
                inst, (wasm_shared_heap_t)chain_head)) {
            log_error("[wasm] failed to attach shared heap to instance");
        }
    }
}

/* ── Option clamping ───────────────────────────────────────────────── */

void hl_cap_wasm_clamp_opts(HlWasmCallOpts *opts,
                             uint32_t cfg_max_input, uint32_t cfg_max_output,
                             uint32_t cfg_heap, uint32_t cfg_stack, int64_t cfg_gas)
{
    if (!opts) return;
    #define CLAMP_U32(field, cfg) do { \
        if ((cfg) && (!(opts->field) || (opts->field) > (cfg))) \
            opts->field = (cfg); \
    } while(0)
    CLAMP_U32(max_input,  cfg_max_input);
    CLAMP_U32(max_output, cfg_max_output);
    CLAMP_U32(heap_size,  cfg_heap);
    CLAMP_U32(stack_size, cfg_stack);
    #undef CLAMP_U32
    if (cfg_gas && (!opts->gas || opts->gas > cfg_gas))
        opts->gas = cfg_gas;
}

/* ── Shared data public API ─────────────────────────────────────────── */

int hl_cap_wasm_data_load(HlWasmCache *cache, const char *module_name,
                           const char *segment_name,
                           const void *data, size_t data_len,
                           void *pre_alloc,
                           const struct HlVfs *app_vfs, const char *app_dir,
                           const char **err_msg)
{
    static const char *err_internal  = "internal_error";
    static const char *err_not_found = "not_found";
    static const char *err_too_many  = "too_many_segments";
    static const char *err_too_large = "data_too_large";

    if (!cache || !cache->initialized || !module_name) {
        if (err_msg) *err_msg = err_internal;
        return -1;
    }
    if (validate_module_name(module_name) != 0) {
        if (err_msg) *err_msg = err_not_found;
        return -1;
    }

    /* Lazy-load the module */
    HlWasmModule *mod = NULL;
    {
        pthread_mutex_lock(&cache->pool_mutex);
        mod = cache_find(cache, module_name);
        pthread_mutex_unlock(&cache->pool_mutex);
    }
    if (!mod) {
        int rc = hl_cap_wasm_load(cache, module_name, app_vfs, app_dir);
        if (rc != 0) {
            if (err_msg) *err_msg = (rc == HL_WASM_ERR_NOT_FOUND) ? err_not_found : err_internal;
            return -1;
        }
        pthread_mutex_lock(&cache->pool_mutex);
        mod = cache_find(cache, module_name);
        pthread_mutex_unlock(&cache->pool_mutex);
        if (!mod) {
            if (err_msg) *err_msg = err_internal;
            return -1;
        }
    }

    pthread_mutex_lock(&cache->pool_mutex);

    /* Case 1: segment_name==NULL && data==NULL → remove all segments */
    if (!segment_name && !data) {
        pool_drain(&mod->pool);
        free_shared_data(mod);
        pthread_mutex_unlock(&cache->pool_mutex);
        log_debug("[wasm] removed all shared data for '%s'", module_name);
        return 0;
    }

    /* From here, segment_name must be non-NULL */
    if (!segment_name) {
        pthread_mutex_unlock(&cache->pool_mutex);
        if (err_msg) *err_msg = err_internal;
        return -1;
    }

    /* Case 2: segment_name!=NULL && data==NULL → remove that segment */
    if (!data) {
        if (mod->shared_data) {
            HlWasmSharedData *sd = mod->shared_data;
            for (int i = 0; i < sd->count; i++) {
                if (strcmp(sd->segments[i].name, segment_name) == 0) {
                    /* Unchain before removing */
                    if (sd->chain_head && sd->count > 1) {
                        wasm_runtime_unchain_shared_heaps(
                            (wasm_shared_heap_t)sd->chain_head, true);
                        sd->chain_head = NULL;
                    }
                    free_segment(&sd->segments[i]);
                    /* Compact: shift remaining segments */
                    for (int j = i; j < sd->count - 1; j++)
                        sd->segments[j] = sd->segments[j + 1];
                    sd->count--;
                    memset(&sd->segments[sd->count], 0, sizeof(HlWasmDataSegment));

                    pool_drain(&mod->pool);
                    if (sd->count > 0)
                        rebuild_chain(mod);
                    else {
                        free(sd);
                        mod->shared_data = NULL;
                    }
                    pthread_mutex_unlock(&cache->pool_mutex);
                    log_debug("[wasm] removed segment '%s' from '%s'",
                              segment_name, module_name);
                    return 0;
                }
            }
        }
        /* Segment not found — OK, nothing to remove */
        pthread_mutex_unlock(&cache->pool_mutex);
        return 0;
    }

    /* Case 3: segment_name!=NULL && data!=NULL → add/replace segment */
    if (data_len == 0 || data_len > HL_WASM_MAX_SHARED_DATA) {
        pthread_mutex_unlock(&cache->pool_mutex);
        if (err_msg) *err_msg = err_too_large;
        return -1;
    }

    /* Allocate shared_data struct if needed */
    if (!mod->shared_data) {
        mod->shared_data = calloc(1, sizeof(HlWasmSharedData));
        if (!mod->shared_data) {
            pthread_mutex_unlock(&cache->pool_mutex);
            if (err_msg) *err_msg = err_internal;
            return -1;
        }
    }
    HlWasmSharedData *sd = mod->shared_data;

    /* Find existing segment to replace, or add new */
    int slot = -1;
    for (int i = 0; i < sd->count; i++) {
        if (strcmp(sd->segments[i].name, segment_name) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* New segment — check limits */
        if (sd->count >= HL_WASM_MAX_DATA_SEGMENTS) {
            pthread_mutex_unlock(&cache->pool_mutex);
            if (err_msg) *err_msg = err_too_many;
            return -1;
        }
        slot = sd->count;
    }

    /* Validate total size across all segments */
    uint64_t total = data_len;
    for (int i = 0; i < sd->count; i++) {
        if (i != slot)
            total += sd->segments[i].size;
    }
    if (total > HL_WASM_MAX_SHARED_DATA) {
        pthread_mutex_unlock(&cache->pool_mutex);
        if (err_msg) *err_msg = err_too_large;
        return -1;
    }

    /* Unchain existing chain before modifying */
    if (sd->chain_head && sd->count > 1) {
        wasm_runtime_unchain_shared_heaps(
            (wasm_shared_heap_t)sd->chain_head, true);
        sd->chain_head = NULL;
    }

    /* Free old segment at slot if replacing */
    if (slot < sd->count && sd->segments[slot].shared_heap)
        free_segment(&sd->segments[slot]);

    /* Create backing memory.
     * WAMR requires pre_allocated_addr regions to be page-aligned in size.
     * We always mmap to ensure page-alignment, even for pre_alloc (which
     * we copy into the page-aligned region). */
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t alloc_size = (data_len + (size_t)page_size - 1) & ~((size_t)page_size - 1);
    if (alloc_size == 0) alloc_size = (size_t)page_size;

    void *backing = NULL;
    int is_mmap_backing = 0;
    if (pre_alloc) {
        /* For pre_alloc, check if already page-aligned in size */
        if (data_len == alloc_size) {
            backing = pre_alloc;
            is_mmap_backing = 1;
        } else {
            /* Need to copy to page-aligned region */
            backing = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (backing == MAP_FAILED) {
                pthread_mutex_unlock(&cache->pool_mutex);
                if (err_msg) *err_msg = err_internal;
                return -1;
            }
            memcpy(backing, pre_alloc, data_len);
            is_mmap_backing = 0;  /* we own this copy */
        }
    } else {
        /* mmap anonymous region + memcpy */
        backing = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (backing == MAP_FAILED) {
            pthread_mutex_unlock(&cache->pool_mutex);
            if (err_msg) *err_msg = err_internal;
            return -1;
        }
        memcpy(backing, data, data_len);
    }

    /* Create WAMR shared heap with pre-allocated backing */
    SharedHeapInitArgs heap_args;
    memset(&heap_args, 0, sizeof(heap_args));
    heap_args.size = (uint32_t)alloc_size;
    heap_args.pre_allocated_addr = backing;

    wasm_shared_heap_t heap = wasm_runtime_create_shared_heap(&heap_args);
    if (!heap) {
        if (!is_mmap_backing)
            munmap(backing, alloc_size);
        pthread_mutex_unlock(&cache->pool_mutex);
        if (err_msg) *err_msg = err_internal;
        log_error("[wasm] failed to create shared heap for segment '%s' (%zu bytes)",
                  segment_name, data_len);
        return -1;
    }

    /* Fill segment slot */
    HlWasmDataSegment *seg = &sd->segments[slot];
    snprintf(seg->name, sizeof(seg->name), "%s", segment_name);
    seg->shared_heap = heap;
    seg->host_addr = backing;
    seg->size = data_len;       /* logical size visible to WASM */
    seg->alloc_size = alloc_size; /* page-aligned size for munmap */
    seg->is_mmap = is_mmap_backing;

    if (slot == sd->count)
        sd->count++;

    /* Rebuild chain and drain pool */
    pool_drain(&mod->pool);
    int chain_rc = rebuild_chain(mod);

    if (chain_rc != 0) {
        /* Rollback: remove the segment we just added */
        free_segment(seg);
        if (slot == sd->count - 1)
            sd->count--;
        if (sd->count == 0) {
            free(sd);
            mod->shared_data = NULL;
        }
        pthread_mutex_unlock(&cache->pool_mutex);
        if (err_msg) *err_msg = err_internal;
        return -1;
    }

    pthread_mutex_unlock(&cache->pool_mutex);

    log_debug("[wasm] loaded segment '%s' for '%s' (%zu bytes, mmap=%d)",
              segment_name, module_name, data_len, is_mmap_backing);
    return 0;
}

void hl_cap_wasm_data_unload(HlWasmCache *cache, const char *module_name)
{
    if (!cache || !cache->initialized || !module_name)
        return;

    pthread_mutex_lock(&cache->pool_mutex);
    HlWasmModule *mod = cache_find(cache, module_name);
    if (mod) {
        pool_drain(&mod->pool);
        free_shared_data(mod);
    }
    pthread_mutex_unlock(&cache->pool_mutex);
}

/* ── Init / Destroy ────────────────────────────────────────────────── */

int hl_cap_wasm_init(HlWasmCache *cache)
{
    if (!cache)
        return -1;

    memset(cache, 0, sizeof(*cache));

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));

    /* Use system allocator (malloc/free) */
    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    /* Register host_call as a native function under "env" module */
    init_args.native_module_name = "env";
    init_args.native_symbols = host_symbols;
    init_args.n_native_symbols = sizeof(host_symbols) / sizeof(NativeSymbol);

    if (!wasm_runtime_full_init(&init_args)) {
        log_error("[wasm] WAMR runtime init failed");
        return -1;
    }

    if (pthread_mutex_init(&cache->pool_mutex, NULL) != 0) {
        log_error("[wasm] mutex init failed");
        wasm_runtime_destroy();
        return -1;
    }
    cache->initialized = 1;
    log_debug("[wasm] WAMR runtime initialized");
    return 0;
}

void hl_cap_wasm_destroy(HlWasmCache *cache)
{
    if (!cache || !cache->initialized)
        return;

    pthread_mutex_lock(&cache->pool_mutex);
    for (int i = 0; i < cache->count; i++) {
        HlWasmModule *m = &cache->modules[i];
        pool_drain(&m->pool);
        free_shared_data(m);
        if (m->module)
            wasm_runtime_unload((wasm_module_t)m->module);
        free(m->wasm_buf);
    }
    cache->count = 0;
    cache->initialized = 0;
    pthread_mutex_unlock(&cache->pool_mutex);

    pthread_mutex_destroy(&cache->pool_mutex);
    wasm_runtime_destroy();
    log_debug("[wasm] WAMR runtime destroyed");
}

/* ── Load module ───────────────────────────────────────────────────── */

int hl_cap_wasm_load(HlWasmCache *cache, const char *name,
                     const struct HlVfs *app_vfs, const char *app_dir)
{
    if (!cache || !cache->initialized || !name)
        return HL_WASM_ERR_INTERNAL;

    if (validate_module_name(name) != 0) {
        log_error("[wasm] invalid module name '%s'", name);
        return HL_WASM_ERR_NOT_FOUND;
    }

    /* Already cached? */
    if (cache_find(cache, name))
        return 0;

    if (cache->count >= HL_WASM_CACHE_MAX) {
        log_error("[wasm] module cache full (max %d)", HL_WASM_CACHE_MAX);
        return HL_WASM_ERR_INTERNAL;
    }

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    int is_aot = 0;

    /* 1. Try AOT from VFS: compute/<name>.aot.<arch> */
    const char *arch = wasm_arch_suffix();
    if (arch && app_vfs) {
        char aot_name[512];
        snprintf(aot_name, sizeof(aot_name), "compute/%s.aot.%s", name, arch);
        const HlEntry *e = hl_vfs_find(app_vfs, aot_name);
        if (e && e->data && e->len > 0) {
            /* Raw malloc required: wasm_runtime_load takes ownership and calls free() */
            buf = malloc(e->len);
            if (buf) {
                memcpy(buf, e->data, e->len);
                buf_len = e->len;
                is_aot = 1;
                log_debug("[wasm] loaded AOT module '%s' from VFS (%u bytes)",
                          name, buf_len);
            }
        }
    }

    /* 2. Try WASM from VFS: compute/<name>.wasm */
    if (!buf && app_vfs) {
        char wasm_name[512];
        snprintf(wasm_name, sizeof(wasm_name), "compute/%s.wasm", name);
        const HlEntry *e = hl_vfs_find(app_vfs, wasm_name);
        if (e && e->data && e->len > 0) {
            /* Raw malloc required: wasm_runtime_load takes ownership and calls free() */
            buf = malloc(e->len);
            if (buf) {
                memcpy(buf, e->data, e->len);
                buf_len = e->len;
                log_debug("[wasm] loaded module '%s' from VFS (%u bytes)",
                          name, buf_len);
            }
        }
    }

    /* 3. Filesystem AOT: <app_dir>/compute/<name>.aot.<arch> */
    if (!buf && app_dir && arch) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/compute/%s.aot.%s", app_dir, name, arch);
        FILE *f = (n > 0 && (size_t)n < sizeof(path)) ? fopen(path, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsize > 0 && fsize < (long)HL_WASM_MAX_IO_SIZE) {
                /* Raw malloc required: wasm_runtime_load takes ownership and calls free() */
                buf = malloc((size_t)fsize);
                if (buf) {
                    size_t nr = fread(buf, 1, (size_t)fsize, f);
                    if (nr == (size_t)fsize) {
                        buf_len = (uint32_t)fsize;
                        is_aot = 1;
                        log_debug("[wasm] loaded AOT module '%s' from disk (%u bytes)",
                                  name, buf_len);
                    } else {
                        free(buf);
                        buf = NULL;
                    }
                }
            }
            fclose(f);
        }
    }

    /* 4. Filesystem WASM fallback: <app_dir>/compute/<name>.wasm */
    if (!buf && app_dir) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/compute/%s.wasm", app_dir, name);
        FILE *f = (n > 0 && (size_t)n < sizeof(path)) ? fopen(path, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsize > 0 && fsize < (long)HL_WASM_MAX_IO_SIZE) {
                /* Raw malloc required: wasm_runtime_load takes ownership and calls free() */
                buf = malloc((size_t)fsize);
                if (buf) {
                    size_t nr = fread(buf, 1, (size_t)fsize, f);
                    if (nr == (size_t)fsize) {
                        buf_len = (uint32_t)fsize;
                        log_debug("[wasm] loaded module '%s' from disk (%u bytes)",
                                  name, buf_len);
                    } else {
                        free(buf);
                        buf = NULL;
                    }
                }
            }
            fclose(f);
        }
    }

    if (!buf) {
        log_warn("[wasm] module '%s' not found", name);
        return HL_WASM_ERR_NOT_FOUND;
    }

    /* Load into WAMR */
    char error_buf[256];
    wasm_module_t module = wasm_runtime_load(buf, buf_len,
                                              error_buf, sizeof(error_buf));
    if (!module) {
        log_error("[wasm] failed to load module '%s': %s", name, error_buf);
        free(buf);
        return HL_WASM_ERR_LOAD;
    }

    /* Probe ABI version before taking the lock */
    uint32_t abi_version = 0;
    {
        wasm_module_inst_t tmp_inst = wasm_runtime_instantiate(
            module, 8192, 8192, error_buf, sizeof(error_buf));
        if (tmp_inst) {
            wasm_function_inst_t ver_fn = wasm_runtime_lookup_function(
                tmp_inst, "hull_version");
            if (ver_fn) {
                wasm_exec_env_t env = wasm_runtime_create_exec_env(tmp_inst, 8192);
                if (env) {
                    uint32_t argv[1] = {0};
                    if (wasm_runtime_call_wasm(env, ver_fn, 0, argv))
                        abi_version = argv[0];
                    wasm_runtime_destroy_exec_env(env);
                }
            }
            wasm_runtime_deinstantiate(tmp_inst);
        }
    }

    /* TS-2: Mutex-protect cache insertion to prevent concurrent duplicate loads */
    pthread_mutex_lock(&cache->pool_mutex);

    /* Double-check: another thread may have loaded this module while we were
     * doing the (unlocked) wasm_runtime_load + ABI probe above. */
    if (cache_find(cache, name)) {
        pthread_mutex_unlock(&cache->pool_mutex);
        /* WAMR ownership: wasm_runtime_load borrows buf; wasm_runtime_unload
         * releases the module but does NOT free buf — caller retains ownership.
         * Both unload and free are required here. */
        wasm_runtime_unload(module);
        free(buf);
        return 0;
    }

    if (cache->count >= HL_WASM_CACHE_MAX) {
        pthread_mutex_unlock(&cache->pool_mutex);
        log_error("[wasm] module cache full (max %d)", HL_WASM_CACHE_MAX);
        wasm_runtime_unload(module);
        free(buf);
        return HL_WASM_ERR_INTERNAL;
    }

    /* Add to cache */
    HlWasmModule *cached = &cache->modules[cache->count];
    snprintf(cached->name, sizeof(cached->name), "%s", name);
    cached->module = module;
    cached->wasm_buf = buf;
    cached->wasm_buf_len = buf_len;
    cached->is_aot = is_aot;
    cached->abi_version = abi_version;
    cache->count++;

    pthread_mutex_unlock(&cache->pool_mutex);

    log_info("[wasm] cached module '%s' (abi=%u, aot=%d)",
             name, cached->abi_version, is_aot);
    return 0;
}

/* ── Call module (thin wrapper over call_buf) ──────────────────────── */

int hl_cap_wasm_call(HlWasmCache *cache, const char *name,
                     const void *input, size_t input_len,
                     void **output, size_t *output_len,
                     const HlWasmCallOpts *opts,
                     HlWasmCallbackFn callback_fn, void *callback_ctx,
                     const struct HlVfs *app_vfs, const char *app_dir,
                     HlAllocator *alloc, const char **err_msg)
{
    if (!output || !output_len) {
        if (err_msg) *err_msg = "internal_error";
        return HL_WASM_ERR_INTERNAL;
    }
    *output = NULL;
    *output_len = 0;

    HlWasmBuffer *buf = NULL;
    int rc = hl_cap_wasm_call_buf(cache, name, input, input_len,
                                   &buf, opts, callback_fn, callback_ctx,
                                   app_vfs, app_dir, alloc, err_msg);
    if (rc != HL_WASM_OK)
        return rc;

    if (buf) {
        size_t buf_len = hl_wasm_buffer_len(buf);
        if (buf_len > 0) {
            *output = hl_wasm_buffer_materialize(buf, output_len, alloc);
            hl_wasm_buffer_destroy(buf);
            hl_alloc_free(alloc, buf, sizeof(*buf));
            if (!*output) {
                if (err_msg) *err_msg = "internal_error";
                return HL_WASM_ERR_INTERNAL;
            }
        } else {
            hl_wasm_buffer_destroy(buf);
            hl_alloc_free(alloc, buf, sizeof(*buf));
        }
    }
    return HL_WASM_OK;
}

/* ── Call module (buffer output) ───────────────────────────────────── */

int hl_cap_wasm_call_buf(HlWasmCache *cache, const char *name,
                         const void *input, size_t input_len,
                         HlWasmBuffer **output_buf,
                         const HlWasmCallOpts *opts,
                         HlWasmCallbackFn callback_fn, void *callback_ctx,
                         const struct HlVfs *app_vfs, const char *app_dir,
                         HlAllocator *alloc, const char **err_msg)
{
    static const char *err_internal  = "internal_error";
    static const char *err_not_found = "not_found";
    static const char *err_gas       = "gas_exhausted";
    static const char *err_output    = "output_too_small";
    static const char *err_input     = "input_too_large";
    static const char *err_load      = "load_failed";
    static const char *err_no_export = "no_hull_process_export";
    static const char *err_call      = "call_failed";

    if (!cache || !cache->initialized || !name || !output_buf) {
        if (err_msg) *err_msg = err_internal;
        return HL_WASM_ERR_INTERNAL;
    }

    if (validate_module_name(name) != 0) {
        if (err_msg) *err_msg = err_not_found;
        return HL_WASM_ERR_NOT_FOUND;
    }

    *output_buf = NULL;

    /* Apply defaults */
    uint32_t max_input  = opts && opts->max_input  ? opts->max_input  : HL_WASM_DEFAULT_MAX_INPUT;
    uint32_t max_output = opts && opts->max_output  ? opts->max_output : HL_WASM_DEFAULT_MAX_OUTPUT;
    uint32_t heap_size  = opts && opts->heap_size   ? opts->heap_size  : HL_WASM_DEFAULT_HEAP;
    uint32_t stack_size = opts && opts->stack_size   ? opts->stack_size : HL_WASM_DEFAULT_STACK;
    int64_t  gas        = opts && opts->gas          ? opts->gas        : HL_WASM_DEFAULT_GAS;

    /* Clamp to maximums */
    if (max_input > HL_WASM_MAX_IO_SIZE)   max_input = HL_WASM_MAX_IO_SIZE;
    if (max_output > HL_WASM_MAX_IO_SIZE)  max_output = HL_WASM_MAX_IO_SIZE;
    if (heap_size > (uint32_t)HL_WASM_MAX_HEAP) heap_size = (uint32_t)HL_WASM_MAX_HEAP;
    if (stack_size > (uint32_t)HL_WASM_MAX_STACK) stack_size = (uint32_t)HL_WASM_MAX_STACK;
    if (gas > HL_WASM_MAX_GAS)             gas = HL_WASM_MAX_GAS;

    /* Validate input size */
    if (input_len > max_input) {
        if (err_msg) *err_msg = err_input;
        return HL_WASM_ERR_INPUT;
    }

    /* Audit log */
    if (hl_audit_enabled) {
        ShJsonWriter w = hl_audit_begin("compute.call_buf");
        sh_json_write_kv_string(&w, "module", name);
        hl_audit_end(&w);
    }

    /* Lazy-load module.  The initial cache_find is under pool_mutex to
     * prevent torn reads if another thread is concurrently in
     * hl_cap_wasm_load (which writes cache entries under the same lock). */
    HlWasmModule *mod = NULL;
    {
        pthread_mutex_lock(&cache->pool_mutex);
        mod = cache_find(cache, name);
        pthread_mutex_unlock(&cache->pool_mutex);
    }
    if (!mod) {
        int rc = hl_cap_wasm_load(cache, name, app_vfs, app_dir);
        if (rc != 0) {
            if (err_msg) *err_msg = (rc == HL_WASM_ERR_NOT_FOUND) ? err_not_found : err_load;
            return rc;
        }
        pthread_mutex_lock(&cache->pool_mutex);
        mod = cache_find(cache, name);
        pthread_mutex_unlock(&cache->pool_mutex);
        if (!mod) {
            if (err_msg) *err_msg = err_internal;
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* Try to acquire from instance pool.
     * Shared data is snapshotted under the mutex for thread safety. */
    wasm_module_inst_t inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t process_fn = NULL;
    const HlWasmSharedData *sd_snapshot = NULL;
    void *chain_head_snapshot = NULL;
    int from_pool = pool_acquire(cache, mod, heap_size, stack_size,
                                 &inst, &exec_env, &process_fn,
                                 &sd_snapshot, &chain_head_snapshot);

    if (!from_pool) {
        char error_buf[256];
        inst = wasm_runtime_instantiate(
            (wasm_module_t)mod->module, stack_size, heap_size,
            error_buf, sizeof(error_buf));
        if (!inst) {
            log_error("[wasm] instantiate '%s' failed: %s", name, error_buf);
            if (err_msg) *err_msg = err_internal;
            return HL_WASM_ERR_INTERNAL;
        }

        process_fn = wasm_runtime_lookup_function(inst, "hull_process");
        if (!process_fn) {
            log_error("[wasm] module '%s' missing hull_process export", name);
            if (err_msg) *err_msg = err_no_export;
            wasm_runtime_deinstantiate(inst);
            return HL_WASM_ERR_NOT_FOUND;
        }

        exec_env = wasm_runtime_create_exec_env(inst, stack_size);
        if (!exec_env) {
            log_error("[wasm] failed to create exec env for '%s'", name);
            if (err_msg) *err_msg = err_internal;
            wasm_runtime_deinstantiate(inst);
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* Attach shared data (snapshotted under mutex) */
    attach_shared_heap(inst, chain_head_snapshot);

    int ret = HL_WASM_ERR_INTERNAL;

    /* Set instruction count limit (gas metering) */
    if (gas > 0) {
        if (gas > INT_MAX)
            log_warn("[wasm] gas %" PRId64 " exceeds INT_MAX, clamped", gas);
        int gas_int = (gas > INT_MAX) ? INT_MAX : (int)gas;
        wasm_runtime_set_instruction_count_limit(exec_env, gas_int);
    }

    /* Allocate input buffer in WASM linear memory */
    void *native_in = NULL;
    uint64_t wasm_in_ptr = 0;
    if (input_len > 0) {
        wasm_in_ptr = wasm_runtime_module_malloc(inst, (uint64_t)input_len, &native_in);
        if (!wasm_in_ptr || !native_in) {
            log_error("[wasm] failed to allocate input buffer (%zu bytes)", input_len);
            if (err_msg) *err_msg = err_internal;
            hl_wasm_pool_release(cache, mod, inst, exec_env, process_fn,
                         heap_size, stack_size, 0);
            return HL_WASM_ERR_INTERNAL;
        }
        memcpy(native_in, input, input_len);
    }

    /* Allocate output buffer in WASM linear memory */
    void *native_out = NULL;
    uint64_t wasm_out_ptr = 0;
    if (max_output > 0) {
        wasm_out_ptr = wasm_runtime_module_malloc(inst, (uint64_t)max_output, &native_out);
        if (!wasm_out_ptr || !native_out) {
            log_error("[wasm] failed to allocate output buffer (%u bytes)", max_output);
            if (err_msg) *err_msg = err_internal;
            if (wasm_in_ptr) wasm_runtime_module_free(inst, wasm_in_ptr);
            hl_wasm_pool_release(cache, mod, inst, exec_env, process_fn,
                         heap_size, stack_size, 0);
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* Save and set thread-local callback context (reentry-safe) */
    HlHostCallCtx saved_ctx = tl_host_ctx;
    tl_host_ctx.fn = callback_fn;
    tl_host_ctx.ctx = callback_ctx;
    tl_host_ctx.shared_data = sd_snapshot;

    /* WASM32 ABI: hull_process arguments are uint32_t. wasm_runtime_module_malloc
     * returns uint64_t but values are guaranteed ≤ UINT32_MAX for WASM32 modules
     * (linear memory is 32-bit addressable). Narrowing cast is intentional. */
    assert(wasm_in_ptr <= UINT32_MAX);
    assert(wasm_out_ptr <= UINT32_MAX);
    uint32_t argv[4] = {
        (uint32_t)wasm_in_ptr,
        (uint32_t)input_len,
        (uint32_t)wasm_out_ptr,
        max_output,
    };

    if (!wasm_runtime_call_wasm(exec_env, process_fn, 4, argv)) {
        const char *exception = wasm_runtime_get_exception(inst);
        if (exception && strstr(exception, "instruction count")) {
            log_warn("[wasm] gas exhausted for '%s'", name);
            if (err_msg) *err_msg = err_gas;
            ret = HL_WASM_ERR_GAS;
        } else {
            log_error("[wasm] call '%s' failed: %s", name,
                      exception ? exception : "unknown");
            if (err_msg) *err_msg = err_call;
        }
        goto cleanup_bufs_err;
    }

    /* Get return value (bytes written or error code) */
    int32_t result = (int32_t)argv[0];

    if (result < 0) {
        if (result == -2) {
            if (err_msg) *err_msg = err_output;
            ret = HL_WASM_ERR_OUTPUT;
        } else {
            if (err_msg) *err_msg = err_call;
            ret = HL_WASM_ERR_INTERNAL;
        }
        goto cleanup_bufs_err;
    }

    /* Free input allocation — no longer needed */
    tl_host_ctx = saved_ctx;
    if (wasm_in_ptr) wasm_runtime_module_free(inst, wasm_in_ptr);

    /* Create output buffer */
    if (result > 0 && (uint32_t)result <= max_output) {
        int poolable = (heap_size <= HL_WASM_POOL_HEAP_THRESHOLD);
        if (poolable) {
            /* Zero-copy: wrap WASM linear memory, keep instance checked out */
            *output_buf = hl_wasm_buffer_create_wasm(
                inst, exec_env, process_fn, mod, cache,
                wasm_out_ptr, native_out, (size_t)result,
                heap_size, stack_size, alloc);
            if (*output_buf)
                return HL_WASM_OK; /* instance stays checked out */
            /* RL-1: When hl_wasm_buffer_create_wasm fails, we fall through to the
             * non-poolable path which properly frees wasm_out_ptr and releases
             * the instance via hl_wasm_pool_release below. */
        }
        /* Non-poolable or alloc failure: copy output, release instance */
        *output_buf = hl_wasm_buffer_create_owned(native_out, (size_t)result, alloc);
    } else {
        /* Zero-length output */
        *output_buf = hl_wasm_buffer_create_owned(NULL, 0, alloc);
    }

    if (wasm_out_ptr) wasm_runtime_module_free(inst, wasm_out_ptr);
    int ok = (*output_buf != NULL);
    hl_wasm_pool_release(cache, mod, inst, exec_env, process_fn,
                 heap_size, stack_size, ok);

    return ok ? HL_WASM_OK : HL_WASM_ERR_INTERNAL;

cleanup_bufs_err:
    tl_host_ctx = saved_ctx;
    if (wasm_in_ptr)  wasm_runtime_module_free(inst, wasm_in_ptr);
    if (wasm_out_ptr) wasm_runtime_module_free(inst, wasm_out_ptr);
    hl_wasm_pool_release(cache, mod, inst, exec_env, process_fn,
                 heap_size, stack_size, 0);
    return ret;
}

/* ── Persistent instance: create ───────────────────────────────────── */

HlWasmInstance *hl_cap_wasm_instance_create(HlWasmCache *cache,
                                             const char *name,
                                             const HlWasmCallOpts *opts,
                                             const struct HlVfs *app_vfs,
                                             const char *app_dir,
                                             HlAllocator *alloc,
                                             const char **err_msg)
{
    static const char *err_internal  = "internal_error";
    static const char *err_not_found = "not_found";
    static const char *err_load      = "load_failed";
    static const char *err_no_export = "no_hull_process_export";

    if (!cache || !cache->initialized || !name) {
        if (err_msg) *err_msg = err_internal;
        return NULL;
    }
    if (validate_module_name(name) != 0) {
        if (err_msg) *err_msg = err_not_found;
        return NULL;
    }

    /* Apply defaults */
    uint32_t heap_size  = opts && opts->heap_size  ? opts->heap_size  : HL_WASM_DEFAULT_HEAP;
    uint32_t stack_size = opts && opts->stack_size  ? opts->stack_size : HL_WASM_DEFAULT_STACK;

    /* Clamp to maximums */
    if (heap_size > (uint32_t)HL_WASM_MAX_HEAP)  heap_size = (uint32_t)HL_WASM_MAX_HEAP;
    if (stack_size > (uint32_t)HL_WASM_MAX_STACK) stack_size = (uint32_t)HL_WASM_MAX_STACK;

    /* Lazy-load module + snapshot shared data under mutex */
    HlWasmModule *mod = NULL;
    void *chain_head_snapshot = NULL;
    {
        pthread_mutex_lock(&cache->pool_mutex);
        mod = cache_find(cache, name);
        if (mod && mod->shared_data)
            chain_head_snapshot = mod->shared_data->chain_head;
        pthread_mutex_unlock(&cache->pool_mutex);
    }
    if (!mod) {
        int rc = hl_cap_wasm_load(cache, name, app_vfs, app_dir);
        if (rc != 0) {
            if (err_msg) *err_msg = (rc == HL_WASM_ERR_NOT_FOUND) ? err_not_found : err_load;
            return NULL;
        }
        pthread_mutex_lock(&cache->pool_mutex);
        mod = cache_find(cache, name);
        if (mod && mod->shared_data)
            chain_head_snapshot = mod->shared_data->chain_head;
        pthread_mutex_unlock(&cache->pool_mutex);
        if (!mod) {
            if (err_msg) *err_msg = err_internal;
            return NULL;
        }
    }

    /* Instantiate — NOT from pool, this is exclusively owned */
    char error_buf[256];
    wasm_module_inst_t inst = wasm_runtime_instantiate(
        (wasm_module_t)mod->module, stack_size, heap_size,
        error_buf, sizeof(error_buf));
    if (!inst) {
        log_error("[wasm] persistent instantiate '%s' failed: %s", name, error_buf);
        if (err_msg) *err_msg = err_internal;
        return NULL;
    }

    wasm_function_inst_t process_fn = wasm_runtime_lookup_function(inst, "hull_process");
    if (!process_fn) {
        log_error("[wasm] module '%s' missing hull_process export", name);
        if (err_msg) *err_msg = err_no_export;
        wasm_runtime_deinstantiate(inst);
        return NULL;
    }

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, stack_size);
    if (!exec_env) {
        log_error("[wasm] failed to create exec env for persistent '%s'", name);
        if (err_msg) *err_msg = err_internal;
        wasm_runtime_deinstantiate(inst);
        return NULL;
    }

    /* Attach shared data (snapshotted under mutex) */
    attach_shared_heap(inst, chain_head_snapshot);

    /* Allocate struct */
    HlWasmInstance *pi = alloc ? hl_alloc_calloc(alloc, 1, sizeof(*pi))
                               : calloc(1, sizeof(*pi));
    if (!pi) {
        if (err_msg) *err_msg = err_internal;
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(inst);
        return NULL;
    }

    pi->instance   = inst;
    pi->exec_env   = exec_env;
    pi->process_fn = process_fn;
    pi->module     = mod;
    pi->cache      = cache;
    snprintf(pi->name, sizeof(pi->name), "%s", name);
    pi->heap_size  = heap_size;
    pi->stack_size = stack_size;
    pi->alloc      = alloc;

    /* Store optional defaults from opts */
    if (opts) {
        pi->default_gas        = opts->gas;
        pi->default_max_input  = opts->max_input;
        pi->default_max_output = opts->max_output;
    }

    log_debug("[wasm] persistent instance '%s' created (heap=%u, stack=%u)",
              name, heap_size, stack_size);
    return pi;
}

/* ── Persistent instance: call_buf ─────────────────────────────────── */

int hl_cap_wasm_instance_call_buf(HlWasmInstance *pi,
                                   const void *input, size_t input_len,
                                   HlWasmBuffer **output_buf,
                                   const HlWasmCallOpts *opts,
                                   HlWasmCallbackFn callback_fn, void *callback_ctx,
                                   HlAllocator *alloc, const char **err_msg)
{
    static const char *err_internal  = "internal_error";
    static const char *err_gas       = "gas_exhausted";
    static const char *err_output    = "output_too_small";
    static const char *err_input     = "input_too_large";
    static const char *err_call      = "call_failed";
    static const char *err_closed    = "instance_closed";
    static const char *err_busy      = "instance_busy";

    if (!pi || !output_buf) {
        if (err_msg) *err_msg = err_internal;
        return HL_WASM_ERR_INTERNAL;
    }
    *output_buf = NULL;

    if (pi->closed) {
        if (err_msg) *err_msg = err_closed;
        return HL_WASM_ERR_INTERNAL;
    }
    if (atomic_load(&pi->busy)) {
        if (err_msg) *err_msg = err_busy;
        return HL_WASM_ERR_INTERNAL;
    }

    /* Resolve per-call opts → instance defaults → global defaults */
    uint32_t max_input  = opts && opts->max_input  ? opts->max_input
                        : pi->default_max_input    ? pi->default_max_input
                        : HL_WASM_DEFAULT_MAX_INPUT;
    uint32_t max_output = opts && opts->max_output  ? opts->max_output
                        : pi->default_max_output    ? pi->default_max_output
                        : HL_WASM_DEFAULT_MAX_OUTPUT;
    int64_t  gas        = opts && opts->gas          ? opts->gas
                        : pi->default_gas            ? pi->default_gas
                        : HL_WASM_DEFAULT_GAS;

    if (max_input > HL_WASM_MAX_IO_SIZE)  max_input = HL_WASM_MAX_IO_SIZE;
    if (max_output > HL_WASM_MAX_IO_SIZE) max_output = HL_WASM_MAX_IO_SIZE;
    if (gas > HL_WASM_MAX_GAS)            gas = HL_WASM_MAX_GAS;

    if (input_len > max_input) {
        if (err_msg) *err_msg = err_input;
        return HL_WASM_ERR_INPUT;
    }

    wasm_module_inst_t inst = (wasm_module_inst_t)pi->instance;
    wasm_exec_env_t exec_env = (wasm_exec_env_t)pi->exec_env;
    wasm_function_inst_t process_fn = (wasm_function_inst_t)pi->process_fn;

    /* Clear any stale exception from prior call (recover from gas exhaustion) */
    wasm_runtime_clear_exception(inst);

    /* Set gas */
    if (gas > 0) {
        if (gas > INT_MAX)
            log_warn("[wasm] gas %" PRId64 " exceeds INT_MAX, clamped", gas);
        int gas_int = (gas > INT_MAX) ? INT_MAX : (int)gas;
        wasm_runtime_set_instruction_count_limit(exec_env, gas_int);
    }

    int ret = HL_WASM_ERR_INTERNAL;

    /* Allocate I/O in WASM linear memory */
    void *native_in = NULL;
    uint64_t wasm_in_ptr = 0;
    if (input_len > 0) {
        wasm_in_ptr = wasm_runtime_module_malloc(inst, (uint64_t)input_len, &native_in);
        if (!wasm_in_ptr || !native_in) {
            if (err_msg) *err_msg = err_internal;
            return HL_WASM_ERR_INTERNAL;
        }
        memcpy(native_in, input, input_len);
    }

    void *native_out = NULL;
    uint64_t wasm_out_ptr = 0;
    if (max_output > 0) {
        wasm_out_ptr = wasm_runtime_module_malloc(inst, (uint64_t)max_output, &native_out);
        if (!wasm_out_ptr || !native_out) {
            if (err_msg) *err_msg = err_internal;
            if (wasm_in_ptr) wasm_runtime_module_free(inst, wasm_in_ptr);
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* Save and set thread-local callback context */
    HlHostCallCtx saved_ctx = tl_host_ctx;
    tl_host_ctx.fn = callback_fn;
    tl_host_ctx.ctx = callback_ctx;
    /* Persistent instance's shared data was attached at creation time.
     * Read shared_data under cache mutex for thread safety. */
    const HlWasmSharedData *pi_sd = NULL;
    if (pi->module && pi->cache) {
        pthread_mutex_lock(&pi->cache->pool_mutex);
        pi_sd = pi->module->shared_data;
        pthread_mutex_unlock(&pi->cache->pool_mutex);
    }
    tl_host_ctx.shared_data = pi_sd;

    assert(wasm_in_ptr <= UINT32_MAX);
    assert(wasm_out_ptr <= UINT32_MAX);
    uint32_t argv[4] = {
        (uint32_t)wasm_in_ptr,
        (uint32_t)input_len,
        (uint32_t)wasm_out_ptr,
        max_output,
    };

    if (!wasm_runtime_call_wasm(exec_env, process_fn, 4, argv)) {
        const char *exception = wasm_runtime_get_exception(inst);
        if (exception && strstr(exception, "instruction count")) {
            if (err_msg) *err_msg = err_gas;
            ret = HL_WASM_ERR_GAS;
        } else {
            if (err_msg) *err_msg = err_call;
        }
        goto cleanup_bufs_err;
    }

    int32_t result = (int32_t)argv[0];
    if (result < 0) {
        if (result == -2) {
            if (err_msg) *err_msg = err_output;
            ret = HL_WASM_ERR_OUTPUT;
        } else {
            if (err_msg) *err_msg = err_call;
            ret = HL_WASM_ERR_INTERNAL;
        }
        goto cleanup_bufs_err;
    }

    /* Success — free input, create OWNED output buffer */
    tl_host_ctx = saved_ctx;
    if (wasm_in_ptr) wasm_runtime_module_free(inst, wasm_in_ptr);

    if (result > 0 && (uint32_t)result <= max_output) {
        *output_buf = hl_wasm_buffer_create_owned(native_out, (size_t)result, alloc);
    } else {
        *output_buf = hl_wasm_buffer_create_owned(NULL, 0, alloc);
    }

    if (wasm_out_ptr) wasm_runtime_module_free(inst, wasm_out_ptr);

    return (*output_buf) ? HL_WASM_OK : HL_WASM_ERR_INTERNAL;

cleanup_bufs_err:
    tl_host_ctx = saved_ctx;
    if (wasm_in_ptr)  wasm_runtime_module_free(inst, wasm_in_ptr);
    if (wasm_out_ptr) wasm_runtime_module_free(inst, wasm_out_ptr);
    return ret;
}

/* ── Persistent instance: call (thin wrapper) ──────────────────────── */

int hl_cap_wasm_instance_call(HlWasmInstance *pi,
                               const void *input, size_t input_len,
                               void **output, size_t *output_len,
                               const HlWasmCallOpts *opts,
                               HlWasmCallbackFn cb_fn, void *cb_ctx,
                               HlAllocator *alloc, const char **err_msg)
{
    if (!output || !output_len) {
        if (err_msg) *err_msg = "internal_error";
        return HL_WASM_ERR_INTERNAL;
    }
    *output = NULL;
    *output_len = 0;

    HlWasmBuffer *buf = NULL;
    int rc = hl_cap_wasm_instance_call_buf(pi, input, input_len,
                                            &buf, opts, cb_fn, cb_ctx,
                                            alloc, err_msg);
    if (rc != HL_WASM_OK)
        return rc;

    if (buf) {
        size_t buf_len = hl_wasm_buffer_len(buf);
        if (buf_len > 0) {
            *output = hl_wasm_buffer_materialize(buf, output_len, alloc);
            hl_wasm_buffer_destroy(buf);
            hl_alloc_free(alloc, buf, sizeof(*buf));
            if (!*output) {
                if (err_msg) *err_msg = "internal_error";
                return HL_WASM_ERR_INTERNAL;
            }
        } else {
            hl_wasm_buffer_destroy(buf);
            hl_alloc_free(alloc, buf, sizeof(*buf));
        }
    }
    return HL_WASM_OK;
}

/* ── Persistent instance: destroy ──────────────────────────────────── */

void hl_cap_wasm_instance_destroy(HlWasmInstance *pi)
{
    if (!pi || pi->closed)
        return;

    if (atomic_load(&pi->busy)) {
        log_warn("[wasm] persistent instance '%s' destroyed while busy", pi->name);
        return; /* let async complete first */
    }

    pi->closed = 1;

    if (pi->exec_env)
        wasm_runtime_destroy_exec_env((wasm_exec_env_t)pi->exec_env);
    if (pi->instance)
        wasm_runtime_deinstantiate((wasm_module_inst_t)pi->instance);

    pi->exec_env   = NULL;
    pi->instance   = NULL;
    pi->process_fn = NULL;

    log_debug("[wasm] persistent instance '%s' destroyed", pi->name);

    if (pi->alloc)
        hl_alloc_free(pi->alloc, pi, sizeof(*pi));
    else
        free(pi);
}

#endif /* HL_ENABLE_WASM */
