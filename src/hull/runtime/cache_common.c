/**
 * @file cache_common.c
 * @brief Implementation of helpers shared by every runtime cache.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/cache_common.h"
#include "hull/cache_dir.h"

#include <limits.h>
#include <stdint.h>
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

HlBlobStore *hl_runtime_cache_singleton(const char *kind,
                                        HlBlobStore **store_slot,
                                        int          *failed_slot)
{
    if (!kind || !store_slot || !failed_slot) return NULL;
    if (*store_slot)  return *store_slot;
    if (*failed_slot) return NULL;

    char root[PATH_MAX];
    if (hl_hull_cache_subdir(kind, root, sizeof(root)) != 0) {
        *failed_slot = 1;
        return NULL;
    }
    /* hl_hull_cache_subdir returns a path with a trailing slash;
     * blob_store_open trims internally but keep it tidy here. */
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/') root[--rl] = '\0';

    HlBlobStore *s = NULL;
    if (hl_blob_store_open(&s, NULL, root, /*shard_depth=*/1, 0) != 0) {
        *failed_slot = 1;
        return NULL;
    }
    *store_slot = s;
    return s;
}

void hl_runtime_cache_singleton_reset(HlBlobStore **store_slot,
                                      int          *failed_slot)
{
    if (!store_slot || !failed_slot) return;
    if (*store_slot) {
        hl_blob_store_close(*store_slot);
        *store_slot = NULL;
    }
    *failed_slot = 0;
}
