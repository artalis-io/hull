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

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Forward declarations */
struct HlVfs;

/* ── Instance pool ─────────────────────────────────────────────────── */

#ifndef HL_WASM_POOL_MAX
#define HL_WASM_POOL_MAX 8
#endif

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

/* ── Module cache ──────────────────────────────────────────────────── */

#define HL_WASM_CACHE_MAX 64

typedef struct {
    char     name[256];
    void    *module;        /* wasm_module_t (opaque to callers) */
    uint8_t *wasm_buf;      /* owned buffer passed to wasm_runtime_load */
    uint32_t wasm_buf_len;
    int      is_aot;
    uint32_t abi_version;
    HlWasmPool pool;       /* per-module instance pool */
} HlWasmModule;

typedef struct HlWasmCache {
    HlWasmModule modules[HL_WASM_CACHE_MAX];
    int          count;
    int          initialized;
    pthread_mutex_t pool_mutex; /* guards all pool operations */
} HlWasmCache;

/* ── Call options ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t max_input;     /* default: 1 MB, max: 16 MB */
    uint32_t max_output;    /* default: 1 MB, max: 16 MB */
    uint32_t heap_size;     /* default: 1 MB, max: 64 MB */
    uint32_t stack_size;    /* default: 64 KB, max: 1 MB */
    int64_t  gas;           /* default: 10M, max: 1B instructions */
} HlWasmCallOpts;

/* ── Callback support ──────────────────────────────────────────────── */

/*
 * Callback function type: called when WASM invokes host_call(CALLBACK, id, ptr, len).
 * Returns bytes written to out_buf, or -1 on error.
 */
typedef int (*HlWasmCallbackFn)(int id, const void *in, size_t in_len,
                                void *out_buf, size_t out_max, void *user_data);

/* ── host_call opcodes ─────────────────────────────────────────────── */

#define HL_WASM_OP_LOG      0x01
#define HL_WASM_OP_CALLBACK 0x10

/* ── Error codes ───────────────────────────────────────────────────── */

#define HL_WASM_OK              0
#define HL_WASM_ERR_NOT_FOUND  -1
#define HL_WASM_ERR_GAS        -2
#define HL_WASM_ERR_OUTPUT     -3
#define HL_WASM_ERR_INPUT      -4
#define HL_WASM_ERR_INTERNAL   -5
#define HL_WASM_ERR_LOAD       -6
#define HL_WASM_ERR_ABI        -7

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
                     const char **err_msg);

#endif /* HL_ENABLE_WASM */
#endif /* HL_CAP_WASM_H */
