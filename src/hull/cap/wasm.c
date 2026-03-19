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
#include "hull/cap/audit.h"
#include "hull/limits.h"
#include "hull/vfs.h"
#include "log.h"
#include "wasm_export.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    void            *module_inst; /* wasm_module_inst_t for addr conversion */
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
        int cb_id = (int)cb_id_u;
        const void *cb_in = data + 4;
        size_t cb_in_len = (size_t)(len - 4);

        /* Allocate output buffer in WASM memory for callback result */
        char out_buf[4096];
        int rc = tl_host_ctx.fn(cb_id, cb_in, cb_in_len,
                                out_buf, sizeof(out_buf), tl_host_ctx.ctx);
        return (int32_t)rc;
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
        if (*p == '/' || *p == '\\' || *p == '\0')
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

    cache->initialized = 1;
    log_debug("[wasm] WAMR runtime initialized");
    return 0;
}

void hl_cap_wasm_destroy(HlWasmCache *cache)
{
    if (!cache || !cache->initialized)
        return;

    for (int i = 0; i < cache->count; i++) {
        HlWasmModule *m = &cache->modules[i];
        if (m->module)
            wasm_runtime_unload((wasm_module_t)m->module);
        free(m->wasm_buf);
    }
    cache->count = 0;
    cache->initialized = 0;

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
            buf = malloc(e->len);
            if (buf) {
                memcpy(buf, e->data, e->len);
                buf_len = e->len;
                log_debug("[wasm] loaded module '%s' from VFS (%u bytes)",
                          name, buf_len);
            }
        }
    }

    /* 3. Filesystem fallback: <app_dir>/compute/<name>.wasm */
    if (!buf && app_dir) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/compute/%s.wasm", app_dir, name);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsize > 0 && fsize < (long)HL_WASM_MAX_IO_SIZE) {
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

    /* Add to cache */
    HlWasmModule *cached = &cache->modules[cache->count];
    snprintf(cached->name, sizeof(cached->name), "%s", name);
    cached->module = module;
    cached->wasm_buf = buf;
    cached->wasm_buf_len = buf_len;
    cached->is_aot = is_aot;

    /* Check hull_version export */
    cached->abi_version = 0;
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
                    cached->abi_version = argv[0];
                wasm_runtime_destroy_exec_env(env);
            }
        }
        wasm_runtime_deinstantiate(tmp_inst);
    }

    cache->count++;
    log_info("[wasm] cached module '%s' (abi=%u, aot=%d)",
             name, cached->abi_version, is_aot);
    return 0;
}

/* ── Call module ────────────────────────────────────────────────────── */

int hl_cap_wasm_call(HlWasmCache *cache, const char *name,
                     const void *input, size_t input_len,
                     void **output, size_t *output_len,
                     const HlWasmCallOpts *opts,
                     HlWasmCallbackFn callback_fn, void *callback_ctx,
                     const struct HlVfs *app_vfs, const char *app_dir,
                     const char **err_msg)
{
    static const char *err_internal  = "internal_error";
    static const char *err_not_found = "not_found";
    static const char *err_gas       = "gas_exhausted";
    static const char *err_output    = "output_too_small";
    static const char *err_input     = "input_too_large";
    static const char *err_load      = "load_failed";
    static const char *err_no_export = "no_hull_process_export";
    static const char *err_call      = "call_failed";

    if (!cache || !cache->initialized || !name || !output || !output_len) {
        if (err_msg) *err_msg = err_internal;
        return HL_WASM_ERR_INTERNAL;
    }

    if (validate_module_name(name) != 0) {
        if (err_msg) *err_msg = err_not_found;
        return HL_WASM_ERR_NOT_FOUND;
    }

    *output = NULL;
    *output_len = 0;

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
        ShJsonWriter w = hl_audit_begin("compute.call");
        sh_json_write_kv_string(&w, "module", name);
        hl_audit_end(&w);
    }

    /* Lazy-load module */
    HlWasmModule *mod = cache_find(cache, name);
    if (!mod) {
        int rc = hl_cap_wasm_load(cache, name, app_vfs, app_dir);
        if (rc != 0) {
            if (err_msg) *err_msg = (rc == HL_WASM_ERR_NOT_FOUND) ? err_not_found : err_load;
            return rc;
        }
        mod = cache_find(cache, name);
        if (!mod) {
            if (err_msg) *err_msg = err_internal;
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* Instantiate with configured limits */
    char error_buf[256];
    wasm_module_inst_t inst = wasm_runtime_instantiate(
        (wasm_module_t)mod->module, stack_size, heap_size,
        error_buf, sizeof(error_buf));
    if (!inst) {
        log_error("[wasm] instantiate '%s' failed: %s", name, error_buf);
        if (err_msg) *err_msg = err_internal;
        return HL_WASM_ERR_INTERNAL;
    }

    int ret = HL_WASM_ERR_INTERNAL;

    /* Lookup hull_process export */
    wasm_function_inst_t process_fn = wasm_runtime_lookup_function(
        inst, "hull_process");
    if (!process_fn) {
        log_error("[wasm] module '%s' missing hull_process export", name);
        if (err_msg) *err_msg = err_no_export;
        ret = HL_WASM_ERR_NOT_FOUND;
        goto cleanup;
    }

    /* Create execution environment */
    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, stack_size);
    if (!exec_env) {
        log_error("[wasm] failed to create exec env for '%s'", name);
        if (err_msg) *err_msg = err_internal;
        goto cleanup;
    }

    /* Set instruction count limit (gas metering) */
    if (gas > 0) {
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
            wasm_runtime_destroy_exec_env(exec_env);
            goto cleanup;
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
            wasm_runtime_destroy_exec_env(exec_env);
            goto cleanup;
        }
    }

    /* Save and set thread-local callback context (reentry-safe) */
    HlHostCallCtx saved_ctx = tl_host_ctx;
    tl_host_ctx.fn = callback_fn;
    tl_host_ctx.ctx = callback_ctx;
    tl_host_ctx.module_inst = inst;

    /* Call hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written */
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
        goto cleanup_bufs;
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
        goto cleanup_bufs;
    }

    /* Copy output back to caller */
    if (result > 0 && (uint32_t)result <= max_output) {
        void *out = malloc((size_t)result);
        if (!out) {
            if (err_msg) *err_msg = err_internal;
            goto cleanup_bufs;
        }
        memcpy(out, native_out, (size_t)result);
        *output = out;
        *output_len = (size_t)result;
    }

    ret = HL_WASM_OK;

cleanup_bufs:
    /* Restore previous callback context (reentry-safe) */
    tl_host_ctx = saved_ctx;

    if (wasm_in_ptr)  wasm_runtime_module_free(inst, wasm_in_ptr);
    if (wasm_out_ptr) wasm_runtime_module_free(inst, wasm_out_ptr);
    wasm_runtime_destroy_exec_env(exec_env);

cleanup:
    wasm_runtime_deinstantiate(inst);
    return ret;
}

#endif /* HL_ENABLE_WASM */
