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
#include "hull/cap/crypto.h"
#include "hull/release.h"   /* hl_release_verify_manifest_sig / pubkey_configured */

#include <keel/allocator.h>
#include <keel/client.h>
#include <keel/redirect.h>
#include "hull/tls_transport.h"
#include <keel/tls.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
    return "cosmo"; /* no darwin-x86_64 native artifact - fall back to APE */
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
    return "linux-aarch64";
#elif defined(__linux__)
    return "cosmo"; /* unknown Linux arch - APE fallback */
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

/* The HTTPS fetch path (open_tls + get) is the only part of this TU that
 * needs Keel's HTTP client; it ships only with HL_ENABLE_HTTP_CLIENT. The
 * offline helpers below (platform, self_path, json_str, sha256_hex,
 * find_checksum, atomic_write) are always built — `hull verify-self` and the
 * platform-signature verifier need them regardless of the HTTP client. */
#ifdef HL_ENABLE_HTTP_CLIENT

/* ── TLS context backed by embedded CA bundle ────────────────────── */

KlTlsCtx *hl_release_io_open_tls(KlAllocator *alloc)
{
    KlTlsCtx *tls = NULL;
    const unsigned char *cab = NULL;
    size_t cab_len = 0;
    if (hl_embedded_ca_bundle(&cab, &cab_len) == 0) {
        tls = hl_tls_client_ctx_create_from_buf(cab, cab_len, alloc);
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
                tls = hl_tls_client_ctx_create(*p, alloc);
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

    KlTlsConfig tls_cfg = {0};
    hl_tls_config_wire(&tls_cfg, tls);
    KlClientConfig cfg = {
        .timeout_ms        = 30000,
        /* 512 MB: a released binary / feature lib is small, but a multi-file
         * TOOL BUNDLE can be large - the `zig` toolchain is ~330 MB (a 168 MB
         * driver + its cross-libc tree). The download is SHA-256-verified
         * against the signed manifest, so this is a sanity bound, not a trust
         * boundary. Buffered in RAM only for the duration of an opt-in install. */
        .max_response_size = 512 * 1024 * 1024,
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

/* ── Verified release manifest (shared trust chain) ──────────────────── */

int hl_release_io_fetch_verified_manifest(const char *repo, const char *tag,
                                          KlAllocator *alloc, KlTlsCtx *tls,
                                          const char *ua,
                                          char **out_manifest,
                                          size_t *out_manifest_len,
                                          char **out_sig,
                                          size_t *out_sig_len)
{
    if (!repo || !tag || !alloc || !tls || !out_manifest || !out_manifest_len)
        return -1;
    if (!ua) ua = "hull";
    if (out_sig) *out_sig = NULL;
    if (out_sig_len) *out_sig_len = 0;

    char sha_url[256];
    snprintf(sha_url, sizeof(sha_url),
             "https://github.com/%s/releases/download/%s/hull.sha256", repo, tag);

    char *manifest = NULL;
    size_t manifest_len = 0;
    if (hl_release_io_get(sha_url, &manifest, &manifest_len, alloc, tls, ua) != 0) {
        fprintf(stderr, "%s: failed to download checksum manifest (%s)\n", ua, sha_url);
        return -1;
    }

    if (hl_release_pubkey_configured()) {
        char sig_url[256];
        snprintf(sig_url, sizeof(sig_url),
                 "https://github.com/%s/releases/download/%s/hull.sha256.sig", repo, tag);

        char *sig_hex = NULL;
        size_t sig_len = 0;
        if (hl_release_io_get(sig_url, &sig_hex, &sig_len, alloc, tls, ua) != 0) {
            fprintf(stderr,
                    "%s: failed to download release signature (hull.sha256.sig); "
                    "refusing to proceed\n", ua);
            kl_free(alloc, manifest, manifest_len);
            return -1;
        }
        int rc = hl_release_verify_manifest_sig(manifest, manifest_len,
                                                sig_hex, sig_len, NULL);
        if (rc != 0) {
            kl_free(alloc, sig_hex, sig_len);
            fprintf(stderr,
                    "%s: release signature verification FAILED (manifest does not "
                    "match the embedded release public key)\n", ua);
            kl_free(alloc, manifest, manifest_len);
            return -1;
        }
        fprintf(stdout, "%s: release signature verified\n", ua);
        /* Hand the sig bytes back if the caller wants to cache them for a
         * later offline re-verify; otherwise free them here. */
        if (out_sig) {
            *out_sig = sig_hex;
            if (out_sig_len) *out_sig_len = sig_len;
        } else {
            kl_free(alloc, sig_hex, sig_len);
        }
    } else {
        fprintf(stderr,
                "%s: WARNING: no embedded release public key; skipping Ed25519 "
                "signature check (SHA-256 only)\n", ua);
    }

    *out_manifest = manifest;
    *out_manifest_len = manifest_len;
    return 0;
}

#endif /* HL_ENABLE_HTTP_CLIENT */

/* ── Offline re-verify of an installed asset (build-time TOCTOU guard) ── */

/* Constant-time compare of two 64-char hex digests (both NUL-terminated). */
static int local_ct_hex_eq(const char *a, const char *b)
{
    unsigned diff = 0;
    for (int i = 0; i < 64; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca == 0 || cb == 0) return 0;  /* short digest: reject */
        diff |= (unsigned)(ca ^ cb);
    }
    return a[64] == '\0' && b[64] == '\0' && diff == 0;
}

/* Read a whole small file into a malloc'd, NUL-terminated buffer.
 * Returns 0 (with out and len set) on success, -1 otherwise. Caller frees. */
static int local_read_file(const char *path, char **out, size_t *len)
{
    *out = NULL; *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(64 * 1024 * 1024)) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return -1; }
    buf[sz] = '\0';
    *out = buf; *len = (size_t)sz;
    return 0;
}

int hl_release_io_verify_local_asset(const char *dir, const char *asset)
{
    if (!dir || !asset) return -1;
    char path[PATH_MAX];
    char *manifest = NULL, *sig = NULL, *lib = NULL;
    size_t mlen = 0, slen = 0, llen = 0;
    int result = -1;

    /* 1. Cached signed manifest (required; its absence means we can't verify). */
    if ((size_t)snprintf(path, sizeof(path), "%s/hull.sha256", dir) >= sizeof(path))
        return -1;
    if (local_read_file(path, &manifest, &mlen) != 0) {
        fprintf(stderr, "hull flavor: no cached signed manifest in %s "
                        "(run `hull flavor install` again)\n", dir);
        return -1;
    }

    /* 2. Signature check against the EMBEDDED release pubkey (the trust anchor,
     *    baked into this binary, NOT the writable cache dir). Skipped only on
     *    a placeholder pubkey, matching install-time behavior. */
    if (hl_release_pubkey_configured()) {
        if ((size_t)snprintf(path, sizeof(path), "%s/hull.sha256.sig", dir) >= sizeof(path))
            goto done;
        if (local_read_file(path, &sig, &slen) != 0) {
            fprintf(stderr, "hull flavor: cached manifest has no signature; "
                            "run `hull flavor install` again\n");
            goto done;
        }
        if (hl_release_verify_manifest_sig(manifest, mlen, sig, slen, NULL) != 0) {
            fprintf(stderr, "hull platform: cached manifest signature INVALID "
                            "(tampered ~/.hull/platform; reinstall)\n");
            goto done;
        }
    }

    /* 3. Expected digest from the now-verified manifest, actual from disk. */
    char expected[65], actual[65];
    if (hl_release_io_find_checksum(manifest, mlen, asset, expected) != 0) {
        fprintf(stderr, "hull platform: %s not in cached manifest (reinstall)\n", asset);
        goto done;
    }
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, asset) >= sizeof(path))
        goto done;
    if (local_read_file(path, &lib, &llen) != 0) goto done;
    if (hl_release_io_sha256_hex((const unsigned char *)lib, llen, actual) != 0)
        goto done;

    /* 4. Constant-time compare. */
    if (!local_ct_hex_eq(expected, actual)) {
        fprintf(stderr, "hull platform: installed %s does not match its signed "
                        "digest (tampered; reinstall)\n", asset);
        goto done;
    }
    result = 0;

done:
    free(manifest); free(sig); free(lib);
    return result;
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

/* ── SHA-256 ─────────────────────────────────────────────────────── */

int hl_release_io_sha256_hex(const unsigned char *data, size_t len,
                             char hex[65])
{
    unsigned char digest[32];
    /* Use the cap layer's self-contained SHA-256 (not mbedTLS) so this
     * helper links in the pure-compute flavor, where mbedTLS is dropped. */
    if (hl_cap_crypto_sha256(data, len, digest) != 0) return -1;
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
