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
#include "hull/cap/wasm_data.h"
#include "hull/utils/alloc.h"
#include "hull/cap/audit.h"
#include "hull/limits/wasm.h"
#include "hull/utils/macros.h"
#include <sh_seal_arena.h>
#include "hull/vfs.h"
#include "log.h"
#include "wasm_export.h"

/* Internal WAMR header for Memory64 detection (is_memory64 on WASMMemoryInstance).
 * Not part of the public API but needed to detect 64-bit memory modules.
 * Path is relative to -I$(WAMR_IWASM)/include set in Makefile. */
#if WASM_ENABLE_MEMORY64 != 0
#include "../interpreter/wasm_runtime.h"
#endif

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uint32_t) == 4, "WASM32 ABI requires 4-byte uint32_t");
_Static_assert(HL_WASM_CALLBACK_BUF_SIZE <= 65536,
               "callback buffer must fit on stack (max 64 KB)");

/* ── W^X / no runtime dynamic code ─────────────────────────────────────
 *
 * Hull's W^X policy forbids WAMR's JIT and Fast-JIT backends, which
 * generate native code into RWX pages at runtime. The interpreter (and
 * AOT artifacts produced at build time by `hull build` and embedded in
 * the VFS) are the only permitted execution modes.
 *
 * If a future Makefile change enables either JIT macro, fail at compile
 * time so the policy violation is loud and immediate. See docs/security.md
 * §3.A "Inject native code at runtime" and §4 for context. */
#if defined(WASM_ENABLE_JIT) && WASM_ENABLE_JIT != 0
#error "Hull W^X policy forbids WAMR JIT. Build with WASM_ENABLE_JIT=0."
#endif
#if defined(WASM_ENABLE_FAST_JIT) && WASM_ENABLE_FAST_JIT != 0
#error "Hull W^X policy forbids WAMR Fast JIT. Build with WASM_ENABLE_FAST_JIT=0."
#endif

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
    /* Streaming I/O metadata (set by wasm_stream.c via set/clear helpers) */
    uint32_t stream_flags;       /* 0 when not streaming */
    uint32_t stream_chunk_index;
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
        /* len=0: address low 32 bits, len=1: size low 32 bits,
         * len=2: address high 32 bits, len=3: size high 32 bits.
         * WASM32 plugins only need len=0/1 (high bits are zero).
         * Memory64 plugins use len=2/3 for full 64-bit values. */
        if (len == 0)
            return (int32_t)(uint32_t)sd->segments[ptr].wasm_addr;
        if (len == 1)
            return (int32_t)(uint32_t)sd->segments[ptr].size;
        if (len == 2)
            return (int32_t)(uint32_t)(sd->segments[ptr].wasm_addr >> 32);
        if (len == 3)
            return (int32_t)(uint32_t)(sd->segments[ptr].size >> 32);
        return -1;
    }

    if (opcode == HL_WASM_OP_STREAM) {
        if (len == HL_WASM_STREAM_FLAGS)
            return (int32_t)tl_host_ctx.stream_flags;
        if (len == HL_WASM_STREAM_CHUNK_INDEX)
            return (int32_t)tl_host_ctx.stream_chunk_index;
        return -1;
    }

    return -1; /* unknown opcode */
}

/* The canonical template for WAMR's native-symbol table.  At init
 * time we memdup this into the seal arena (see hl_cap_wasm_init);
 * the runtime never touches the static copy.  WAMR qsorts the
 * table in place during full_init (wasm_native.c:283), so the
 * arena starts RW, is populated, used by full_init, and only THEN
 * sealed — once sealed it's mprotect-RO for the rest of process
 * lifetime, defeating any heap-write primitive against the
 * host_call dispatcher. */
static const NativeSymbol host_symbols_template[] = {
    { "host_call", (void *)(uintptr_t)host_call_handler, "(iii)i", NULL },
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

HlWasmModule *hl_cap_wasm_module_lookup(HlWasmCache *cache, const char *name)
{
    if (!cache || !cache->initialized || !name)
        return NULL;
    /* cache->count / modules[] reads race with concurrent inserts under
     * C11; take the cache-level mutex (same as data_load's lookup). */
    pthread_mutex_lock(&cache->pool_mutex);
    HlWasmModule *mod = cache_find(cache, name);
    pthread_mutex_unlock(&cache->pool_mutex);
    return mod;
}

/* ── Instance pool ─────────────────────────────────────────────────── */

/* Drain all pooled instances for a module. Caller must hold mod->mutex. */
void hl_wasm_pool_drain(HlWasmPool *pool)
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
 * Always snapshots shared data pointers under mod->mutex for thread safety. */
static int pool_acquire(HlWasmModule *mod,
                        uint32_t heap_size, uint32_t stack_size,
                        wasm_module_inst_t *out_inst,
                        wasm_exec_env_t *out_env,
                        wasm_function_inst_t *out_fn,
                        const HlWasmSharedData **out_sd,
                        void **out_chain_head)
{
    pthread_mutex_lock(&mod->mutex);
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
            pthread_mutex_unlock(&mod->mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&mod->mutex);
    return 0;
}

/* Return an instance to the pool, or destroy it.
 * Only pools on success, small heaps, and when pool isn't full. */
void hl_wasm_pool_release(HlWasmCache *cache, HlWasmModule *mod,
                          void *inst_v, void *exec_env_v, void *process_fn_v,
                          uint32_t heap_size, uint32_t stack_size,
                          int success)
{
    (void)cache;
    wasm_module_inst_t inst = (wasm_module_inst_t)inst_v;
    wasm_exec_env_t exec_env = (wasm_exec_env_t)exec_env_v;

    if (success && heap_size <= HL_WASM_POOL_HEAP_THRESHOLD) {
        pthread_mutex_lock(&mod->mutex);
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
            pthread_mutex_unlock(&mod->mutex);
            return;
        }
        pthread_mutex_unlock(&mod->mutex);
    }
    /* process_fn is owned by the instance — no separate cleanup needed */
    (void)process_fn_v;
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(inst);
}

/* ── Init / Destroy ────────────────────────────────────────────────── */

int hl_cap_wasm_init(HlWasmCache *cache)
{
    if (!cache)
        return -1;

    memset(cache, 0, sizeof(*cache));

    /* Stage the NativeSymbol table in a seal arena.  Plain `static
     * const` won't work — WAMR's register_natives qsorts the table
     * in place during wasm_runtime_full_init (wasm_native.c:283),
     * which would SIGSEGV against rodata.  The arena starts RW, is
     * sorted by WAMR during init, then sealed below; subsequent
     * lookups (bsearch only) work fine against the RO mapping.
     *
     * Arena allocation: 1 page is plenty (host_symbols is tiny;
     * each NativeSymbol is ~40 bytes, we have one entry, the
     * mmap rounds up to a page regardless). */
    const size_t n_symbols = HL_ARRAY_LEN(host_symbols_template);
    ShSealArena *arena = calloc(1, sizeof(*arena));
    if (!arena) {
        log_error("[wasm] seal arena alloc failed");
        return -1;
    }
    if (sh_seal_arena_init(arena, sizeof(host_symbols_template),
                            "wasm-native-symbols") != 0) {
        log_error("[wasm] seal arena init failed");
        free(arena);
        return -1;
    }
    NativeSymbol *host_symbols = sh_seal_arena_memdup(
        arena, host_symbols_template, sizeof(host_symbols_template));
    if (!host_symbols) {
        log_error("[wasm] seal arena memdup failed");
        sh_seal_arena_destroy(arena);
        free(arena);
        return -1;
    }
    cache->native_arena = arena;

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));

    /* Use system allocator (malloc/free) */
    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    /* Hull's W^X policy: no JIT. The Makefile leaves WASM_ENABLE_JIT
     * and WASM_ENABLE_FAST_JIT undefined, and the _Static_assert above
     * fails the build if either is ever turned on. We leave
     * `init_args.running_mode` at Mode_Default (0); WAMR's auto-select
     * then decays to fast-interpreter for bytecode modules and the AOT
     * runtime for AOT modules — the two paths Hull supports. Forcing
     * Mode_Interp here would prevent AOT modules from executing. */
    /* init_args.running_mode left as Mode_Default */

    /* Register host_call as a native function under "env" module */
    init_args.native_module_name = "env";
    init_args.native_symbols = host_symbols;
    init_args.n_native_symbols = (uint32_t)n_symbols;

    if (!wasm_runtime_full_init(&init_args)) {
        log_error("[wasm] WAMR runtime init failed");
        sh_seal_arena_destroy(arena);
        free(arena);
        cache->native_arena = NULL;
        return -1;
    }

    /* WAMR has now sorted host_symbols in place.  Seal the arena —
     * post-seal, the entire mapping is RO and any write fault. */
    if (sh_seal_arena_seal(arena) != 0) {
        log_error("[wasm] seal arena seal failed");
        wasm_runtime_destroy();
        sh_seal_arena_destroy(arena);
        free(arena);
        cache->native_arena = NULL;
        return -1;
    }

    if (pthread_mutex_init(&cache->pool_mutex, NULL) != 0) {
        log_error("[wasm] mutex init failed");
        wasm_runtime_destroy();
        sh_seal_arena_destroy(arena);
        free(arena);
        cache->native_arena = NULL;
        return -1;
    }
    cache->initialized = 1;
    log_debug("[wasm] WAMR runtime initialized (native symbols sealed)");
    return 0;
}

void hl_cap_wasm_destroy(HlWasmCache *cache)
{
    if (!cache || !cache->initialized)
        return;

    pthread_mutex_lock(&cache->pool_mutex);
    for (int i = 0; i < cache->count; i++) {
        HlWasmModule *m = &cache->modules[i];
        pthread_mutex_lock(&m->mutex);
        hl_wasm_pool_drain(&m->pool);
        hl_wasm_free_shared_data(m);
        if (m->module)
            wasm_runtime_unload((wasm_module_t)m->module);
        free(m->wasm_buf);
        pthread_mutex_unlock(&m->mutex);
        pthread_mutex_destroy(&m->mutex);
    }
    cache->count = 0;
    cache->initialized = 0;
    pthread_mutex_unlock(&cache->pool_mutex);

    pthread_mutex_destroy(&cache->pool_mutex);
    wasm_runtime_destroy();
    /* Destroy the seal arena AFTER wasm_runtime_destroy so WAMR
     * can no longer dereference its node->native_symbols pointer
     * (g_native_symbols_list cleanup happens inside
     * wasm_runtime_destroy). */
    if (cache->native_arena) {
        sh_seal_arena_destroy(cache->native_arena);
        free(cache->native_arena);
        cache->native_arena = NULL;
    }
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

    /* Skip early unlocked cache_find / count check — the authoritative
     * double-check happens under pool_mutex at the insertion point below.
     * Unlocked reads of cache->count and modules[] are data races under C11. */

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
            long fsize = -1;
            if (fseek(f, 0, SEEK_END) == 0) {
                fsize = ftell(f);
                if (fseek(f, 0, SEEK_SET) != 0) fsize = -1;
            }
            if (fsize > 0 && (uint64_t)fsize < HL_WASM_MAX_IO_SIZE
                && (uint64_t)fsize <= UINT32_MAX) {
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
            long fsize = -1;
            if (fseek(f, 0, SEEK_END) == 0) {
                fsize = ftell(f);
                if (fseek(f, 0, SEEK_SET) != 0) fsize = -1;
            }
            if (fsize > 0 && (uint64_t)fsize < HL_WASM_MAX_IO_SIZE
                && (uint64_t)fsize <= UINT32_MAX) {
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

    /* Probe ABI version and Memory64 flag before taking the lock */
    uint32_t abi_version = 0;
    int detected_memory64 = 0;
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
#if WASM_ENABLE_MEMORY64 != 0
            /* Detect Memory64 via internal WAMR API */
            WASMMemoryInstance *mem = wasm_get_default_memory(
                (WASMModuleInstance *)tmp_inst);
            if (mem && mem->is_memory64)
                detected_memory64 = 1;
#endif
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
    cached->is_memory64 = detected_memory64;
    cached->abi_version = abi_version;
    if (pthread_mutex_init(&cached->mutex, NULL) != 0) {
        pthread_mutex_unlock(&cache->pool_mutex);
        log_error("[wasm] per-module mutex init failed for '%s'", name);
        wasm_runtime_unload(module);
        free(buf);
        return HL_WASM_ERR_INTERNAL;
    }
    cache->count++;

    pthread_mutex_unlock(&cache->pool_mutex);

    log_info("[wasm] cached module '%s' (abi=%u, aot=%d, mem64=%d)",
             name, cached->abi_version, is_aot, detected_memory64);

    /* AOT-fallback warning: Hull always links the WAMR AOT loader, so
     * loading via the (much slower) fast interpreter when an AOT-compiled
     * variant could exist is almost always a build-pipeline oversight.
     * Fires once per module load (which is cached, so once per process).
     *
     * Suppress the warning when HULL_QUIET_AOT=1 (set by tests + CI that
     * intentionally exercise the interpreter path). */
    if (!is_aot && getenv("HULL_QUIET_AOT") == NULL) {
        log_warn("[wasm] module '%s' is running in the fast interpreter "
                 "(no AOT artifact found). Compile with `wamrc` (make wamrc) "
                 "and rebuild — AOT is typically ~50x faster.",
                 name);
    }
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
    uint64_t max_input  = opts && opts->max_input  ? opts->max_input  : HL_WASM_DEFAULT_MAX_INPUT;
    uint64_t max_output = opts && opts->max_output  ? opts->max_output : HL_WASM_DEFAULT_MAX_OUTPUT;
    uint32_t heap_size  = opts && opts->heap_size   ? opts->heap_size  : HL_WASM_DEFAULT_HEAP;
    uint32_t stack_size = opts && opts->stack_size   ? opts->stack_size : HL_WASM_DEFAULT_STACK;
    int64_t  gas        = opts && opts->gas          ? opts->gas        : HL_WASM_DEFAULT_GAS;

    /* Clamp to maximums (I/O limits widened for Memory64 after module detection) */
    if (max_input > HL_WASM64_MAX_IO_SIZE)   max_input = HL_WASM64_MAX_IO_SIZE;
    if (max_output > HL_WASM64_MAX_IO_SIZE)  max_output = HL_WASM64_MAX_IO_SIZE;
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

    /* Memory64 modules require AOT (fast interpreter doesn't support Memory64) */
    if (mod->is_memory64 && !mod->is_aot) {
        if (err_msg) *err_msg = "memory64_requires_aot";
        return HL_WASM_ERR_LOAD;
    }

    /* Tighten I/O clamp for WASM32 modules */
    if (!mod->is_memory64) {
        if (max_input > HL_WASM_MAX_IO_SIZE)   max_input = HL_WASM_MAX_IO_SIZE;  // NOLINT
        if (max_output > HL_WASM_MAX_IO_SIZE)  max_output = HL_WASM_MAX_IO_SIZE;
    }
    (void)max_input; /* used only for validation at line 614; narrowing preserved for consistency */

    /* Try to acquire from instance pool.
     * Shared data is snapshotted under the mutex for thread safety. */
    wasm_module_inst_t inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t process_fn = NULL;
    const HlWasmSharedData *sd_snapshot = NULL;
    void *chain_head_snapshot = NULL;
    int from_pool = pool_acquire(mod, heap_size, stack_size,
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
    hl_wasm_attach_shared_heap(inst, chain_head_snapshot);

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
            log_error("[wasm] failed to allocate output buffer (%" PRIu64 " bytes)", max_output);
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

    /* Call hull_process with the appropriate ABI.
     * WASM32: hull_process(i32, i32, i32, i32) -> i32 = 4 argv cells
     * Memory64: hull_process(i64, i64, i64, i64) -> i32 = 8 argv cells (2 per i64) */
    uint32_t argv[9]; /* max 8 cells + 1 for result */
    int argc;
    if (mod->is_memory64) {
        argv[0] = (uint32_t)(wasm_in_ptr);        argv[1] = (uint32_t)(wasm_in_ptr >> 32);
        argv[2] = (uint32_t)(input_len);           argv[3] = (uint32_t)((uint64_t)input_len >> 32);
        argv[4] = (uint32_t)(wasm_out_ptr);        argv[5] = (uint32_t)(wasm_out_ptr >> 32);
        argv[6] = (uint32_t)(max_output);          argv[7] = (uint32_t)(max_output >> 32);
        argc = 8;
    } else {
        /* WASM32: addresses guaranteed ≤ UINT32_MAX by module_malloc */
        if (wasm_in_ptr > UINT32_MAX || wasm_out_ptr > UINT32_MAX) {
            if (err_msg) *err_msg = err_internal;
            goto cleanup_bufs_err;
        }
        argv[0] = (uint32_t)wasm_in_ptr;
        argv[1] = (uint32_t)input_len;
        argv[2] = (uint32_t)wasm_out_ptr;
        argv[3] = (uint32_t)max_output;
        argc = 4;
    }

    if (!wasm_runtime_call_wasm(exec_env, process_fn, (uint32_t)argc, argv)) {
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

    /* Lazy-load module + snapshot shared data under mod->mutex */
    HlWasmModule *mod = NULL;
    void *chain_head_snapshot = NULL;
    {
        pthread_mutex_lock(&cache->pool_mutex);
        mod = cache_find(cache, name);
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
        pthread_mutex_unlock(&cache->pool_mutex);
        if (!mod) {
            if (err_msg) *err_msg = err_internal;
            return NULL;
        }
    }
    /* Snapshot shared data under per-module mutex */
    {
        pthread_mutex_lock(&mod->mutex);
        if (mod->shared_data)
            chain_head_snapshot = mod->shared_data->chain_head;
        pthread_mutex_unlock(&mod->mutex);
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
    hl_wasm_attach_shared_heap(inst, chain_head_snapshot);

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
    uint64_t max_input  = opts && opts->max_input  ? opts->max_input
                        : pi->default_max_input    ? pi->default_max_input
                        : HL_WASM_DEFAULT_MAX_INPUT;
    uint64_t max_output = opts && opts->max_output  ? opts->max_output
                        : pi->default_max_output    ? pi->default_max_output
                        : HL_WASM_DEFAULT_MAX_OUTPUT;
    int64_t  gas        = opts && opts->gas          ? opts->gas
                        : pi->default_gas            ? pi->default_gas
                        : HL_WASM_DEFAULT_GAS;

    {
        uint64_t io_max = (pi->module && pi->module->is_memory64)
                        ? HL_WASM64_MAX_IO_SIZE : HL_WASM_MAX_IO_SIZE;
        if (max_input > io_max)  max_input = io_max;
        if (max_output > io_max) max_output = io_max;
    }
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
     * Read shared_data under per-module mutex for thread safety. */
    const HlWasmSharedData *pi_sd = NULL;
    if (pi->module) {
        pthread_mutex_lock(&pi->module->mutex);
        pi_sd = pi->module->shared_data;
        pthread_mutex_unlock(&pi->module->mutex);
    }
    tl_host_ctx.shared_data = pi_sd;

    int is_mem64 = pi->module ? pi->module->is_memory64 : 0;
    uint32_t argv[9];
    int argc_call;
    if (is_mem64) {
        argv[0] = (uint32_t)(wasm_in_ptr);        argv[1] = (uint32_t)(wasm_in_ptr >> 32);
        argv[2] = (uint32_t)(input_len);           argv[3] = (uint32_t)((uint64_t)input_len >> 32);
        argv[4] = (uint32_t)(wasm_out_ptr);        argv[5] = (uint32_t)(wasm_out_ptr >> 32);
        argv[6] = (uint32_t)(max_output);          argv[7] = (uint32_t)(max_output >> 32);
        argc_call = 8;
    } else {
        if (wasm_in_ptr > UINT32_MAX || wasm_out_ptr > UINT32_MAX) {
            if (err_msg) *err_msg = err_internal;
            goto cleanup_bufs_err;
        }
        argv[0] = (uint32_t)wasm_in_ptr;
        argv[1] = (uint32_t)input_len;
        argv[2] = (uint32_t)wasm_out_ptr;
        argv[3] = (uint32_t)max_output;
        argc_call = 4;
    }

    if (!wasm_runtime_call_wasm(exec_env, process_fn, (uint32_t)argc_call, argv)) {
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

/* ── Streaming I/O context helpers (for wasm_stream.c) ─────────────── */

void hl_cap_wasm_set_stream_ctx(uint32_t flags, uint32_t chunk_index)
{
    tl_host_ctx.stream_flags = flags;
    tl_host_ctx.stream_chunk_index = chunk_index;
}

void hl_cap_wasm_clear_stream_ctx(void)
{
    tl_host_ctx.stream_flags = 0;
    tl_host_ctx.stream_chunk_index = 0;
}

#endif /* HL_ENABLE_WASM */
