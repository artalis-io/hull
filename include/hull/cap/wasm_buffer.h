/*
 * cap/wasm_buffer.h — Zero-copy buffer for WASM compute output
 *
 * HlWasmBuffer wraps output from compute.call() without copying.
 * Three backing kinds: OWNED (malloc'd), MMAP (kernel mapping),
 * WASM (pointer into pooled WASM instance linear memory).
 *
 * WASM-backed buffers keep the instance checked out of the pool.
 * The instance is returned on hl_wasm_buffer_destroy().
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_WASM_BUFFER_H
#define HL_CAP_WASM_BUFFER_H

#ifdef HL_ENABLE_WASM

#include "hull/cap/fs.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct HlWasmCache;
struct HlWasmModule;
typedef struct HlAllocator HlAllocator;

/* ── Buffer kind ──────────────────────────────────────────────────── */

typedef enum {
    HL_WASM_BUF_OWNED,  /* malloc'd bytes, owned by this buffer */
    HL_WASM_BUF_MMAP,   /* kernel mmap'd region, wraps HlMappedBuffer */
    HL_WASM_BUF_WASM,   /* pointer into pooled WASM instance linear memory */
} HlWasmBufKind;

/* ── Buffer struct ────────────────────────────────────────────────── */

typedef struct HlWasmBuffer {
    HlWasmBufKind  kind;
    const void    *data;      /* native host pointer, always valid while !closed */
    size_t         len;
    int            closed;
    int            borrow_count; /* live zero-copy borrowers (e.g. images) */
    int            pending_free; /* close()/gc deferred while borrowed */
    HlAllocator   *alloc;     /* tracked allocator (NULL = raw malloc) */

    union {
        struct { void *alloc; } owned;                    /* free(alloc) */
        struct { HlMappedBuffer *mbuf; } mmap;            /* hl_cap_fs_munmap(mbuf) */
        struct {
            void    *instance;      /* wasm_module_inst_t */
            void    *exec_env;      /* wasm_exec_env_t */
            void    *process_fn;    /* wasm_function_inst_t (cached hull_process) */
            void    *module;        /* HlWasmModule* for pool return */
            void    *cache;         /* HlWasmCache* for pool mutex */
            uint64_t wasm_ptr;      /* WASM-space address (for module_free) */
            uint32_t heap_size;
            uint32_t stack_size;
        } wasm;                                            /* pool_release() */
    } u;
} HlWasmBuffer;

/* ── API ──────────────────────────────────────────────────────────── */

/**
 * Create a buffer owning a copy of [src, src+len).
 * Returns NULL on allocation failure. src may be NULL if len == 0.
 */
HlWasmBuffer *hl_wasm_buffer_create_owned(const void *src, size_t len,
                                           HlAllocator *alloc);

/**
 * Create a buffer wrapping an mmap'd region. Takes ownership of mbuf.
 * Returns NULL if mbuf is NULL or closed.
 */
HlWasmBuffer *hl_wasm_buffer_create_mmap(HlMappedBuffer *mbuf,
                                          HlAllocator *alloc);

/**
 * Create a buffer that takes ownership of a malloc'd pointer.
 * Does NOT copy — the buffer will free(data) on destroy.
 * data must have been allocated with malloc/calloc (not HlAllocator).
 */
HlWasmBuffer *hl_wasm_buffer_create_adopted(void *data, size_t len,
                                              HlAllocator *alloc);

/**
 * Create a buffer backed by WASM instance linear memory.
 * The instance stays checked out until hl_wasm_buffer_destroy().
 */
HlWasmBuffer *hl_wasm_buffer_create_wasm(
    void *inst, void *exec_env, void *process_fn,
    void *module, void *cache,
    uint64_t wasm_ptr, const void *native_ptr, size_t len,
    uint32_t heap_size, uint32_t stack_size,
    HlAllocator *alloc);

/**
 * Get data pointer. Returns NULL if buffer is closed.
 */
const void *hl_wasm_buffer_data(const HlWasmBuffer *buf);

/**
 * Get data length. Returns 0 if buffer is closed or NULL.
 */
size_t hl_wasm_buffer_len(const HlWasmBuffer *buf);

/**
 * Materialize buffer contents into a new allocated copy.
 * Caller must free (via hl_alloc_free) the returned pointer. Sets *out_len.
 * Returns NULL on failure or if buffer is closed.
 */
void *hl_wasm_buffer_materialize(const HlWasmBuffer *buf, size_t *out_len,
                                  HlAllocator *alloc);

/**
 * Destroy buffer and release all backing resources.
 * For WASM kind: frees WASM allocation and returns instance to pool.
 * Safe to call multiple times (idempotent). Safe on NULL.
 *
 * NOTE: this releases the BACKING resources but does NOT free the
 * HlWasmBuffer struct itself. Bindings should call hl_wasm_buffer_close()
 * (below) for full teardown so borrow-deferral is honored.
 */
void hl_wasm_buffer_destroy(HlWasmBuffer *buf);

/**
 * Full teardown for a binding close()/finalizer: destroys the backing AND
 * frees the struct. If the buffer still has live zero-copy borrowers
 * (borrow_count > 0), the teardown is deferred (records pending_free and
 * returns); the last hl_wasm_buffer_release() completes it. Safe on NULL.
 */
void hl_wasm_buffer_close(HlWasmBuffer *buf);

/**
 * Register a zero-copy borrower so a subsequent close()/gc defers teardown.
 * Event-loop thread only; not atomic. NULL-safe.
 */
void hl_wasm_buffer_borrow(HlWasmBuffer *buf);

/**
 * Release a borrow; if it was the last and a close was deferred, completes
 * the teardown now. `void *` typed for use as an HlImage::on_free hook.
 * NULL-safe.
 */
void hl_wasm_buffer_release(void *buf);

#endif /* HL_ENABLE_WASM */
#endif /* HL_CAP_WASM_BUFFER_H */
