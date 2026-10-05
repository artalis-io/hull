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

#include "hull/shared/blob_store.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Canonical arch tag for the host CPU.
 *
 * Returns one of: "x86_64", "aarch64", "i386", "arm", "riscv64",
 * "unknown". Stable string literal - safe to store. Used as a
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
 * @brief Per-cache slot state. Caller owns the storage (one static
 *        instance per cache module); the helper owns the mutation.
 *
 * Zero-initialised at file scope (C guarantees zero init for
 * static storage), so callers don't need an explicit initializer.
 * Fields are not part of the public API - treat as opaque.
 */
typedef struct {
    HlBlobStore *store;             /* opened store, NULL until first call */
    int          failed;            /* 1 = open failed, don't retry */
    int          atexit_registered; /* 1 = close hook registered */
} HlRuntimeCacheSlot;

/**
 * @brief Lazy process-wide `HlBlobStore` singleton for @p kind.
 *
 * On first successful open: resolves the cache subdir via
 * `hl_hull_cache_subdir`, opens the store with `shard_depth=1`,
 * caches it in `slot->store`, and (if @p atexit_close is non-NULL)
 * registers @p atexit_close via `atexit(3)` so the store is closed
 * cleanly at process exit. On open failure `slot->failed` is set
 * and subsequent calls short-circuit to NULL - failures aren't
 * re-attempted to avoid log spam on a permanent issue (HOME unset,
 * permission denied, full disk).
 *
 * Thread-safe. An internal mutex serialises the open + atexit
 * registration so concurrent first-touches from multiple threads
 * neither leak a duplicate store nor double-register the close
 * hook. The fast path (store already open) holds the mutex briefly
 * around a single pointer read.
 *
 * Allocator is NULL → blob_store falls back to libc malloc/free
 * (cache I/O isn't charged to any HlRuntime's memory limit).
 *
 * @param kind         Registered cache kind (e.g. "lua-bytecode").
 * @param slot         Caller-owned per-cache slot (static storage).
 * @param atexit_close Optional close hook; called once at process
 *                     exit via atexit(3). Caller-provided because
 *                     it needs to close the caller's specific slot.
 *                     Pass NULL to skip atexit registration (tests).
 * @return the open store, or NULL on failure.
 */
HlBlobStore *hl_runtime_cache_singleton(const char         *kind,
                                        HlRuntimeCacheSlot *slot,
                                        void              (*atexit_close)(void));

/**
 * @brief Close + reset a cache singleton opened by
 *        `hl_runtime_cache_singleton`. Idempotent.
 *
 * After this call `slot->store == NULL` and `slot->failed == 0`,
 * so the next `_singleton` call retries the open. The
 * `atexit_registered` flag is preserved - once registered, the
 * atexit handler stays registered for the process lifetime
 * (re-registering on a fresh open would double-fire on exit).
 *
 * Intended for test teardown and the atexit hook itself.
 */
void hl_runtime_cache_singleton_reset(HlRuntimeCacheSlot *slot);

/**
 * @brief Read / write a cache entry that is loaded as code (Lua or QuickJS
 *        bytecode): sealed with an HMAC-SHA256.
 *
 * The runtimes load these entries as binary chunks, and neither Lua nor
 * QuickJS verifies bytecode - a crafted chunk corrupts memory and can carry
 * a stdlib source name. Anything able to write the cache directory (another
 * app of the same user, an app whose fs.write grant covers it) could plant
 * one under a stdlib module's key. Each entry is stored as MAC || bytes,
 * the MAC keyed by a per-user secret kept OUTSIDE the cache directory
 * ($HOME/.hull/cache.key, 0600, made on first use and read once, before the
 * sandbox narrows file access). A missing key, or a key file others can
 * read, turns these caches off.
 *
 * get: 0 with *out (malloc'd, caller frees) / *out_len when the entry
 * exists and its MAC verifies; -1 otherwise (an entry that fails to verify
 * is deleted). put: best-effort.
 *
 * The MAC covers the cache KIND and KEY as well as the bytes, so a valid
 * entry moved to another key (or another cache) fails to verify. @p kind is
 * the store's kind ("lua-bytecode", "templates", ...).
 */
int  hl_runtime_cache_get_sealed(HlBlobStore *store, const char *kind,
                                 const char *key, uint8_t **out, size_t *out_len);
void hl_runtime_cache_put_sealed(HlBlobStore *store, const char *kind,
                                 const char *key, const uint8_t *data, size_t len);

/**
 * @brief Load (or create) the seal key now. Called by runtime init, which
 *        runs before the kernel sandbox narrows file access: read lazily,
 *        an app whose first cache use came after that found the key out of
 *        reach and silently ran with its code caches off. Idempotent.
 */
void hl_runtime_cache_seal_prepare(void);

/**
 * @brief The same sealing, under the BUILD TOOL's own key
 *        ($HOME/.hull/tool-cache.key), for caches whose entries a build
 *        embeds (the compute-aot cache: native code).
 *
 * Every app process loads the runtime key above before its sandbox
 * applies, and keeps read-write access to the shared cache pool, so an app
 * compromised at native level could read that key from its own memory and
 * seal a forged AOT blob under a known .wasm's key - which the next
 * `hull build` of another app embedded as a "cache hit". The tool key is
 * loaded only by the tool VM (hull build / eject), never by an app runtime,
 * and lives outside the cache directory apps can reach.
 */
int  hl_tool_cache_get_sealed(HlBlobStore *store, const char *kind,
                              const char *key, uint8_t **out, size_t *out_len);
void hl_tool_cache_put_sealed(HlBlobStore *store, const char *kind,
                              const char *key, const uint8_t *data, size_t len);

/** @brief Load (or create) the tool key now - before the tool sandbox. */
void hl_tool_cache_seal_prepare(void);

#endif /* HL_RUNTIME_CACHE_COMMON_H */
