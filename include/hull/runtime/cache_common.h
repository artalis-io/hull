/**
 * @file cache_common.h
 * @brief Helpers shared by every runtime cache module (Lua / JS
 *        bytecode + template caches).
 *
 * Each runtime cache file independently needs the same three things:
 *
 *   1. An arch tag and endian tag, folded into the cache key so an
 *      `$HOME` shared across machines (NFS, dotfile sync) never
 *      surfaces a foreign-arch artifact under a matching key.
 *
 *   2. A way to encode a sha256 digest as 64 lowercase hex chars
 *      (the on-disk blob filename format).
 *
 *   3. A lazy process-wide `HlBlobStore` singleton per cache kind,
 *      honoring `hl_hull_cache_disabled(KIND)` and falling back
 *      cleanly if the cache root can't be opened.
 *
 * Promoting these to one helper keeps the four cache files focused
 * on what's actually unique (key composition + serialization
 * format) and removes ~160 LOC of copy-pasted boilerplate.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_CACHE_COMMON_H
#define HL_RUNTIME_CACHE_COMMON_H

#include "hull/blob_store.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Canonical arch tag for the host CPU.
 *
 * Returns one of: "x86_64", "aarch64", "i386", "arm", "riscv64",
 * "unknown". Stable string literal — safe to store. Used as a
 * cache-key component so a key never spans architectures.
 */
const char *hl_runtime_cache_arch_tag(void);

/**
 * @brief Endianness tag for the host: "le" or "be".
 *
 * Detected at first call via a probe; result is stable for the
 * process lifetime.
 */
const char *hl_runtime_cache_endian_tag(void);

/**
 * @brief Encode @p src_len bytes as `2*src_len` lowercase hex chars
 *        plus a terminating NUL into @p hex_out.
 *
 * Caller must provide a buffer of at least `2*src_len + 1` bytes.
 * Used to render a sha256 digest into the on-disk blob id format.
 */
void hl_runtime_cache_hex_encode(const uint8_t *src, size_t src_len,
                                 char *hex_out);

/**
 * @brief Lazy process-wide `HlBlobStore` singleton for @p kind.
 *
 * @p store_slot and @p failed_slot point to two static variables
 * owned by the caller (one `HlBlobStore *` initialised to NULL,
 * one `int` initialised to 0). The first call resolves the
 * cache subdir via `hl_hull_cache_subdir`, opens the store with
 * `shard_depth=1`, caches it in `*store_slot`, and returns it.
 * Subsequent calls return the cached handle. On open failure
 * `*failed_slot` is set to 1 and subsequent calls short-circuit
 * to NULL — failures aren't re-attempted to avoid log spam on a
 * permanent issue (HOME unset, permission denied, full disk).
 *
 * Allocator is NULL → blob_store falls back to libc malloc/free
 * (cache I/O isn't charged to any HlRuntime's memory limit).
 *
 * @return the open store, or NULL on failure.
 */
HlBlobStore *hl_runtime_cache_singleton(const char *kind,
                                        HlBlobStore **store_slot,
                                        int          *failed_slot);

/**
 * @brief Close + reset a cache singleton opened by
 *        `hl_runtime_cache_singleton`. Idempotent.
 *
 * After this call `*store_slot == NULL` and `*failed_slot == 0`,
 * so the next `_singleton` call retries the open. Intended for
 * test teardown and the eventual atexit hook.
 */
void hl_runtime_cache_singleton_reset(HlBlobStore **store_slot,
                                      int          *failed_slot);

#endif /* HL_RUNTIME_CACHE_COMMON_H */
