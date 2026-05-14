/*
 * hull_cap_wasm_data.c — WASM shared data management
 *
 * Shared heap segments, chain rebuild, option clamping, data load/unload.
 * Split from wasm.c to keep the module manageable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm_data.h"
#include "hull/cap/wasm.h"
#include "hull/limits/wasm.h"
#include "hull/vfs.h"
#include "log.h"
#include "wasm_export.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── Module name validation (duplicated from wasm.c — small, avoids export) */

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

/* ── Cache lookup (duplicated — small helper) ─────────────────────── */

static HlWasmModule *cache_find(HlWasmCache *cache, const char *name)
{
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->modules[i].name, name) == 0)
            return &cache->modules[i];
    }
    return NULL;
}

/* ── Shared data helpers ───────────────────────────────────────────── */

int hl_wasm_rebuild_chain(HlWasmModule *mod)
{
    HlWasmSharedData *sd = mod->shared_data;
    if (!sd)
        return 0;
    if (sd->count == 0) {
        sd->chain_head = NULL;
        return 0;
    }

    /* Address space ceiling depends on module memory width.
     * WASM32: UINT32_MAX, Memory64: UINT64_MAX.
     * WAMR places shared heaps at the high end of the address space. */
    int is_mem64 = mod->is_memory64;
    uint64_t addr_ceil = is_mem64 ? UINT64_MAX : (uint64_t)UINT32_MAX;

    /* For a single segment, no chaining needed — just create the heap */
    if (sd->count == 1) {
        if (sd->segments[0].alloc_size == 0) return -1;
        sd->segments[0].wasm_addr = addr_ceil - sd->segments[0].alloc_size + 1;
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

    /* Compute WASM addresses from the ceiling downward.
     * WAMR uses alloc_size (page-aligned) for heap mapping.
     * Last segment ends at addr_ceil. Work backwards without overflow:
     * addr_ceil - alloc_size + 1 gives the start of the last segment. */
    uint64_t next_start = addr_ceil;
    for (int i = sd->count - 1; i >= 0; i--) {
        sd->segments[i].wasm_addr = next_start - sd->segments[i].alloc_size + 1;
        if (i > 0)
            next_start = sd->segments[i].wasm_addr - 1;
    }

    return 0;
}

void hl_wasm_free_segment(HlWasmDataSegment *seg)
{
    if (!seg->is_mmap && seg->host_addr) {
        munmap(seg->host_addr, seg->alloc_size);
        seg->host_addr = NULL;
    }
    /* WAMR has no public API to destroy individual shared heaps.
     * The WASMSharedHeap struct (~64 bytes) is freed globally on
     * wasm_runtime_destroy(). For pre-allocated heaps (our case),
     * WAMR does not own the backing memory — we freed it above. */
    seg->shared_heap = NULL;
    seg->size = 0;
    seg->wasm_addr = 0;
    seg->name[0] = '\0';
}

void hl_wasm_free_shared_data(HlWasmModule *mod)
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
        hl_wasm_free_segment(&sd->segments[i]);

    sd->count = 0;
    sd->chain_head = NULL;
    free(sd);
    mod->shared_data = NULL;
}

void hl_wasm_attach_shared_heap(void *inst, void *chain_head)
{
    if (chain_head) {
        if (!wasm_runtime_attach_shared_heap(
                (wasm_module_inst_t)inst, (wasm_shared_heap_t)chain_head)) {
            log_error("[wasm] failed to attach shared heap to instance");
        }
    }
}

/* ── Option clamping ───────────────────────────────────────────────── */

void hl_cap_wasm_clamp_opts(HlWasmCallOpts *opts,
                             uint64_t cfg_max_input, uint64_t cfg_max_output,
                             uint32_t cfg_heap, uint32_t cfg_stack, int64_t cfg_gas)
{
    if (!opts) return;
    #define CLAMP_VAL(field, cfg) do { \
        if ((cfg) && (!(opts->field) || (opts->field) > (cfg))) \
            opts->field = (cfg); \
    } while(0)
    CLAMP_VAL(max_input,  cfg_max_input);
    CLAMP_VAL(max_output, cfg_max_output);
    CLAMP_VAL(heap_size,  cfg_heap);
    CLAMP_VAL(stack_size, cfg_stack);
    #undef CLAMP_VAL
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
    static const char *err_bad_name  = "segment_name_too_long";

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

    pthread_mutex_lock(&mod->mutex);

    /* Case 1: segment_name==NULL && data==NULL → remove all segments */
    if (!segment_name && !data) {
        hl_wasm_pool_drain(&mod->pool);
        hl_wasm_free_shared_data(mod);
        pthread_mutex_unlock(&mod->mutex);
        log_debug("[wasm] removed all shared data for '%s'", module_name);
        return 0;
    }

    /* From here, segment_name must be non-NULL */
    if (!segment_name) {
        pthread_mutex_unlock(&mod->mutex);
        if (err_msg) *err_msg = err_internal;
        return -1;
    }

    /* Validate segment name length (must fit in HlWasmDataSegment.name[64]) */
    if (strlen(segment_name) >= 64) {
        pthread_mutex_unlock(&mod->mutex);
        if (err_msg) *err_msg = err_bad_name;
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
                    hl_wasm_free_segment(&sd->segments[i]);
                    /* Compact: shift remaining segments */
                    for (int j = i; j < sd->count - 1; j++)
                        sd->segments[j] = sd->segments[j + 1];
                    sd->count--;
                    memset(&sd->segments[sd->count], 0, sizeof(HlWasmDataSegment));

                    hl_wasm_pool_drain(&mod->pool);
                    if (sd->count > 0)
                        hl_wasm_rebuild_chain(mod);
                    else {
                        free(sd);
                        mod->shared_data = NULL;
                    }
                    pthread_mutex_unlock(&mod->mutex);
                    log_debug("[wasm] removed segment '%s' from '%s'",
                              segment_name, module_name);
                    return 0;
                }
            }
        }
        /* Segment not found — OK, nothing to remove */
        pthread_mutex_unlock(&mod->mutex);
        return 0;
    }

    /* Case 3: segment_name!=NULL && data!=NULL → add/replace segment */
    if (data_len == 0 || data_len > HL_WASM_MAX_SHARED_DATA) {
        pthread_mutex_unlock(&mod->mutex);
        if (err_msg) *err_msg = err_too_large;
        return -1;
    }

    /* Allocate shared_data struct if needed */
    if (!mod->shared_data) {
        mod->shared_data = calloc(1, sizeof(HlWasmSharedData));
        if (!mod->shared_data) {
            pthread_mutex_unlock(&mod->mutex);
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
            pthread_mutex_unlock(&mod->mutex);
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
        pthread_mutex_unlock(&mod->mutex);
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
        hl_wasm_free_segment(&sd->segments[slot]);

    /* Create backing memory.
     * WAMR requires pre_allocated_addr regions to be page-aligned in size.
     * If pre_alloc is provided but data_len is not page-aligned, we copy
     * into a new page-aligned mmap region (not zero-copy in that case). */
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    size_t alloc_size = (data_len + (size_t)page_size - 1) & ~((size_t)page_size - 1);
    if (alloc_size == 0) alloc_size = (size_t)page_size;

    void *backing = NULL;
    int is_mmap_backing = 0;
    if (pre_alloc) {
        if (data_len == alloc_size) {
            backing = pre_alloc;
            is_mmap_backing = 1;
        } else {
            backing = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (backing == MAP_FAILED) {
                pthread_mutex_unlock(&mod->mutex);
                if (err_msg) *err_msg = err_internal;
                return -1;
            }
            memcpy(backing, pre_alloc, data_len);
            is_mmap_backing = 0;
        }
    } else {
        backing = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (backing == MAP_FAILED) {
            pthread_mutex_unlock(&mod->mutex);
            if (err_msg) *err_msg = err_internal;
            return -1;
        }
        memcpy(backing, data, data_len);
    }

    /* Create WAMR shared heap with pre-allocated backing.
     * SharedHeapInitArgs.size is uint32_t — guard against overflow. */
    if (alloc_size > UINT32_MAX) {
        if (!is_mmap_backing)
            munmap(backing, alloc_size);
        pthread_mutex_unlock(&mod->mutex);
        if (err_msg) *err_msg = err_too_large;
        return -1;
    }
    SharedHeapInitArgs heap_args;
    memset(&heap_args, 0, sizeof(heap_args));
    heap_args.size = (uint32_t)alloc_size;
    heap_args.pre_allocated_addr = backing;

    wasm_shared_heap_t heap = wasm_runtime_create_shared_heap(&heap_args);
    if (!heap) {
        if (!is_mmap_backing)
            munmap(backing, alloc_size);
        pthread_mutex_unlock(&mod->mutex);
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
    seg->size = data_len;
    seg->alloc_size = alloc_size;
    seg->is_mmap = is_mmap_backing;

    if (slot == sd->count)
        sd->count++;

    /* Rebuild chain and drain pool */
    hl_wasm_pool_drain(&mod->pool);
    int chain_rc = hl_wasm_rebuild_chain(mod);

    if (chain_rc != 0) {
        /* Rollback: remove the segment we just added */
        hl_wasm_free_segment(seg);
        if (slot == sd->count - 1)
            sd->count--;
        if (sd->count == 0) {
            free(sd);
            mod->shared_data = NULL;
        }
        pthread_mutex_unlock(&mod->mutex);
        if (err_msg) *err_msg = err_internal;
        return -1;
    }

    pthread_mutex_unlock(&mod->mutex);

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
    pthread_mutex_unlock(&cache->pool_mutex);

    if (mod) {
        pthread_mutex_lock(&mod->mutex);
        hl_wasm_pool_drain(&mod->pool);
        hl_wasm_free_shared_data(mod);
        pthread_mutex_unlock(&mod->mutex);
    }
}

#endif /* HL_ENABLE_WASM */
