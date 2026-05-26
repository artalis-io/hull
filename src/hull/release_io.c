/*
 * release_io.c — shared HTTPS + manifest + atomic-install helpers.
 *
 * Extracted from commands/update.c. Used by `hull update` and
 * `hull tools install` so the trust chain (release pubkey,
 * hull.sha256 signature, SHA-256 verification) is implemented once.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/release_io.h"
#include "hull/cacert.h"

#include <keel/allocator.h>
#include <keel/client.h>
#include <keel/redirect.h>
#include <keel/tls_mbedtls.h>
#include <keel/tls.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
# include <mach-o/dyld.h>
# include <limits.h>
#elif defined(__linux__) || defined(__COSMOPOLITAN__)
# include <limits.h>
#endif

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

/* ── Platform identifier ─────────────────────────────────────────── */

const char *hl_release_io_platform(void)
{
#if defined(__COSMOPOLITAN__)
    return "cosmo";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__)
    return "cosmo"; /* no darwin-x86_64 native artifact — fall back to APE */
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
    return "linux-aarch64";
#elif defined(__linux__)
    return "cosmo"; /* unknown Linux arch — APE fallback */
#else
    return "cosmo";
#endif
}

/* ── Resolve the running binary's path ───────────────────────────── */

int hl_release_io_self_path(char *out, size_t out_sz)
{
#if defined(__APPLE__)
    uint32_t sz = (uint32_t)out_sz;
    if (_NSGetExecutablePath(out, &sz) != 0)
        return -1;
    /* _NSGetExecutablePath may return a path with symlinks; canonicalize. */
    char *resolved = realpath(out, NULL);
    if (resolved) {
        snprintf(out, out_sz, "%s", resolved);
        free(resolved);
    }
    return 0;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
#else
    /* Cosmo: no /proc, no _NSGetExecutablePath. Caller must fall
     * back to argv[0]. */
    (void)out; (void)out_sz;
    return -1;
#endif
}

/* ── TLS context backed by embedded CA bundle ────────────────────── */

KlTlsCtx *hl_release_io_open_tls(KlAllocator *alloc)
{
    KlTlsCtx *tls = NULL;
    const unsigned char *cab = NULL;
    size_t cab_len = 0;
    if (hl_embedded_ca_bundle(&cab, &cab_len) == 0) {
        tls = kl_tls_mbedtls_client_ctx_create_from_buf(cab, cab_len, alloc);
    }
    if (!tls) {
        /* Fall back to a system CA bundle path. */
        const char *paths[] = {
            "/etc/ssl/cert.pem",
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            NULL,
        };
        for (const char **p = paths; *p && !tls; p++) {
            if (access(*p, R_OK) == 0)
                tls = kl_tls_mbedtls_client_ctx_create(*p, alloc);
        }
    }
    return tls;
}

/* ── HTTPS GET ───────────────────────────────────────────────────── */

int hl_release_io_get(const char *url,
                      char **out_body, size_t *out_len,
                      KlAllocator *alloc, KlTlsCtx *tls,
                      const char *user_agent)
{
    if (!url || !out_body || !out_len || !alloc || !tls) return -1;

    KlTlsConfig tls_cfg = {
        .ctx         = tls,
        .factory     = (KlTlsFactory)kl_tls_mbedtls_create,
        .ctx_destroy = (void (*)(KlTlsCtx *))kl_tls_mbedtls_ctx_destroy,
    };
    KlClientConfig cfg = {
        .timeout_ms        = 30000,
        .max_response_size = 200 * 1024 * 1024,  /* 200 MB headroom for releases */
        .tls               = &tls_cfg,
    };
    KlRedirectConfig redir = { .max_redirects = 10 };
    KlClientResponse resp;
    memset(&resp, 0, sizeof(resp));

    /* Compose UA: caller-supplied identifier + HL_VERSION. The
     * separator format matches RFC 7231 §5.5.3 — product/version
     * tokens separated by spaces. */
    char ua_buf[128];
    snprintf(ua_buf, sizeof(ua_buf), "%s/%s",
             user_agent ? user_agent : "hull", HL_VERSION);

    KlClientHeader hdr = { .name = "User-Agent", .value = ua_buf };

    int rc = kl_redirect_request(alloc, &cfg, &redir, "GET", url,
                                  &hdr, 1, NULL, 0, &resp);
    if (rc != 0) {
        kl_client_response_free(&resp);
        return -1;
    }
    if (resp.status < 200 || resp.status >= 300) {
        fprintf(stderr, "%s: %s returned HTTP %d\n",
                user_agent ? user_agent : "hull", url, resp.status);
        kl_client_response_free(&resp);
        return -1;
    }
    /* Steal the body buffer so kl_client_response_free doesn't drop it. */
    *out_len = resp.body_len;
    *out_body = (char *)resp.body;
    resp.body = NULL;
    resp.body_len = 0;
    kl_client_response_free(&resp);
    return 0;
}

/* ── JSON string extraction (deliberately tiny) ──────────────────── */

int hl_release_io_json_str(const char *json, const char *key,
                           char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz < 2) return -1;

    char needle[64];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || (size_t)nlen >= sizeof(needle)) return -1;
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += nlen;
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) p++;  /* skip simple escapes */
        out[i++] = *p++;
    }
    if (*p != '"') return -1;
    out[i] = '\0';
    return 0;
}

/* ── SHA-256 (mbedTLS) ───────────────────────────────────────────── */

extern int mbedtls_sha256(const unsigned char *input, size_t ilen,
                           unsigned char output[32], int is224);

int hl_release_io_sha256_hex(const unsigned char *data, size_t len,
                             char hex[65])
{
    unsigned char digest[32];
    if (mbedtls_sha256(data, len, digest, 0) != 0) return -1;
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i*2]   = h[digest[i] >> 4];
        hex[i*2+1] = h[digest[i] & 0xF];
    }
    hex[64] = '\0';
    return 0;
}

/* ── Checksum manifest lookup ────────────────────────────────────── */

int hl_release_io_find_checksum(const char *manifest, size_t mlen,
                                const char *asset, char hex_out[65])
{
    if (!manifest || !asset || !hex_out) return -1;
    size_t alen = strlen(asset);
    /* Each line is "<64-hex>  <asset>\n" */
    const char *p = manifest;
    const char *end = manifest + mlen;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t ll = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (ll >= 66 + alen) {
            const char *anchor = p + 64;
            /* Exact-match guard. Bounds check FIRST so we never
             * dereference `anchor[2 + alen]` when it points one past
             * the manifest buffer (the no-trailing-newline edge case).
             * The `||` short-circuits, so the deref only runs when
             * the byte is in-bounds. */
            if (anchor[0] == ' ' && anchor[1] == ' ' &&
                strncmp(anchor + 2, asset, alen) == 0 &&
                (anchor + 2 + alen >= end ||
                 anchor[2 + alen] == '\n' ||
                 anchor[2 + alen] == '\r' ||
                 anchor[2 + alen] == '\0')) {
                memcpy(hex_out, p, 64);
                hex_out[64] = '\0';
                return 0;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

/* ── Atomic write ────────────────────────────────────────────────── */

int hl_release_io_atomic_write(const char *target_path,
                               const void *data, size_t len,
                               int mode)
{
    if (!target_path || (!data && len > 0)) return -1;

    char new_path[PATH_MAX + 8];
    int n = snprintf(new_path, sizeof(new_path), "%s.new", target_path);
    if (n < 0 || (size_t)n >= sizeof(new_path)) return -1;

    int fd = open(new_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        fprintf(stderr, "atomic_write: cannot create %s: %s\n",
                new_path, strerror(errno));
        return -1;
    }
    const unsigned char *p = (const unsigned char *)data;
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, p + written, len - written);
        if (w <= 0) {
            fprintf(stderr, "atomic_write: write failed: %s\n",
                    strerror(errno));
            close(fd);
            unlink(new_path);
            return -1;
        }
        written += (size_t)w;
    }
    /* fsync before rename so a power-loss doesn't leave a half-written
     * executable behind. ENOSPC and EIO are the realistic failures;
     * either way, abort rather than rename a partially-flushed file. */
    if (fsync(fd) != 0) {
        fprintf(stderr, "atomic_write: fsync failed: %s\n", strerror(errno));
        close(fd);
        unlink(new_path);
        return -1;
    }
    /* close(2) can also surface deferred write errors on some
     * filesystems (NFS, network mounts) — treat the same way. */
    if (close(fd) != 0) {
        fprintf(stderr, "atomic_write: close failed: %s\n", strerror(errno));
        unlink(new_path);
        return -1;
    }

    /* chmod again — open(O_CREAT, mode) honours umask, which on most
     * systems clears the world / group bits we asked for. */
    if (chmod(new_path, (mode_t)mode) != 0) {
        fprintf(stderr, "atomic_write: chmod failed: %s\n", strerror(errno));
        unlink(new_path);
        return -1;
    }

    if (rename(new_path, target_path) != 0) {
        fprintf(stderr, "atomic_write: rename %s -> %s failed: %s\n",
                new_path, target_path, strerror(errno));
        unlink(new_path);
        return -1;
    }
    return 0;
}
