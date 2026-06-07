/**
 * @file bytecode_cache.c
 * @brief On-disk Lua bytecode cache. See hull/runtime/lua_bytecode_cache.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/lua_bytecode_cache.h"
#include "hull/cache_dir.h"
#include "hull/cap/crypto.h"

#include "lauxlib.h"
#include "lua.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BC_SUBDIR  "lua-bytecode"

/* Endian + arch tags get hashed into the cache key so cross-platform
 * cache pools (e.g. NFS-mounted $HOME) don't accidentally load a
 * mismatched .luac. The Lua precompiled chunk header already encodes
 * these and `luaL_loadbuffer` validates them — this is a belt-and-
 * suspenders early reject so we don't even open the file. */
static const char *arch_tag(void)
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

static const char *endian_tag(void)
{
    uint16_t probe = 0x0102;
    return (*(const uint8_t *)&probe == 0x01) ? "be" : "le";
}

/* Cache-key digest = sha256(LUA_VERSION || "|" || arch || "|" ||
 * endian || "|" || source). LUA_VERSION moves on every Lua upgrade
 * (header constant), so stale cache files from an old vendor bump
 * never collide. */
static int compute_key(const char *src, size_t src_len,
                       char hex_out[65])
{
    HlSha256Ctx ctx;
    hl_cap_crypto_sha256_init(&ctx);

    const char *ver = LUA_VERSION;          /* e.g. "Lua 5.4" */
    const char *arch = arch_tag();
    const char *end = endian_tag();

    if (hl_cap_crypto_sha256_update(&ctx, ver, strlen(ver))   != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)             != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, arch, strlen(arch)) != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)             != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, end, strlen(end))   != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, "|", 1)             != 0) return -1;
    if (hl_cap_crypto_sha256_update(&ctx, src, src_len)       != 0) return -1;

    uint8_t digest[32];
    if (hl_cap_crypto_sha256_final(&ctx, digest) != 0) return -1;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i*2]     = hex[digest[i] >> 4];
        hex_out[i*2 + 1] = hex[digest[i] & 0xF];
    }
    hex_out[64] = '\0';
    return 0;
}

/* lua_dump writer that appends to a malloc'd buffer. Returns 0 (success)
 * to lua_dump per its contract; allocation failure flips an error flag
 * we check after. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    failed;
} DumpAcc;

static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    DumpAcc *a = (DumpAcc *)ud;
    if (a->failed) return 1;
    if (a->len + sz > a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 1024;
        while (new_cap < a->len + sz) new_cap *= 2;
        char *nb = (char *)realloc(a->buf, new_cap);
        if (!nb) { a->failed = 1; return 1; }
        a->buf = nb;
        a->cap = new_cap;
    }
    memcpy(a->buf + a->len, p, sz);
    a->len += sz;
    return 0;
}

/* Whole-file read into a malloc'd buffer. Returns 0 + (*out, *out_len)
 * on success; -1 on any failure (caller falls back to compiling source). */
static int read_all(const char *path, char **out, size_t *out_len)
{
    int fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                   | O_CLOEXEC
#endif
    );
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd); return -1;
    }
    if ((size_t)st.st_size > (size_t)64 * 1024 * 1024) { /* 64 MB sanity cap */
        close(fd); return -1;
    }

    size_t sz = (size_t)st.st_size;
    char *buf = (char *)malloc(sz);
    if (!buf) { close(fd); return -1; }

    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, buf + got, sz - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf); close(fd); return -1;
        }
        if (n == 0) { free(buf); close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);

    *out = buf;
    *out_len = sz;
    return 0;
}

/* Atomic write via tmp + rename — same recipe as cap/blob. */
static int write_atomic(const char *final_path, const char *data, size_t len)
{
    char tmp_path[PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d.%ld",
                     final_path, (int)getpid(), (long)time(NULL));
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) return -1;

    int fd = open(tmp_path,
                  O_WRONLY | O_CREAT | O_TRUNC
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
                  , 0644);
    if (fd < 0) return -1;

    size_t put = 0;
    while (put < len) {
        ssize_t w = write(fd, data + put, len - put);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp_path); return -1;
        }
        put += (size_t)w;
    }
    close(fd);

    /* rename(2) is atomic on POSIX — first-write-wins is fine
     * because the key encodes everything that affects the bytecode. */
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int hl_lua_load_cached(lua_State *L,
                       const char *src, size_t src_len,
                       const char *chunkname)
{
    /* Fast bail: cache globally or per-kind disabled, or source too
     * tiny to bother caching (compile cost ≪ disk I/O). */
    if (!src || src_len < 256 ||
        hl_hull_cache_disabled("bytecode")) {
        return luaL_loadbuffer(L, src, src_len, chunkname);
    }

    char cache_root[PATH_MAX];
    if (hl_hull_cache_subdir(BC_SUBDIR, cache_root, sizeof(cache_root)) != 0) {
        return luaL_loadbuffer(L, src, src_len, chunkname);
    }

    char hex[65];
    if (compute_key(src, src_len, hex) != 0) {
        return luaL_loadbuffer(L, src, src_len, chunkname);
    }

    char cache_path[PATH_MAX];
    int n = snprintf(cache_path, sizeof(cache_path), "%s%s.luac",
                     cache_root, hex);
    if (n < 0 || (size_t)n >= sizeof(cache_path)) {
        return luaL_loadbuffer(L, src, src_len, chunkname);
    }

    /* ── Cache hit: load precompiled bytecode. ─────────────────── */
    char *bc = NULL;
    size_t bc_len = 0;
    if (read_all(cache_path, &bc, &bc_len) == 0) {
        int rc = luaL_loadbuffer(L, bc, bc_len, chunkname);
        free(bc);
        if (rc == LUA_OK) return LUA_OK;
        /* Corrupt or mismatched bytecode — pop error, fall through
         * to source compile + cache rewrite. */
        lua_pop(L, 1);
        (void)unlink(cache_path);  /* best-effort eviction */
    }

    /* ── Cache miss: compile, then persist. ─────────────────────── */
    int rc = luaL_loadbuffer(L, src, src_len, chunkname);
    if (rc != LUA_OK) return rc;  /* parse error on stack; mirror loadbuffer */

    /* Dump compiled function (stack top) to bytecode.
     *
     * strip=0 keeps the source-name + line-number debug info. We
     * NEED this — `mod_db.c::lua_is_stdlib_caller()` consults
     * `lua_Debug.source` to decide whether a SQL caller is allowed
     * to touch `_hull_*` internal tables. Stripping makes
     * `ar.source` return "=?" and every stdlib write (session,
     * search, rbac, idempotency, ...) gets denied. The ~30%
     * on-disk size win isn't worth breaking the namespace gate. */
    DumpAcc acc = { NULL, 0, 0, 0 };
    int dump_rc = lua_dump(L, dump_writer, &acc, 0);
    if (dump_rc != 0 || acc.failed || !acc.buf) {
        /* Dump failed — keep the compiled function, skip caching. */
        free(acc.buf);
        return LUA_OK;
    }

    /* Best-effort persist; failures (disk full, sandbox veto, race
     * with another writer) are silent — the compiled function is
     * already on the stack and usable. */
    (void)write_atomic(cache_path, acc.buf, acc.len);
    free(acc.buf);
    return LUA_OK;
}
