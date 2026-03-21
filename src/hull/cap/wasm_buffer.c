/*
 * hull_cap_wasm_buffer.c — Zero-copy buffer for WASM compute output
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm_buffer.h"
#include "hull/cap/wasm.h"
#include "hull/alloc.h"
#include "wasm_export.h"

#include <stdlib.h>
#include <string.h>

/* ── OWNED ────────────────────────────────────────────────────────── */

HlWasmBuffer *hl_wasm_buffer_create_owned(const void *src, size_t len,
                                           HlAllocator *alloc)
{
    HlWasmBuffer *buf = hl_alloc_calloc(alloc, 1, sizeof(*buf));
    if (!buf) return NULL;

    buf->kind  = HL_WASM_BUF_OWNED;
    buf->len   = len;
    buf->alloc = alloc;

    if (len > 0 && src) {
        void *copy = hl_alloc_malloc(alloc, len);
        if (!copy) { hl_alloc_free(alloc, buf, sizeof(*buf)); return NULL; }
        memcpy(copy, src, len);
        buf->data = copy;
        buf->u.owned.alloc = copy;
    }

    return buf;
}

/* ── MMAP ─────────────────────────────────────────────────────────── */

HlWasmBuffer *hl_wasm_buffer_create_mmap(HlMappedBuffer *mbuf,
                                          HlAllocator *alloc)
{
    if (!mbuf || mbuf->closed) return NULL;

    HlWasmBuffer *buf = hl_alloc_calloc(alloc, 1, sizeof(*buf));
    if (!buf) return NULL;

    buf->kind  = HL_WASM_BUF_MMAP;
    buf->data  = mbuf->addr;
    buf->len   = mbuf->len;
    buf->alloc = alloc;
    buf->u.mmap.mbuf = mbuf;

    return buf;
}

/* ── WASM (instance linear memory) ────────────────────────────────── */

HlWasmBuffer *hl_wasm_buffer_create_wasm(
    void *inst, void *exec_env, void *process_fn,
    void *module, void *cache,
    uint64_t wasm_ptr, const void *native_ptr, size_t len,
    uint32_t heap_size, uint32_t stack_size,
    HlAllocator *alloc)
{
    if (!inst || !native_ptr) return NULL;

    HlWasmBuffer *buf = hl_alloc_calloc(alloc, 1, sizeof(*buf));
    if (!buf) return NULL;

    buf->kind  = HL_WASM_BUF_WASM;
    buf->data  = native_ptr;
    buf->len   = len;
    buf->alloc = alloc;

    buf->u.wasm.instance   = inst;
    buf->u.wasm.exec_env   = exec_env;
    buf->u.wasm.process_fn = process_fn;
    buf->u.wasm.module     = module;
    buf->u.wasm.cache      = cache;
    buf->u.wasm.wasm_ptr   = wasm_ptr;
    buf->u.wasm.heap_size  = heap_size;
    buf->u.wasm.stack_size = stack_size;

    return buf;
}

/* ── Accessors ────────────────────────────────────────────────────── */

const void *hl_wasm_buffer_data(const HlWasmBuffer *buf)
{
    if (!buf || buf->closed) return NULL;
    return buf->data;
}

size_t hl_wasm_buffer_len(const HlWasmBuffer *buf)
{
    if (!buf || buf->closed) return 0;
    return buf->len;
}

void *hl_wasm_buffer_materialize(const HlWasmBuffer *buf, size_t *out_len,
                                  HlAllocator *alloc)
{
    if (out_len) *out_len = 0;
    if (!buf || buf->closed) return NULL;

    if (buf->len == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    void *copy = hl_alloc_malloc(alloc, buf->len);
    if (!copy) return NULL;

    memcpy(copy, buf->data, buf->len);
    if (out_len) *out_len = buf->len;
    return copy;
}

/* ── Destroy ──────────────────────────────────────────────────────── */

void hl_wasm_buffer_destroy(HlWasmBuffer *buf)
{
    if (!buf || buf->closed) return;
    buf->closed = 1;
    buf->data = NULL;

    switch (buf->kind) {
    case HL_WASM_BUF_OWNED:
        hl_alloc_free(buf->alloc, buf->u.owned.alloc, buf->len);
        buf->u.owned.alloc = NULL;
        break;

    case HL_WASM_BUF_MMAP:
        if (buf->u.mmap.mbuf) {
            hl_cap_fs_munmap(buf->u.mmap.mbuf);
            buf->u.mmap.mbuf = NULL;
        }
        break;

    case HL_WASM_BUF_WASM: {
        wasm_module_inst_t inst = (wasm_module_inst_t)buf->u.wasm.instance;

        /* Free the output allocation in WASM linear memory */
        if (buf->u.wasm.wasm_ptr)
            wasm_runtime_module_free(inst, buf->u.wasm.wasm_ptr);

        /* Return instance to pool (success=1 since call succeeded) */
        hl_wasm_pool_release(
            (HlWasmCache *)buf->u.wasm.cache,
            (HlWasmModule *)buf->u.wasm.module,
            buf->u.wasm.instance,
            buf->u.wasm.exec_env,
            buf->u.wasm.process_fn,
            buf->u.wasm.heap_size,
            buf->u.wasm.stack_size,
            1);

        buf->u.wasm.instance = NULL;
        buf->u.wasm.exec_env = NULL;
        break;
    }
    }

    buf->len = 0;
}

#endif /* HL_ENABLE_WASM */
