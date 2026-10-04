/**
 * @file cache_common.c
 * @brief Implementation of helpers shared by every runtime cache.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/cache_common.h"
#include "hull/shared/cache_dir.h"
#include "../utils/hex.h"

#include "hull/shared/host.h"
#include "hull/cap/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    /* hex_out holds src_len * 2 + 1 by this function's contract. */
    (void)hl_hex_encode(src, src_len, hex_out, src_len * 2 + 1);
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

/* ── Sealed entries (see cache_common.h) ─────────────────────────────── */

#define SEAL_MAC_LEN 32

static pthread_once_t g_seal_once = PTHREAD_ONCE_INIT;
static int            g_seal_ok;
static uint8_t        g_seal_key[32];

static int seal_read_key(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    int bad = fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
              st.st_size != (off_t)sizeof g_seal_key;
    if (!bad && !hl_host_is_windows())
        bad = st.st_uid != geteuid() || (st.st_mode & (S_IRWXG | S_IRWXO));
    size_t got = 0;
    while (!bad && got < sizeof g_seal_key) {
        ssize_t n = read(fd, g_seal_key + got, sizeof g_seal_key - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { bad = 1; break; }
        got += (size_t)n;
    }
    close(fd);
    return bad ? -1 : 0;
}

static void seal_init(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char dir[PATH_MAX], path[PATH_MAX];
    if ((size_t)snprintf(dir, sizeof dir, "%s/.hull", home) >= sizeof dir ||
        (size_t)snprintf(path, sizeof path, "%s/cache.key", dir) >= sizeof path)
        return;
    if (seal_read_key(path) == 0) { g_seal_ok = 1; return; }

    /* First use: make one. O_EXCL - a racing process that made it first
     * wins, and its key is read back. */
    (void)mkdir(dir, 0700);
    uint8_t k[32];
    if (hl_cap_crypto_random(k, sizeof k) != 0) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        size_t off = 0;
        int bad = fchmod(fd, 0600) != 0;
        while (!bad && off < sizeof k) {
            ssize_t n = write(fd, k + off, sizeof k - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { bad = 1; break; }
            off += (size_t)n;
        }
        if (close(fd) != 0) bad = 1;
        if (bad) unlink(path);
    }
    memset(k, 0, sizeof k);
    if (seal_read_key(path) == 0) g_seal_ok = 1;
}

static int seal_key(void)
{
    pthread_once(&g_seal_once, seal_init);
    return g_seal_ok;
}

void hl_runtime_cache_seal_prepare(void)
{
    (void)seal_key();
}

/* The MAC binds the entry to WHERE it is stored, not only to its bytes:
 * HMAC(key, kind || 0 || cache key || 0 || SHA-256(bytes)). A MAC over the
 * bytes alone let anything that can write the cache directory move a valid
 * entry to another key - app-chosen template code (it compiles to a sealed
 * Lua dump) planted as a stdlib module's bytecode. */
static int seal_mac(const char *kind, const char *key, const uint8_t *data,
                    size_t len, uint8_t mac[SEAL_MAC_LEN])
{
    uint8_t digest[32];
    if (hl_cap_crypto_sha256(data, len, digest) != 0) return -1;
    size_t kl = strlen(kind), keyl = strlen(key);
    if (kl > 256 || keyl > 1024) return -1;
    uint8_t msg[256 + 1 + 1024 + 1 + 32];
    size_t off = 0;
    memcpy(msg + off, kind, kl);  off += kl;  msg[off++] = 0;
    memcpy(msg + off, key, keyl); off += keyl; msg[off++] = 0;
    memcpy(msg + off, digest, sizeof digest); off += sizeof digest;
    return hl_cap_crypto_hmac_sha256(g_seal_key, sizeof g_seal_key, msg, off, mac);
}

int hl_runtime_cache_get_sealed(HlBlobStore *store, const char *kind,
                                const char *key, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!store || !kind || !key || !seal_key()) return -1;
    uint8_t *buf = NULL;
    size_t len = 0;
    if (hl_blob_store_get(store, key, /*track_access=*/1, &buf, &len) != 0)
        return -1;
    uint8_t mac[SEAL_MAC_LEN];
    int ok = len > SEAL_MAC_LEN &&
             seal_mac(kind, key, buf + SEAL_MAC_LEN, len - SEAL_MAC_LEN, mac) == 0;
    if (ok) {
        uint8_t diff = 0;
        for (size_t i = 0; i < SEAL_MAC_LEN; i++) diff |= (uint8_t)(mac[i] ^ buf[i]);
        ok = diff == 0;
    }
    if (!ok) {
        free(buf);
        (void)hl_blob_store_delete(store, key);   /* not ours: drop it */
        return -1;
    }
    memmove(buf, buf + SEAL_MAC_LEN, len - SEAL_MAC_LEN);
    *out = buf;
    *out_len = len - SEAL_MAC_LEN;
    return 0;
}

void hl_runtime_cache_put_sealed(HlBlobStore *store, const char *kind,
                                 const char *key, const uint8_t *data, size_t len)
{
    if (!store || !kind || !key || !data || len == 0 || !seal_key()) return;
    if (len > SIZE_MAX - SEAL_MAC_LEN) return;
    uint8_t *buf = malloc(len + SEAL_MAC_LEN);
    if (!buf) return;
    if (seal_mac(kind, key, data, len, buf) == 0) {
        memcpy(buf + SEAL_MAC_LEN, data, len);
        (void)hl_blob_store_put_keyed(store, key, buf, len + SEAL_MAC_LEN);
    }
    free(buf);
}
