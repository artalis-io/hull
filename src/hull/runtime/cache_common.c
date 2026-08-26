/**
 * @file cache_common.c
 * @brief Implementation of helpers shared by every runtime cache.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/cache_common.h"
#include "hull/shared/cache_dir.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char *hl_runtime_cache_arch_tag(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#elif defined(__arm__)
    return "arm";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return "unknown";
#endif
}

const char *hl_runtime_cache_endian_tag(void)
{
    /* Detected on every call, but the compiler folds the probe into
     * a constant since it has no side effects and the result is
     * a function of the build. Cheap. */
    uint16_t probe = 0x0102;
    return (*(const uint8_t *)&probe == 0x01) ? "be" : "le";
}

void hl_runtime_cache_hex_encode(const uint8_t *src, size_t src_len,
                                 char *hex_out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < src_len; i++) {
        hex_out[i * 2]     = hex[src[i] >> 4];
        hex_out[i * 2 + 1] = hex[src[i] & 0xF];
    }
    hex_out[src_len * 2] = '\0';
}

/* One process-wide mutex serialises the lazy-open + atexit-register
 * dance across all cache kinds. The slot itself is caller-owned
 * static storage; the mutex is owned here. The fast path (store
 * already open) holds the mutex for one pointer read - concurrent
 * cache hits on the same kind cost a mutex acquire + a load + a
 * release. That's fine for a cache lookup that's already an order
 * of magnitude faster than the parse it skips.
 *
 * PTHREAD_MUTEX_INITIALIZER works for static storage without a
 * constructor / pthread_once dance; it's the cleanest answer to
 * "I need a process-wide lock with no init step". */
static pthread_mutex_t g_singleton_mutex = PTHREAD_MUTEX_INITIALIZER;

HlBlobStore *hl_runtime_cache_singleton(const char         *kind,
                                        HlRuntimeCacheSlot *slot,
                                        void              (*atexit_close)(void))
{
    if (!kind || !slot) return NULL;

    pthread_mutex_lock(&g_singleton_mutex);
    if (slot->store) {
        HlBlobStore *s = slot->store;
        pthread_mutex_unlock(&g_singleton_mutex);
        return s;
    }
    if (slot->failed) {
        pthread_mutex_unlock(&g_singleton_mutex);
        return NULL;
    }

    char root[PATH_MAX];
    if (hl_hull_cache_subdir(kind, root, sizeof(root)) != 0) {
        slot->failed = 1;
        pthread_mutex_unlock(&g_singleton_mutex);
        return NULL;
    }
    /* hl_hull_cache_subdir returns a path with a trailing slash;
     * blob_store_open trims internally but keep it tidy here. */
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/') root[--rl] = '\0';

    HlBlobStore *s = NULL;
    if (hl_blob_store_open(&s, NULL, root, /*shard_depth=*/1, 0) != 0) {
        slot->failed = 1;
        pthread_mutex_unlock(&g_singleton_mutex);
        return NULL;
    }
    slot->store = s;

    /* Register the close hook exactly once per slot. Idempotent
     * across re-opens after a reset - once atexit owns the
     * callback we don't re-arm it. */
    if (atexit_close && !slot->atexit_registered) {
        slot->atexit_registered = 1;
        (void)atexit(atexit_close);
    }

    pthread_mutex_unlock(&g_singleton_mutex);
    return s;
}

void hl_runtime_cache_singleton_reset(HlRuntimeCacheSlot *slot)
{
    if (!slot) return;
    pthread_mutex_lock(&g_singleton_mutex);
    if (slot->store) {
        hl_blob_store_close(slot->store);
        slot->store = NULL;
    }
    slot->failed = 0;
    /* atexit_registered intentionally NOT cleared - see header
     * docstring. The atexit handler stays armed; re-opening a
     * slot must not re-register or we'd double-close on exit. */
    pthread_mutex_unlock(&g_singleton_mutex);
}
