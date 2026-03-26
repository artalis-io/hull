/*
 * cap/wasm.h — WASM compute capability (WAMR)
 *
 * Module cache + lifecycle + call dispatch for compute-only WASM plugins.
 * Plugins export hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written.
 * No WASI, no I/O — pure computation only.
 *
 * Gated by HL_ENABLE_WASM (set in Makefile when WAMR is available).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_WASM_H
#define HL_CAP_WASM_H

#ifdef HL_ENABLE_WASM

#include "hull/limits.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Forward declarations */
struct HlVfs;
typedef struct HlAllocator HlAllocator;

#include <stdatomic.h>

/* ── Instance pool ─────────────────────────────────────────────────── */

typedef struct {
    void    *instance;      /* wasm_module_inst_t */
    void    *exec_env;      /* wasm_exec_env_t */
    void    *process_fn;    /* wasm_function_inst_t (cached hull_process) */
    uint32_t heap_size;
    uint32_t stack_size;
} HlWasmPoolEntry;

typedef struct {
    HlWasmPoolEntry entries[HL_WASM_POOL_MAX];
    int count;
} HlWasmPool;

/* ── Shared data segments ──────────────────────────────────────────── */

/* One named data segment within a module's shared data */
typedef struct HlWasmDataSegment {
    char     name[64];       /* segment name (e.g. "graph", "landmarks") */
    void    *shared_heap;    /* wasm_shared_heap_t for this segment */
    void    *host_addr;      /* native pointer to data start */
    size_t   size;           /* logical data size in bytes (visible to WASM) */
    size_t   alloc_size;     /* page-aligned mmap size (for munmap) */
    uint64_t wasm_addr;      /* WASM-space start address (computed on chain) */
    int      is_mmap;        /* 1 = pre_alloc'd (caller owns backing), 0 = we own */
} HlWasmDataSegment;

/* All shared data for a module — segments chained into one shared heap */
typedef struct HlWasmSharedData {
    HlWasmDataSegment segments[HL_WASM_MAX_DATA_SEGMENTS];
    int               count;            /* number of active segments */
    void             *chain_head;       /* wasm_shared_heap_t chain head (for attach) */
} HlWasmSharedData;

/* ── Module cache ──────────────────────────────────────────────────── */

#define HL_WASM_CACHE_MAX 64

typedef struct {
    char     name[256];
    void    *module;        /* wasm_module_t (opaque to callers) */
    uint8_t *wasm_buf;      /* owned buffer passed to wasm_runtime_load */
    uint32_t wasm_buf_len;
    int      is_aot;
    int      is_memory64;  /* 1 = Memory64 module (i64 addresses) */
    uint32_t abi_version;
    HlWasmPool pool;       /* per-module instance pool */
    HlWasmSharedData *shared_data;  /* NULL until compute.data() called */
    pthread_mutex_t mutex;  /* guards pool + shared_data for this module */
} HlWasmModule;

typedef struct HlWasmCache {
    HlWasmModule modules[HL_WASM_CACHE_MAX];
    int          count;
    int          initialized;
    pthread_mutex_t pool_mutex; /* guards cache-level operations (module insertion/lookup) */
} HlWasmCache;

/* ── Call options ──────────────────────────────────────────────────── */

typedef struct {
    uint64_t max_input;     /* default: 1 MB, max: 16 GB (Memory64) / 256 MB (WASM32) */
    uint64_t max_output;    /* default: 1 MB, max: 16 GB (Memory64) / 256 MB (WASM32) */
    uint32_t heap_size;     /* default: 2 MB, max: ~4 GB */
    uint32_t stack_size;    /* default: 64 KB, max: 8 MB */
    int64_t  gas;           /* default: 10M, max: 100B instructions.
                             * WAMR's API takes int, so values > INT_MAX
                             * (~2.1B) are clamped with a log warning. */
} HlWasmCallOpts;

/* Clamp call options to configured maximums (CLI > manifest > defaults).
 * cfg_* fields are ceilings — 0 means "use compile-time default". */
void hl_cap_wasm_clamp_opts(HlWasmCallOpts *opts,
                             uint64_t cfg_max_input, uint64_t cfg_max_output,
                             uint32_t cfg_heap, uint32_t cfg_stack, int64_t cfg_gas);

/* ── Callback support ──────────────────────────────────────────────── */

/*
 * Callback function type: called when WASM invokes host_call(CALLBACK, id, ptr, len).
 * Returns bytes written to out_buf, or -1 on error.
 */
typedef int (*HlWasmCallbackFn)(int id, const void *in, size_t in_len,
                                void *out_buf, size_t out_max, void *user_data);

/* ── host_call opcodes ─────────────────────────────────────────────── */

#define HL_WASM_OP_LOG       0x01
#define HL_WASM_OP_DATA_INFO 0x02
#define HL_WASM_OP_CALLBACK  0x10

/* ── Error codes ───────────────────────────────────────────────────── */

typedef enum {
    HL_WASM_OK             =  0,
    HL_WASM_ERR_NOT_FOUND  = -1,
    HL_WASM_ERR_GAS        = -2,
    HL_WASM_ERR_OUTPUT     = -3,
    HL_WASM_ERR_INPUT      = -4,
    HL_WASM_ERR_INTERNAL   = -5,
    HL_WASM_ERR_LOAD       = -6,
    HL_WASM_ERR_ABI        = -7,
} HlWasmError;

/* Forward declaration */
struct HlWasmBuffer;

/* ── Persistent WASM instance ──────────────────────────────────────── */

typedef struct HlWasmInstance {
    void        *instance;      /* wasm_module_inst_t */
    void        *exec_env;      /* wasm_exec_env_t */
    void        *process_fn;    /* wasm_function_inst_t */
    HlWasmModule *module;       /* borrowed from cache */
    HlWasmCache  *cache;        /* borrowed */
    char         name[256];
    uint32_t     heap_size;     /* immutable after creation */
    uint32_t     stack_size;    /* immutable after creation */
    int64_t      default_gas;   /* 0 = use HL_WASM_DEFAULT_GAS */
    uint64_t     default_max_input;
    uint64_t     default_max_output;
    int          closed;
    atomic_int   busy;          /* 1 = async call in flight */
    HlAllocator *alloc;
} HlWasmInstance;

/* ── API ───────────────────────────────────────────────────────────── */

/**
 * Initialize WAMR runtime and module cache.
 * Returns 0 on success, -1 on failure.
 */
int hl_cap_wasm_init(HlWasmCache *cache);

/**
 * Destroy all cached modules and shut down WAMR runtime.
 */
void hl_cap_wasm_destroy(HlWasmCache *cache);

/**
 * Pre-load a WASM module by name.
 * Searches: VFS "compute/<name>.aot.<arch>" -> VFS "compute/<name>.wasm"
 *           -> filesystem fallback.
 * Returns 0 on success, negative on error.
 */
int hl_cap_wasm_load(HlWasmCache *cache, const char *name,
                     const struct HlVfs *app_vfs, const char *app_dir);

/**
 * Call a WASM compute module.
 *
 * Loads module on first call (lazy). Creates a fresh instance per call
 * with isolated linear memory. Enforces gas metering and size limits.
 *
 * @param cache       Module cache (must be initialized)
 * @param name        Module name (e.g. "score" -> compute/score.wasm)
 * @param input       Input bytes to pass to hull_process
 * @param input_len   Input byte length
 * @param output      Output: caller-freed buffer with result bytes
 * @param output_len  Output: number of bytes written
 * @param opts        Call options (NULL for defaults)
 * @param callback_fn Optional callback for CALLBACK opcode (NULL = disabled)
 * @param callback_ctx User data passed to callback_fn
 * @param app_vfs     App VFS for module loading
 * @param app_dir     App directory for filesystem fallback
 * @param err_msg     Output: static error string on failure
 * @return 0 on success, negative error code on failure
 */
int hl_cap_wasm_call(HlWasmCache *cache, const char *name,
                     const void *input, size_t input_len,
                     void **output, size_t *output_len,
                     const HlWasmCallOpts *opts,
                     HlWasmCallbackFn callback_fn, void *callback_ctx,
                     const struct HlVfs *app_vfs, const char *app_dir,
                     HlAllocator *alloc, const char **err_msg);

/**
 * Call a WASM compute module, returning output as an HlWasmBuffer.
 *
 * Same as hl_cap_wasm_call but returns zero-copy HlWasmBuffer instead of
 * malloc'd bytes. When the instance is poolable (success && heap <= threshold),
 * the buffer holds the instance checked out (WASM kind). Otherwise, output is
 * copied to an OWNED buffer and the instance is released immediately.
 *
 * Caller must call hl_wasm_buffer_destroy() on the returned buffer.
 */
int hl_cap_wasm_call_buf(HlWasmCache *cache, const char *name,
                         const void *input, size_t input_len,
                         struct HlWasmBuffer **output_buf,
                         const HlWasmCallOpts *opts,
                         HlWasmCallbackFn callback_fn, void *callback_ctx,
                         const struct HlVfs *app_vfs, const char *app_dir,
                         HlAllocator *alloc, const char **err_msg);

/**
 * Drain all pooled instances for a module. Caller must hold mod->mutex.
 */
void hl_wasm_pool_drain(HlWasmPool *pool);

/**
 * Return an instance to the pool, or destroy it.
 * Exported for use by wasm_buffer.c when destroying WASM-backed buffers.
 */
void hl_wasm_pool_release(HlWasmCache *cache, HlWasmModule *mod,
                          void *inst, void *exec_env, void *process_fn,
                          uint32_t heap_size, uint32_t stack_size,
                          int success);

/* ── Persistent instance API ───────────────────────────────────────── */

/**
 * Create a persistent WASM instance. Not pooled — exclusively owned by caller.
 * Linear memory is preserved across calls. Caller must destroy when done.
 *
 * @param cache    Module cache (must be initialized)
 * @param name     Module name (e.g. "model" -> compute/model.wasm)
 * @param opts     Instance options (heap, stack, default gas) — NULL for defaults
 * @param app_vfs  App VFS for module loading
 * @param app_dir  App directory for filesystem fallback
 * @param alloc    Allocator (NULL = raw malloc)
 * @param err_msg  Output: static error string on failure
 * @return New instance, or NULL on error
 */
HlWasmInstance *hl_cap_wasm_instance_create(HlWasmCache *cache,
                                             const char *name,
                                             const HlWasmCallOpts *opts,
                                             const struct HlVfs *app_vfs,
                                             const char *app_dir,
                                             HlAllocator *alloc,
                                             const char **err_msg);

/**
 * Call hull_process on a persistent instance.
 * Gas resets per call. I/O is allocated/freed in linear memory per call
 * but the instance (globals, heap, data segments) persists.
 *
 * @param inst       Persistent instance
 * @param input      Input bytes
 * @param input_len  Input length
 * @param output     Output: caller-freed buffer
 * @param output_len Output: bytes written
 * @param opts       Per-call overrides (gas, max_input, max_output) — NULL for instance defaults
 * @param cb_fn      Optional callback
 * @param cb_ctx     Callback context
 * @param err_msg    Output: static error string on failure
 * @return 0 on success, negative error code on failure
 */
int hl_cap_wasm_instance_call(HlWasmInstance *inst,
                               const void *input, size_t input_len,
                               void **output, size_t *output_len,
                               const HlWasmCallOpts *opts,
                               HlWasmCallbackFn cb_fn, void *cb_ctx,
                               HlAllocator *alloc, const char **err_msg);

/**
 * Call hull_process on a persistent instance, returning HlWasmBuffer.
 * Always returns OWNED buffer (instance is persistent, not checked out).
 */
int hl_cap_wasm_instance_call_buf(HlWasmInstance *inst,
                                   const void *input, size_t input_len,
                                   struct HlWasmBuffer **output_buf,
                                   const HlWasmCallOpts *opts,
                                   HlWasmCallbackFn cb_fn, void *cb_ctx,
                                   HlAllocator *alloc, const char **err_msg);

/**
 * Destroy a persistent instance. NULL-safe, idempotent (closed flag).
 * If busy (async in-flight), logs a warning and does not destroy.
 */
void hl_cap_wasm_instance_destroy(HlWasmInstance *inst);

/* ── Shared data API ──────────────────────────────────────────────── */

/**
 * Load a named data segment for a module.
 *
 * segment_name=NULL + data=NULL: remove all segments for module.
 * segment_name!=NULL + data=NULL: remove that segment.
 * segment_name!=NULL + data!=NULL: add/replace that segment.
 *
 * pre_alloc non-NULL: zero-copy (caller owns backing memory, e.g. mmap'd).
 * Drains instance pool on any change.
 *
 * @return 0 on success, -1 on error
 */
int hl_cap_wasm_data_load(HlWasmCache *cache, const char *module_name,
                           const char *segment_name,
                           const void *data, size_t data_len,
                           void *pre_alloc,
                           const struct HlVfs *app_vfs, const char *app_dir,
                           const char **err_msg);

/**
 * Remove all shared data for a module.
 */
void hl_cap_wasm_data_unload(HlWasmCache *cache, const char *module_name);

#endif /* HL_ENABLE_WASM */
#endif /* HL_CAP_WASM_H */
